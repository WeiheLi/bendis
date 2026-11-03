// Bendis: periodic burst detection in the data plane. This is the reference model of
// p4/bendis.p4 and fpga/rtl/bendis.v, plus the control plane that aggregates the reports.
//
// Every operation maps to one register access in one pipeline stage, executed at most once per
// packet, with data flowing forward only (no recirculation).
//
// Counting sketch: d rows, each made of three 16-bit register arrays (banks) whose roles rotate
// with the window number t: bank t%3 counts the current window, bank (t-1)%3 holds the previous
// window and is only read, and bank (t+1)%3 is cleared one entry per packet (entry = packet index
// within the window mod w), so it is all zero when it becomes the counting bank as long as a
// window carries at least w packets; otherwise, the switch injects packets that only clear the rest
// of the bank, emulated here at the window boundary. Rows are updated with pipelined conservative update (CU): row
// 0 always increments, row r > 0 increments only if its value is below the minimum of the rows
// before it. This never underestimates and needs a single stateful-ALU condition.
//
//   counting rows : c = min of the counting banks, p = min of the previous banks
//   onset test    : c >= H and c >= k * p (a shift when k is a power of two)
//   onset table   : m rows of {16-bit fingerprint, 16-bit window of the last onset}
//
// An onset-table row evaluates "fingerprint matches" and "entry is stale" (last onset more than
// V windows ago): on a match it returns the old window and stores the current one; otherwise a
// stale entry is claimed; otherwise the flow passes to the next row (and the onset is dropped after
// the last row, so tracked flows are never evicted by newcomers). The interval v = t - old is
// reported when R < v <= V; v = 0 marks later packets of the same window, so each flow is reported
// at most once per window without deduplication state. The control plane counts every reported
// pair <flow, floor(v / delta)> exactly and answers top-K queries.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace bendis {

struct BendisConfig {
    size_t mem_bytes = 32 * 1024;  // counting sketch and onset table
    double table_ratio = 0.3;      // share of mem_bytes given to the onset table
    int rows = 3;                  // counting rows d
    int table_rows = 2;            // onset-table rows m
    size_t cm_entries = 0;         // if non-zero, counters per bank and entries per table row as on
    size_t table_entries = 0;      // hardware (powers of two); mem_bytes is then unused
    uint32_t W = 30000;            // packets per window
    uint32_t H = 80, k = 2;        // burst threshold and burst ratio
    uint32_t R = 1, vmax = 65;     // intervals v with R < v <= V are reported (V = vmax, the
                                   // staleness bound; V - R = 2^6 on hardware)
    uint64_t seed = 1;
};

class Bendis {
public:
    static constexpr size_t kCounterBytes = 3 * 2;  // three 16-bit banks
    static constexpr size_t kEntryBytes = 4;        // 16-bit fingerprint + 16-bit window

    explicit Bendis(const BendisConfig& cfg) : c_(cfg) {
        size_t table_bytes = (size_t)(cfg.mem_bytes * cfg.table_ratio);
        w_ = std::max<size_t>(1, (cfg.mem_bytes - table_bytes) / (cfg.rows * kCounterBytes));
        wo_ = std::max<size_t>(1, table_bytes / (cfg.table_rows * kEntryBytes));
        if (cfg.cm_entries) w_ = cfg.cm_entries;
        if (cfg.table_entries) wo_ = cfg.table_entries;
        cell_.assign((size_t)cfg.rows * w_, Cell{});
        table_.assign(cfg.table_rows, std::vector<uint32_t>(wo_, 0));
        seed_cm_ = splitmix64(cfg.seed);
        seed_tab_ = splitmix64(cfg.seed + 1000);
    }

    struct NoObserver {
        void operator()(uint32_t, uint32_t) const {}
    };

    // Processes packet i (packet index, from 0) of flow id; report(id, v) receives each interval, and
    // observe(c, p) the estimates of the onset test (used only to validate the analysis).
    template <typename F, typename O = NoObserver>
    void insert(uint64_t id, uint64_t i, F&& report, O&& observe = O{}) {
        bool new_window = false;
        while (last_ + c_.W < i) {  // count windows
            last_ += c_.W;
            ++win_;
            new_window = true;
        }
        if (new_window) {
            if (!wrapped_)  // fewer than w packets: injected packets clear the rest of the bank
                for (int r = 0; r < c_.rows; ++r)
                    for (size_t j = z_; j < w_; ++j) cell_[r * w_ + j].bank[bz_] = 0;
            z_ = 0, wrapped_ = false;
            bc_ = win_ % 3, bp_ = (win_ + 2) % 3, bz_ = (win_ + 1) % 3;
        }
        const size_t z = z_;
        if (++z_ == w_) z_ = 0, wrapped_ = true;

        // Counting rows: conservative update of the counting bank, read of the previous bank,
        // zeroing of one entry of the clearing bank.
        uint64_t h = splitmix64(id ^ seed_cm_);
        uint32_t c = UINT32_MAX, p = UINT32_MAX;
        for (int r = 0; r < c_.rows; ++r) {
            Cell& cell = cell_[r * w_ + range(row_hash(h, r), w_)];
            uint16_t& x = cell.bank[bc_];
            if ((r == 0 || x < c) && x < 0xFFFF) ++x;
            c = std::min<uint32_t>(c, x);
            p = std::min<uint32_t>(p, cell.bank[bp_]);
            cell_[r * w_ + z].bank[bz_] = 0;
        }
        observe(c, p);
        if (c < c_.H || c < c_.k * p) return;  // onset test

        // Onset table. Windows are offset by V + 1 so that never-written entries (window 0) are stale.
        const uint16_t T = (uint16_t)(win_ + c_.vmax + 1);
        uint64_t g = splitmix64(id ^ seed_tab_);
        uint16_t fp = (uint16_t)(splitmix64(g) >> 48);
        if (fp == 0) fp = 1;
        for (int r = 0; r < c_.table_rows; ++r) {
            uint32_t hr = r == 0 ? (uint32_t)g : r == 1 ? (uint32_t)(g >> 32) : (uint32_t)splitmix64(g + r);
            uint32_t& e = table_[r][range(hr, wo_)];
            uint16_t efp = (uint16_t)(e >> 16), et = (uint16_t)(e & 0xFFFF);
            if (efp == fp) {  // match: interval to the last onset
                uint16_t v = (uint16_t)(T - et);
                e = ((uint32_t)fp << 16) | T;
                if (v > c_.R && v <= c_.vmax) report(id, (uint32_t)v);
                return;
            }
            if ((uint16_t)(T - et) > c_.vmax) {  // stale entry: claim it, first onset of this flow
                e = ((uint32_t)fp << 16) | T;
                return;
            }
        }
    }

    size_t counters_per_row() const { return w_; }
    // Index of the counter of flow id in counting row r.
    size_t counter_index(uint64_t id, int r) const { return range(row_hash(splitmix64(id ^ seed_cm_), r), w_); }
    size_t table_entries_per_row() const { return wo_; }

private:
    static uint64_t splitmix64(uint64_t x) {
        x += 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    }
    static size_t range(uint32_t x, size_t n) { return (size_t)(((uint64_t)x * n) >> 32); }
    static uint32_t row_hash(uint64_t h, int r) {
        return r == 0 ? (uint32_t)h : r == 1 ? (uint32_t)(h >> 32) : (uint32_t)(((h + r) * 0x9E3779B97F4A7C15ULL) >> 32);
    }

    BendisConfig c_;
    size_t w_, wo_;
    uint64_t last_ = 0;             // last packet index of the previous window
    uint32_t win_ = 0;              // window number t
    size_t z_ = 0;                  // entry of the clearing bank to zero next
    bool wrapped_ = false;          // the whole clearing bank has been zeroed in this window
    int bc_ = 0, bp_ = 2, bz_ = 1;  // counting, previous and clearing bank of the current window
    uint64_t seed_cm_, seed_tab_;
    // The three banks of a (row, index) are stored together for cache locality in software; on the
    // switch they are three separate register arrays.
    struct Cell {
        uint16_t bank[3] = {0, 0, 0};
    };
    std::vector<Cell> cell_;                    // row r, index i at r * w_ + i
    std::vector<std::vector<uint32_t>> table_;  // fp16 << 16 | window16
};

// Control plane: exact frequency n(e, b) of every reported pair <flow e, interval bin
// b = floor(v / delta)> and top-K query.
class ControlPlane {
public:
    using Pair = std::pair<uint64_t, uint64_t>;  // (flow, interval bin)

    explicit ControlPlane(double delta = 2) : delta_(delta) {}

    void report(uint64_t flow, uint32_t v) { ++n_[{flow, (uint64_t)((double)v / delta_)}]; }

    // The K most frequent pairs, by decreasing frequency.
    std::vector<std::pair<Pair, uint64_t>> top(size_t K) const {
        std::vector<std::pair<Pair, uint64_t>> v(n_.begin(), n_.end());
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        if (v.size() > K) v.resize(K);
        return v;
    }

    const std::map<Pair, uint64_t>& frequencies() const { return n_; }

private:
    double delta_;
    std::map<Pair, uint64_t> n_;
};

}  // namespace bendis
