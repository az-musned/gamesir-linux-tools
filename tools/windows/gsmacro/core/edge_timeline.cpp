#include "edge_timeline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace gsm {

std::string Burst::signature() const {
    std::string s;
    for (auto& e : edges) {
        if (!s.empty()) s += " | ";
        s += e.label;
    }
    return s;
}

std::vector<Burst> splitBursts(const std::vector<StateSample>& samples, const EdgeOptions& o) {
    std::vector<Burst> out;
    if (samples.empty()) return out;
    GamepadState prev = samples.front().s;
    double axisRef[AXIS_COUNT];
    for (int a = 0; a < AXIS_COUNT; a++) axisRef[a] = axisToNormalized((Axis)a, prev.axis((Axis)a));
    Time lastEdge = kNever;
    for (size_t i = 1; i < samples.size(); i++) {
        const GamepadState& s = samples[i].s;
        Time t = samples[i].t;
        std::vector<std::string> labels;
        uint16_t changed = (uint16_t)((s.buttons ^ prev.buttons) & ~o.ignoreButtons);
        for (uint16_t b : kAllButtons) {
            if (!(changed & b)) continue;
            labels.push_back(std::string(buttonName(b)) + (s.has(b) ? " down" : " up"));
        }
        for (int a = 0; a < AXIS_COUNT; a++) {
            double v = axisToNormalized((Axis)a, s.axis((Axis)a));
            if (std::fabs(v - axisRef[a]) < o.axisThreshold) continue;
            axisRef[a] = v;
            char buf[48];
            snprintf(buf, sizeof buf, "%s -> %.2f", axisName((Axis)a), std::round(v * 100) / 100 + 0.0);
            labels.push_back(buf);
        }
        prev = s;
        if (labels.empty()) continue;
        if (out.empty() || lastEdge == kNever || t - lastEdge >= o.quiet) out.push_back(Burst{t, {}});
        for (auto& l : labels) out.back().edges.push_back(Edge{l, t - out.back().start});
        lastEdge = t;
    }
    return out;
}

static SequenceGroup aggregate(const std::vector<Burst>& bursts, const std::string& sig) {
    SequenceGroup g;
    g.signature = sig;
    const Burst* first = nullptr;
    for (auto& b : bursts)
        if (b.signature() == sig) { if (!first) first = &b; g.count++; }
    g.edges.resize(first->edges.size());
    for (size_t e = 0; e < first->edges.size(); e++) {
        double sum = 0, sq = 0, mn = 1e300, mx = -1e300;
        int n = 0;
        for (auto& b : bursts) {
            if (b.signature() != sig) continue;
            double v = toMs(b.edges[e].rel);
            sum += v; sq += v * v; n++;
            mn = std::min(mn, v); mx = std::max(mx, v);
        }
        double mean = sum / n;
        g.edges[e] = EdgeStats{first->edges[e].label, mean, mn, mx, std::sqrt(std::max(0.0, sq / n - mean * mean))};
    }
    return g;
}

TimelineReport analyzeTimeline(const std::vector<StateSample>& samples, const EdgeOptions& o) {
    TimelineReport r;
    r.bursts = splitBursts(samples, o);
    std::map<std::string, int> count;
    for (auto& b : r.bursts) count[b.signature()]++;
    std::vector<std::pair<int, std::string>> order;
    for (auto& kv : count) order.push_back({kv.second, kv.first});
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (auto& o2 : order) r.groups.push_back(aggregate(r.bursts, o2.second));
    if (!r.groups.empty()) {
        r.signature = r.groups[0].signature;
        r.matching = r.groups[0].count;
        r.edges = r.groups[0].edges;
    }
    return r;
}

std::string formatReport(const TimelineReport& r) {
    std::string s;
    char b[256];
    snprintf(b, sizeof b, "%zu burst(s), %zu distinct sequence(s).\n", r.bursts.size(), r.groups.size());
    s += b;
    if (r.groups.empty()) return s + "No edges recorded.\n";
    int singles = 0;
    for (size_t gi = 0; gi < r.groups.size(); gi++) {
        const SequenceGroup& g = r.groups[gi];
        if (g.count < 2 && gi > 0) { singles++; continue; }  // one-off sequences: summarised below
        snprintf(b, sizeof b, "  Sequence %c - seen %d time(s):\n", (char)('A' + gi), g.count);
        s += b;
        s += "    edge                         mean ms     min ms     max ms   stddev ms\n";
        for (auto& e : g.edges) {
            snprintf(b, sizeof b, "    %-26s %9.3f  %9.3f  %9.3f  %9.3f\n", e.label.c_str(), e.meanMs, e.minMs, e.maxMs, e.sdMs);
            s += b;
        }
    }
    if (singles) {
        snprintf(b, sizeof b, "  %d burst(s) had a one-off sequence (e.g. interrupted, merged or touched by hand) - not shown; see the CSV.\n", singles);
        s += b;
    }
    return s;
}

}  // namespace gsm
