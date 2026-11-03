// Runs Bendis on a packet trace and prints the top-K periodic burst pairs <flow, interval bin>
// reported by the control plane, with the parameters of the paper (count windows of W = 30,000
// packets, H = 80, k = 2, R = 1, V = 65, d = 3, m = 2, 30% of the memory for the onset table,
// interval bins of delta = 2 windows).
//
// The trace is a binary file in one of these formats (src/pbo_common.h), one record per packet in
// arrival order: ids (little-endian uint64 flow IDs, e.g., srcIP << 32 | dstIP; the default), pbs16
// (output of gen_synth), tuple13, or tuple21 (output of pcap2tuple).
//   make bin/example
//   ./bin/example trace.bin [memory in KB = 32] [K = 20] [format = ids]
#include <cstdio>
#include <string>
#include <vector>

#include "bendis.h"
#include "pbo_common.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <trace> [memory in KB = 32] [K = 20] [format = ids|pbs16|tuple13|tuple21]\n", argv[0]);
        return 1;
    }
    bendis::BendisConfig cfg;
    cfg.mem_bytes = (argc > 2 ? std::stoul(argv[2]) : 32) * 1024;
    const size_t K = argc > 3 ? std::stoul(argv[3]) : 20;
    const double delta = 2;

    bendis::Bendis sketch(cfg);       // data plane
    bendis::ControlPlane cp(delta);   // exact aggregation of the reports
    const std::string fmt = argc > 4 ? argv[4] : "ids";
    std::vector<uint64_t> ids;
    if (fmt == "ids") {
        FILE* f = std::fopen(argv[1], "rb");
        if (!f) {
            std::perror(argv[1]);
            return 1;
        }
        std::vector<uint64_t> buf(1 << 16);
        for (size_t n; (n = std::fread(buf.data(), sizeof(uint64_t), buf.size(), f)) > 0;)
            ids.insert(ids.end(), buf.begin(), buf.begin() + n);
        std::fclose(f);
    } else {
        ids = pbo::load_ids(argv[1], fmt, UINT64_MAX);
    }
    uint64_t i = 0, reports = 0;
    for (; i < ids.size(); ++i)
        sketch.insert(ids[i], i, [&](uint64_t flow, uint32_t v) {
            cp.report(flow, v);
            ++reports;
        });

    std::printf("%llu packets, %llu reports; %zu counters per bank, %zu entries per onset-table row\n",
                (unsigned long long)i, (unsigned long long)reports, sketch.counters_per_row(),
                sketch.table_entries_per_row());
    std::printf("%4s  %-16s  %-18s  %s\n", "rank", "flow", "interval (windows)", "frequency");
    size_t rank = 0;
    for (const auto& [pair, n] : cp.top(K)) {
        const uint64_t lo = (uint64_t)(pair.second * delta), hi = (uint64_t)((pair.second + 1) * delta) - 1;
        std::printf("%4zu  %016llx  %-18s  %llu\n", ++rank, (unsigned long long)pair.first,
                    (std::to_string(lo) + "-" + std::to_string(hi)).c_str(), (unsigned long long)n);
    }
    return 0;
}
