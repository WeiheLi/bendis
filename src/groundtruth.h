// Exact ground truth for two definitions of periodic burst items.
//
//  full : PBSketch's definition. Bursts follow BurstSketch's CorrectDetector rules
//         (start: c >= k*p and c >= H; end: c*k <= p within L windows; c >= H in between),
//         and the interval is the gap between the start windows of consecutive completed bursts.
//  onset: our definition. An onset happens in window w when c_w >= H and c_w >= k*c_{w-1}.
//         The interval is the gap to the flow's previous onset; gaps of at most R windows
//         (multi-window ramps) are not counted.
//
// Both report counts per (flow, floor(interval / delta)).
#pragma once

#include "pbo_common.h"

namespace pbo {

struct DefConfig {
    uint32_t W = 30000;  // packets per window
    uint32_t H = 80;     // burst threshold
    uint32_t k = 2;      // burst ratio
    uint32_t L = 10;     // max burst width (full definition only)
    double delta = 2;    // interval bin width
    uint32_t R = 1;      // intervals <= R are not counted (onset definition only)
};

struct GroundTruth {
    PBMap full, onset;
    uint64_t n_bursts = 0, n_onsets = 0, n_kept_onsets = 0;
    uint32_t n_windows = 0;
};

inline GroundTruth compute_ground_truth(const std::vector<uint64_t>& ids, const DefConfig& c) {
    GroundTruth g;
    std::unordered_map<uint64_t, uint32_t> cur, prev;
    std::unordered_map<uint64_t, int64_t> burst_start;  // flows with an open burst (T_str)
    std::unordered_map<uint64_t, int64_t> last_burst;   // start of the flow's previous completed burst
    std::unordered_map<uint64_t, int64_t> last_onset;   // flow's previous onset
    cur.reserve(c.W * 2);
    prev.reserve(c.W * 2);

    auto process = [&](uint64_t id, uint64_t cnt, uint64_t pc, int64_t w) {
        // full definition (BurstSketch CorrectDetector order of checks)
        auto it = burst_start.find(id);
        int64_t t = it == burst_start.end() ? -1 : it->second;
        if (t != -1 && w - t > (int64_t)c.L) t = -1;
        if (cnt * c.k <= pc && t != -1) {
            ++g.n_bursts;
            auto lb = last_burst.find(id);
            if (lb != last_burst.end()) {
                g.full[{id, vbin_of((uint64_t)(t - lb->second), c.delta)}]++;
                lb->second = t;
            } else {
                last_burst.emplace(id, t);
            }
            t = -1;
        }
        if (cnt < c.H) t = -1;
        bool onset = cnt >= (uint64_t)c.k * pc && cnt >= c.H;
        if (onset) t = w;
        if (t == -1) {
            if (it != burst_start.end()) burst_start.erase(it);
        } else {
            burst_start[id] = t;
        }

        // onset definition
        if (onset) {
            ++g.n_onsets;
            auto lo = last_onset.find(id);
            if (lo == last_onset.end()) {
                last_onset.emplace(id, w);
            } else {
                if ((uint64_t)(w - lo->second) > c.R) {
                    g.onset[{id, vbin_of((uint64_t)(w - lo->second), c.delta)}]++;
                    ++g.n_kept_onsets;
                }
                lo->second = w;
            }
        }
    };

    auto transition = [&](int64_t w) {
        for (const auto& e : cur) {
            auto p = prev.find(e.first);
            process(e.first, e.second, p == prev.end() ? 0 : p->second, w);
        }
        for (const auto& e : prev)
            if (!cur.count(e.first)) process(e.first, 0, e.second, w);
        prev.swap(cur);
        cur.clear();
    };

    WindowClock clk(c.W);
    for (uint64_t i = 0; i < ids.size(); ++i) {
        for (uint32_t n = clk.advance(i); n > 0; --n) transition(clk.win - n);
        cur[ids[i]]++;
    }
    transition(clk.win);
    g.n_windows = clk.win + 1;
    return g;
}

}  // namespace pbo
