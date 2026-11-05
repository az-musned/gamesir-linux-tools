// Scripted controller input for Demo Mode: each scenario is a list of
// (time offset, physical state) steps played into a MockInput.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mock_backend.h"

namespace gsm {

struct ScenarioStep {
    Time at = 0;  // offset from scenario start
    GamepadState state;
    bool connected = true;
};

struct Scenario {
    std::string name, description;
    std::vector<ScenarioStep> steps;
};

std::vector<Scenario> builtinScenarios();

class ScenarioPlayer {
public:
    ~ScenarioPlayer() { stop(); }
    // Plays `sc` into `input` on a background thread. Calling play() again
    // stops the previous scenario first.
    void play(const Scenario& sc, MockInput* input);
    void stop();
    bool playing() const { return playing_.load(); }
    std::string current() { std::lock_guard<std::mutex> l(m_); return name_; }

private:
    std::thread t_;
    std::atomic<bool> stop_{false}, playing_{false};
    std::mutex m_;
    std::string name_;
};

}  // namespace gsm
