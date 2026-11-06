#include "gui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "imgui.h"
#include "imgui_internal.h"
#include "../core/merged_source.h"

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
    ioPath_ = cfgPath_;
    std::string err;
    bool existed = std::ifstream(cfgPath_).good();
    if (existed && loadConfigFile(cfgPath_, &cfg_, &err)) {
        setStatus("Loaded " + cfgPath_, false);
    } else if (!existed && std::ifstream(bundledConfigPath()).good() && loadConfigFile(bundledConfigPath(), &cfg_, &err)) {
        // First run with a gsmacro.cfg shipped next to the exe (e.g. a shared package): use it and save it as ours.
        std::string serr;
        if (saveConfigFile(cfgPath_, cfg_, &serr)) setStatus("Loaded the macros shipped with gsmacro and saved them to " + cfgPath_, false);
        else setStatus("Loaded the macros shipped with gsmacro (could not save: " + serr + ")", true);
    } else {
        cfg_ = defaultConfig();
        if (existed) {
            setStatus("Could not load " + cfgPath_ + " - using the built-in example macros", true);
            showError("Your saved configuration could not be loaded", err,
                      "The file was left untouched. Fix the line mentioned above in a text editor, or use Export to "
                      "overwrite it with the current settings.");
        } else {
            setStatus("No saved configuration yet - using the built-in EXAMPLE macros (not your wall-dash timings)", false);
        }
    }
    startDemo();
    if (liveBackendsAvailable() && !existed) showSetup(true);  // first run on Windows
}

App::~App() {
    player_.stop();
    stopEngine();
}

void App::stopEngine() {
    engine_->stop();
    live_ = LiveBackends();
    virtualSlot_ = -1;
    readbackOk_ = false;
}

void App::startDemo() {
    player_.stop();
    stopEngine();
    mode_ = Mode::Demo;
    demoIn_.set(GamepadState(), true);
    std::string err;
    if (!engine_->start(&demoIn_, &demoOut_, std::make_shared<const Config>(cfg_), &err))
        showError("Demo Mode could not start", err, "This should not happen; please report it with the message above.");
    timeline_.clear();
}

void App::startLive(bool quiet) {
    player_.stop();
    stopEngine();
    // Live Mode always starts as pure passthrough: macros are armed only after
    // the user has confirmed normal input works.
    cfg_.settings.macrosArmed = false;
    confirmedPassthrough_ = false;
    selfMatch_ = selfMismatch_ = 0;
    LiveBackends lb;
    std::string err = createLiveBackends(&lb, cfg_.settings.controller);
    if (err.empty()) {
        live_ = std::move(lb);
        if (engine_->start(live_.input.get(), live_.output.get(), std::make_shared<const Config>(cfg_), &err)) {
            mode_ = Mode::Live;
            timeline_.clear();
            gameRunning_ = -1;  // background mode re-applies the game-based arming
            return;
        }
        live_ = LiveBackends();
    }
    if (quiet) {
        startDemo();
        autoStatus_ = "Live Mode could not start yet (" + err + ") - retrying";
        return;
    }
    std::string guidance;
    if (err.find("ViGEmClient.dll") != std::string::npos)
        guidance = "Keep ViGEmClient.dll in the same folder as gsmacro.exe (re-extract the ZIP if it is missing).";
    else if (err.find("ViGEmBus") != std::string::npos || err.find("vigem_connect") != std::string::npos)
        guidance = "The ViGEmBus driver is not reachable. Check Device Manager > System devices > 'Nefarius Virtual Gamepad "
                   "Emulation Bus'. gsmacro never installs drivers.";
    else if (err.find("xinput") != std::string::npos)
        guidance = "XInput is unavailable. gsmacro needs Windows 10 or 11.";
    else
        guidance = "Run the setup checks (Setup button) to see which part is missing.";
    startDemo();
    showError("Live Mode could not start", err, guidance + " gsmacro is back in Demo Mode; your controller is unaffected.");
}

// ───────────────────────── background mode ─────────────────────────

void App::enableAutostart() {
    autostart_ = true;
    setupOpen_ = false;   // never pop the first-run wizard in the background
    nextLiveTry_ = 0;
    autoStatus_ = "Background mode: starting Live Mode";
}

void App::backgroundTick() {
    if (!autostart_) return;
    Time now = clock_.now();

    // 1. Before Live Mode: free XInput slot 0 for the virtual pad (games treat
    //    slot 0 as player 1). This must happen before gsmacro reads the GameSir
    //    through XInput: XInput keeps the device open, and Windows then refuses
    //    to disable it ("reboot needed"). Only the PnP tree is queried here.
    if (mode_ != Mode::Live) {
        if (now < nextLiveTry_) return;
        if (boot_ == Boot::Start) {
            // Only the GameSir is an XInput pad that can hold slot 0; a DualSense
            // (raw HID) never takes an XInput slot, so nothing needs freeing.
            if (cfg_.settings.controller != ControllerType::DualSense && gameSirXusbState() >= 0) {
                std::string err;
                if (runSlot0Task(&err)) {
                    boot_ = Boot::WaitOff;
                    bootDeadline_ = now + ms(30000);
                    autoStatus_ = "Freeing XInput slot 0 (GameSir briefly off)";
                } else {
                    boot_ = Boot::Ready;
                    autoStatus_ = err;
                }
            } else {
                boot_ = Boot::Ready;  // not plugged in: it will join after the virtual pad (slot 1)
            }
        }
        if (boot_ == Boot::WaitOff) {
            if (gameSirXusbState() != 0) boot_ = Boot::Ready;  // its XInput interface is off: slot 0 is free
            else if (now >= bootDeadline_) { boot_ = Boot::Ready; autoStatus_ = "The slot-0 task did not free slot 0"; }
            else return;
        }
        // 2. Live Mode (retried: ViGEmBus may not be ready right after login).
        nextLiveTry_ = now + ms(5000);
        startLive(true);  // the task re-enables the GameSir once the virtual pad has slot 0
        return;
    }
    if (virtualSlot_ < 0) virtualSlot_ = virtualPadSlot(&live_);
    slotWarn_ = virtualSlot_ > 0;
    if (slotWarn_) autoStatus_ = "Virtual pad is in XInput slot " + std::to_string(virtualSlot_) + ", not player 1 - restart gsmacro (tray > Exit, then start it)";

    // 3. Arm the macros only while the game runs (Square is a hidden shift key
    //    while armed, so everything else keeps a normal Square).
    if (now >= nextGameCheck_) {
        nextGameCheck_ = now + ms(1000);
        int running = isProcessRunning(kGameExe) ? 1 : 0;
        if (running != gameRunning_) {
            gameRunning_ = running;
            cfg_.settings.macrosArmed = running == 1;
            applyConfig();
            if (!slotWarn_)
                autoStatus_ = running ? "Rocket League running - macros ARMED" : "Rocket League not running - macros off";
        }
    }
}

void App::applyConfig() {
    sanitize(&cfg_.settings);
    engine_->setConfig(std::make_shared<const Config>(cfg_));
}

void App::setTextMode(int side, bool on) {
    textMode_[side] = on;
    if (on) { textBuf_[side] = macroToText(cfg_.macros[side]); textErr_[side].clear(); }
}

void App::runChecks() {
    chkController_ = checkController();
    chkViGEm_ = checkViGEm();
    chkHidHide_ = checkHidHide();
}

void App::showError(const std::string& title, const std::string& text, const std::string& guidance) {
    errTitle_ = title;
    errText_ = text;
    errGuidance_ = guidance;
    errPending_ = true;
}

bool App::choosePath(bool save, std::string* path) {
    if (pickFile(save, path)) return true;
    // No native dialog on this host: use the path typed in the box.
    if (!liveBackendsAvailable()) { *path = ioPath_; return !path->empty(); }
    return false;  // cancelled
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

void App::updateSelfTest() {
    if (mode_ != Mode::Live) return;
    Time now = clock_.now();
    if (virtualSlot_ < 0) virtualSlot_ = virtualPadSlot(&live_);
    readbackOk_ = virtualSlot_ >= 0 && readXInputSlot(virtualSlot_, &readback_);
    // Compare only once the engine output has been stable for 30 ms, so the
    // GUI's own sampling lag never counts as a mismatch.
    if (snap_.output != prevSnapOut_) {
        prevSnapOut_ = snap_.output;
        outStableSince_ = now;
    } else if (readbackOk_ && now - outStableSince_ > ms(30)) {
        (readback_ == snap_.output ? selfMatch_ : selfMismatch_)++;
    }
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
    updateSelfTest();
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
    pollAdmin();

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
    drawStatusStrip();

    float left = 372 * scale_, right = 380 * scale_;
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
        else drawLiveSelfTest();
        ImGui::EndChild();

        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("##midcol", ImVec2(0, 0), ImGuiChildFlags_None);
        drawMacroEditor();
        drawTimeline();
        ImGui::EndChild();

        ImGui::TableSetColumnIndex(2);
        ImGui::BeginChild("##rightcol", ImVec2(0, 0), ImGuiChildFlags_None);
        drawSyncSettings();
        ImGui::Spacing();
        sectionTitle("WINDOWS SETUP");
        drawOneClickSetup();
        drawConfigFile();
        drawDiagnostics();
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::End();
    drawSetupWindow();
    drawErrorPopup();
}

void App::drawTopBar() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("gsmacro");
    ImGui::SameLine();
    ImGui::TextDisabled("GameSir Tarantula 3537:103D  -  controller-native macros");

    // Right-aligned controls.
    const float w = 640 * scale_;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 10, ImGui::GetWindowContentRegionMax().x - w));
    auto segment = [&](const char* label, bool active) {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.298f, 0.604f, 1.0f, 0.75f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.298f, 0.604f, 1.0f, 0.85f));
        }
        bool clicked = ImGui::Button(label);
        if (active) ImGui::PopStyleColor(2);
        return clicked && !active;
    };
    if (segment(" Demo ", mode_ == Mode::Demo)) startDemo();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mock controller, no virtual device. Safe to explore.");
    ImGui::SameLine(0, 2);
    if (segment(" Live ", mode_ == Mode::Live)) showSetup(true);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Real controller -> virtual Xbox 360 pad. Opens the guided setup first.");
    ImGui::SameLine(0, 14 * scale_);

    // Armed pill.
    bool armed = cfg_.settings.macrosArmed;
    ImGui::PushStyleColor(ImGuiCol_Button, armed ? ImVec4(1.0f, 0.62f, 0.26f, 0.85f) : ImVec4(0.25f, 0.28f, 0.33f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, armed ? ImVec4(1.0f, 0.70f, 0.38f, 0.95f) : ImVec4(0.32f, 0.36f, 0.42f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, armed ? ImVec4(0.08f, 0.09f, 0.11f, 1) : ImVec4(0.9f, 0.91f, 0.94f, 1));
    if (ImGui::Button(armed ? " MACROS ARMED " : " MACROS OFF ")) {
        if (!armed && mode_ == Mode::Live && !confirmedPassthrough_) ImGui::OpenPopup("arm?");
        else { cfg_.settings.macrosArmed = !armed; applyConfig(); }
    }
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(armed ? "L1/R1 + Square run macros. Click to switch to pure passthrough."
                                : "Pure passthrough: Square always reaches the game. Click to arm the chord macros.");
    if (ImGui::BeginPopup("arm?")) {
        ImGui::TextUnformatted("You have not confirmed that passthrough works yet (Setup, step 4).");
        if (ImGui::Button("Arm anyway")) { cfg_.settings.macrosArmed = true; applyConfig(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Open setup")) { showSetup(true); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, 14 * scale_);
    if (ImGui::Button("Setup...")) showSetup(true);
    ImGui::SameLine();
    if (ImGui::Button("Cancel macros")) engine_->cancelMacros();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop the running macro, clear the queue and release everything the macros hold.");
}

void App::drawStatusStrip() {
    ImGui::Separator();
    auto chip = [&](const char* label, ImU32 dot, const char* tip) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float r = 4 * scale_, h = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + h * 0.5f), r, dot);
        ImGui::Dummy(ImVec2(r * 2.5f, h));
        ImGui::SameLine(0, 2 * scale_);
        ImGui::TextUnformatted(label);
        if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        ImGui::SameLine(0, 18 * scale_);
    };
    auto stateCol = [](CheckResult::State s) {
        return s == CheckResult::Ok ? kOk : s == CheckResult::Warn ? kWarn : s == CheckResult::Fail ? kBlocked : kDim;
    };
    char b[160];
    chip(snap_.running ? "Engine running" : "Engine stopped", snap_.running ? kOk : kBlocked, nullptr);
    if (mode_ == Mode::Demo) {
        chip("Controller: mock (Demo)", kPhys, "Demo Mode uses a simulated controller.");
        chip("Virtual pad: none", kDim, "Demo Mode never creates a virtual device.");
    } else {
        if (cfg_.settings.controller == ControllerType::Auto) {
            auto* m = dynamic_cast<MergedSource*>(live_.input.get());
            bool g = m && m->firstConnected(), d = m && m->secondConnected();
            const char* t = g && d ? "GameSir + DualSense connected" : g ? "GameSir connected" : d ? "DualSense connected (USB)"
                                                                         : "No controller found - connect the GameSir or a DualSense (USB)";
            chip(t, g || d ? kOk : kBlocked,
                 "Controller = auto: reads the GameSir (XInput, VID 3537) and a DualSense (USB HID) at the same time; "
                 "whichever is plugged in works, both together are merged.");
        } else if (cfg_.settings.controller == ControllerType::DualSense)
            chip(snap_.input.connected ? "DualSense connected (USB)" : "DualSense NOT found - connect it by USB cable",
                 snap_.input.connected ? kOk : kBlocked, "Physical controller read as raw HID (Sony 054C:0CE6/0DF2), event-driven.");
        else
            chip(snap_.input.connected ? "GameSir connected" : "GameSir NOT found - check cable / PC mode", snap_.input.connected ? kOk : kBlocked,
                 "Physical controller read through XInput (VID 3537).");
        if (virtualSlot_ >= 0) snprintf(b, sizeof b, "Virtual Xbox 360 pad: XInput slot %d", virtualSlot_);
        else snprintf(b, sizeof b, "Virtual Xbox 360 pad: created (slot pending)");
        chip(b, snap_.submitFailures ? kBlocked : kOk, "Created through ViGEmBus. Removed when Live Mode stops or gsmacro closes.");
    }
    if (autostart_)
        chip(autoStatus_.c_str(), slotWarn_ ? kBlocked : kOk,
             "Background mode (--autostart): Live Mode starts by itself, macros arm while RocketLeague.exe runs, and the "
             "virtual pad is moved to XInput slot 0. Close the window to hide it; tray icon > Exit to quit.");
    if (liveBackendsAvailable()) {
        chip("ViGEmBus", stateCol(chkViGEm_.state), chkViGEm_.text.empty() ? "Not checked yet - open Setup." : chkViGEm_.text.c_str());
        chip("HidHide", stateCol(chkHidHide_.state), chkHidHide_.text.empty() ? "Not checked yet - open Setup." : chkHidHide_.text.c_str());
    }
    if (snap_.currentMacro >= 0)
        snprintf(b, sizeof b, "Running: %s macro  |  queued %d", snap_.currentMacro == 0 ? "left" : "right", snap_.queued);
    else
        snprintf(b, sizeof b, "Idle  |  queued %d", snap_.queued);
    chip(b, snap_.currentMacro >= 0 ? macroColor(snap_.currentMacro, 255) : kFaint, nullptr);
    ImGui::NewLine();
    if (mode_ == Mode::Live && snap_.submitFailures)
        ImGui::TextColored(kErrV, "Virtual pad updates are failing (%llu). ViGEmBus may have stopped: click Demo, then run Setup checks.",
                           (unsigned long long)snap_.submitFailures);
    if (mode_ == Mode::Live && !cfg_.settings.macrosArmed)
        ImGui::TextColored(kWarnV, "Live Mode: macros are OFF (pure passthrough) until you confirm normal input works - see Setup step 7.");
    ImGui::Spacing();
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
    sectionTitle(mode_ == Mode::Demo ? "PHYSICAL INPUT  (mock - click / drag to play)" : "PHYSICAL INPUT  (GameSir via XInput)");
    drawPad("in", snap_.input.state, mode_ == Mode::Demo && !player_.playing(), false);
    if (mode_ == Mode::Demo) {
        ImGui::TextDisabled("L1/R1/L2/R2 latch on click; buttons press while held.");
        if (latched_ || manual_.lt || manual_.rt) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Release latches")) { latched_ = 0; manual_.lt = manual_.rt = 0; }
        }
    } else if (!snap_.input.connected) {
        ImGui::TextColored(kErrV, "Controller not found. The output is held neutral; gsmacro rescans every second.");
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

void App::drawLiveSelfTest() {
    ImGui::Spacing();
    sectionTitle("LIVE SELF-TEST");
    ImGui::PushTextWrapPos(0);
    if (virtualSlot_ < 0) {
        ImGui::TextColored(kWarnV, "Waiting for Windows to assign the virtual pad an XInput slot... If this stays, another "
                                   "Xbox 360 pad appeared at the same time and gsmacro can't tell which slot is its own.");
    } else if (!readbackOk_) {
        ImGui::TextColored(kErrV, "Virtual pad (slot %d) can't be read back through XInput.", virtualSlot_);
    } else {
        uint64_t total = selfMatch_ + selfMismatch_;
        ImGui::Text("Virtual pad read back from XInput slot %d.", virtualSlot_);
        if (total < 60) ImGui::TextDisabled("Move the sticks and press buttons to collect samples (%llu so far).", (unsigned long long)total);
        else {
            double pct = 100.0 * (double)selfMatch_ / (double)total;
            ImGui::TextColored(pct > 99.0 ? kOkV : kWarnV, "Readback equals engine output in %.1f %% of %llu stable samples.", pct,
                               (unsigned long long)total);
        }
        ImGui::TextDisabled("This only proves the virtual pad mirrors gsmacro's output. Check the game sees exactly one controller "
                            "(Setup step 6) before playing.");
    }
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Reset self-test")) selfMatch_ = selfMismatch_ = 0;
    ImGui::SameLine();
    if (ImGui::Button("Open joy.cpl")) openGameControllersPanel();
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
                drawMacroHeader(side);
                if (!textMode_[side]) drawMacroVisual(side);
                drawMacroSteps(side);
                ImGui::EndTabItem();
            }
        }
        requestTab_ = -1;
        ImGui::EndTabBar();
    }
}

void App::drawMacroHeader(int side) {
    Macro& m = cfg_.macros[side];
    bool& enabled = side == 0 ? cfg_.settings.leftEnabled : cfg_.settings.rightEnabled;
    bool changed = false;
    ImGui::PushID(side);
    changed |= ImGui::Checkbox("Enabled", &enabled);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(230 * scale_);
    changed |= inputText("##name", &m.name);
    ImGui::SameLine();
    if (ImGui::Button("Test")) {
        if (!snap_.input.connected)
            showError("Test needs a connected controller", "No controller is connected, and gsmacro cancels every macro while the "
                      "controller is missing (so nothing can stay pressed).", "Connect the GameSir (or switch to Demo Mode) and try again.");
        else engine_->triggerMacro(side);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(mode_ == Mode::Live ? "Runs the macro now on the VIRTUAL pad (through the FIFO). Keep the game unfocused."
                                              : "Runs the macro now through the FIFO and shows the result.");
    ImGui::SameLine();
    if (ImGui::Checkbox("Edit as text", &textMode_[side])) setTextMode(side, textMode_[side]);
    ImGui::SameLine();
    if (ImGui::Button("Import...")) {
        std::string p = ioPath_;
        if (choosePath(false, &p)) {
            std::ifstream f(p, std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            Macro parsed;
            parsed.name = m.name;
            std::string err;
            if (!f) showError("Import failed", "Cannot read " + p, "Check the path and try again.");
            else if (!macroFromText(ss.str(), &parsed, &err))
                showError("Import failed", err, "The macro was not changed. A macro file has one action per line, e.g. 'press Cross', 'wait 16.7'.");
            else { m.actions = parsed.actions; changed = true; setStatus("Imported macro from " + p, false); }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Export...")) {
        std::string p = ioPath_;
        if (choosePath(true, &p)) {
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f << "# gsmacro macro: " << m.name << "\n" << macroToText(m);
            if (f) setStatus("Exported macro to " + p, false);
            else showError("Export failed", "Cannot write " + p, "Choose a folder you can write to (e.g. Documents).");
        }
    }
    double tickMs = 1000.0 / cfg_.settings.gameTickHz;
    double total = 0;
    for (auto& a : m.actions) if (a.op == Op::Wait) total += std::max(0.0, a.ms);
    ImGui::TextDisabled("Total %.2f ms  =  %.2f game ticks at %.0f Hz  |  %zu steps", total, total / tickMs, cfg_.settings.gameTickHz,
                        m.actions.size());
    if (changed) applyConfig();
    ImGui::PopID();
}

// A compact visual of the macro: one row per button/axis it touches, with a
// draggable handle at the end of every wait.
void App::drawMacroVisual(int side) {
    Macro& m = cfg_.macros[side];
    struct Seg { double t0, t1; uint16_t on, off; uint8_t axMask; double ax[AXIS_COUNT]; };
    std::vector<Seg> segs;
    std::vector<std::pair<int, double>> handles;  // (action index, boundary time)
    uint16_t on = 0, off = 0, usedB = 0;
    uint8_t axMask = 0, usedA = 0;
    double ax[AXIS_COUNT] = {};
    double t = 0;
    for (size_t i = 0; i < m.actions.size(); i++) {
        const Action& a = m.actions[i];
        switch (a.op) {
        case Op::Press: on |= a.buttons; off &= (uint16_t)~a.buttons; usedB |= a.buttons; break;
        case Op::Release: on &= (uint16_t)~a.buttons; off &= (uint16_t)~a.buttons; usedB |= a.buttons; break;
        case Op::Suppress: off |= a.buttons; on &= (uint16_t)~a.buttons; usedB |= a.buttons; break;
        case Op::SetAxis: axMask |= (uint8_t)(1u << a.axis); ax[a.axis] = a.value; usedA |= (uint8_t)(1u << a.axis); break;
        case Op::ReleaseAxis: axMask &= (uint8_t)~(1u << a.axis); usedA |= (uint8_t)(1u << a.axis); break;
        case Op::ReleaseAll: on = off = 0; axMask = 0; break;
        case Op::Wait: {
            double d = std::max(0.0, a.ms);
            Seg s{t, t + d, on, off, axMask, {}};
            std::copy(ax, ax + AXIS_COUNT, s.ax);
            segs.push_back(s);
            t += d;
            handles.push_back({(int)i, t});
            break;
        }
        }
    }
    std::vector<std::pair<std::string, int>> rows;  // label, bit (>=0 button) or -(axis+1)
    for (uint16_t b : kAllButtons) if (usedB & b) rows.push_back({buttonName(b), b});
    for (int a = 0; a < AXIS_COUNT; a++) if (usedA & (1u << a)) rows.push_back({axisName((Axis)a), -(a + 1)});
    if (rows.empty() || t <= 0) {
        ImGui::TextDisabled("Add steps with waits to see the macro timeline here.");
        return;
    }
    const double tickMs = 1000.0 / cfg_.settings.gameTickHz;
    const bool dragging = dragSide_ == side && dragAction_ >= 0;
    const double D = dragging ? std::max(dragTotalMs_, t) : t;
    const float rowH = 20 * scale_, labelW = 72 * scale_;
    const float w = ImGui::GetContentRegionAvail().x, h = rows.size() * rowH + 16 * scale_;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x0 = p0.x + labelW, x1 = p0.x + w - 6 * scale_;
    auto X = [&](double tm) { return x0 + (float)(tm / D) * (x1 - x0); };
    dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(x1, p0.y + h), IM_COL32(18, 21, 27, 255), 4);
    for (double g = 0; g <= D; g += tickMs) dl->AddLine(ImVec2(X(g), p0.y), ImVec2(X(g), p0.y + h - 14 * scale_), IM_COL32(255, 255, 255, 14));
    for (size_t r = 0; r < rows.size(); r++) {
        float y = p0.y + 2 * scale_ + r * rowH;
        dl->AddText(nullptr, ImGui::GetFontSize() * 0.85f, ImVec2(p0.x, y), kDim, rows[r].first.c_str());
        bool prevHad = false;
        double prevVal = 0;
        for (auto& s : segs) {
            float a = X(s.t0), b = std::max(X(s.t1), X(s.t0) + 1);
            int bit = rows[r].second;
            if (bit >= 0) {
                if (s.on & bit) dl->AddRectFilled(ImVec2(a, y + 2), ImVec2(b, y + rowH - 2), macroColor(side, 230), 2);
                else if (s.off & bit) dl->AddRect(ImVec2(a, y + 2), ImVec2(b, y + rowH - 2), kBlocked, 2);
            } else {
                int axn = -bit - 1;
                if (s.axMask & (1u << axn)) {
                    // Bar height shows the value (centre line = 0; triggers grow upward).
                    float mid = y + rowH / 2, hh = (float)(s.ax[axn] * (rowH / 2 - 2));
                    dl->AddRectFilled(ImVec2(a, std::min(mid, mid - hh) - 1), ImVec2(b, std::max(mid, mid - hh) + 1), macroColor(side, 200));
                    // Label the value once per run of equal values.
                    if (!prevHad || prevVal != s.ax[axn]) {
                        char vb[16];
                        snprintf(vb, sizeof vb, "%+.2f", s.ax[axn]);
                        float fs = ImGui::GetFontSize() * 0.75f;
                        dl->AddText(nullptr, fs, ImVec2(a + 3, y + 1), kText, vb);
                    }
                    prevHad = true;
                    prevVal = s.ax[axn];
                } else {
                    prevHad = false;
                }
            }
        }
    }
    // Playhead while this macro runs.
    if (snap_.currentMacro == side) {
        double el = toMs(snap_.now - snap_.runStart);
        if (el >= 0 && el <= D) dl->AddLine(ImVec2(X(el), p0.y), ImVec2(X(el), p0.y + h), kText, 1.5f * scale_);
    }
    // Draggable wait boundaries.
    bool changed = false;
    float handleTop = p0.y, handleH = h - 14 * scale_;
    handlePos_.clear();
    for (size_t k = 0; k < handles.size(); k++) {
        handlePos_.push_back({X(handles[k].second), handleTop + handleH * 0.5f});
        int ai = handles[k].first;
        float hx = X(handles[k].second);
        ImGui::PushID(ai);
        ImGui::SetCursorScreenPos(ImVec2(hx - 4 * scale_, handleTop));
        ImGui::InvisibleButton("##h", ImVec2(8 * scale_, handleH));
        bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemActivated()) {
            dragSide_ = side; dragAction_ = ai; dragStartMs_ = m.actions[ai].ms; dragTotalMs_ = D; dragMouseX_ = ImGui::GetIO().MousePos.x;
        }
        if (active && dragSide_ == side && dragAction_ == ai) {
            double delta = (ImGui::GetIO().MousePos.x - dragMouseX_) / (x1 - x0) * dragTotalMs_;
            double v = std::max(0.0, dragStartMs_ + delta);
            v = ImGui::GetIO().KeyShift ? std::round(v / tickMs) * tickMs : std::round(v * 10.0) / 10.0;
            if (std::fabs(v - m.actions[ai].ms) > 1e-9) { m.actions[ai].ms = v; changed = true; }
            ImGui::SetTooltip("wait = %.2f ms  (%.2f ticks)   Shift = snap to game ticks", m.actions[ai].ms, m.actions[ai].ms / tickMs);
        } else if (hovered) {
            ImGui::SetTooltip("Step %d: wait %.2f ms (%.2f ticks)\nDrag to change. Hold Shift to snap to %.0f Hz game ticks.", ai + 1,
                              m.actions[ai].ms, m.actions[ai].ms / tickMs, cfg_.settings.gameTickHz);
        }
        if (ImGui::IsItemDeactivated() && dragSide_ == side) { dragSide_ = -1; dragAction_ = -1; }
        ImU32 col = active ? kText : hovered ? kPhys : IM_COL32(150, 160, 178, 140);
        dl->AddLine(ImVec2(hx, handleTop), ImVec2(hx, handleTop + handleH), col, (active || hovered ? 2.0f : 1.0f) * scale_);
        dl->AddTriangleFilled(ImVec2(hx - 4 * scale_, handleTop + handleH), ImVec2(hx + 4 * scale_, handleTop + handleH),
                              ImVec2(hx, handleTop + handleH - 6 * scale_), col);
        ImGui::PopID();
    }
    char lbl[32];
    snprintf(lbl, sizeof lbl, "%.1f ms", t);
    dl->AddText(nullptr, ImGui::GetFontSize() * 0.8f, ImVec2(x1 - ImGui::CalcTextSize(lbl).x * 0.8f, p0.y + h - 13 * scale_), kDim, lbl);
    dl->AddText(nullptr, ImGui::GetFontSize() * 0.8f, ImVec2(x0 + 2, p0.y + h - 13 * scale_), kDim, "0");
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(w, h));
    if (changed) applyConfig();
}


void App::drawMacroSteps(int side) {
    Macro& m = cfg_.macros[side];
    bool changed = false;
    ImGui::PushID(side);
    double tickMs = 1000.0 / cfg_.settings.gameTickHz;

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
    sectionTitle("FIFO / SYNCHRONIZATION");
    Settings& s = cfg_.settings;
    bool c = false;
    ImGui::PushItemWidth(150 * scale_);
    float v = (float)s.interMacroGapMs;
    if (ImGui::DragFloat("Gap between macros", &v, 0.1f, 0, 200, "%.2f ms")) { s.interMacroGapMs = std::max(0.0f, v); c = true; }
    helpMarker("Neutral time between two queued macros. 0 is allowed: gsmacro still emits one released update between them, "
               "but Rocket League only samples input every 8.33 ms (120 Hz), so it may not see that release. Keep it at or above "
               "one game tick if both macros press the same button.");
    ImGui::TextDisabled("Presets:");
    ImGui::SameLine();
    if (ImGui::SmallButton("0 ms")) { s.interMacroGapMs = 0; c = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("1 tick")) { s.interMacroGapMs = 1000.0 / s.gameTickHz; c = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("10 ms")) { s.interMacroGapMs = 10; c = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("2 ticks")) { s.interMacroGapMs = 2000.0 / s.gameTickHz; c = true; }
    if (s.interMacroGapMs < 1000.0 / s.gameTickHz)
        ImGui::TextColored(kWarnV, s.interMacroGapMs == 0 ? "0 ms: back to back. The game may merge consecutive presses."
                                                         : "Gap is shorter than one game tick (%.2f ms).", 1000.0 / s.gameTickHz);
    int qc = s.queueCapacity;
    if (ImGui::SliderInt("FIFO queue", &qc, 0, MacroScheduler::kMaxQueue)) { s.queueCapacity = qc; c = true; }
    helpMarker("Activations allowed to wait behind the running macro (0 = presses during a macro are ignored). Extra presses are "
               "dropped and counted in Diagnostics. Macros never overlap.");
    int ct = (int)s.controller;
    const char* ctNames[] = {"GameSir only (XInput)", "DualSense only (USB)", "Auto: GameSir or DualSense"};
    if (ImGui::Combo("Controller", &ct, ctNames, 3)) { s.controller = (ControllerType)ct; c = true; }
    helpMarker("Which physical controller Live Mode reads. Auto (default) reads both at once: whichever is plugged in works, "
               "with no restart. Takes effect the next time Live Mode starts (Stop, then Start Live Mode). DualSense: USB cable only; touchpad click = Back/View (Rocket League scoreboard). Hide it in HidHide's "
               "Devices tab like the GameSir.");
    int co = (int)s.chordOrder;
    const char* coNames[] = {"hold bumper, tap chord button", "hold chord button (shift), tap bumper"};
    if (ImGui::Combo("Trigger order", &co, coNames, 2)) { s.chordOrder = (ChordOrder)co; c = true; }
    helpMarker("hold bumper, tap chord button: hold L1/R1, tap Square. Square alone and L1/R1 always reach the game.\n"
               "hold chord button (shift), tap bumper: hold Square as a shift key, tap L1 (left macro) or R1 (right macro). "
               "While macros are armed Square never reaches the game; L1/R1 tapped without it pass through with no delay.");
    int bb = (int)s.bothBumpers;
    const char* bbNames[] = {"latest pressed", "always left", "always right", "no macro"};
    if (ImGui::Combo("L1+R1+Square", &bb, bbNames, 4)) { s.bothBumpers = (BothBumpers)bb; c = true; }
    helpMarker("Which macro runs when both bumpers are held. 'latest pressed' = the bumper pressed most recently (left on a tie).");

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
    if (ImGui::TreeNode("Timing (advanced)")) {
        v = (float)s.pollPeriodUs;
        if (ImGui::DragFloat("Input poll period", &v, 10, 100, 16000, "%.0f us")) { s.pollPeriodUs = v; c = true; }
        helpMarker("How often the controller is read (XInput is polled). Average sampling delay is half this period. Lower = more CPU.");
        v = (float)s.spinTailUs;
        if (ImGui::DragFloat("Spin tail", &v, 5, 0, 2000, "%.0f us")) { s.spinTailUs = v; c = true; }
        helpMarker("The last part of the wait before each macro step is spun for precision. Only used while a macro runs.");
        v = (float)s.gameTickHz;
        if (ImGui::DragFloat("Game tick rate", &v, 1, 10, 1000, "%.0f Hz")) { s.gameTickHz = v; c = true; }
        helpMarker("Display only: timeline grids and tick counts. Rocket League's physics runs at 120 Hz.");
        ImGui::TreePop();
    }
    ImGui::PopItemWidth();
    if (c) applyConfig();
}

void App::drawConfigFile() {
    ImGui::Spacing();
    sectionTitle("CONFIGURATION");
    ImGui::TextDisabled("Saved configuration (loaded at start):");
    ImGui::SetNextItemWidth(-1);
    inputText("##path", &cfgPath_);
    if (ImGui::Button("Save")) {
        std::string err;
        if (saveConfigFile(cfgPath_, cfg_, &err)) setStatus("Saved " + cfgPath_, false);
        else showError("Save failed", err, "Check that the folder exists and is writable.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        Config c2;
        std::string err;
        if (loadConfigFile(cfgPath_, &c2, &err)) { cfg_ = c2; applyConfig(); setStatus("Loaded " + cfgPath_, false); }
        else showError("Load failed", err, "Your current settings were not changed.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Import...")) {
        std::string p = ioPath_;
        if (choosePath(false, &p)) {
            Config c2;
            std::string err;
            if (loadConfigFile(p, &c2, &err)) { cfg_ = c2; applyConfig(); setStatus("Imported " + p + " (not saved yet)", false); }
            else showError("Import failed", err, "Your current settings were not changed. The message names the line to fix.");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Export...")) {
        std::string p = ioPath_;
        if (choosePath(true, &p)) {
            std::string err;
            if (saveConfigFile(p, cfg_, &err)) setStatus("Exported to " + p, false);
            else showError("Export failed", err, "Choose a folder you can write to.");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Examples")) ImGui::OpenPopup("reset?");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Replace both macros and all settings with the built-in examples.");
    if (ImGui::BeginPopup("reset?")) {
        ImGui::TextUnformatted("Replace the current macros and settings with the built-in examples?");
        if (ImGui::Button("Replace")) {
            bool armed = cfg_.settings.macrosArmed;
            cfg_ = defaultConfig();
            if (mode_ == Mode::Live) cfg_.settings.macrosArmed = armed;
            applyConfig();
            setStatus("Built-in examples loaded (not saved)", false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep mine")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (!liveBackendsAvailable()) {
        ImGui::TextDisabled("Import / Export path (no file dialog on this host):");
        ImGui::SetNextItemWidth(-1);
        inputText("##iopath", &ioPath_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Import/Export path (this host has no file dialog).");
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(fileStatusError_ ? kErrV : kDimV, "%s", fileStatus_.c_str());
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
        hrow("Wake / poll err", snap_.wakeError, "How late the engine woke after a timed deadline - input polls and macro steps (OS timer + spin tail).");
        hrow("Macro step late", snap_.eventLateness, "How late each timed macro event was applied vs its schedule.");
        hrow("Queued start late", snap_.startLateness, "How late a queued macro started vs its earliest allowed start.");
        ImGui::EndTable();
    }
    const auto& sc = snap_.sched;
    const auto& ch = snap_.chords;
    ImGui::TextDisabled("Chords  L %llu  R %llu  |  Square alone %llu", (unsigned long long)ch.left, (unsigned long long)ch.right,
                        (unsigned long long)ch.passthrough);
    ImGui::TextDisabled("Not run: macro off %llu, disarmed %llu  |  Test %llu", (unsigned long long)ch.disabled,
                        (unsigned long long)ch.disarmed, (unsigned long long)ch.manual);
    // Live FIFO view.
    ImGui::TextDisabled("Queue:");
    ImGui::SameLine();
    if (snap_.currentMacro >= 0) {
        ImGui::TextColored(snap_.currentMacro == 0 ? ImVec4(1, 0.62f, 0.26f, 1) : ImVec4(0.73f, 0.41f, 1, 1), "[%s running]",
                           snap_.currentMacro == 0 ? "L" : "R");
        ImGui::SameLine();
    }
    for (int i = 0; i < snap_.queued; i++) {
        ImGui::TextColored(snap_.queueIds[i] == 0 ? ImVec4(1, 0.62f, 0.26f, 0.7f) : ImVec4(0.73f, 0.41f, 1, 0.7f), "[%s]",
                           snap_.queueIds[i] == 0 ? "L" : "R");
        ImGui::SameLine();
    }
    if (snap_.currentMacro < 0 && !snap_.queued) ImGui::TextDisabled("empty");
    else ImGui::NewLine();
    ImGui::TextDisabled("Macros  started %llu  done %llu  dropped %llu  cancelled %llu", (unsigned long long)sc.started,
                        (unsigned long long)sc.completed, (unsigned long long)sc.dropped, (unsigned long long)sc.cancelled);
    if (snap_.submitFailures) ImGui::TextColored(kErrV, "Submit failures: %llu", (unsigned long long)snap_.submitFailures);
    if (ImGui::Button("Reset statistics")) engine_->resetStats();
}


// ───────────────────────── setup wizard ─────────────────────────

void App::drawSetupWindow() {
    if (!setupOpen_) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(780 * scale_, std::min(vp->WorkSize.y - 40 * scale_, 820 * scale_)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.115f, 0.14f, 1));
    if (!ImGui::Begin("Setup & validation - Live Mode###setup", &setupOpen_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        ImGui::PopStyleColor();
        return;
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(kDimV, "Each step only reads system state unless you press its button. gsmacro never installs drivers or "
                              "changes HidHide, reWASD or GameSir Connect. Unapply your reWASD config before starting.");
    if (ImGui::Button("Re-run checks")) runChecks();
    ImGui::Separator();

    auto stepHeader = [&](int n, const char* title, CheckResult::State st) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float r = ImGui::GetTextLineHeight() * 0.62f;
        ImU32 col = st == CheckResult::Ok ? kOk : st == CheckResult::Warn ? kWarn : st == CheckResult::Fail ? kBlocked : kFaint;
        ImVec2 c(p.x + r, p.y + ImGui::GetTextLineHeight() * 0.5f);
        dl->AddCircleFilled(c, r, col);
        char num[4];
        snprintf(num, sizeof num, "%d", n);
        ImVec2 ts = ImGui::CalcTextSize(num);
        dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), IM_COL32(15, 18, 24, 255), num);
        ImGui::Dummy(ImVec2(r * 2 + 6 * scale_, ImGui::GetTextLineHeight()));
        ImGui::SameLine();
        ImGui::TextUnformatted(title);
    };
    auto showCheck = [&](const CheckResult& r) {
        ImGui::Indent(26 * scale_);
        ImVec4 col = r.state == CheckResult::Ok ? kOkV : r.state == CheckResult::Warn ? kWarnV : r.state == CheckResult::Fail ? kErrV : kDimV;
        ImGui::TextColored(col, "%s", r.text.empty() ? "Not checked yet." : r.text.c_str());
        if (!r.guidance.empty()) ImGui::TextColored(kDimV, "%s", r.guidance.c_str());
        ImGui::Unindent(26 * scale_);
    };
    const bool live = mode_ == Mode::Live;
    const bool avail = liveBackendsAvailable();

    stepHeader(1, "GameSir controller detected", chkController_.state);
    showCheck(chkController_);
    ImGui::Spacing();

    stepHeader(2, "ViGEmBus driver and ViGEmClient.dll available", chkViGEm_.state);
    showCheck(chkViGEm_);
    ImGui::Spacing();

    stepHeader(3, "Create the virtual Xbox 360 controller", live ? CheckResult::Ok : CheckResult::Unknown);
    ImGui::Indent(26 * scale_);
    if (live) {
        if (virtualSlot_ >= 0) ImGui::TextColored(kOkV, "Virtual pad active in XInput slot %d. Macros are %s.", virtualSlot_,
                                                  cfg_.settings.macrosArmed ? "ARMED" : "OFF (pure passthrough)");
        else ImGui::TextColored(kWarnV, "Virtual pad created; waiting for its XInput slot.");
    } else {
        bool ready = avail && chkViGEm_.state == CheckResult::Ok;
        if (!ready) ImGui::BeginDisabled();
        if (ImGui::Button("Start Live Mode (passthrough only, macros OFF)")) startLive();
        if (!ready) ImGui::EndDisabled();
        ImGui::TextColored(kDimV, avail ? "Needs step 2 to pass. Creates one virtual pad; Stop removes it."
                                        : "Not available on this host (Windows build only).");
    }
    ImGui::Unindent(26 * scale_);
    ImGui::Spacing();

    uint64_t total = selfMatch_ + selfMismatch_;
    bool selfOk = live && readbackOk_ && total >= 60 && selfMismatch_ * 100 <= total;
    stepHeader(4, "Check passthrough outside Rocket League", confirmedPassthrough_ ? CheckResult::Ok : selfOk ? CheckResult::Warn : CheckResult::Unknown);
    ImGui::Indent(26 * scale_);
    if (live) {
        if (total) ImGui::Text("Self-test: virtual pad readback equals gsmacro's output in %llu of %llu stable samples.",
                               (unsigned long long)selfMatch_, (unsigned long long)total);
        else ImGui::TextDisabled("Self-test: move sticks and press buttons to collect samples.");
    }
    ImGui::TextColored(kDimV, "Open Windows' controller panel and test the 'Controller (XBOX 360 For Windows)' entry: every button, both "
                              "sticks and both triggers should respond normally, with no stuck inputs.");
    if (ImGui::Button("Open joy.cpl")) openGameControllersPanel();
    ImGui::SameLine();
    if (!live) ImGui::BeginDisabled();
    ImGui::Checkbox("Normal input works on the virtual pad", &confirmedPassthrough_);
    if (!live) ImGui::EndDisabled();
    ImGui::Unindent(26 * scale_);
    ImGui::Spacing();

    stepHeader(5, "HidHide available", chkHidHide_.state);
    showCheck(chkHidHide_);
    ImGui::Spacing();

    stepHeader(6, "Hide the physical controller from games (manual, reversible)", confirmedHidden_ ? CheckResult::Ok : CheckResult::Unknown);
    ImGui::Indent(26 * scale_);
    ImGui::TextColored(kDimV,
        "In HidHide Configuration Client:  (a) Applications tab: add gsmacro.exe from this folder, so gsmacro can still read the "
        "pad.  (b) Devices tab: tick the GameSir controller entries, then tick 'Enable device hiding'.  (c) In joy.cpl only ONE "
        "controller should now be listed (the Xbox 360 virtual pad).  To undo at any time: untick the device or 'Enable device "
        "hiding' in HidHide - it takes effect immediately (replug the pad if a game still misses it).");
    ImGui::TextColored(kDimV, "Or let gsmacro do it:");
    drawOneClickSetup();
    std::string herr;
    if (ImGui::Button("Open HidHide Configuration Client")) {
        if (!openHidHideClient(&herr)) showError("Could not open HidHide", herr, "");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Games see exactly one controller", &confirmedHidden_);
    ImGui::Unindent(26 * scale_);
    ImGui::Spacing();

    stepHeader(7, "Confirm normal input, then arm the macros", cfg_.settings.macrosArmed && live ? CheckResult::Ok : CheckResult::Unknown);
    ImGui::Indent(26 * scale_);
    bool canArm = live && confirmedPassthrough_;
    if (!canArm) ImGui::BeginDisabled();
    if (!cfg_.settings.macrosArmed) {
        if (ImGui::Button("Arm macros (L1/R1 + Square)")) { cfg_.settings.macrosArmed = true; applyConfig(); }
    } else if (ImGui::Button("Disarm macros (pure passthrough)")) {
        cfg_.settings.macrosArmed = false;
        applyConfig();
    }
    if (!canArm) ImGui::EndDisabled();
    if (!confirmedHidden_ && canArm) ImGui::TextColored(kWarnV, "Step 6 not confirmed: a game may see both controllers (doubled input).");
    ImGui::TextColored(kDimV, "Try each macro with the Test button and with L1/R1 + Square while watching the output timeline before "
                              "using it in a match. The built-in macros are only examples.");
    ImGui::Unindent(26 * scale_);
    ImGui::Spacing();

    stepHeader(8, "Turning Live Mode off / restoring normal controller use", CheckResult::Unknown);
    ImGui::Indent(26 * scale_);
    ImGui::TextColored(kDimV,
        "Click Demo (top bar) or Stop below: the virtual pad is removed at once with all inputs released. Closing gsmacro does the "
        "same. If you hid the GameSir with HidHide, untick it there so games see the physical pad directly again. Nothing else "
        "on the system was changed.");
    if (!live) ImGui::BeginDisabled();
    if (ImGui::Button("Stop Live Mode")) startDemo();
    if (!live) ImGui::EndDisabled();
    ImGui::Unindent(26 * scale_);
    ImGui::PopTextWrapPos();
    ImGui::End();
    ImGui::PopStyleColor();
}

void App::drawErrorPopup() {
    if (errPending_) {
        ImGui::OpenPopup("Problem###err");
        errPending_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(560 * scale_, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Problem###err", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(kErrV, "%s", errTitle_.c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted(errText_.c_str());
        if (!errGuidance_.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(kDimV, "What to do: %s", errGuidance_.c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(100 * scale_, 0))) ImGui::CloseCurrentPopup();
        if (liveBackendsAvailable()) {
            ImGui::SameLine();
            if (ImGui::Button("Open setup")) { showSetup(true); ImGui::CloseCurrentPopup(); }
        }
        ImGui::EndPopup();
    }
}

// ───────────────────────── one-click setup ─────────────────────────

void App::startBackgroundNow() {
    if (autostart_) return;
    autostart_ = true;
    nextLiveTry_ = 0;
    boot_ = Boot::Start;
    gameRunning_ = -1;
    autoStatus_ = "Background mode: starting Live Mode";
}

void App::pollAdmin() {
    if (!adminJob_) return;
    std::string r;
    if (pollAdminScript(&r) != 2) return;
    adminOk_ = r.rfind("ok", 0) == 0;
    adminResult_ = r.size() > 3 ? r.substr(adminOk_ ? 3 : 5) : r;
    if (adminJob_ == 1 && adminOk_) confirmedHidden_ = true;
    if (adminJob_ == 3) {
        if (!adminOk_) adminResult_ += " Start with Windows is on, but the GameSir may become player 2.";
        startBackgroundNow();  // after the player-1 helper exists
    }
    adminJob_ = 0;
}

void App::drawOneClickSetup() {
    std::string err;
    const bool busy = adminJob_ != 0;
    auto run = [&](int job, const char* script, const char* args) {
        if (startAdminScript(script, args, &err)) {
            adminJob_ = job;
            adminOk_ = true;
            adminResult_ = "Waiting for the Windows admin prompt (click Yes)...";
            return true;
        }
        adminOk_ = false;
        adminResult_ = err;
        return false;
    };
    if (busy) ImGui::BeginDisabled();
    if (ImGui::Button("Hide controller from games")) run(1, "hidhide-setup.ps1", "-Mode hide");
    ImGui::SameLine();
    if (ImGui::Button("Unhide")) run(2, "hidhide-setup.ps1", "-Mode unhide");
    ImGui::SameLine();
    helpMarker("Needs HidHide installed. Hide: lets gsmacro read the controller, hides the DualSense / GameSir from games "
               "(so a game sees only gsmacro's virtual pad - no second player, no double input) and turns hiding on. One admin "
               "prompt; close HidHide's own window first; replug the controller once afterwards. Unhide gives the controller "
               "back to games. Nothing else is changed.");

    if (!autostartChecked_) {
        autostartOn_ = autostartEntryPresent();
        autostartChecked_ = true;
    }
    bool on = autostartOn_;
    if (ImGui::Checkbox("Start with Windows (in the background)", &on)) {
        if (!setAutostartEntry(on, &err)) {
            adminOk_ = false;
            adminResult_ = err;
        } else if ((autostartOn_ = on)) {
            const bool needTask = cfg_.settings.controller != ControllerType::DualSense && !slot0TaskRegistered();
            if (!needTask || !run(3, "setup-autostart.ps1", "")) {
                if (needTask) adminResult_ += " Start with Windows is on, but the GameSir may become player 2.";
                else { adminOk_ = true; adminResult_ = "Start with Windows is on. gsmacro is now also running in the background (tray icon)."; }
                startBackgroundNow();
            }
        } else {
            adminOk_ = true;
            adminResult_ = "Start with Windows is off. This copy keeps running until you exit it (tray icon > Exit).";
        }
    }
    ImGui::SameLine();
    helpMarker("At every login gsmacro starts by itself with only a tray icon, starts Live Mode, and arms the macros only while "
               "Rocket League runs. Turning it on also starts that right now. With a GameSir it asks once for admin rights to "
               "set up the player-1 helper. Untick to stop starting with Windows.");
    if (busy) ImGui::EndDisabled();
    if (!adminResult_.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(adminOk_ ? kOkV : kWarnV, "%s", adminResult_.c_str());
        ImGui::PopTextWrapPos();
    }
}

}  // namespace gsm
