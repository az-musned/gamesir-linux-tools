// Turns a recording of controller states into per-activation edge timelines,
// so the macro output of two tools (e.g. reWASD's virtual pad vs gsmacro's)
// can be compared step by step.
//
//  * Channels in `ignoreButtons` (default: L1, R1, Square, the player's own
//    chord) are not edges.
//  * Axis moves smaller than `axisThreshold` (normalised) are ignored, and
//    axis values are reported rounded to 0.01.
//  * A burst starts at the first edge after at least `quiet` of silence;
//    edge times are relative to that first edge.
//  * Bursts with the same edge sequence ("signature") are aggregated, giving
//    mean / min / max / standard deviation per edge.
#pragma once
#include <string>
#include <vector>

#include "clock.h"
#include "gamepad.h"

namespace gsm {

struct StateSample {
    Time t = 0;
    GamepadState s;
};

struct Edge {
    std::string label;  // e.g. "Cross down", "Cross up", "LX -> -1.00"
    Time rel = 0;       // from the burst's first edge
};

struct Burst {
    Time start = 0;
    std::vector<Edge> edges;
    std::string signature() const;
};

struct EdgeStats {
    std::string label;
    double meanMs = 0, minMs = 0, maxMs = 0, sdMs = 0;
};

struct SequenceGroup {
    std::string signature;
    int count = 0;
    std::vector<EdgeStats> edges;      // aggregated over the bursts with this signature
};

struct TimelineReport {
    std::vector<Burst> bursts;
    std::vector<SequenceGroup> groups; // by count, most common first
    // Shortcuts to the most common group.
    std::string signature;
    int matching = 0;
    std::vector<EdgeStats> edges;
};

struct EdgeOptions {
    uint16_t ignoreButtons = BTN_LB | BTN_RB | BTN_X;
    double axisThreshold = 0.10;
    Time quiet = ms(250);
};

std::vector<Burst> splitBursts(const std::vector<StateSample>& samples, const EdgeOptions& o);
TimelineReport analyzeTimeline(const std::vector<StateSample>& samples, const EdgeOptions& o);
std::string formatReport(const TimelineReport& r);

}  // namespace gsm
