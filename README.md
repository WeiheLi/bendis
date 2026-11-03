# Bendis

Source code and technical report of the paper

> **Bendis: Accurate and Hardware-Friendly Detection of Periodic Bursts in High-Speed Data Streams**
> Weihe Li, Xicheng Li, Dimitrios Pezaros, and Paul Patras. Submitted to PVLDB.

Bendis is a sketch that detects *periodic bursts* in a data stream, i.e., sharp rises in the arrivals of
an item (such as a network flow or a database query) that recur at nearly regular intervals. Since the
period of bursts is determined by when they start, Bendis detects only the window in which each burst
starts, using three rotating banks of conservatively updated counters, and keeps the last burst start of
each candidate item in a table whose entries are reclaimed only after a bounded number of windows
without a burst start. Each arrival accesses every memory array once and needs only hashes, additions,
shifts, and comparisons, so the same design runs on a CPU, a Tofino switch, and an FPGA. The data plane
reports `<item, interval>` pairs, and the control plane counts them exactly and answers top-K queries.

[`technical_report.pdf`](technical_report.pdf) contains the proofs of the error bounds, the details of
the experimental setup and of the Tofino and FPGA implementations, and additional results.

*Terminology.* The code follows the networking setting of the implementations: an item is a *flow*, an
arrival is a *packet*, and a burst start is called an *onset* (e.g., the onset test and the onset table
are the start test and the start table of the paper).

## Repository

| path | |
|---|---|
| `src/bendis.h` | C++ reference implementation (header only): data plane and control plane |
| `src/bendis_simd.h` | AVX-512 implementation, bit-exact with `src/bendis.h` |
| `src/example.cpp` | runs Bendis on a trace and prints the top-K `<item, interval bin>` pairs |
| `src/p4_model.h` | behavioral model of the P4 program (CRC hashes, count windows), shared by the P4 and FPGA tests |
| `src/groundtruth.h`, `src/pbo_common.h` | exact ground truth, trace readers, and hashing shared by the tools and tests |
| `src/gen_synth.cpp`, `src/pcap2tuple.cpp` | synthetic trace generator and pcap converter |
| `tests/` | equivalence checks: SIMD versus scalar, P4 model versus `src/bendis.h`, expected digests for replays |
| `p4/` | Tofino 1 data plane (P4_16, TNA), control plane, PTF tests, and SDE scripts ([`p4/README.md`](p4/README.md)) |
| `fpga/` | FPGA data plane (Verilog), Verilator testbench, and synthesis flow ([`fpga/README.md`](fpga/README.md)) |

## CPU

Requirements: a C++17 compiler (GCC or Clang); an x86-64 CPU with AVX-512 (F, CD, VL, BW, DQ) only for `src/bendis_simd.h`, whose check `make` skips elsewhere.

```bash
make            # bin/example, bin/gen_synth, bin/pcap2tuple, bin/simd_check, bin/p4_equiv, bin/p4_expect
make test       # generates a synthetic trace and runs the example and the equivalence checks
```

`make test` writes a trace of 3M arrivals with planted periodic bursts to `data/synth.dat`, prints the
top pairs found by Bendis, and checks that the P4 behavioral model reports exactly what `src/bendis.h`
reports (`exact yes`) and that the AVX-512 version reports exactly what the scalar version reports
(`all identical`).

### Usage

```cpp
#include "bendis.h"

bendis::BendisConfig cfg;          // windows of W = 30,000 arrivals, H = 80, k = 2, R = 1, V = 65, d = 3, m = 2
cfg.mem_bytes = 32 * 1024;         // 70% counting sketch, 30% table (cfg.table_ratio)
bendis::Bendis sketch(cfg);        // data plane
bendis::ControlPlane cp(2);        // interval bins of 2 windows
for (uint64_t i = 0; i < n; ++i)   // arrival i of item ids[i]
    sketch.insert(ids[i], i, [&](uint64_t item, uint32_t v) { cp.report(item, v); });
auto top = cp.top(200);            // ((item, interval bin), frequency), most frequent first
```

`bendis::BendisSimd` (in `src/bendis_simd.h`) takes the same configuration and processes a batch of
arrivals per call, `insert(ids, i0, n, report)`, with exactly the same reports in the same order.

```bash
./bin/example trace [memory in KB = 32] [K = 20] [format = ids]
```

### Trace formats

| format | record (little-endian) | item |
|---|---|---|
| `ids` | `uint64` item ID | the ID |
| `pbs16` | `{uint64 timestamp, uint64 id}` (output of `gen_synth`, BurstSketch/PBSketch CAIDA format) | `id` |
| `tuple13` | `{srcIP, dstIP, srcPort, dstPort, proto}` | `srcIP << 32 \| dstIP` |
| `tuple21` | `tuple13` followed by a `uint64` timestamp in ns (output of `pcap2tuple`) | `srcIP << 32 \| dstIP` |

```bash
./bin/gen_synth --packets 30000000 --alpha 1.0 --out synth.dat   # planted periodic bursts, Zipf background
./bin/pcap2tuple trace.pcap trace.t21                            # IPv4 packets of a pcap trace
```

## Tofino

`p4/bendis.p4` implements the data plane for Tofino 1 with the Tofino Native Architecture: it occupies
the 12 ingress stages, accesses every register array once per packet, needs no recirculation, and sends
each `<item, interval>` pair to the control plane as a digest. `p4/cp/onset_collector.py` receives the
digests, counts the pairs exactly, answers top-K queries, and periodically resets stale table entries.
The program compiles with `bf-p4c` 9.13.4 of the open-source P4 Studio SDE. See
[`p4/README.md`](p4/README.md) for the compile-time parameters, the stage layout, the PTF tests on
tofino-model, and the replay of a trace against the behavioral model.

## FPGA

`fpga/rtl/bendis.v` implements the same data plane as a pipeline that accepts one arrival per clock
cycle. Its testbench streams a trace through the RTL with Verilator and checks that it emits exactly the
digests of the behavioral model; the flow synthesizes and places the design on a Lattice ECP5-85F with
Yosys and nextpnr. See [`fpga/README.md`](fpga/README.md).

```bash
cd fpga && make sim BITS=11 && ./build/tb_b11/Vbendis trace.t21   # needs verilator
make ecp5 BITS=11 FREQ=90                                         # needs yosys and nextpnr-ecp5
```
