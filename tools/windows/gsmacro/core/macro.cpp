#include "macro.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace gsm {

std::shared_ptr<const CompiledMacro> compileMacro(const Macro& m, int id) {
    auto c = std::make_shared<CompiledMacro>();
    c->name = m.name;
    c->id = id;
    double tMs = 0;
    for (size_t i = 0; i < m.actions.size(); i++) {
        const Action& a = m.actions[i];
        if (a.op == Op::Wait) {
            if (a.ms > 0) tMs += a.ms;
            continue;
        }
        MacroEvent e;
        e.offset = (Time)std::llround(tMs * 1e6);
        e.op = a.op;
        e.buttons = a.buttons;
        e.axis = a.axis;
        e.raw = a.op == Op::SetAxis ? axisFromNormalized(a.axis, a.value) : 0;
        e.sourceIndex = (int)i;
        c->events.push_back(e);
    }
    c->duration = (Time)std::llround(tMs * 1e6);
    return c;
}

static std::string buttonsText(uint16_t mask) {
    std::string s;
    for (uint16_t b : kAllButtons) {
        if (!(mask & b)) continue;
        if (!s.empty()) s += "+";
        s += buttonName(b);
    }
    return s.empty() ? "none" : s;
}

std::vector<std::string> validateMacro(const Macro& m) {
    std::vector<std::string> w;
    if (m.actions.empty()) { w.push_back("Macro is empty."); return w; }
    // Track when each button/axis was last asserted, to find zero-length holds.
    double t = 0;
    double pressedAt[16];
    for (double& p : pressedAt) p = -1;
    double axisAt[AXIS_COUNT];
    for (double& p : axisAt) p = -1;
    bool anyWait = false;
    for (size_t i = 0; i < m.actions.size(); i++) {
        const Action& a = m.actions[i];
        char buf[200];
        switch (a.op) {
        case Op::Wait:
            if (a.ms <= 0) { snprintf(buf, sizeof buf, "Step %zu: wait of %.3g ms does nothing.", i + 1, a.ms); w.push_back(buf); }
            if (a.ms > 0) anyWait = true;
            t += std::max(0.0, a.ms);
            break;
        case Op::Press:
        case Op::Suppress:
            if (!a.buttons) { snprintf(buf, sizeof buf, "Step %zu: no button selected.", i + 1); w.push_back(buf); }
            for (int b = 0; b < 16; b++) if (a.buttons & (1u << b)) pressedAt[b] = t;
            break;
        case Op::Release:
            if (!a.buttons) { snprintf(buf, sizeof buf, "Step %zu: no button selected.", i + 1); w.push_back(buf); }
            for (int b = 0; b < 16; b++)
                if ((a.buttons & (1u << b)) && pressedAt[b] == t) {
                    snprintf(buf, sizeof buf, "Step %zu: %s is released at the same instant it was set - the game never sees it. Add a wait.",
                             i + 1, buttonName((uint16_t)(1u << b)));
                    w.push_back(buf);
                }
            for (int b = 0; b < 16; b++) if (a.buttons & (1u << b)) pressedAt[b] = -1;
            break;
        case Op::SetAxis:
            axisAt[a.axis] = t;
            break;
        case Op::ReleaseAxis:
            if (axisAt[a.axis] == t) {
                snprintf(buf, sizeof buf, "Step %zu: %s is released at the same instant it was set - the game never sees it.", i + 1, axisName(a.axis));
                w.push_back(buf);
            }
            axisAt[a.axis] = -1;
            break;
        case Op::ReleaseAll:
            break;
        }
    }
    for (int b = 0; b < 16; b++)
        if (pressedAt[b] >= 0 && pressedAt[b] == t) {
            w.push_back(std::string("Last step sets ") + buttonName((uint16_t)(1u << b)) +
                        " with no wait after it - the macro ends (dropping it) at the same instant.");
        }
    for (int a = 0; a < AXIS_COUNT; a++)
        if (axisAt[a] >= 0 && axisAt[a] == t)
            w.push_back(std::string("Last step sets ") + axisName((Axis)a) + " with no wait after it - it is dropped at once.");
    if (!anyWait) w.push_back("Macro has no waits: everything happens in one instant.");
    return w;
}

std::string actionToText(const Action& a) {
    char buf[96];
    switch (a.op) {
    case Op::Press: return "press " + buttonsText(a.buttons);
    case Op::Release: return "release " + buttonsText(a.buttons);
    case Op::Suppress: return "suppress " + buttonsText(a.buttons);
    case Op::SetAxis: snprintf(buf, sizeof buf, "axis %s %.4g", axisName(a.axis), a.value); return buf;
    case Op::ReleaseAxis: return std::string("axis-release ") + axisName(a.axis);
    case Op::ReleaseAll: return "release-all";
    case Op::Wait: snprintf(buf, sizeof buf, "wait %.4g", a.ms); return buf;
    }
    return "";
}

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static bool parseButtons(const std::string& s, uint16_t* mask, std::string* err) {
    *mask = 0;
    std::string tok;
    std::stringstream ss(s);
    while (std::getline(ss, tok, '+')) {
        tok = trim(tok);
        uint16_t b = buttonFromName(tok);
        if (!b) { *err = "unknown button '" + tok + "'"; return false; }
        *mask |= b;
    }
    if (!*mask) { *err = "no button given"; return false; }
    return true;
}

static bool parseNumber(const std::string& s, double* v) {
    if (s.empty()) return false;
    char* end = nullptr;
    *v = std::strtod(s.c_str(), &end);
    return end && *end == '\0' && std::isfinite(*v);
}

bool actionFromText(const std::string& raw, Action* out, std::string* err) {
    std::string line = trim(raw);
    std::stringstream ss(line);
    std::string verb, a1, a2, extra;
    ss >> verb >> a1 >> a2 >> extra;
    for (auto& c : verb) c = (char)std::tolower((unsigned char)c);
    if (!extra.empty()) { *err = "too many arguments"; return false; }
    Action a;
    if (verb == "press" || verb == "release" || verb == "suppress") {
        if (a1.empty() || !a2.empty()) { *err = verb + " takes one button list, e.g. 'press Cross+Square'"; return false; }
        if (!parseButtons(a1, &a.buttons, err)) return false;
        a.op = verb == "press" ? Op::Press : verb == "release" ? Op::Release : Op::Suppress;
    } else if (verb == "axis") {
        if (!axisFromName(a1, &a.axis)) { *err = "unknown axis '" + a1 + "' (LX LY RX RY L2 R2)"; return false; }
        if (!parseNumber(a2, &a.value)) { *err = "axis needs a value, e.g. 'axis LX -1'"; return false; }
        double lo = isTrigger(a.axis) ? 0 : -1;
        if (a.value < lo || a.value > 1) { *err = isTrigger(a.axis) ? "trigger value must be 0..1" : "stick value must be -1..1"; return false; }
        a.op = Op::SetAxis;
    } else if (verb == "axis-release") {
        if (!axisFromName(a1, &a.axis) || !a2.empty()) { *err = "usage: axis-release LX"; return false; }
        a.op = Op::ReleaseAxis;
    } else if (verb == "release-all") {
        if (!a1.empty()) { *err = "release-all takes no arguments"; return false; }
        a.op = Op::ReleaseAll;
    } else if (verb == "wait") {
        if (!parseNumber(a1, &a.ms) || !a2.empty()) { *err = "usage: wait <milliseconds>"; return false; }
        if (a.ms < 0 || a.ms > 60000) { *err = "wait must be 0..60000 ms"; return false; }
        a.op = Op::Wait;
    } else {
        *err = "unknown action '" + verb + "' (press, release, suppress, axis, axis-release, release-all, wait)";
        return false;
    }
    *out = a;
    return true;
}

std::string macroToText(const Macro& m) {
    std::string s;
    for (const auto& a : m.actions) s += actionToText(a) + "\n";
    return s;
}

bool macroFromText(const std::string& text, Macro* out, std::string* err) {
    Macro m;
    m.name = out->name;
    std::stringstream ss(text);
    std::string line;
    int n = 0;
    while (std::getline(ss, line)) {
        n++;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        if (trim(line).empty()) continue;
        Action a;
        std::string e;
        if (!actionFromText(line, &a, &e)) { *err = "line " + std::to_string(n) + ": " + e; return false; }
        m.actions.push_back(a);
    }
    *out = m;
    return true;
}

}  // namespace gsm
