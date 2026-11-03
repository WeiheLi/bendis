# Bendis on Tofino 1 (TNA)

P4_16 implementation of Bendis for the Tofino Native Architecture; `src/bendis.h` is the reference
model used in all simulations.

| file | |
|---|---|
| `bendis.p4` | data plane |
| `cp/onset_collector.py` | control plane: digest receiver, exact `(flow, interval // delta)` counts, top-K, periodic reset of stale onset-table entries |
| `test/test_bendis.py` | PTF functional tests on tofino-model (small count windows): periodic onsets, stale-entry reset |
| `test/test_replay.py` | PTF replay of a CAIDA segment, driven by `sde/replay.sh` |
| `sde/` | SDE installation, build, PTF, replay, and resource scripts |

## Usage

```bash
p4/sde/install_sde.sh ~/open-p4studio      # open-p4studio: bf-p4c, tofino-model, bf_switchd (1-3 hours)
source ~/setup-open-p4studio.bash
p4/sde/build.sh                            # compile with the defaults below and install
p4/sde/resources.sh                        # resources for w = 2^9..2^13 -> results/tofino/
p4/sde/build.sh test && p4/sde/run_ptf.sh  # functional test on tofino-model (needs sudo for veths)
make bin/p4_expect && p4/sde/replay.sh trace.t21   # 262,144 packets on tofino-model vs. the behavioral model
```

## Parameters (`-D` at compile time)

| macro | default | meaning |
|---|---|---|
| `CM_IDX_BITS` | 11 | 2^bits entries per bank (3 rows x 3 banks x 16 bit) |
| `TAB_IDX_BITS` | 11 | 2^bits entries per onset-table row (2 rows x 32 bit) |
| `BURST_H` | 80 | burst threshold H |
| `BURST_K_SHIFT` | 1 | burst ratio k = 2^shift |
| `MIN_GAP_R` | 1 | intervals v are reported iff `MIN_GAP_R < v <= VMAX` |
| `REPORT_BITS` | 6 | `VMAX = MIN_GAP_R + 2^REPORT_BITS` (65): staleness bound and longest reported interval |
| `WIN_SHIFT` | 23 | time windows of 2^WIN_SHIFT ns (default ~8.4 ms) |
| `WINDOW_BY_COUNT` | off | count windows of 2^`WIN_COUNT_BITS` packets per pipe instead (for tests and replays; at most 2^15 windows) |
| `ML_FLOWKEY` | off | the item key also includes the UDP source port (distinguishes the queue pairs of RoCEv2 traffic) |
| `DIGEST_WINDOW` | off | digests also carry the window number |

SRAM = 18 B x 2^CM_IDX_BITS + 8 B x 2^TAB_IDX_BITS. Equal widths give a ~70/30 split:

| bits | 9 | 10 | 11 | 12 | 13 |
|---|---|---|---|---|---|
| total | 13 KB | 26 KB | 52 KB | 104 KB | 208 KB |

## Pipeline

Placement chosen by `bf-p4c` 9.13.4 (time windows, 52 KB); all 12 stages of Tofino 1, 13 stateful
ALUs, no recirculation:

| stage | work |
|---|---|
| 0 | packet counter (clearing cursor), window number (identity hash of the timestamp), forward |
| 1 | bank rotation register, `winr = win - R - 1`, CRC hashes, fingerprint |
| 2 | counting row 0: count bank `rot`, read bank `(rot+2)%3`, clear bank `(rot+1)%3` |
| 3 | counting row 1, conservative: increment iff `value < c0` |
| 4 | counting row 2, conservative: increment iff `value < c0 && value < c1`; `min(c0, c1)` |
| 5–7 | `c = min(...)`, `p = min(...)`; `ck = c >> k_shift`, `hd = H -| c`; `deficit = p -| ck` |
| 8 | onset table row 0 (onset iff `hd == 0 && deficit == 0`) |
| 9 | onset table row 1 (iff row 0 passed); interval from row 0 |
| 10 | interval from row 1 |
| 11 | digest iff `(winr - old)[15:REPORT_BITS] == 0` |

`p4/sde/resources.sh` writes the `bf-p4c` resource reports to `results/tofino/`; for 13–208 KB
(SRAM 2.81–3.02%, hash bits 3.2–4.2%, gateways 19.8%, stateful ALUs 27.1%, no TCAM).

Tofino 1 constraints that shape the code:

- A register action has a single output source, so an onset-table row returns the stored window
  for both a match and a stale entry (both write `{fp, win}`) and 0 for a pass. Entries start at
  window 1, which never equals a `WIN_FLAG` window, so claiming an unused entry returns 1.
- A table drives one stateful address, so every bank access is its own action, selected by
  mutually exclusive branches on the rotation value.
- A stateful ALU takes two PHV operands (`fp`, `win`): staleness is `win - e.hi > VMAX`.
- Gateways take 4 bytes + 12 bits, so the onset test is `hd == 0 && deficit == 0` and the report
  range test is a slice test, which needs `VMAX - MIN_GAP_R` to be a power of two.
- An action moves at most 32 bits of hash output: one hash per action.

## Differences from the simulator

- Sizes are powers of two and indexes come from CRC hashes (simulator: 64-bit hash, any size).
- The bank rotation advances once per observed window (`rotation` register) instead of `T % 3`;
  the two agree whenever every window carries packets, which the clearing already requires.
- Windows are time-based by default (simulator: count-based). Window numbers are 16-bit with
  `WIN_FLAG` added; count windows come from a `{packets in window, window}` register.

## Known limitations to handle in deployment

1. **Window wrap.** Window numbers wrap every 2^15 windows (~4.6 min at 8.4 ms), while the staleness
   test subtracts 16-bit values. `cp/onset_collector.py` therefore scans the onset table every
   `--reset-every` seconds (default 60, i.e., between VMAX and 2^14 windows) and resets every entry
   that did not change since the previous scan to window 1 (the unused state, always stale), so that no
   entry looks fresh again after a wrap; an interval that spans a wrap is lost.
2. **Clearing needs traffic.** A window must carry at least 2^CM_IDX_BITS packets for the clearing
   bank to be fully zeroed; at low load use count windows or a packet-generator sweep.
3. **Skipped windows.** If a whole window passes without packets, the rotation advances by one, so the
   "previous" bank holds an older window (p is overestimated: onsets may be missed, not invented).
4. **Per-pipe state.** Registers are per pipe; flows entering through different pipes are measured
   separately (the control plane can merge by key).
5. **Only IPv4 packets** update the sketch and advance clearing.
6. **Digest rate** equals the number of reported onsets; size `VMAX`/`H` for the CPU budget.

## Golden model, tofino-model, and FPGA

`src/p4_model.h` mirrors this program action by action (count windows). `tests/p4_equiv.cpp` checks
that, with the simulator's hash functions, every report equals the simulator's, and compares F1
with the program's CRC hashes on full traces.
`p4/sde/replay.sh` replays a trace segment through tofino-model and compares the digests with the
golden model. It starts the model with `-q`: with full logging (several GB
per 100K packets) the model falls behind the PTF sender and drops packets, which shows up as missing
digests. The test reads `pkt_counter` back at the end to confirm that every packet was processed.
The Verilog design in `fpga/` is checked against the same golden model on full CAIDA traces.

## Status

Compiles with `bf-p4c` 9.13.4 for Tofino 1 (open-p4studio).
On tofino-model, the first 262,144 packets of CAIDA 2019 (4,096-packet windows, 2^10 entries,
H = 10) produce 1,265 digests, all identical to the golden model.
