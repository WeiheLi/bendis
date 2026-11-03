// Checks that BendisSimd reports exactly what Bendis reports, in the same order, and measures the single-core
// throughput of both with the same report aggregation (exact counts of <item, interval bin> pairs).
// Usage: simd_check --data FILE [--fmt tuple21] [--label L] [--mem 16,32,...] [--seeds 3] [--reps 5] [--out CSV]
//                   [--window 30000] [--H 80]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "bendis.h"
#include "bendis_simd.h"
#include "pbo_common.h"

using namespace pbo;

template <typename T>
static std::vector<T> list(const std::string& s) {
    std::vector<T> v;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(',', a);
        if (b == std::string::npos) b = s.size();
        v.push_back((T)std::stod(s.substr(a, b - a)));
        a = b + 1;
    }
    return v;
}

int main(int argc, char** argv) {
    std::string data, fmt = "tuple21", label = "trace", out;
    std::vector<double> mems = {16, 32, 64, 128, 256};
    int seeds = 3, reps = 5;
    uint32_t W = 30000, H = 80;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--data") data = v;
        else if (k == "--fmt") fmt = v;
        else if (k == "--label") label = v;
        else if (k == "--mem") mems = list<double>(v);
        else if (k == "--seeds") seeds = std::stoi(v);
        else if (k == "--reps") reps = std::stoi(v);
        else if (k == "--out") out = v;
        else if (k == "--window") W = std::stoul(v);
        else if (k == "--H") H = std::stoul(v);
    }
    if (!bendis::BendisSimd::supported()) {
        std::fprintf(stderr, "AVX-512 not supported: check skipped\n");
        return 0;
    }
    auto ids = load_ids(data, fmt, UINT64_MAX);
    std::fprintf(stderr, "[%s] %zu arrivals\n", label.c_str(), ids.size());
    FILE* f = out.empty() ? nullptr : std::fopen(out.c_str(), "a");
    if (f && std::ftell(f) == 0) std::fprintf(f, "label,mem_kb,seed,rep,reports,identical,mops_scalar,mops_simd\n");
    bool all_ok = true;
    for (double mem : mems)
        for (int seed = 0; seed < seeds; ++seed) {
            bendis::BendisConfig c;
            c.mem_bytes = (size_t)(mem * 1024);
            c.W = W, c.H = H;
            c.seed += seed * 7919;
            // correctness: the sequences of reports
            std::vector<std::pair<uint64_t, uint32_t>> rs, rv;
            {
                bendis::Bendis s(c);
                for (uint64_t i = 0; i < ids.size(); ++i) s.insert(ids[i], i, [&](uint64_t e, uint32_t v) { rs.push_back({e, v}); });
                bendis::BendisSimd v(c);
                v.insert(ids.data(), 0, ids.size(), [&](uint64_t e, uint32_t x) { rv.push_back({e, x}); });
            }
            const bool ok = rs == rv;
            all_ok &= ok;
            // throughput, both aggregating the reports exactly
            for (int rep = 0; rep < reps; ++rep) {
                PBMap a1, a2;
                bendis::Bendis s(c);
                auto t0 = std::chrono::steady_clock::now();
                for (uint64_t i = 0; i < ids.size(); ++i)
                    s.insert(ids[i], i, [&](uint64_t e, uint32_t v) { a1[{e, vbin_of(v, 2)}]++; });
                const double ts = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                bendis::BendisSimd v(c);
                t0 = std::chrono::steady_clock::now();
                v.insert(ids.data(), 0, ids.size(), [&](uint64_t e, uint32_t x) { a2[{e, vbin_of(x, 2)}]++; });
                const double tv = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                const double ms = ids.size() / ts / 1e6, mv = ids.size() / tv / 1e6;
                std::fprintf(stderr, "[%s] %g KB seed %d rep %d: %zu reports, %s, scalar %.1f Mops, SIMD %.1f Mops (%.2fx)\n",
                             label.c_str(), mem, seed, rep, rs.size(), ok ? "identical" : "DIFFERENT", ms, mv, mv / ms);
                if (f) std::fprintf(f, "%s,%g,%d,%d,%zu,%d,%.2f,%.2f\n", label.c_str(), mem, seed, rep, rs.size(), ok, ms, mv);
            }
        }
    if (f) std::fclose(f);
    std::fprintf(stderr, all_ok ? "all identical\n" : "MISMATCH\n");
    return all_ok ? 0 : 1;
}
