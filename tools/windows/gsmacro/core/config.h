// Settings + the two macros, stored as a small text file:
//
//   [settings]
//   poll_period_us = 500
//   ...
//   [macro left]
//   name = Wall dash left
//   press Cross
//   wait 16.7
//   ...
#pragma once
#include <string>

#include "macro.h"

namespace gsm {

enum class BothBumpers : uint8_t { Latest, Left, Right, Ignore };
const char* bothBumpersName(BothBumpers b);

struct Settings {
    // Input sampling. Used by polled sources (XInput). Event-driven sources
    // wake the engine directly and this is only a safety net.
    double pollPeriodUs = 500;
    // Final part of each wait done by spinning, for sub-timer-resolution
    // accuracy. 0 = never spin (lowest CPU, coarser timing).
    double spinTailUs = 300;
    // Activations waiting behind the running macro. Further presses are dropped.
    int queueCapacity = 2;
    // Neutral time between two queued macros, so the game can see buttons
    // released before the next macro presses them again. Rocket League
    // samples input at 120 Hz (8.33 ms), so keep this above one tick.
    double interMacroGapMs = 10;
    // Which macro L1+R1+Square picks.
    BothBumpers bothBumpers = BothBumpers::Latest;
    bool leftEnabled = true, rightEnabled = true;
    uint16_t chordButton = BTN_X;     // Square
    uint16_t leftModifier = BTN_LB;   // L1
    uint16_t rightModifier = BTN_RB;  // R1
    // Display only: game input-sampling rate used for the tick grid / hints.
    double gameTickHz = 120;
};

struct Config {
    Settings settings;
    Macro macros[2];  // 0 = left (L1 + Square), 1 = right (R1 + Square)
};

Config defaultConfig();
std::string configToText(const Config& c);
bool configFromText(const std::string& text, Config* out, std::string* err);
bool loadConfigFile(const std::string& path, Config* out, std::string* err);
bool saveConfigFile(const std::string& path, const Config& c, std::string* err);
// Clamp settings into safe ranges (GUI and loader both call this).
void sanitize(Settings* s);

}  // namespace gsm
