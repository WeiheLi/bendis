// Checks the P4 golden model (src/p4_model.h) against the simulator (src/bendis.h):
//   1. bit-exact: with the simulator's hash functions and P4's count windows, every report
//      (packet index, flow, interval) must be identical;
//   2. statistical: with the P4 program's CRC hashes, F1 against the exact ground truth must
//      match the simulator's.
// Usage: p4_equiv <trace> [max packets] [--fmt pbs16|tuple13|tuple21] [--label L] [--out f.csv]
//                 [--bits 9,10,11,12,13] [--win-bits 15]
#include "groundtruth.h"
#include "bendis.h"
#include "p4_model.h"

using namespace pbo;

struct Report {
    uint64_t i, id;
    uint32_t v;
    bool operator==(const Report& o) const { return i == o.i && id == o.id && v == o.v; }
};

int main(int argc, char** argv) {
    std::string path = argv[1], fmt = "pbs16", label = "trace", out;
    uint64_t max_n = UINT64_MAX;
    std::vector<int> bits_list = {9, 10, 11, 12, 13};
    int win_bits = 15;
    for (int i = 2; i < argc; ++i) {
        std::string k = argv[i];
        if (k[0] != '-') max_n = std::stoull(k);
        else if (i + 1 < argc) {
            std::string v = argv[++i];
            if (k == "--fmt") fmt = v;
            else if (k == "--label") label = v;
            else if (k == "--out") out = v;
            else if (k == "--win-bits") win_bits = std::stoi(v);
            else if (k == "--bits") {
                bits_list.clear();
                for (size_t p = 0; p < v.size();) {
                    size_t q = v.find(',', p);
                    bits_list.push_back(std::stoi(v.substr(p, q - p)));
                    p = q == std::string::npos ? v.size() : q + 1;
                }
            }
        }
    }
    auto ids = load_ids(path, fmt, max_n);
    const uint32_t W = 1u << win_bits;

    // Ground truth on P4-aligned windows [kW, (k+1)W): shift the trace by one dummy packet,
    // since the reference clock puts packet i in window k when kW < i <= (k+1)W.
    std::vector<uint64_t> shifted;
    shifted.reserve(ids.size() + 1);
    shifted.push_back(0);
    shifted.insert(shifted.end(), ids.begin(), ids.end());
    DefConfig d;
    d.W = W;
    GroundTruth gt = compute_ground_truth(shifted, d);

    FILE* csv = out.empty() ? nullptr : std::fopen(out.c_str(), "w");
    if (csv) std::fprintf(csv, "label,bits,mem_kb,reports_sim,reports_p4crc,exact,K,F1_sim,F1_p4crc,ARE_sim,ARE_p4crc\n");
    bool all_exact = true;
    std::printf("[%s] bits  reports  exact   F1@100 sim/p4crc   F1@500 sim/p4crc   F1@1000 sim/p4crc\n", label.c_str());
    for (int bits : bits_list) {
        bendis::BendisConfig sc;
        sc.cm_entries = sc.table_entries = (size_t)1 << bits;
        sc.W = W;
        bendis::Bendis sim(sc);
        P4ModelConfig pc;
        pc.cm_idx_bits = pc.tab_idx_bits = bits;
        pc.win_count_bits = win_bits;
        P4Model p4sim(pc);
        pc.hash = P4ModelConfig::kCrc;
        P4Model p4crc(pc);

        std::vector<Report> a, b;
        PBMap agg_sim, agg_crc;
        size_t n_crc = 0;
        for (uint64_t i = 0; i < ids.size(); ++i) {
            sim.insert(ids[i], i + 1, [&](uint64_t f, uint32_t v) {
                a.push_back({i, f, v});
                agg_sim[{f, vbin_of(v, d.delta)}]++;
            });
            p4sim.packet(ids[i], [&](uint64_t f, uint32_t v) { b.push_back({i, f, v}); });
            p4crc.packet(ids[i], [&](uint64_t f, uint32_t v) {
                agg_crc[{f, vbin_of(v, d.delta)}]++;
                ++n_crc;
            });
        }
        bool exact = a == b;
        all_exact &= exact;
        if (!exact) {
            size_t k = 0;
            while (k < a.size() && k < b.size() && a[k] == b[k]) ++k;
            std::printf("  first difference at report %zu (sim %zu reports, p4 %zu)\n", k, a.size(), b.size());
        }
        std::printf("[%s] %4d %8zu  %-5s", label.c_str(), bits, a.size(), exact ? "yes" : "NO");
        for (int K : {100, 500, 1000})
            std::printf("   %6.3f / %6.3f  ", compare_top(agg_sim, gt.onset, K).f1, compare_top(agg_crc, gt.onset, K).f1);
        std::printf("\n");
        if (csv)
            for (int K : {20, 100, 200, 500, 1000}) {
                Perf ps = compare_top(agg_sim, gt.onset, K), pp = compare_top(agg_crc, gt.onset, K);
                std::fprintf(csv, "%s,%d,%.2f,%zu,%zu,%d,%d,%.6f,%.6f,%.6f,%.6f\n", label.c_str(), bits,
                             26.0 * (1 << bits) / 1024, a.size(), n_crc, exact ? 1 : 0, K, ps.f1, pp.f1, ps.are, pp.are);
            }
    }
    if (csv) std::fclose(csv);
    return all_exact ? 0 : 1;
}
