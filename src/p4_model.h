// Golden model of p4/bendis.p4 built with -DWINDOW_BY_COUNT: one function per P4 action or
// RegisterAction, applied in the same order as the ingress control, on the same register layout.
// Used to check that the P4 logic matches the simulator (src/bendis.h) and, later, to produce the
// expected digests for a trace replayed on tofino-model or hardware.
//
// Hashes: kSim reproduces the simulator's index and fingerprint functions (for a bit-exact
// comparison with it); kCrc reproduces the CRC hashes of the P4 program, with which a replay on
// tofino-model emits exactly the digests of this model (p4/sde/replay.sh).
#pragma once

#include "pbo_common.h"

namespace pbo {

struct P4ModelConfig {
    int cm_idx_bits = 11, tab_idx_bits = 11, win_count_bits = 15;
    uint32_t burst_h = 80, burst_k_shift = 1, min_gap_r = 1, report_bits = 6;  // VMAX = R + 2^bits
    enum Hash { kSim, kCrc } hash = kSim;
    uint64_t seed = 1;  // kSim only: same meaning as BendisConfig::seed
    // Count windows: the P4 count_window register computes WIN_FLAG + window in 16 bits, which drops
    // WIN_FLAG after 2^15 windows; the FPGA keeps it (WIN_FLAG | window mod 2^15), like time windows.
    bool keep_win_flag = false;
};

class P4Model {
public:
    static constexpr uint16_t WIN_FLAG = 0x8000, TAB_PASSED = 0;

    explicit P4Model(const P4ModelConfig& c) : c_(c) {
        size_t w = (size_t)1 << c.cm_idx_bits, t = (size_t)1 << c.tab_idx_bits;
        for (auto& row : cm_)
            for (auto& bank : row) bank.assign(w, 0);
        for (auto& row : otab_) row.assign(t, Pair{0, 1});  // entries start at window 1
        seed_cm_ = splitmix64(c.seed);
        seed_tab_ = splitmix64(c.seed + 1000);
    }

    // One IPv4 packet; calls digest(id, interval) when the P4 program would emit a digest.
    template <typename F>
    void packet(uint64_t id, F&& digest) {
        Meta m{};
        // count_packet; set_window (count_window register: WIN_FLAG + window, 16-bit)
        uint32_t n = pkt_counter_++;
        uint32_t wnum = n >> c_.win_count_bits;
        m.win = c_.keep_win_flag ? (uint16_t)(WIN_FLAG | (wnum & 0x7FFF)) : (uint16_t)(WIN_FLAG + wnum);
        m.zidx = n & mask(c_.cm_idx_bits);
        // advance_rotation; report_base
        if (rotation_.lo != m.win) {
            rotation_.hi = rotation_.hi == 2 ? 0 : rotation_.hi + 1;
            rotation_.lo = m.win;
        }
        m.rot = rotation_.hi;
        uint16_t winr = (uint16_t)(m.win - (c_.min_gap_r + 1));
        // hash_*, fix_zero_fp
        hashes(id, m);
        if (m.fp == 0) m.fp = 1;
        // CM rows: count bank rot, read bank (rot+2)%3, clear bank (rot+1)%3
        for (int r = 0; r < 3; ++r) {
            uint16_t& x = cm_[r][m.rot][m.idx[r]];
            bool cu = r == 0 || (r == 1 && x < m.c[0]) || (r == 2 && x < m.c[0] && x < m.c[1]);
            if (cu && x < 0xFFFF) ++x;  // value |+| 1
            m.c[r] = x;
            m.p[r] = cm_[r][(m.rot + 2) % 3][m.idx[r]];
            cm_[r][(m.rot + 1) % 3][m.zidx] = 0;
        }
        // min_01, min_012, onset_terms, onset_deficit
        uint16_t c = std::min({m.c[0], m.c[1], m.c[2]});
        uint16_t p = std::min({m.p[0], m.p[1], m.p[2]});
        uint16_t ck = (uint16_t)(c >> c_.burst_k_shift);
        uint16_t hd = c_.burst_h > c ? (uint16_t)(c_.burst_h - c) : 0;
        uint16_t deficit = p > ck ? (uint16_t)(p - ck) : 0;
        // onset table: old0 = 1 when there is no onset (parser default)
        uint16_t old0 = 1, old;
        if (hd == 0 && deficit == 0) old0 = otab_access(0, m);
        if (old0 == TAB_PASSED) old = otab_access(1, m);
        else old = old0;
        uint16_t v = (uint16_t)(m.win - old), vr = (uint16_t)(winr - old);
        if ((vr >> c_.report_bits) == 0) digest(id, (uint32_t)v);
    }

private:
    struct Pair {
        uint16_t lo = 0, hi = 0;
    };
    struct Meta {
        uint16_t win, rot, fp;
        uint32_t zidx, idx[3], tidx[2];
        uint16_t c[3], p[3];
    };

    static uint32_t mask(int bits) { return (uint32_t)((1ULL << bits) - 1); }

    // Match or stale entry: store {fp, win}, return the old window; otherwise pass (0).
    uint16_t otab_access(int r, const Meta& m) {
        Pair& e = otab_[r][m.tidx[r]];
        uint32_t vmax = c_.min_gap_r + (1u << c_.report_bits);
        if (e.lo == m.fp || (uint16_t)(m.win - e.hi) > vmax) {
            uint16_t rv = e.hi;
            e.lo = m.fp;
            e.hi = m.win;
            return rv;
        }
        return TAB_PASSED;
    }

    void hashes(uint64_t id, Meta& m) const {
        if (c_.hash == P4ModelConfig::kSim) {
            // the simulator's functions; range(x, 2^b) is the top b bits of x.
            uint64_t h = splitmix64(id ^ seed_cm_);
            uint32_t hr[3] = {(uint32_t)h, (uint32_t)(h >> 32), (uint32_t)(((h + 2) * 0x9E3779B97F4A7C15ULL) >> 32)};
            for (int r = 0; r < 3; ++r) m.idx[r] = hr[r] >> (32 - c_.cm_idx_bits);
            uint64_t g = splitmix64(id ^ seed_tab_);
            m.tidx[0] = (uint32_t)g >> (32 - c_.tab_idx_bits);
            m.tidx[1] = (uint32_t)(g >> 32) >> (32 - c_.tab_idx_bits);
            m.fp = (uint16_t)(splitmix64(g) >> 48);
            return;
        }
        uint8_t key[8];  // {src_addr, dst_addr}, network byte order
        uint32_t src = (uint32_t)(id >> 32), dst = (uint32_t)id;
        for (int i = 0; i < 4; ++i) {
            key[i] = (uint8_t)(src >> (24 - 8 * i));
            key[4 + i] = (uint8_t)(dst >> (24 - 8 * i));
        }
        m.idx[0] = crc32(key, 0x04C11DB7, true, 0xFFFFFFFF, 0xFFFFFFFF) & mask(c_.cm_idx_bits);
        m.idx[1] = crc32(key, 0x1EDC6F41, true, 0xFFFFFFFF, 0xFFFFFFFF) & mask(c_.cm_idx_bits);
        m.idx[2] = crc32(key, 0x741B8CD7, true, 0xFFFFFFFF, 0xFFFFFFFF) & mask(c_.cm_idx_bits);
        m.tidx[0] = crc32(key, 0x814141AB, false, 0, 0) & mask(c_.tab_idx_bits);
        m.tidx[1] = crc32(key, 0xA833982B, true, 0xFFFFFFFF, 0xFFFFFFFF) & mask(c_.tab_idx_bits);
        m.fp = crc16(key, 0x8005, true);
    }

    static uint32_t reflect(uint32_t x, int bits) {
        uint32_t r = 0;
        for (int i = 0; i < bits; ++i)
            if (x & (1u << i)) r |= 1u << (bits - 1 - i);
        return r;
    }

    static uint32_t crc32(const uint8_t* d, uint32_t poly, bool reversed, uint32_t init, uint32_t xorout) {
        uint32_t crc = init;
        for (int i = 0; i < 8; ++i) {
            uint32_t byte = reversed ? reflect(d[i], 8) : d[i];
            crc ^= byte << 24;
            for (int b = 0; b < 8; ++b) crc = (crc & 0x80000000u) ? (crc << 1) ^ poly : crc << 1;
        }
        if (reversed) crc = reflect(crc, 32);
        return crc ^ xorout;
    }

    static uint16_t crc16(const uint8_t* d, uint16_t poly, bool reversed) {
        uint16_t crc = 0;
        for (int i = 0; i < 8; ++i) {
            uint16_t byte = reversed ? (uint16_t)reflect(d[i], 8) : d[i];
            crc ^= (uint16_t)(byte << 8);
            for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ poly) : (uint16_t)(crc << 1);
        }
        return reversed ? (uint16_t)reflect(crc, 16) : crc;
    }

    P4ModelConfig c_;
    uint32_t pkt_counter_ = 0;
    Pair rotation_;
    std::vector<uint16_t> cm_[3][3];  // row, bank
    std::vector<Pair> otab_[2];
    uint64_t seed_cm_, seed_tab_;
};

}  // namespace pbo
