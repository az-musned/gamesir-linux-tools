// The whole user interface (Dear ImGui). Host mains (GLFW/OpenGL on
// Linux, Win32/DX11 on Windows) only create the window and call frame().
#pragma once
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "../backends/demo_scenarios.h"
#include "../backends/mock_backend.h"
#include "../core/config.h"
#include "../core/engine.h"
#include "platform.h"

namespace gsm {

class App {
public:
    App();
    ~App();
    void loadFonts(float scale);
    void applyStyle(float scale);
    void frame();

    // Background mode (gsmacro.exe --autostart): Live Mode starts by itself,
    // macros arm while the game process runs, and the virtual pad is moved to
    // XInput slot 0 (player 1) through the "gsmacro slot0" task if needed.
    // backgroundTick() runs every host loop iteration, also while hidden.
    void enableAutostart();
    bool autostart() const { return autostart_; }
    void backgroundTick();
    // Background mode started from the window ("Start with Windows" ticked).
    void startBackgroundNow();

    // Automation hooks (screenshots).
    bool playScenario(const std::string& name);
    void selectMacroTab(int side) { requestTab_ = side; }
    void setTimelinePaused(bool p) { timelinePaused_ = p; }
    void setTextMode(int side, bool on);
    void showSetup(bool on) { setupOpen_ = on; if (on) runChecks(); }
    void triggerMacro(int side) { engine_->triggerMacro(side); }
    // Tutorial automation: replace a macro's steps, read back where the
    // visual timeline drew its wait handles (screen coordinates).
    void setMacro(int side, const std::string& name, const std::vector<Action>& actions) {
        cfg_.macros[side].name = name;
        cfg_.macros[side].actions = actions;
        applyConfig();
    }
    void setSavePath(const std::string& p) { cfgPath_ = p; }
    void saveNow() {
        std::string err;
        if (saveConfigFile(cfgPath_, cfg_, &err)) setStatus("Saved " + cfgPath_, false);
    }
    std::vector<std::pair<float, float>> waitHandles() const { return handlePos_; }

private:
    enum class Mode { Demo, Live };

    void startDemo();
    void startLive(bool quiet = false);  // quiet: no error popup, stays in Demo on failure
    void stopEngine();
    void applyConfig();
    void pollEngine();
    void runChecks();
    void updateSelfTest();
    void showError(const std::string& title, const std::string& text, const std::string& guidance);
    bool choosePath(bool save, std::string* path);
    void setStatus(const std::string& s, bool error) { fileStatus_ = s; fileStatusError_ = error; }

    void drawTopBar();
    void drawStatusStrip();
    void drawControllerPanel();
    void drawPad(const char* id, const GamepadState& s, bool interactive, bool isOutput);
    void drawScenarios();
    void drawLiveSelfTest();
    void drawMacroEditor();
    void drawMacroHeader(int side);
    void drawMacroVisual(int side);
    void drawMacroSteps(int side);
    void drawTimeline();
    void drawSyncSettings();
    void drawOneClickSetup();
    void pollAdmin();
    void drawConfigFile();
    void drawDiagnostics();
    void drawSetupWindow();
    void drawErrorPopup();

    SteadyClock clock_;
    std::unique_ptr<IWaiter> waiter_;
    std::unique_ptr<Engine> engine_;
    MockInput demoIn_{true};
    RecordingSink demoOut_{clock_, 0};
    LiveBackends live_;
    Mode mode_ = Mode::Demo;

    Config cfg_;
    std::string cfgPath_, ioPath_;
    std::string fileStatus_;
    bool fileStatusError_ = false;
    bool textMode_[2] = {false, false};
    std::string textBuf_[2];
    std::string textErr_[2];
    int requestTab_ = -1;

    // Drag state for the macro visual timeline.
    int dragSide_ = -1, dragAction_ = -1;
    double dragStartMs_ = 0, dragTotalMs_ = 0, dragMouseX_ = 0;

    GamepadState manual_;
    uint16_t latched_ = 0;
    std::vector<Scenario> scenarios_;
    ScenarioPlayer player_;

    EngineSnapshot snap_;
    GamepadState prevSnapOut_;
    std::deque<TimelineSample> timeline_;
    std::vector<TimelineSample> drainBuf_;
    bool timelinePaused_ = false;
    float timelineSpanMs_ = 1000;
    Time timelineEnd_ = 0;

    struct Rates { double cycles = 0, submits = 0, inputs = 0, engineCpuPct = 0, processCpuPct = 0; } rates_;
    EngineSnapshot rateBase_;
    Time rateBaseTime_ = 0;
    double rateBaseProcCpu_ = 0;

    // Setup wizard / live status.
    bool setupOpen_ = false;
    CheckResult chkController_, chkViGEm_, chkHidHide_;
    bool confirmedPassthrough_ = false, confirmedHidden_ = false;
    int virtualSlot_ = -1;
    GamepadState readback_;
    bool readbackOk_ = false;
    Time outStableSince_ = 0;
    uint64_t selfMatch_ = 0, selfMismatch_ = 0;

    // Background mode.
    static constexpr const char* kGameExe = "RocketLeague.exe";
    bool autostart_ = false;
    // One-click setup: 0 none, 1 hide, 2 unhide, 3 autostart helper task.
    int adminJob_ = 0;
    bool adminOk_ = false;
    std::string adminResult_;
    bool autostartOn_ = false, autostartChecked_ = false;
    Time nextLiveTry_ = 0, nextGameCheck_ = 0;
    int gameRunning_ = -1;  // -1 unknown, 0/1
    // Boot order: free slot 0 BEFORE Live Mode opens the GameSir through XInput.
    enum class Boot { Start, WaitOff, Ready } boot_ = Boot::Start;
    Time bootDeadline_ = 0;
    bool slotWarn_ = false;
    std::string autoStatus_;

    std::string errTitle_, errText_, errGuidance_;
    bool errPending_ = false;
    float scale_ = 1;
    std::vector<std::pair<float, float>> handlePos_;  // last drawn wait handles (x, y)
};

}  // namespace gsm
