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

    // Automation hooks (screenshots).
    bool playScenario(const std::string& name);
    void selectMacroTab(int side) { requestTab_ = side; }
    void setTimelinePaused(bool p) { timelinePaused_ = p; }
    void setTextMode(int side, bool on);
    void showSetup(bool on) { setupOpen_ = on; if (on) runChecks(); }
    void triggerMacro(int side) { engine_->triggerMacro(side); }

private:
    enum class Mode { Demo, Live };

    void startDemo();
    void startLive();
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

    std::string errTitle_, errText_, errGuidance_;
    bool errPending_ = false;
    float scale_ = 1;
};

}  // namespace gsm
