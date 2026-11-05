#include "gui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "imgui_internal.h"

namespace gsm {

namespace {

// ── palette ──
const ImU32 kText = IM_COL32(230, 233, 239, 255);
const ImU32 kDim = IM_COL32(138, 147, 163, 255);
const ImU32 kFaint = IM_COL32(52, 58, 70, 255);
const ImU32 kPadBody = IM_COL32(32, 37, 46, 255);
const ImU32 kPadEdge = IM_COL32(64, 72, 88, 255);
const ImU32 kPhys = IM_COL32(76, 154, 255, 255);     // pressed by the player
const ImU32 kMacro = IM_COL32(255, 159, 67, 255);    // driven by a macro
const ImU32 kBlocked = IM_COL32(255, 92, 92, 255);   // held but withheld from the game
const ImU32 kOk = IM_COL32(61, 214, 140, 255);
const ImU32 kWarn = IM_COL32(255, 196, 61, 255);
const ImVec4 kWarnV = ImVec4(1.0f, 0.77f, 0.24f, 1);
const ImVec4 kDimV = ImVec4(0.54f, 0.58f, 0.64f, 1);
const ImVec4 kOkV = ImVec4(0.24f, 0.84f, 0.55f, 1);
const ImVec4 kErrV = ImVec4(1.0f, 0.36f, 0.36f, 1);

const char* kOpNames[] = {"press", "release", "suppress", "axis", "axis-release", "release-all", "wait"};

ImU32 macroColor(int id, int alpha) {
    return id == 0 ? IM_COL32(255, 159, 67, alpha) : IM_COL32(186, 104, 255, alpha);
}

std::string buttonsLabel(uint16_t mask) {
    std::string s;
    for (uint16_t b : kAllButtons) {
        if (!(mask & b)) continue;
        if (!s.empty()) s += "+";
        s += buttonName(b);
    }
    return s.empty() ? "(none)" : s;
}

int resizeCb(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = (std::string*)d->UserData;
        s->resize((size_t)d->BufTextLen);
        d->Buf = s->data();
    }
    return 0;
}
bool inputTextMultiline(const char* label, std::string* s, ImVec2 size) {
    return ImGui::InputTextMultiline(label, s->data(), s->capacity() + 1, size, ImGuiInputTextFlags_CallbackResize, resizeCb, s);
}
bool inputText(const char* label, std::string* s) {
    return ImGui::InputText(label, s->data(), s->capacity() + 1, ImGuiInputTextFlags_CallbackResize, resizeCb, s);
}

void sectionTitle(const char* t) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDimV);
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

void helpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

std::string fmtUs(Time ns) {
    char b[32];
    if (ns >= 1'000'000) snprintf(b, sizeof b, "%.2f ms", ns / 1e6);
    else snprintf(b, sizeof b, "%.1f us", ns / 1e3);
    return b;
}

}  // namespace

// ───────────────────────── lifecycle ─────────────────────────

App::App() {
    waiter_ = makeEngineWaiter();
    engine_ = std::make_unique<Engine>(clock_, *waiter_);
    scenarios_ = builtinScenarios();
    cfgPath_ = defaultConfigPath();
    std::string err;
    if (loadConfigFile(cfgPath_, &cfg_, &err)) fileStatus_ = "Loaded " + cfgPath_;
    else { cfg_ = defaultConfig(); fileStatus_ = "Using built-in templates (no saved config yet)"; }
    hidHideText_ = hidHideStatusText();
    startDemo();
}

App::~App() {
    player_.stop();
    stopEngine();
}

void App::stopEngine() {
    engine_->stop();
    live_ = LiveBackends();
}

void App::startDemo() {
    player_.stop();
    stopEngine();
    mode_ = Mode::Demo;
    demoIn_.set(GamepadState(), true);
    std::string err;
    if (!engine_->start(&demoIn_, &demoOut_, std::make_shared<const Config>(cfg_), &err)) engineError_ = err;
    else engineError_.clear();
    timeline_.clear();
}

void App::startLive() {
    player_.stop();
    stopEngine();
    LiveBackends lb;
    std::string err = createLiveBackends(&lb);
    if (err.empty()) {
        live_ = std::move(lb);
        if (engine_->start(live_.input.get(), live_.output.get(), std::make_shared<const Config>(cfg_), &err)) {
            mode_ = Mode::Live;
            engineError_.clear();
            timeline_.clear();
            return;
        }
        live_ = LiveBackends();
    }
    engineError_ = "Live start failed: " + err;
    startDemo();
    engineError_ = "Live start failed: " + err;
}

void App::applyConfig() {
    sanitize(&cfg_.settings);
    engine_->setConfig(std::make_shared<const Config>(cfg_));
}

bool App::playScenario(const std::string& name) {
    for (auto& s : scenarios_)
        if (s.name == name) {
            if (mode_ != Mode::Demo) return false;
            player_.play(s, &demoIn_);
            return true;
        }
    return false;
}

void App::pollEngine() {
    engine_->snapshot(&snap_);
    drainBuf_.clear();
    engine_->drainTimeline(&drainBuf_);
    for (auto& s : drainBuf_) timeline_.push_back(s);
    Time now = clock_.now();
    while (!timeline_.empty() && timeline_.front().t < now - ms(6000) && timeline_.size() > 1) timeline_.pop_front();

    if (rateBaseTime_ == 0) { rateBase_ = snap_; rateBaseTime_ = now; rateBaseProcCpu_ = processCpuSeconds(); }
    double dt = toMs(now - rateBaseTime_) / 1000.0;
    if (dt >= 1.0) {
        double pc = processCpuSeconds();
        rates_.cycles = (double)(snap_.cycles - std::min(snap_.cycles, rateBase_.cycles)) / dt;
        rates_.submits = (double)(snap_.submits - std::min(snap_.submits, rateBase_.submits)) / dt;
        rates_.inputs = (double)(snap_.inputChanges - std::min(snap_.inputChanges, rateBase_.inputChanges)) / dt;
        rates_.engineCpuPct = std::max(0.0, snap_.engineCpuSeconds - rateBase_.engineCpuSeconds) / dt * 100.0;
        rates_.processCpuPct = (pc - rateBaseProcCpu_) / dt * 100.0;
        rateBase_ = snap_;
        rateBaseTime_ = now;
        rateBaseProcCpu_ = pc;
    }
}

// ───────────────────────── style ─────────────────────────

void App::loadFonts(float scale) {
    scale_ = scale;
    ImGuiIO& io = ImGui::GetIO();
    const char* candidates[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "/usr/share/fonts/opentype/inter/Inter-Regular.otf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    };
    for (const char* p : candidates) {
        if (FILE* f = std::fopen(p, "rb")) {
            std::fclose(f);
            if (io.Fonts->AddFontFromFileTTF(p, 15.0f * scale)) return;
        }
    }
    ImFontConfig fc;
    fc.SizePixels = 13.0f * scale;
    io.Fonts->AddFontDefault(&fc);
}

void App::applyStyle(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 0;
    s.ChildRounding = 8;
    s.FrameRounding = 5;
    s.PopupRounding = 6;
    s.GrabRounding = 5;
    s.TabRounding = 6;
    s.ScrollbarRounding = 6;
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(8, 6);
    s.WindowPadding = ImVec2(12, 10);
    s.ChildBorderSize = 1;
    s.FrameBorderSize = 0;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.078f, 0.090f, 0.110f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0.106f, 0.122f, 0.149f, 1);
    c[ImGuiCol_PopupBg] = ImVec4(0.120f, 0.137f, 0.168f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.165f, 0.188f, 0.227f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.153f, 0.176f, 0.215f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.196f, 0.224f, 0.275f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.227f, 0.259f, 0.318f, 1);
    c[ImGuiCol_Button] = ImVec4(0.176f, 0.204f, 0.251f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.231f, 0.302f, 0.400f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.298f, 0.604f, 1.0f, 0.8f);
    c[ImGuiCol_Header] = ImVec4(0.196f, 0.243f, 0.318f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.231f, 0.302f, 0.400f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.298f, 0.604f, 1.0f, 0.6f);
    c[ImGuiCol_Tab] = ImVec4(0.137f, 0.157f, 0.192f, 1);
    c[ImGuiCol_TabHovered] = ImVec4(0.231f, 0.302f, 0.400f, 1);
    c[ImGuiCol_TabSelected] = ImVec4(0.204f, 0.267f, 0.361f, 1);
    c[ImGuiCol_CheckMark] = ImVec4(0.298f, 0.604f, 1.0f, 1);
    c[ImGuiCol_SliderGrab] = ImVec4(0.298f, 0.604f, 1.0f, 1);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.70f, 1.0f, 1);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.137f, 0.157f, 0.192f, 1);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_Separator] = ImVec4(0.165f, 0.188f, 0.227f, 1);
    c[ImGuiCol_Text] = ImVec4(0.902f, 0.914f, 0.937f, 1);
    c[ImGuiCol_TextDisabled] = kDimV;
    s.ScaleAllSizes(scale);
}

// ───────────────────────── frame ─────────────────────────

void App::frame() {
    pollEngine();

    // Manual demo input from the interactive pad (scenario playback has priority).
    if (mode_ == Mode::Demo && !player_.playing()) {
        GamepadState st = manual_;
        st.buttons |= latched_;
        demoIn_.set(st, true);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus);
    drawTopBar();
    ImGui::Spacing();

    float left = 372 * scale_, right = 372 * scale_;
    if (ImGui::BeginTable("##layout", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthFixed, left);
        ImGui::TableSetupColumn("mid", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("right", ImGuiTableColumnFlags_WidthFixed, right);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::BeginChild("##leftcol", ImVec2(0, 0), ImGuiChildFlags_None);
        drawControllerPanel();
        if (mode_ == Mode::Demo) drawScenarios();
        ImGui::EndChild();

        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("##midcol", ImVec2(0, 0), ImGuiChildFlags_None);
        drawMacroEditor();
        drawTimeline();
        ImGui::EndChild();

        ImGui::TableSetColumnIndex(2);
        ImGui::BeginChild("##rightcol", ImVec2(0, 0), ImGuiChildFlags_None);
        drawSyncSettings();
        drawDiagnostics();
        drawLivePanel();
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::End();
}

void App::drawTopBar() {
    ImGui::PushFont(nullptr);
    ImGui::TextUnformatted("gsmacro");
    ImGui::SameLine();
    ImGui::TextDisabled("GameSir Tarantula  3537:103D  |  low-latency controller macros");
    ImGui::PopFont();

    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 520 * scale_);
    // Mode badge.
    {
        const char* label = mode_ == Mode::Demo ? " DEMO MODE  (mock controller, no virtual device) " : " LIVE  (UNTESTED ON HARDWARE) ";
        ImVec2 sz = ImGui::CalcTextSize(label);
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImU32 col = mode_ == Mode::Demo ? IM_COL32(76, 154, 255, 60) : IM_COL32(255, 92, 92, 70);
        ImU32 edge = mode_ == Mode::Demo ? kPhys : kBlocked;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y - 2), ImVec2(p.x + sz.x, p.y + sz.y + 2), col, 4 * scale_);
        ImGui::GetWindowDrawList()->AddRect(ImVec2(p.x, p.y - 2), ImVec2(p.x + sz.x, p.y + sz.y + 2), edge, 4 * scale_);
        ImGui::TextUnformatted(label);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel macros")) engine_->cancelMacros();
    ImGui::SameLine();
    if (mode_ == Mode::Live && ImGui::Button("Stop live -> Demo")) startDemo();

    // Status strip.
    ImGui::Separator();
    auto chip = [&](const char* label, ImU32 dot) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float r = 4 * scale_, h = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + h * 0.5f), r, dot);
        ImGui::Dummy(ImVec2(r * 2.5f, h));
        ImGui::SameLine(0, 2 * scale_);
        ImGui::TextUnformatted(label);
        ImGui::SameLine(0, 18 * scale_);
    };
    chip(snap_.running ? "Engine running" : "Engine stopped", snap_.running ? kOk : kBlocked);
    std::string in = "Input: " + (snap_.inputName.empty() ? std::string("-") : snap_.inputName) +
                     (snap_.input.connected ? "  (connected)" : "  (disconnected)");
    chip(in.c_str(), snap_.input.connected ? kOk : kWarn);
    std::string out = "Output: " + (snap_.outputName.empty() ? std::string("-") : snap_.outputName);
    chip(out.c_str(), mode_ == Mode::Live ? kMacro : kDim);
    char q[96];
    if (snap_.currentMacro >= 0)
        snprintf(q, sizeof q, "Running: %s macro  |  queued %d", snap_.currentMacro == 0 ? "left" : "right", snap_.queued);
    else
        snprintf(q, sizeof q, "Idle  |  queued %d", snap_.queued);
    chip(q, snap_.currentMacro >= 0 ? macroColor(snap_.currentMacro, 255) : kFaint);
    ImGui::NewLine();
    if (!engineError_.empty()) ImGui::TextColored(kErrV, "%s", engineError_.c_str());
}

// ───────────────────────── controller drawing ─────────────────────────

namespace {

enum Glyph { G_NONE, G_TRIANGLE, G_CIRCLE, G_CROSS, G_SQUARE };

void drawGlyph(ImDrawList* dl, Glyph g, ImVec2 c, float r, ImU32 col, float th) {
    float k = r * 0.5f;
    switch (g) {
    case G_TRIANGLE: dl->AddTriangle(ImVec2(c.x, c.y - k * 1.05f), ImVec2(c.x + k, c.y + k * 0.75f), ImVec2(c.x - k, c.y + k * 0.75f), col, th); break;
    case G_CIRCLE: dl->AddCircle(c, k * 0.9f, col, 0, th); break;
    case G_CROSS:
        dl->AddLine(ImVec2(c.x - k * 0.8f, c.y - k * 0.8f), ImVec2(c.x + k * 0.8f, c.y + k * 0.8f), col, th);
        dl->AddLine(ImVec2(c.x + k * 0.8f, c.y - k * 0.8f), ImVec2(c.x - k * 0.8f, c.y + k * 0.8f), col, th);
        break;
    case G_SQUARE: dl->AddRect(ImVec2(c.x - k * 0.75f, c.y - k * 0.75f), ImVec2(c.x + k * 0.75f, c.y + k * 0.75f), col, 1, 0, th); break;
    default: break;
    }
}

}  // namespace

void App::drawPad(const char* id, const GamepadState& s, bool interactive, bool isOutput) {
    ImGui::PushID(id);
    const float sc = scale_ * std::min(1.0f, (ImGui::GetContentRegionAvail().x / scale_) / 340.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(340 * sc, 196 * sc);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto P = [&](float x, float y) { return ImVec2(p0.x + x * sc, p0.y + y * sc); };

    const GamepadState& in = snap_.input.state;
    const Overrides& ov = snap_.overrides;
    // Colour for a button bit on this pad.
    auto btnCol = [&](uint16_t bit, ImU32* edge) -> ImU32 {
        bool on = s.has(bit);
        *edge = kPadEdge;
        if (isOutput) {
            if (on && (ov.forceOn & bit)) return kMacro;
            if (!on && in.has(bit)) { *edge = kBlocked; return IM_COL32(255, 92, 92, 40); }
        }
        return on ? kPhys : kPadBody;
    };

    // Body.
    dl->AddRectFilled(P(6, 52), P(334, 190), IM_COL32(24, 28, 35, 255), 40 * sc);
    dl->AddRect(P(6, 52), P(334, 190), kFaint, 40 * sc, 0, 1.5f * sc);

    // Triggers and bumpers.
    auto trigger = [&](float x0, float x1, uint8_t v, Axis ax, uint16_t latchBit) {
        dl->AddRectFilled(P(x0, 6), P(x1, 18), kPadBody, 4 * sc);
        float f = v / 255.0f;
        bool over = isOutput && (ov.axisMask & (1u << ax));
        if (f > 0) dl->AddRectFilled(P(x0, 6), P(x0 + (x1 - x0) * f, 18), over ? kMacro : kPhys, 4 * sc);
        dl->AddRect(P(x0, 6), P(x1, 18), kPadEdge, 4 * sc);
        dl->AddText(P(x0 + 4, 5.5f), kDim, ax == AX_LT ? "L2" : "R2");
        if (interactive) {
            ImGui::SetCursorScreenPos(P(x0, 6));
            ImGui::InvisibleButton(ax == AX_LT ? "lt" : "rt", ImVec2((x1 - x0) * sc, 12 * sc));
            if (ImGui::IsItemClicked()) {
                uint8_t& t = ax == AX_LT ? manual_.lt : manual_.rt;
                t = t ? 0 : 255;
            }
            (void)latchBit;
        }
    };
    trigger(20, 110, s.lt, AX_LT, 0);
    trigger(230, 320, s.rt, AX_RT, 0);
    auto bumper = [&](float x0, float x1, uint16_t bit, const char* name) {
        ImU32 edge;
        ImU32 col = btnCol(bit, &edge);
        dl->AddRectFilled(P(x0, 24), P(x1, 42), col, 7 * sc);
        dl->AddRect(P(x0, 24), P(x1, 42), edge, 7 * sc, 0, 1.5f * sc);
        ImVec2 ts = ImGui::CalcTextSize(name);
        dl->AddText(ImVec2(P((x0 + x1) / 2, 33).x - ts.x / 2, P(0, 33).y - ts.y / 2), s.has(bit) ? IM_COL32(15, 18, 24, 255) : kText, name);
        if (interactive) {
            ImGui::SetCursorScreenPos(P(x0, 24));
            ImGui::InvisibleButton(name, ImVec2((x1 - x0) * sc, 18 * sc));
            if (ImGui::IsItemClicked()) latched_ ^= bit;  // bumpers latch so you can hold L1 and click Square
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to toggle (latched) - hold %s, then click Square", name);
        }
    };
    bumper(20, 110, BTN_LB, "L1");
    bumper(230, 320, BTN_RB, "R1");

    // Sticks.
    auto stick = [&](float cx, float cy, int x, int y, Axis ax, Axis ay, uint16_t clickBit, const char* sid) {
        const float r = 30;
        dl->AddCircleFilled(P(cx, cy), r * sc, kPadBody);
        dl->AddCircle(P(cx, cy), r * sc, s.has(clickBit) ? kPhys : kPadEdge, 0, 1.5f * sc);
        dl->AddLine(P(cx - r, cy), P(cx + r, cy), kFaint);
        dl->AddLine(P(cx, cy - r), P(cx, cy + r), kFaint);
        float nx = (float)axisToNormalized(ax, x), ny = (float)axisToNormalized(ay, y);
        bool over = isOutput && (ov.axisMask & ((1u << ax) | (1u << ay)));
        ImVec2 dot = P(cx + nx * (r - 8), cy - ny * (r - 8));
        dl->AddLine(P(cx, cy), dot, over ? kMacro : kPhys, 2 * sc);
        dl->AddCircleFilled(dot, 8 * sc, over ? kMacro : kPhys);
        if (interactive) {
            ImGui::SetCursorScreenPos(P(cx - r, cy - r));
            ImGui::InvisibleButton(sid, ImVec2(2 * r * sc, 2 * r * sc));
            if (ImGui::IsItemActive()) {
                ImVec2 m = ImGui::GetIO().MousePos, c = P(cx, cy);
                float dx = (m.x - c.x) / ((r - 8) * sc), dy = -(m.y - c.y) / ((r - 8) * sc);
                float len = std::sqrt(dx * dx + dy * dy);
                if (len > 1) { dx /= len; dy /= len; }
                manual_.setAxis(ax, axisFromNormalized(ax, dx));
                manual_.setAxis(ay, axisFromNormalized(ay, dy));
            } else if (ImGui::IsItemDeactivated()) {
                manual_.setAxis(ax, 0);
                manual_.setAxis(ay, 0);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to move the stick");
        }
    };
    stick(78, 96, s.lx, s.ly, AX_LX, AX_LY, BTN_LTHUMB, "ls");
    stick(218, 150, s.rx, s.ry, AX_RX, AX_RY, BTN_RTHUMB, "rs");

    // Momentary buttons (face, d-pad, centre).
    auto momentary = [&](ImVec2 c, float r, uint16_t bit, Glyph g, const char* label, bool round) {
        ImU32 edge;
        ImU32 col = btnCol(bit, &edge);
        if (round) {
            dl->AddCircleFilled(c, r, col);
            dl->AddCircle(c, r, edge, 0, 1.5f * sc);
        } else {
            dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, 3 * sc);
            dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), edge, 3 * sc, 0, 1.5f * sc);
        }
        ImU32 fg = s.has(bit) ? IM_COL32(15, 18, 24, 255) : kText;
        if (g != G_NONE) drawGlyph(dl, g, c, r, fg, 1.8f * sc);
        else if (label) {
            ImVec2 ts = ImGui::CalcTextSize(label);
            float fs = ImGui::GetFontSize() * 0.75f;
            dl->AddText(nullptr, fs, ImVec2(c.x - ts.x * 0.375f, c.y - fs / 2), fg, label);
        }
        if (interactive) {
            ImGui::SetCursorScreenPos(ImVec2(c.x - r, c.y - r));
            ImGui::InvisibleButton(buttonName(bit), ImVec2(2 * r, 2 * r));
            if (ImGui::IsItemActive()) manual_.buttons |= bit;
            else manual_.buttons &= (uint16_t)~bit;
        }
    };
    float fr = 11 * sc;
    momentary(P(282, 74), fr, BTN_Y, G_TRIANGLE, nullptr, true);
    momentary(P(306, 98), fr, BTN_B, G_CIRCLE, nullptr, true);
    momentary(P(282, 122), fr, BTN_A, G_CROSS, nullptr, true);
    momentary(P(258, 98), fr, BTN_X, G_SQUARE, nullptr, true);
    float dr = 8 * sc;
    momentary(P(134, 132), dr, BTN_DPAD_UP, G_NONE, "^", false);
    momentary(P(134, 166), dr, BTN_DPAD_DOWN, G_NONE, "v", false);
    momentary(P(117, 149), dr, BTN_DPAD_LEFT, G_NONE, "<", false);
    momentary(P(151, 149), dr, BTN_DPAD_RIGHT, G_NONE, ">", false);
    momentary(P(150, 82), 6 * sc, BTN_BACK, G_NONE, nullptr, false);
    momentary(P(190, 82), 6 * sc, BTN_START, G_NONE, nullptr, false);
    momentary(P(170, 106), 8 * sc, BTN_GUIDE, G_NONE, nullptr, true);

    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(size);
    ImGui::PopID();
}

void App::drawControllerPanel() {
    sectionTitle(mode_ == Mode::Demo ? "PHYSICAL INPUT  (mock - click / drag to play)" : "PHYSICAL INPUT");
    drawPad("in", snap_.input.state, mode_ == Mode::Demo && !player_.playing(), false);
    if (mode_ == Mode::Demo) {
        ImGui::TextDisabled("L1/R1/L2/R2 latch on click; buttons press while held.");
        if (latched_ || manual_.lt || manual_.rt) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Release latches")) { latched_ = 0; manual_.lt = manual_.rt = 0; }
        }
    }
    ImGui::Spacing();
    sectionTitle("VIRTUAL OUTPUT  (what the game would see)");
    drawPad("out", snap_.output, false, true);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto legend = [&](ImU32 c, const char* t) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = ImGui::GetTextLineHeight();
        dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.2f), ImVec2(p.x + h * 0.6f, p.y + h * 0.8f), c, 2);
        ImGui::Dummy(ImVec2(h * 0.7f, h));
        ImGui::SameLine(0, 4);
        ImGui::TextDisabled("%s", t);
        ImGui::SameLine(0, 14);
    };
    legend(kPhys, "passthrough");
    legend(kMacro, "macro");
    legend(kBlocked, "withheld");
    ImGui::NewLine();
}

void App::drawScenarios() {
    ImGui::Spacing();
    sectionTitle("DEMO SCENARIOS");
    std::string cur = player_.playing() ? player_.current() : "";
    for (auto& sc : scenarios_) {
        ImGui::PushID(sc.name.c_str());
        bool active = cur == sc.name;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.298f, 0.604f, 1.0f, 0.55f));
        if (ImGui::Button(active ? "Playing" : "Play", ImVec2(70 * scale_, 0))) player_.play(sc, &demoIn_);
        if (active) ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextUnformatted(sc.name.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", sc.description.c_str());
        ImGui::PopID();
    }
    if (!cur.empty()) {
        for (auto& sc : scenarios_)
            if (sc.name == cur) {
                ImGui::PushTextWrapPos(0);
                ImGui::TextColored(kDimV, "%s", sc.description.c_str());
                ImGui::PopTextWrapPos();
            }
    }
}

// ───────────────────────── macro editor ─────────────────────────

void App::drawMacroEditor() {
    sectionTitle("MACROS");
    if (ImGui::BeginTabBar("##macros")) {
        const char* tabs[2] = {"L1 + Square   (left)", "R1 + Square   (right)"};
        for (int side = 0; side < 2; side++) {
            ImGuiTabItemFlags f = requestTab_ == side ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(tabs[side], nullptr, f)) {
                drawMacroSteps(side);
                ImGui::EndTabItem();
            }
        }
        requestTab_ = -1;
        ImGui::EndTabBar();
    }
}

void App::drawMacroSteps(int side) {
    Macro& m = cfg_.macros[side];
    bool& enabled = side == 0 ? cfg_.settings.leftEnabled : cfg_.settings.rightEnabled;
    bool changed = false;
    ImGui::PushID(side);

    changed |= ImGui::Checkbox("Enabled", &enabled);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260 * scale_);
    changed |= inputText("Name", &m.name);
    ImGui::SameLine();
    if (ImGui::Checkbox("Edit as text", &textMode_[side]) && textMode_[side]) {
        textBuf_[side] = macroToText(m);
        textErr_[side].clear();
    }

    double tickMs = 1000.0 / cfg_.settings.gameTickHz;
    double total = 0;
    for (auto& a : m.actions) if (a.op == Op::Wait) total += std::max(0.0, a.ms);
    ImGui::TextDisabled("Total %.2f ms  =  %.2f game ticks at %.0f Hz  |  %zu steps", total, total / tickMs, cfg_.settings.gameTickHz, m.actions.size());

    if (textMode_[side]) {
        ImGui::TextDisabled("press / release / suppress <Btn+Btn>, axis <LX|LY|RX|RY|L2|R2> <value>, axis-release <axis>, release-all, wait <ms>");
        inputTextMultiline("##text", &textBuf_[side], ImVec2(-1, 230 * scale_));
        if (ImGui::Button("Apply text")) {
            Macro parsed;
            parsed.name = m.name;
            if (macroFromText(textBuf_[side], &parsed, &textErr_[side])) {
                m = parsed;
                textErr_[side].clear();
                changed = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert")) { textBuf_[side] = macroToText(m); textErr_[side].clear(); }
        if (!textErr_[side].empty()) ImGui::TextColored(kErrV, "%s", textErr_[side].c_str());
    } else {
        // Which wait step is executing (from the engine snapshot).
        int activeRow = -1;
        if (snap_.currentMacro == side) {
            double el = toMs(snap_.now - snap_.runStart), t = 0;
            for (size_t i = 0; i < m.actions.size(); i++) {
                if (m.actions[i].op != Op::Wait) continue;
                if (el >= t && el < t + m.actions[i].ms) { activeRow = (int)i; break; }
                t += m.actions[i].ms;
            }
        }
        int moveFrom = -1, moveTo = -1, removeAt = -1, dupAt = -1;
        ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
        float tableH = std::max(160 * scale_, std::min(330 * scale_, ImGui::GetContentRegionAvail().y - 330 * scale_));
        if (ImGui::BeginTable("##steps", 6, tf, ImVec2(0, tableH))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 26 * scale_);
            ImGui::TableSetupColumn("At", ImGuiTableColumnFlags_WidthFixed, 66 * scale_);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 118 * scale_);
            ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, 150 * scale_);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 96 * scale_);
            ImGui::TableHeadersRow();
            double at = 0;
            for (size_t i = 0; i < m.actions.size(); i++) {
                Action& a = m.actions[i];
                ImGui::PushID((int)i);
                ImGui::TableNextRow();
                if ((int)i == activeRow) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, macroColor(side, 70));
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%zu", i + 1);
                ImGui::TableSetColumnIndex(1);
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%.1f", at);
                ImGui::TableSetColumnIndex(2);
                ImGui::SetNextItemWidth(-1);
                int op = (int)a.op;
                if (ImGui::Combo("##op", &op, kOpNames, IM_ARRAYSIZE(kOpNames))) {
                    a.op = (Op)op;
                    if ((a.op == Op::Press || a.op == Op::Release || a.op == Op::Suppress) && !a.buttons) a.buttons = BTN_A;
                    if (a.op == Op::Wait && a.ms <= 0) a.ms = 10;
                    changed = true;
                }
                ImGui::TableSetColumnIndex(3);
                ImGui::SetNextItemWidth(-1);
                if (a.op == Op::Press || a.op == Op::Release || a.op == Op::Suppress) {
                    std::string lbl = buttonsLabel(a.buttons);
                    if (ImGui::Button((lbl + "##b").c_str(), ImVec2(-1, 0))) ImGui::OpenPopup("buttons");
                    if (ImGui::BeginPopup("buttons")) {
                        for (uint16_t b : kAllButtons) {
                            unsigned int mask = a.buttons;
                            char nm[48];
                            snprintf(nm, sizeof nm, "%s  (%s)", buttonName(b), buttonXName(b));
                            if (ImGui::CheckboxFlags(nm, &mask, b)) { a.buttons = (uint16_t)mask; changed = true; }
                        }
                        ImGui::EndPopup();
                    }
                } else if (a.op == Op::SetAxis || a.op == Op::ReleaseAxis) {
                    const char* axes[] = {"LX (left stick X)", "LY (left stick Y)", "RX (right stick X)", "RY (right stick Y)", "L2 (left trigger)", "R2 (right trigger)"};
                    int ax = a.axis;
                    if (ImGui::Combo("##ax", &ax, axes, IM_ARRAYSIZE(axes))) {
                        a.axis = (Axis)ax;
                        if (isTrigger(a.axis)) a.value = std::clamp(a.value, 0.0, 1.0);
                        changed = true;
                    }
                }
                ImGui::TableSetColumnIndex(4);
                ImGui::SetNextItemWidth(-1);
                if (a.op == Op::SetAxis) {
                    float v = (float)a.value;
                    if (ImGui::SliderFloat("##v", &v, isTrigger(a.axis) ? 0.0f : -1.0f, 1.0f, "%.2f")) { a.value = v; changed = true; }
                } else if (a.op == Op::Wait) {
                    float v = (float)a.ms;
                    ImGui::SetNextItemWidth(110 * scale_);
                    if (ImGui::DragFloat("##ms", &v, 0.1f, 0.0f, 2000.0f, "%.2f ms")) { a.ms = std::max(0.0f, v); changed = true; }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%.2f ticks", a.ms / tickMs);
                }
                ImGui::TableSetColumnIndex(5);
                if (ImGui::ArrowButton("##up", ImGuiDir_Up) && i > 0) { moveFrom = (int)i; moveTo = (int)i - 1; }
                ImGui::SameLine(0, 2);
                if (ImGui::ArrowButton("##dn", ImGuiDir_Down) && i + 1 < m.actions.size()) { moveFrom = (int)i; moveTo = (int)i + 1; }
                ImGui::SameLine(0, 2);
                if (ImGui::SmallButton("+")) dupAt = (int)i;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Duplicate");
                ImGui::SameLine(0, 2);
                if (ImGui::SmallButton("x")) removeAt = (int)i;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete");
                if (a.op == Op::Wait) at += std::max(0.0, a.ms);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (moveFrom >= 0) { std::swap(m.actions[moveFrom], m.actions[moveTo]); changed = true; }
        if (dupAt >= 0) { m.actions.insert(m.actions.begin() + dupAt + 1, m.actions[dupAt]); changed = true; }
        if (removeAt >= 0) { m.actions.erase(m.actions.begin() + removeAt); changed = true; }

        auto add = [&](const char* label, Action a) {
            if (ImGui::Button(label)) { m.actions.push_back(a); changed = true; }
            ImGui::SameLine();
        };
        add("+ Press", Action::press(BTN_A));
        add("+ Release", Action::release(BTN_A));
        add("+ Axis", Action::setAxis(AX_LX, 0));
        add("+ Axis release", Action::releaseAxis(AX_LX));
        add("+ Wait", Action::wait(10));
        if (ImGui::Button("Clear")) { m.actions.clear(); changed = true; }
    }

    auto warnings = validateMacro(m);
    for (auto& w : warnings) ImGui::TextColored(kWarnV, "! %s", w.c_str());
    if (changed) applyConfig();
    ImGui::PopID();
}

// ───────────────────────── timeline ─────────────────────────

void App::drawTimeline() {
    ImGui::Spacing();
    sectionTitle("OUTPUT TIMELINE");
    ImGui::Checkbox("Pause", &timelinePaused_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160 * scale_);
    ImGui::SliderFloat("Span", &timelineSpanMs_, 200, 4000, "%.0f ms", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    ImGui::TextDisabled("grid = %.0f Hz game ticks (%.2f ms)", cfg_.settings.gameTickHz, 1000.0 / cfg_.settings.gameTickHz);

    if (!timelinePaused_) timelineEnd_ = clock_.now();
    const Time end = timelineEnd_, span = ms(timelineSpanMs_), begin = end - span;

    struct Row { const char* name; bool out; uint16_t bit; };
    const Row rows[] = {{"Square in", false, BTN_X}, {"Square out", true, BTN_X}, {"L1 out", true, BTN_LB},
                        {"R1 out", true, BTN_RB}, {"Cross out", true, BTN_A}, {"Circle out", true, BTN_B}};
    const int nRows = IM_ARRAYSIZE(rows);
    const float rowH = 17 * scale_, anaH = 46 * scale_, labelW = 82 * scale_;
    float w = ImGui::GetContentRegionAvail().x;
    float h = nRows * rowH + 2 * anaH + 8 * scale_;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float x0 = p0.x + labelW, x1 = p0.x + w;
    auto X = [&](Time t) { return x0 + (float)((double)(t - begin) / (double)span) * (x1 - x0); };
    dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(x1, p0.y + h), IM_COL32(18, 21, 27, 255), 4);

    // Game-tick grid.
    Time tick = (Time)(1e9 / cfg_.settings.gameTickHz);
    if ((x1 - x0) / ((double)span / tick) > 3) {
        for (Time t = begin - (begin % tick); t <= end; t += tick)
            if (t >= begin) dl->AddLine(ImVec2(X(t), p0.y), ImVec2(X(t), p0.y + h), IM_COL32(255, 255, 255, 12));
    }
    // 100 ms labels.
    for (Time t = begin - (begin % ms(100)); t <= end; t += ms(100))
        if (t >= begin) dl->AddLine(ImVec2(X(t), p0.y + h - 4 * scale_), ImVec2(X(t), p0.y + h), kDim);

    // Collect samples in view (plus the one before, which defines the left edge).
    std::vector<const TimelineSample*> vis;
    const TimelineSample* before = nullptr;
    for (auto& s : timeline_) {
        if (s.t < begin) before = &s;
        else if (s.t <= end) vis.push_back(&s);
    }
    if (before) vis.insert(vis.begin(), before);

    // Macro bands.
    for (size_t i = 0; i < vis.size(); i++) {
        if (vis[i]->macro < 0) continue;
        Time a = std::max(vis[i]->t, begin), b = i + 1 < vis.size() ? vis[i + 1]->t : end;
        dl->AddRectFilled(ImVec2(X(a), p0.y), ImVec2(X(b), p0.y + h), macroColor(vis[i]->macro, 26));
    }

    for (int r = 0; r < nRows; r++) {
        float y = p0.y + 4 * scale_ + r * rowH;
        dl->AddText(ImVec2(p0.x, y), kDim, rows[r].name);
        dl->AddLine(ImVec2(x0, y + rowH - 2), ImVec2(x1, y + rowH - 2), kFaint);
        // Merge consecutive samples with the same on/colour state into one bar.
        Time runStart = 0;
        ImU32 runCol = 0;
        bool runOn = false;
        auto flush = [&](Time until) {
            if (runOn) dl->AddRectFilled(ImVec2(X(runStart), y + 2), ImVec2(std::max(X(until), X(runStart) + 1), y + rowH - 4), runCol, 2);
            runOn = false;
        };
        for (size_t i = 0; i < vis.size(); i++) {
            const GamepadState& st = rows[r].out ? vis[i]->out : vis[i]->in;
            bool on = st.has(rows[r].bit);
            ImU32 col = kPhys;
            if (rows[r].out && on && vis[i]->macro >= 0 && !vis[i]->in.has(rows[r].bit)) col = kMacro;
            Time a = std::max(vis[i]->t, begin);
            if (runOn && (!on || col != runCol)) flush(a);
            if (on && !runOn) { runOn = true; runStart = a; runCol = col; }
        }
        flush(end);
    }
    auto analog = [&](float y, const char* name, Axis ax) {
        dl->AddText(ImVec2(p0.x, y + anaH / 2 - ImGui::GetTextLineHeight() / 2), kDim, name);
        dl->AddLine(ImVec2(x0, y + anaH / 2), ImVec2(x1, y + anaH / 2), kFaint);
        for (int pass = 0; pass < 2; pass++) {  // input (dim) then output
            ImVec2 prev;
            bool have = false;
            for (size_t i = 0; i < vis.size(); i++) {
                const GamepadState& st = pass ? vis[i]->out : vis[i]->in;
                float v = (float)axisToNormalized(ax, st.axis(ax));
                float yy = y + anaH / 2 - v * (anaH / 2 - 3);
                Time a = std::max(vis[i]->t, begin), b = i + 1 < vis.size() ? vis[i + 1]->t : end;
                ImVec2 pa(X(a), yy), pb(X(b), yy);
                ImU32 col = pass ? (vis[i]->macro >= 0 && st.axis(ax) != vis[i]->in.axis(ax) ? kMacro : kPhys) : IM_COL32(138, 147, 163, 90);
                if (have) dl->AddLine(prev, pa, col, 1.5f * scale_);
                dl->AddLine(pa, pb, col, 1.5f * scale_);
                prev = pb;
                have = true;
            }
        }
    };
    float ya = p0.y + 4 * scale_ + nRows * rowH;
    analog(ya, "LX", AX_LX);
    analog(ya + anaH, "LY", AX_LY);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::TextDisabled("Blue = passthrough, orange = macro-driven, grey line = physical stick. Shaded = macro running.");
}

// ───────────────────────── settings ─────────────────────────

void App::drawSyncSettings() {
    sectionTitle("SYNCHRONIZATION");
    Settings& s = cfg_.settings;
    bool c = false;
    ImGui::PushItemWidth(150 * scale_);
    float v = (float)s.pollPeriodUs;
    if (ImGui::DragFloat("Input poll period", &v, 10, 100, 16000, "%.0f us")) { s.pollPeriodUs = v; c = true; }
    helpMarker("Used by polled sources (XInput). Event-driven sources wake the engine directly. Average sampling delay is half this period.");
    v = (float)s.spinTailUs;
    if (ImGui::DragFloat("Spin tail", &v, 5, 0, 2000, "%.0f us")) { s.spinTailUs = v; c = true; }
    helpMarker("The last part of each wait is spun instead of slept, for precise macro timing. 0 = lowest CPU, timing limited by the OS timer.");
    int qc = s.queueCapacity;
    if (ImGui::SliderInt("FIFO queue", &qc, 0, MacroScheduler::kMaxQueue)) { s.queueCapacity = qc; c = true; }
    helpMarker("Activations allowed to wait behind the running macro. Extra presses are dropped (counted in Diagnostics).");
    v = (float)s.interMacroGapMs;
    if (ImGui::DragFloat("Gap between macros", &v, 0.1f, 0, 200, "%.2f ms")) { s.interMacroGapMs = v; c = true; }
    helpMarker("Neutral time between queued macros so the game sees buttons released. Keep it above one game tick (8.33 ms at 120 Hz).");
    if (s.interMacroGapMs < 1000.0 / s.gameTickHz) {
        ImGui::TextColored(kWarnV, "Gap is shorter than one game tick (%.2f ms).", 1000.0 / s.gameTickHz);
    }
    int bb = (int)s.bothBumpers;
    const char* bbNames[] = {"latest pressed", "always left", "always right", "no macro"};
    if (ImGui::Combo("L1+R1+Square", &bb, bbNames, 4)) { s.bothBumpers = (BothBumpers)bb; c = true; }

    // Chord buttons.
    auto buttonCombo = [&](const char* label, uint16_t* bit) {
        int idx = 0;
        for (int i = 0; i < 15; i++) if (kAllButtons[i] == *bit) idx = i;
        if (ImGui::BeginCombo(label, buttonName(*bit))) {
            for (int i = 0; i < 15; i++) {
                uint16_t b = kAllButtons[i];
                bool taken = b != *bit && (b == s.chordButton || b == s.leftModifier || b == s.rightModifier);
                if (taken) ImGui::BeginDisabled();
                if (ImGui::Selectable(buttonName(b), i == idx)) { *bit = b; c = true; }
                if (taken) ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
    };
    buttonCombo("Chord button", &s.chordButton);
    buttonCombo("Left modifier", &s.leftModifier);
    buttonCombo("Right modifier", &s.rightModifier);
    v = (float)s.gameTickHz;
    if (ImGui::DragFloat("Game tick rate", &v, 1, 10, 1000, "%.0f Hz")) { s.gameTickHz = v; c = true; }
    helpMarker("Display only: draws the timeline grid and converts waits to ticks. Rocket League's physics runs at 120 Hz.");
    ImGui::PopItemWidth();
    if (c) applyConfig();

    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    inputText("##path", &cfgPath_);
    if (ImGui::Button("Save")) {
        std::string err;
        fileStatus_ = saveConfigFile(cfgPath_, cfg_, &err) ? "Saved " + cfgPath_ : "Save failed: " + err;
    }
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        Config c2;
        std::string err;
        if (loadConfigFile(cfgPath_, &c2, &err)) { cfg_ = c2; applyConfig(); fileStatus_ = "Loaded " + cfgPath_; }
        else fileStatus_ = "Load failed: " + err;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset templates")) { cfg_ = defaultConfig(); applyConfig(); fileStatus_ = "Reset to built-in templates (not saved)"; }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", fileStatus_.c_str());
    ImGui::PopTextWrapPos();
}

void App::drawDiagnostics() {
    ImGui::Spacing();
    sectionTitle("PERFORMANCE DIAGNOSTICS");
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(kDimV, "Measured live on THIS machine with the %s backend. Engine-internal timings only - not input-to-game latency.",
                       mode_ == Mode::Demo ? "mock" : "live");
    ImGui::PopTextWrapPos();
    if (ImGui::BeginTable("##rates", 2, ImGuiTableFlags_SizingStretchProp)) {
        auto row = [](const char* k, const char* fmt, double v) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", k);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text(fmt, v);
        };
        row("Engine wakes", "%.0f /s", rates_.cycles);
        row("Output submits", "%.0f /s", rates_.submits);
        row("Input changes", "%.0f /s", rates_.inputs);
        row("Engine thread CPU", "%.2f %% of a core", rates_.engineCpuPct);
        row("Whole process CPU", "%.1f %% (incl. GUI rendering)", rates_.processCpuPct);
        ImGui::EndTable();
    }
    if (ImGui::BeginTable("##hist", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Timing");
        ImGui::TableSetupColumn("n");
        ImGui::TableSetupColumn("mean");
        ImGui::TableSetupColumn("p99 <=");
        ImGui::TableSetupColumn("max");
        ImGui::TableHeadersRow();
        auto hrow = [&](const char* name, const Histogram& h, const char* tip) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(name);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%llu", (unsigned long long)h.n);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(h.n ? fmtUs((Time)h.meanNs()).c_str() : "-");
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(h.n ? fmtUs(h.percentileUpperBound(99)).c_str() : "-");
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(h.n ? fmtUs(h.max).c_str() : "-");
        };
        hrow("Cycle cost", snap_.cycleCost, "Read input + chord + scheduler + merge + submit, per engine wake.");
        hrow("Submit", snap_.submitCost, "Time inside the output sink's submit (mock: a mutex + copy; live: vigem_target_x360_update).");
        hrow("Timer wake err", snap_.wakeError, "How late the engine woke after a timed deadline (OS timer + spin tail).");
        hrow("Macro step late", snap_.eventLateness, "How late each timed macro event was applied vs its schedule.");
        hrow("Queued start late", snap_.startLateness, "How late a queued macro started vs its earliest allowed start.");
        ImGui::EndTable();
    }
    const auto& sc = snap_.sched;
    const auto& ch = snap_.chords;
    ImGui::TextDisabled("Chords  L %llu  R %llu  | Square alone %llu  | disabled %llu", (unsigned long long)ch.left,
                        (unsigned long long)ch.right, (unsigned long long)ch.passthrough, (unsigned long long)ch.disabled);
    ImGui::TextDisabled("Macros  started %llu  done %llu  dropped %llu  cancelled %llu", (unsigned long long)sc.started,
                        (unsigned long long)sc.completed, (unsigned long long)sc.dropped, (unsigned long long)sc.cancelled);
    if (snap_.submitFailures) ImGui::TextColored(kErrV, "Submit failures: %llu", (unsigned long long)snap_.submitFailures);
    if (ImGui::Button("Reset statistics")) engine_->resetStats();
}

void App::drawLivePanel() {
    ImGui::Spacing();
    sectionTitle("LIVE MODE  (WINDOWS, UNTESTED)");
    ImGui::PushTextWrapPos(0);
    if (!liveBackendsAvailable()) {
        ImGui::TextColored(kDimV, "Not available in this build. Live mode (XInput input + ViGEmBus virtual Xbox 360 pad) is compiled only for Windows.");
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::TextColored(kWarnV, "Live mode has NOT been tested with the real controller yet.");
    ImGui::TextDisabled("HidHide: %s", hidHideText_.c_str());
    ImGui::PopTextWrapPos();
    if (mode_ == Mode::Live) {
        ImGui::TextColored(kOkV, "Live output running.");
        if (ImGui::Button("Stop live output")) startDemo();
        return;
    }
    ImGui::Checkbox("HidHide hides the GameSir from games", &liveChecks_[0]);
    ImGui::Checkbox("gsmacro is on HidHide's allowed apps", &liveChecks_[1]);
    ImGui::Checkbox("reWASD config is unapplied", &liveChecks_[2]);
    bool ready = liveChecks_[0] && liveChecks_[1] && liveChecks_[2];
    if (!ready) ImGui::BeginDisabled();
    if (ImGui::Button("Start live output (creates a virtual Xbox 360 pad)")) startLive();
    if (!ready) ImGui::EndDisabled();
    if (ImGui::Button("Refresh HidHide status")) hidHideText_ = hidHideStatusText();
}

}  // namespace gsm
