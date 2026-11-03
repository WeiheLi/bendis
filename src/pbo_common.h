// Shared utilities: hashing, dataset loading, (id, interval-bin) maps and top-K evaluation.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pbo {

struct Record {
    uint64_t ts;
    uint64_t id;
};

inline uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

inline uint64_t hash64(uint64_t x, uint64_t seed) { return splitmix64(x ^ splitmix64(seed)); }

// Supported record formats:
//   pbs16   : {uint64 timestamp, uint64 id}            (BurstSketch / PBSketch CAIDA format)
//   tuple13 : {srcIP, dstIP, srcPort, dstPort, proto}  (id = srcIP << 32 | dstIP)
//   tuple21 : tuple13 followed by uint64 timestamp     (id = srcIP << 32 | dstIP)
inline std::vector<uint64_t> load_ids(const std::string& path, const std::string& fmt, uint64_t max_n) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    size_t rec;
    if (fmt == "pbs16") rec = 16;
    else if (fmt == "tuple13") rec = 13;
    else if (fmt == "tuple21") rec = 21;
    else throw std::runtime_error("unknown format " + fmt);

    std::vector<uint64_t> ids;
    std::vector<char> buf(rec * 65536);
    while (ids.size() < max_n && in) {
        in.read(buf.data(), (std::streamsize)buf.size());
        size_t got = (size_t)in.gcount() / rec;
        for (size_t i = 0; i < got && ids.size() < max_n; ++i) {
            const char* p = buf.data() + i * rec;
            uint64_t id;
            if (rec == 16) {
                std::memcpy(&id, p + 8, 8);
            } else {
                uint32_t src, dst;
                std::memcpy(&src, p, 4);
                std::memcpy(&dst, p + 4, 4);
                id = ((uint64_t)src << 32) | dst;
            }
            ids.push_back(id);
        }
    }
    return ids;
}

// Count-based windows exactly as PBSketch/BurstSketch drive them with timestamp = packet index:
// a transition happens before packet i whenever last + W < i.
struct WindowClock {
    uint64_t W, last = 0;
    uint32_t win = 0;
    explicit WindowClock(uint64_t w) : W(w) {}
    // Returns the number of window transitions that must happen before packet i.
    uint32_t advance(uint64_t i) {
        uint32_t n = 0;
        while (last + W < i) { last += W; ++win; ++n; }
        return n;
    }
};

struct PairKey {
    uint64_t id;
    uint64_t vbin;
    bool operator==(const PairKey& o) const { return id == o.id && vbin == o.vbin; }
};

struct PairKeyHash {
    size_t operator()(const PairKey& k) const { return (size_t)hash64(k.id, k.vbin + 0x51ED27); }
};

using PBMap = std::unordered_map<PairKey, int64_t, PairKeyHash>;

inline uint64_t vbin_of(uint64_t v, double delta) { return (uint64_t)((double)v / delta); }

struct Perf {
    double pr = 0, rr = 0, f1 = 0, aae = NAN, are = NAN;
    double are_gt = NAN;  // ARE over the ground-truth top-K pairs, an unreported pair counting as estimate 0
    size_t n_result = 0, n_gt = 0, n_correct = 0;
};

// Replicates PBSketch's Comparer::compareTop, including its tie handling:
// items are taken in descending count order; once K-1 items are taken, the threshold is fixed
// to the count of the (K-1)-th item and every item with at least that count is kept.
inline PBMap select_top(const PBMap& m, int K) {
    std::vector<std::pair<PairKey, int64_t>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    PBMap out;
    int count = 1;
    int64_t hit = 0;
    for (const auto& e : v) {
        if (hit > e.second) break;
        out.emplace(e.first, e.second);
        ++count;
        if (count == K) hit = e.second;
    }
    return out;
}

inline Perf compare_top(const PBMap& result, const PBMap& gt, int K) {
    PBMap rt = select_top(result, K), gtt = select_top(gt, K);
    Perf p;
    p.n_result = rt.size();
    p.n_gt = gtt.size();
    double aae = 0, are = 0;
    for (const auto& r : rt) {
        auto it = gtt.find(r.first);
        if (it == gtt.end()) continue;
        ++p.n_correct;
        double err = std::fabs((double)r.second - (double)it->second);
        aae += err;
        are += err / (double)it->second;
    }
    p.pr = p.n_result ? (double)p.n_correct / p.n_result : 0;
    p.rr = p.n_gt ? (double)p.n_correct / p.n_gt : 0;
    p.f1 = (p.pr + p.rr) > 0 ? 2 * p.pr * p.rr / (p.pr + p.rr) : 0;
    if (p.n_correct) {
        p.aae = aae / p.n_correct;
        p.are = are / p.n_correct;
    }
    if (!gtt.empty()) {
        double s = 0;
        for (const auto& g : gtt) {
            auto it = result.find(g.first);
            const double est = it == result.end() ? 0.0 : (double)it->second;
            s += std::fabs(est - (double)g.second) / (double)g.second;
        }
        p.are_gt = s / gtt.size();
    }
    return p;
}

}  // namespace pbo
