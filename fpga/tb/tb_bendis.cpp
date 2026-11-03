// Verilator testbench: streams a trace through the RTL (one packet per cycle, no idle cycles) and
// through the P4 golden model with CRC hashes, checks that the digest sequences are identical,
// and scores the RTL digests against the exact onset ground truth.
// Usage: tb_bendis <trace> [--fmt tuple21] [--label L] [--max N] [--out f.csv]
#include <cstdio>
#include <memory>

#include "Vbendis.h"
#include "groundtruth.h"
#include "p4_model.h"
#include "verilated.h"

#ifndef CM_BITS
#define CM_BITS 11
#endif
#ifndef WIN_BITS
#define WIN_BITS 15
#endif

using namespace pbo;

int main(int argc, char** argv) {
    std::string path = argv[1], fmt = "tuple21", label = "trace", out;
    uint64_t max_n = UINT64_MAX;
    for (int i = 2; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--fmt") fmt = v;
        else if (k == "--label") label = v;
        else if (k == "--max") max_n = std::stoull(v);
        else if (k == "--out") out = v;
    }
    auto ids = load_ids(path, fmt, max_n);

    P4ModelConfig pc;
    pc.cm_idx_bits = pc.tab_idx_bits = CM_BITS;
    pc.win_count_bits = WIN_BITS;
    pc.hash = P4ModelConfig::kCrc;
    pc.keep_win_flag = true;  // as rtl/bendis.v
    P4Model model(pc);
    std::vector<std::pair<uint64_t, uint32_t>> want, got;
    want.reserve(1 << 20);
    got.reserve(1 << 20);

    auto ctx = std::make_unique<VerilatedContext>();
    auto dut = std::make_unique<Vbendis>(ctx.get());
    auto tick = [&] {
        dut->clk = 0;
        dut->eval();
        dut->clk = 1;
        dut->eval();
        if (dut->out_valid) got.push_back({dut->out_key, (uint32_t)dut->out_v});
    };
    dut->rst = 1;
    dut->in_valid = 0;
    for (int i = 0; i < 4; ++i) tick();
    dut->rst = 0;
    PBMap agg;
    for (uint64_t i = 0; i < ids.size(); ++i) {
        dut->in_valid = 1;
        dut->in_key = ids[i];
        tick();
        model.packet(ids[i], [&](uint64_t f, uint32_t v) { want.push_back({f, v}); });
    }
    dut->in_valid = 0;
    for (int i = 0; i < 8; ++i) tick();
    dut->final();

    DefConfig d;
    d.W = 1u << WIN_BITS;
    std::vector<uint64_t> shifted;  // P4-aligned windows, as in tests/p4_equiv.cpp
    shifted.reserve(ids.size() + 1);
    shifted.push_back(0);
    shifted.insert(shifted.end(), ids.begin(), ids.end());
    GroundTruth gt = compute_ground_truth(shifted, d);
    for (auto& [f, v] : got) agg[{f, vbin_of(v, d.delta)}]++;

    bool exact = got == want;
    std::printf("[%s] CM_BITS=%d packets=%zu digests rtl=%zu model=%zu identical=%s", label.c_str(), CM_BITS,
                ids.size(), got.size(), want.size(), exact ? "yes" : "NO");
    for (int K : {100, 500, 1000}) std::printf("  F1@%d=%.3f", K, compare_top(agg, gt.onset, K).f1);
    std::printf("\n");
    if (!out.empty()) {
        FILE* f = std::fopen(out.c_str(), "w");
        std::fprintf(f, "label,bits,packets,digests_rtl,digests_model,identical,K,F1,ARE\n");
        for (int K : {20, 100, 200, 500, 1000}) {
            Perf p = compare_top(agg, gt.onset, K);
            std::fprintf(f, "%s,%d,%zu,%zu,%zu,%d,%d,%.6f,%.6f\n", label.c_str(), CM_BITS, ids.size(), got.size(),
                         want.size(), exact ? 1 : 0, K, p.f1, p.are);
        }
        std::fclose(f);
    }
    return exact ? 0 : 1;
}
