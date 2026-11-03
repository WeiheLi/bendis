// AVX-512 implementation of Bendis, bit-exact with bendis::Bendis (the same reports in the same order;
// tests/simd_check.cpp). Arrivals are processed in batches of 16 within one window:
//   - the hashes and counter indices of the batch are computed in vector registers;
//   - every counting row is updated by gathers and scatters of the aligned 32-bit words that hold the 16-bit
//     counters, with conservative update across the rows as in Bendis;
//   - a lane whose word in some row is also used by an earlier lane of the batch is deferred and processed
//     in order after the other lanes, with which it shares no counter, so the counts equal those of
//     processing the batch one arrival at a time;
//   - the clearing bank is zeroed for the 16 arrivals at once, and the onset table is probed by scalar code,
//     in arrival order, for the lanes that pass the onset test.
// Each bank is a separate array of 16-bit counters, as the register arrays of the switch; the memory equals
// that of Bendis for the same configuration.
#pragma once
#include "bendis.h"

#include <cstring>
#include <stdexcept>
#include <vector>

#if defined(__x86_64__)
#include <immintrin.h>
#define BENDIS_AVX512 __attribute__((target("avx512f,avx512cd,avx512vl,avx512bw,avx512dq")))
#endif

namespace bendis {

class BendisSimd {
public:
    static bool supported() {
        return __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512cd") &&
               __builtin_cpu_supports("avx512vl") && __builtin_cpu_supports("avx512bw") &&
               __builtin_cpu_supports("avx512dq");
    }

    explicit BendisSimd(const BendisConfig& cfg) : c_(cfg) {
        if (!supported()) throw std::runtime_error("BendisSimd: AVX-512 F/CD/VL/BW/DQ required");
        if (cfg.rows < 1 || cfg.rows > kMaxRows) throw std::runtime_error("BendisSimd: 1 to 4 counting rows");
        // the sizes of Bendis
        size_t table_bytes = (size_t)(cfg.mem_bytes * cfg.table_ratio);
        w_ = std::max<size_t>(1, (cfg.mem_bytes - table_bytes) / (cfg.rows * 6));
        wo_ = std::max<size_t>(1, table_bytes / (cfg.table_rows * 4));
        if (cfg.cm_entries) w_ = cfg.cm_entries;
        if (cfg.table_entries) wo_ = cfg.table_entries;
        words_ = (w_ + 1) / 2;
        for (int r = 0; r < cfg.rows; ++r)
            for (int b = 0; b < 3; ++b) bank_[r][b].assign(2 * words_, 0);
        table_.assign(cfg.table_rows, std::vector<uint32_t>(wo_, 0));
        seed_cm_ = splitmix64(cfg.seed);
        seed_tab_ = splitmix64(cfg.seed + 1000);
    }

    // Processes the n arrivals ids[0..n) with packet indices i0, i0 + 1, ...; report(id, v) receives each
    // interval, as Bendis::insert.
    template <typename F>
    BENDIS_AVX512 void insert(const uint64_t* ids, uint64_t i0, size_t n, F&& report) {
        size_t pos = 0;
        while (pos < n) {
            const uint64_t i = i0 + pos;
            advance(i);
            const uint64_t room = last_ + c_.W - i + 1;  // arrivals left in this window
            const size_t cnt = (size_t)std::min<uint64_t>({(uint64_t)B, room, (uint64_t)(n - pos)});
            if (cnt == B) {
                batch(ids + pos, report);
            } else {
                for (size_t j = 0; j < cnt; ++j) {
                    uint32_t c, p;
                    count(ids[pos + j], c, p);
                    clear(1);
                    if (c >= c_.H && c >= c_.k * p) table(ids[pos + j], report);
                }
            }
            pos += cnt;
        }
    }

    size_t counters_per_row() const { return w_; }
    size_t table_entries_per_row() const { return wo_; }

private:
    static constexpr int B = 16, kMaxRows = 4;

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

    void advance(uint64_t i) {  // count windows, as Bendis::insert
        bool new_window = false;
        while (last_ + c_.W < i) {
            last_ += c_.W;
            ++win_;
            new_window = true;
        }
        if (new_window) {
            if (!wrapped_)  // fewer than w arrivals: injected packets clear the rest of the bank (as Bendis)
                for (int r = 0; r < c_.rows; ++r) std::memset(bank_[r][bz_].data() + z_, 0, (w_ - z_) * 2);
            z_ = 0, wrapped_ = false;
            bc_ = win_ % 3, bp_ = (win_ + 2) % 3, bz_ = (win_ + 1) % 3;
        }
    }

    // Counting rows of one arrival: conservative update of the counting bank and read of the previous bank.
    void count(uint64_t id, uint32_t& c, uint32_t& p) {
        uint64_t h = splitmix64(id ^ seed_cm_);
        c = UINT32_MAX, p = UINT32_MAX;
        for (int r = 0; r < c_.rows; ++r) {
            const size_t x = range(row_hash(h, r), w_);
            uint16_t& v = bank_[r][bc_][x];
            if ((r == 0 || v < c) && v < 0xFFFF) ++v;
            c = std::min<uint32_t>(c, v);
            p = std::min<uint32_t>(p, bank_[r][bp_][x]);
        }
    }

    // Zeroes the next n entries of the clearing bank of every row (n <= w).
    void clear(size_t n) {
        const size_t a = std::min(n, w_ - z_);
        for (int r = 0; r < c_.rows; ++r) {
            uint16_t* zb = bank_[r][bz_].data();
            std::memset(zb + z_, 0, a * 2);
            if (a < n) std::memset(zb, 0, (n - a) * 2);
        }
        z_ += n;
        if (z_ >= w_) z_ -= w_, wrapped_ = true;
    }

#if defined(__x86_64__)
    // clear(16) by one 32-byte store per row when the 16 entries do not wrap around
    BENDIS_AVX512 void clear16() {
        if (z_ + B > w_) return clear(B);
        for (int r = 0; r < c_.rows; ++r) _mm256_storeu_si256((__m256i*)(bank_[r][bz_].data() + z_), _mm256_setzero_si256());
        z_ += B;
        if (z_ >= w_) z_ -= w_, wrapped_ = true;
    }
#endif

    // Onset table, as Bendis::insert.
    template <typename F>
    void table(uint64_t id, F&& report) {
        const uint16_t T = (uint16_t)(win_ + c_.vmax + 1);
        uint64_t g = splitmix64(id ^ seed_tab_);
        uint16_t fp = (uint16_t)(splitmix64(g) >> 48);
        if (fp == 0) fp = 1;
        for (int r = 0; r < c_.table_rows; ++r) {
            uint32_t hr = r == 0 ? (uint32_t)g : r == 1 ? (uint32_t)(g >> 32) : (uint32_t)splitmix64(g + r);
            uint32_t& e = table_[r][range(hr, wo_)];
            uint16_t efp = (uint16_t)(e >> 16), et = (uint16_t)(e & 0xFFFF);
            if (efp == fp) {
                uint16_t v = (uint16_t)(T - et);
                e = ((uint32_t)fp << 16) | T;
                if (v > c_.R && v <= c_.vmax) report(id, (uint32_t)v);
                return;
            }
            if ((uint16_t)(T - et) > c_.vmax) {
                e = ((uint32_t)fp << 16) | T;
                return;
            }
        }
    }

#if defined(__x86_64__)
    BENDIS_AVX512 static __m512i set1(uint64_t v) { return _mm512_set1_epi64((long long)v); }

    BENDIS_AVX512 static __m512i splitmix(__m512i x) {
        x = _mm512_add_epi64(x, set1(0x9E3779B97F4A7C15ULL));
        x = _mm512_mullo_epi64(_mm512_xor_si512(x, _mm512_srli_epi64(x, 30)), set1(0xBF58476D1CE4E5B9ULL));
        x = _mm512_mullo_epi64(_mm512_xor_si512(x, _mm512_srli_epi64(x, 27)), set1(0x94D049BB133111EBULL));
        return _mm512_xor_si512(x, _mm512_srli_epi64(x, 31));
    }

    // Counter indices range(row_hash(h, r), w) of 8 hashes, as 32-bit lanes.
    BENDIS_AVX512 __m256i index8(__m512i h, int r) const {
        __m512i x;
        if (r == 0) x = h;
        else if (r == 1) x = _mm512_srli_epi64(h, 32);
        else x = _mm512_srli_epi64(_mm512_mullo_epi64(_mm512_add_epi64(h, set1((uint64_t)r)), set1(0x9E3779B97F4A7C15ULL)), 32);
        return _mm512_cvtepi64_epi32(_mm512_srli_epi64(_mm512_mul_epu32(x, set1(w_)), 32));
    }

    template <typename F>
    BENDIS_AVX512 void batch(const uint64_t* ids, F&& report) {
        const __m512i h0 = splitmix(_mm512_xor_si512(_mm512_loadu_si512(ids), set1(seed_cm_)));
        const __m512i h1 = splitmix(_mm512_xor_si512(_mm512_loadu_si512(ids + 8), set1(seed_cm_)));
        __m512i wi[kMaxRows], sh[kMaxRows];
        const __m512i one = _mm512_set1_epi32(1);
        __m512i dep = _mm512_setzero_si512();  // per lane, the earlier lanes that share a word in some row
        for (int r = 0; r < c_.rows; ++r) {
            const __m512i idx = _mm512_inserti64x4(_mm512_castsi256_si512(index8(h0, r)), index8(h1, r), 1);
            wi[r] = _mm512_srli_epi32(idx, 1);
            sh[r] = _mm512_slli_epi32(_mm512_and_si512(idx, one), 4);
            dep = _mm512_or_si512(dep, _mm512_conflict_epi32(wi[r]));
        }
        const __mmask16 defer = _mm512_test_epi32_mask(dep, dep);
        __mmask16 onset = lanes((__mmask16)~defer, wi, sh);
        if (defer) {  // one by one, in order, branch-free, with the indices computed above
            alignas(64) uint32_t ix[kMaxRows][B];
            for (int r = 0; r < c_.rows; ++r)
                _mm512_store_si512(ix[r], _mm512_add_epi32(_mm512_slli_epi32(wi[r], 1), _mm512_srli_epi32(sh[r], 4)));
            for (unsigned m = defer; m; m &= m - 1) {
                const int j = __builtin_ctz(m);
                uint32_t c = UINT32_MAX, p = UINT32_MAX;
                for (int r = 0; r < c_.rows; ++r) {
                    uint16_t* v = &bank_[r][bc_][ix[r][j]];
                    const uint32_t x = *v, x1 = x + (uint32_t)(((r == 0) | (x < c)) & (x < 0xFFFF));
                    *v = (uint16_t)x1;
                    c = std::min(c, x1);
                    p = std::min<uint32_t>(p, bank_[r][bp_][ix[r][j]]);
                }
                onset |= (__mmask16)((unsigned)((c >= c_.H) & (c >= c_.k * p)) << j);
            }
        }
        clear16();
        for (unsigned m = onset; m; m &= m - 1) table(ids[__builtin_ctz(m)], report);
    }

    // Counting rows of the lanes vm, which share no word: returns the lanes that pass the onset test.
    BENDIS_AVX512 __mmask16 lanes(__mmask16 vm, const __m512i* wi, const __m512i* sh) {
        const __m512i one = _mm512_set1_epi32(1), lo16 = _mm512_set1_epi32(0xFFFF);
        __m512i cmin = _mm512_set1_epi32(-1), pmin = cmin;
        for (int r = 0; r < c_.rows; ++r) {
            int* cb = (int*)bank_[r][bc_].data();
            const int* pb = (const int*)bank_[r][bp_].data();
            const __m512i word = _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), vm, wi[r], cb, 4);
            const __m512i x = _mm512_and_si512(_mm512_srlv_epi32(word, sh[r]), lo16);
            __mmask16 inc = _mm512_mask_cmplt_epu32_mask(vm, x, lo16);
            if (r > 0) inc &= _mm512_cmplt_epu32_mask(x, cmin);
            const __m512i x1 = _mm512_mask_add_epi32(x, inc, x, one);
            const __m512i nw = _mm512_add_epi32(word, _mm512_maskz_sllv_epi32(inc, one, sh[r]));
            _mm512_mask_i32scatter_epi32(cb, inc, wi[r], nw, 4);
            cmin = _mm512_min_epu32(cmin, x1);
            const __m512i pw = _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), vm, wi[r], pb, 4);
            pmin = _mm512_min_epu32(pmin, _mm512_and_si512(_mm512_srlv_epi32(pw, sh[r]), lo16));
        }
        return vm & _mm512_cmpge_epu32_mask(cmin, _mm512_set1_epi32((int)c_.H)) &
               _mm512_cmpge_epu32_mask(cmin, _mm512_mullo_epi32(pmin, _mm512_set1_epi32((int)c_.k)));
    }
#endif

    BendisConfig c_;
    size_t w_, wo_, words_;
    uint64_t last_ = 0;
    uint32_t win_ = 0;
    size_t z_ = 0;
    bool wrapped_ = false;
    int bc_ = 0, bp_ = 2, bz_ = 1;
    uint64_t seed_cm_, seed_tab_;
    std::vector<uint16_t> bank_[kMaxRows][3];   // row r, bank b: 2 * ceil(w / 2) counters
    std::vector<std::vector<uint32_t>> table_;  // fp16 << 16 | window16
};

}  // namespace bendis
