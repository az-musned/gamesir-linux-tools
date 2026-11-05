#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace gsm {

const char* bothBumpersName(BothBumpers b) {
    switch (b) {
    case BothBumpers::Latest: return "latest";
    case BothBumpers::Left: return "left";
    case BothBumpers::Right: return "right";
    case BothBumpers::Ignore: return "ignore";
    }
    return "latest";
}

void sanitize(Settings* s) {
    s->pollPeriodUs = std::clamp(s->pollPeriodUs, 100.0, 16000.0);
    s->spinTailUs = std::clamp(s->spinTailUs, 0.0, 2000.0);
    s->queueCapacity = std::clamp(s->queueCapacity, 0, 8);
    s->interMacroGapMs = std::clamp(s->interMacroGapMs, 0.0, 1000.0);
    s->gameTickHz = std::clamp(s->gameTickHz, 10.0, 1000.0);
    if (!s->chordButton) s->chordButton = BTN_X;
    if (!s->leftModifier) s->leftModifier = BTN_LB;
    if (!s->rightModifier) s->rightModifier = BTN_RB;
}

// Templates only: they show the action types and timing style. They are NOT
// tuned wall-dash inputs - transcribe your working reWASD timings instead.
Config defaultConfig() {
    Config c;
    for (int side = 0; side < 2; side++) {
        double dir = side == 0 ? -1.0 : 1.0;
        Macro& m = c.macros[side];
        m.name = side == 0 ? "Wall dash left (example)" : "Wall dash right (example)";
        m.actions = {
            Action::setAxis(AX_LX, dir),   // stick toward the dash side
            Action::setAxis(AX_LY, 0.0),
            Action::press(BTN_A),          // jump
            Action::wait(25),
            Action::release(BTN_A),
            Action::wait(25),
            Action::press(BTN_A),          // second jump = dodge in stick direction
            Action::wait(25),
            Action::release(BTN_A),
            Action::wait(50),
            Action::releaseAxis(AX_LX),
            Action::releaseAxis(AX_LY),
            Action::wait(17),
        };
    }
    return c;
}

static const char* buttonOrDefault(uint16_t b) { return buttonName(b); }

std::string configToText(const Config& c) {
    const Settings& s = c.settings;
    char buf[1200];
    snprintf(buf, sizeof buf,
             "# gsmacro configuration\n"
             "[settings]\n"
             "poll_period_us = %.6g\n"
             "spin_tail_us = %.6g\n"
             "queue_capacity = %d\n"
             "inter_macro_gap_ms = %.6g\n"
             "both_bumpers = %s\n"
             "left_enabled = %s\n"
             "right_enabled = %s\n"
             "macros_armed = %s\n"
             "chord_button = %s\n"
             "left_modifier = %s\n"
             "right_modifier = %s\n"
             "game_tick_hz = %.6g\n",
             s.pollPeriodUs, s.spinTailUs, s.queueCapacity, s.interMacroGapMs, bothBumpersName(s.bothBumpers),
             s.leftEnabled ? "true" : "false", s.rightEnabled ? "true" : "false", s.macrosArmed ? "true" : "false",
             buttonOrDefault(s.chordButton), buttonOrDefault(s.leftModifier), buttonOrDefault(s.rightModifier),
             s.gameTickHz);
    std::string out = buf;
    const char* names[2] = {"left", "right"};
    for (int i = 0; i < 2; i++) {
        out += "\n[macro ";
        out += names[i];
        out += "]\nname = " + c.macros[i].name + "\n" + macroToText(c.macros[i]);
    }
    return out;
}

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string lower(std::string s) {
    for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

static bool parseBool(const std::string& v, bool* out) {
    std::string l = lower(v);
    if (l == "true" || l == "1" || l == "yes" || l == "on") { *out = true; return true; }
    if (l == "false" || l == "0" || l == "no" || l == "off") { *out = false; return true; }
    return false;
}

static bool parseDouble(const std::string& v, double* out) {
    char* end = nullptr;
    double d = std::strtod(v.c_str(), &end);
    if (v.empty() || !end || *end) return false;
    *out = d;
    return true;
}

bool configFromText(const std::string& text, Config* out, std::string* err) {
    Config c;  // starts from defaults so a partial file still works
    c.macros[0].actions.clear();
    c.macros[1].actions.clear();
    bool seenMacro[2] = {false, false};
    Config defaults = defaultConfig();
    enum { NONE, SETTINGS, MACRO } section = NONE;
    int macroIdx = -1;
    std::stringstream ss(text);
    std::string line;
    int n = 0;
    auto fail = [&](const std::string& m) { *err = "line " + std::to_string(n) + ": " + m; return false; };
    while (std::getline(ss, line)) {
        n++;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            std::string h = lower(trim(line.substr(1, line.find(']') == std::string::npos ? std::string::npos : line.find(']') - 1)));
            if (h == "settings") { section = SETTINGS; continue; }
            if (h == "macro left" || h == "macro right") {
                macroIdx = h == "macro left" ? 0 : 1;
                if (seenMacro[macroIdx]) return fail("duplicate section [" + h + "]");
                seenMacro[macroIdx] = true;
                section = MACRO;
                continue;
            }
            return fail("unknown section [" + h + "]");
        }
        size_t eq = line.find('=');
        if (section == SETTINGS) {
            if (eq == std::string::npos) return fail("expected key = value");
            std::string k = lower(trim(line.substr(0, eq))), v = trim(line.substr(eq + 1));
            Settings& s = c.settings;
            double d;
            bool b;
            if (k == "poll_period_us") { if (!parseDouble(v, &d)) return fail("bad number"); s.pollPeriodUs = d; }
            else if (k == "spin_tail_us") { if (!parseDouble(v, &d)) return fail("bad number"); s.spinTailUs = d; }
            else if (k == "queue_capacity") { if (!parseDouble(v, &d)) return fail("bad number"); s.queueCapacity = (int)d; }
            else if (k == "inter_macro_gap_ms") { if (!parseDouble(v, &d)) return fail("bad number"); s.interMacroGapMs = d; }
            else if (k == "game_tick_hz") { if (!parseDouble(v, &d)) return fail("bad number"); s.gameTickHz = d; }
            else if (k == "left_enabled") { if (!parseBool(v, &b)) return fail("bad bool"); s.leftEnabled = b; }
            else if (k == "right_enabled") { if (!parseBool(v, &b)) return fail("bad bool"); s.rightEnabled = b; }
            else if (k == "macros_armed") { if (!parseBool(v, &b)) return fail("bad bool"); s.macrosArmed = b; }
            else if (k == "both_bumpers") {
                std::string l = lower(v);
                if (l == "latest") s.bothBumpers = BothBumpers::Latest;
                else if (l == "left") s.bothBumpers = BothBumpers::Left;
                else if (l == "right") s.bothBumpers = BothBumpers::Right;
                else if (l == "ignore") s.bothBumpers = BothBumpers::Ignore;
                else return fail("both_bumpers must be latest|left|right|ignore");
            } else if (k == "chord_button" || k == "left_modifier" || k == "right_modifier") {
                uint16_t bit = buttonFromName(v);
                if (!bit) return fail("unknown button '" + v + "'");
                (k == "chord_button" ? s.chordButton : k == "left_modifier" ? s.leftModifier : s.rightModifier) = bit;
            } else return fail("unknown setting '" + k + "'");
        } else if (section == MACRO) {
            if (eq != std::string::npos && lower(trim(line.substr(0, eq))) == "name") {
                c.macros[macroIdx].name = trim(line.substr(eq + 1));
                continue;
            }
            Action a;
            std::string e;
            if (!actionFromText(line, &a, &e)) return fail(e);
            c.macros[macroIdx].actions.push_back(a);
        } else {
            return fail("text outside a section");
        }
    }
    for (int i = 0; i < 2; i++)
        if (!seenMacro[i]) c.macros[i] = defaults.macros[i];
    if (c.settings.chordButton == c.settings.leftModifier || c.settings.chordButton == c.settings.rightModifier ||
        c.settings.leftModifier == c.settings.rightModifier) {
        *err = "chord_button, left_modifier and right_modifier must be three different buttons";
        return false;
    }
    sanitize(&c.settings);
    *out = c;
    return true;
}

bool loadConfigFile(const std::string& path, Config* out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { *err = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    return configFromText(ss.str(), out, err);
}

bool saveConfigFile(const std::string& path, const Config& c, std::string* err) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { *err = "cannot write " + tmp; return false; }
        f << configToText(c);
        if (!f) { *err = "write failed"; return false; }
    }
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) { *err = "cannot replace " + path; return false; }
    return true;
}

}  // namespace gsm
