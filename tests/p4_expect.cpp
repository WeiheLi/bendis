// Expected digests of p4/bendis.p4 (count windows, CRC hashes) for the first N packets of a trace,
// for comparison with a replay on tofino-model or hardware (p4/sde/replay.sh).
// Writes <prefix>.ids (little-endian uint64 flow id per packet) and <prefix>.expected.csv.
// Usage: p4_expect <trace> <fmt> <N> <prefix> [--win-bits 12] [--bits 10] [--h 10] [--report-bits 6]
#include <cstdio>

#include "groundtruth.h"
#include "p4_model.h"

using namespace pbo;

int main(int argc, char** argv) {
    std::string path = argv[1], fmt = argv[2], prefix = argv[4];
    uint64_t n = std::stoull(argv[3]);
    P4ModelConfig pc;
    pc.hash = P4ModelConfig::kCrc;
    pc.win_count_bits = 12;
    pc.cm_idx_bits = pc.tab_idx_bits = 10;
    pc.burst_h = 10;
    for (int i = 5; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        int v = std::stoi(argv[i + 1]);
        if (k == "--win-bits") pc.win_count_bits = v;
        else if (k == "--bits") pc.cm_idx_bits = pc.tab_idx_bits = v;
        else if (k == "--h") pc.burst_h = v;
        else if (k == "--report-bits") pc.report_bits = v;
    }
    auto ids = load_ids(path, fmt, n);
    FILE* f = std::fopen((prefix + ".ids").c_str(), "wb");
    std::fwrite(ids.data(), sizeof(uint64_t), ids.size(), f);
    std::fclose(f);

    P4Model model(pc);
    PBMap agg;
    f = std::fopen((prefix + ".expected.csv").c_str(), "w");
    std::fprintf(f, "src,dst,interval\n");
    size_t nd = 0;
    for (uint64_t id : ids)
        model.packet(id, [&](uint64_t fl, uint32_t v) {
            std::fprintf(f, "%u,%u,%u\n", (uint32_t)(fl >> 32), (uint32_t)fl, v);
            agg[{fl, vbin_of(v, 2)}]++;
            ++nd;
        });
    std::fclose(f);

    DefConfig d;
    d.W = 1u << pc.win_count_bits;
    d.H = pc.burst_h;
    std::vector<uint64_t> shifted{0};
    shifted.insert(shifted.end(), ids.begin(), ids.end());
    GroundTruth gt = compute_ground_truth(shifted, d);
    std::printf("packets=%zu windows=%zu digests=%zu gt_pairs=%zu", ids.size(), ids.size() >> pc.win_count_bits, nd,
                gt.onset.size());
    for (int K : {20, 50, 100}) std::printf(" F1@%d=%.3f", K, compare_top(agg, gt.onset, K).f1);
    std::printf("\n");
}
