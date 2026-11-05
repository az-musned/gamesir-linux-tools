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

struct ImFont;

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
    void setTextMode(int side, bool on) {
        textMode_[side] = on;
        if (on) { textBuf_[side] = macroToText(cfg_.macros[side]); textErr_[side].clear(); }
    }

private:
    enum class Mode { Demo, Live };

    void startDemo();
    void startLive();
    void stopEngine();
    void applyConfig();
    void pollEngine();

    void drawTopBar();
    void drawControllerPanel();
    void drawPad(const char* id, const GamepadState& s, bool interactive, bool isOutput);
    void drawScenarios();
    void drawMacroEditor();
    void drawMacroSteps(int side);
    void drawTimeline();
    void drawSyncSettings();
    void drawDiagnostics();
    void drawLivePanel();

    SteadyClock clock_;
    std::unique_ptr<IWaiter> waiter_;
    std::unique_ptr<Engine> engine_;
    MockInput demoIn_{true};
    RecordingSink demoOut_{clock_, 0};
    LiveBackends live_;
    Mode mode_ = Mode::Demo;
    std::string engineError_;

    Config cfg_;
    std::string cfgPath_;
    std::string fileStatus_;
    bool textMode_[2] = {false, false};
    std::string textBuf_[2];
    std::string textErr_[2];
    int requestTab_ = -1;

    GamepadState manual_;
    uint16_t latched_ = 0;
    std::vector<Scenario> scenarios_;
    ScenarioPlayer player_;

    EngineSnapshot snap_;
    std::deque<TimelineSample> timeline_;
    std::vector<TimelineSample> drainBuf_;
    bool timelinePaused_ = false;
    float timelineSpanMs_ = 1000;
    Time timelineEnd_ = 0;

    // Rates computed once per second from snapshot deltas.
    struct Rates { double cycles = 0, submits = 0, inputs = 0, engineCpuPct = 0, processCpuPct = 0; } rates_;
    EngineSnapshot rateBase_;
    Time rateBaseTime_ = 0;
    double rateBaseProcCpu_ = 0;

    bool liveChecks_[3] = {false, false, false};
    std::string hidHideText_;
    float scale_ = 1;
};

}  // namespace gsm
