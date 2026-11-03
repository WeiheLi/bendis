#!/usr/bin/env python3
"""Control plane for bendis.p4.

Receives onset digests (src, dst, interval), counts (flow, interval // delta) exactly, prints the
top-K periodically and writes all counts to a CSV on exit. It also resets stale onset-table entries
periodically (StaleReset) and optionally installs port mappings in the `forward` table. Uses the bfrt
gRPC client shipped with the Tofino SDE (bfrt_grpc).

Example:
    python3 onset_collector.py --port 0:1 --port 1:0 --delta 2 --topk 100 --out onsets.csv
"""
import argparse
import collections
import csv
import ipaddress
import signal
import time

import bfrt_grpc.client as gc


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--grpc", default="localhost:50052", help="bf_switchd gRPC address")
    ap.add_argument("--program", default="bendis")
    ap.add_argument("--delta", type=int, default=2, help="interval bin width in windows")
    ap.add_argument("--topk", type=int, default=100)
    ap.add_argument("--report-every", type=float, default=10.0, help="seconds between top-K prints")
    ap.add_argument("--out", default="", help="CSV with all (src, dst, bin, count) on exit")
    ap.add_argument("--port", action="append", default=[], metavar="IN:OUT",
                    help="forward packets from ingress port IN to egress port OUT (repeatable)")
    ap.add_argument("--reset-every", type=float, default=60.0,
                    help="seconds between stale-entry scans: more than V and at most 2^14 windows "
                         "(V = 65 windows is 0.55 s and 2^14 windows are 137 s with 2^23-ns windows)")
    ap.add_argument("--pipes", type=int, default=4, help="number of pipes of the device")
    return ap.parse_args()


class StaleReset:
    """Resets onset-table entries that did not change between two scans to the unused state
    {fingerprint 0, window 1}, which the data plane always treats as stale.

    Window numbers have 15 bits while the staleness test subtracts 16-bit values, so an entry left
    untouched for about 2^15 windows would look fresh again after a wrap. With a scan period of more
    than V and at most 2^14 windows, an entry that did not change since the previous scan is older
    than V (stale, so resetting it changes no decision), and every untouched entry is reset within
    two periods, before it can wrap. An entry updated between the read and the write of a scan is
    reset as well, which loses at most one interval of that flow.
    """

    UNUSED = (0, 1)

    def __init__(self, info, pipes, rows=2):
        self.tables = [(f"otab{r}", info.table_get(f"pipe.SwitchIngress.otab{r}")) for r in range(rows)]
        self.targets = [(p, gc.Target(device_id=0, pipe_id=p)) for p in range(pipes)]
        self.prev = {}

    def read(self, name, table, target):
        out = {}
        for data, key in table.entry_get(target, flags={"from_hw": True}):
            d, k = data.to_dict(), key.to_dict()
            lo, hi = d[f"SwitchIngress.{name}.lo"], d[f"SwitchIngress.{name}.hi"]
            out[k["$REGISTER_INDEX"]["value"]] = (lo[0] if isinstance(lo, list) else lo,
                                                  hi[0] if isinstance(hi, list) else hi)
        return out

    def scan(self):
        """One scan; returns the number of entries reset."""
        n = 0
        for name, table in self.tables:
            for pipe, target in self.targets:
                cur = self.read(name, table, target)
                prev = self.prev.get((name, pipe), {})
                stale = [i for i, e in cur.items() if e != self.UNUSED and prev.get(i) == e]
                if stale:
                    table.entry_mod(target, [table.make_key([gc.KeyTuple("$REGISTER_INDEX", i)]) for i in stale],
                                    [table.make_data([gc.DataTuple(f"SwitchIngress.{name}.lo", self.UNUSED[0]),
                                                      gc.DataTuple(f"SwitchIngress.{name}.hi", self.UNUSED[1])])
                                     for _ in stale])
                    for i in stale:
                        cur[i] = self.UNUSED
                    n += len(stale)
                self.prev[(name, pipe)] = cur
        return n


def install_ports(info, target, mappings):
    fwd = info.table_get("pipe.SwitchIngress.forward")
    for m in mappings:
        src, dst = (int(x) for x in m.split(":"))
        fwd.entry_add(
            target,
            [fwd.make_key([gc.KeyTuple("ig_intr_md.ingress_port", src)])],
            [fwd.make_data([gc.DataTuple("port", dst)], "SwitchIngress.set_port")],
        )


def print_top(counts, k, delta, n_digests):
    print(f"--- {time.strftime('%H:%M:%S')}  digests={n_digests}  pairs={len(counts)}")
    for (src, dst, b), r in counts.most_common(k):
        print(f"{ipaddress.IPv4Address(src)!s:>15} -> {ipaddress.IPv4Address(dst)!s:<15} "
              f"interval {b * delta}-{b * delta + delta - 1}  r={r}")


def main():
    args = parse_args()
    iface = gc.ClientInterface(args.grpc, client_id=0, device_id=0)
    iface.bind_pipeline_config(args.program)
    info = iface.bfrt_info_get(args.program)
    target = gc.Target(device_id=0, pipe_id=0xFFFF)
    install_ports(info, target, args.port)
    learn = info.learn_get("pipe.SwitchIngressDeparser.onset_digest")
    reset = StaleReset(info, args.pipes)

    counts = collections.Counter()
    n_digests = 0
    running = True

    def stop(*_):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    next_report = time.time() + args.report_every
    next_reset = time.time()
    while running:
        try:
            digest = iface.digest_get(timeout=1)
        except Exception:  # timeout: no digest in the last second
            digest = None
        if digest is not None:
            for data in learn.make_data_list(digest):
                f = data.to_dict()
                counts[(f["src_addr"], f["dst_addr"], f["interval"] // args.delta)] += 1
                n_digests += 1
        if time.time() >= next_report:
            print_top(counts, args.topk, args.delta, n_digests)
            next_report += args.report_every
        if time.time() >= next_reset:
            reset.scan()
            next_reset += args.reset_every

    print_top(counts, args.topk, args.delta, n_digests)
    if args.out:
        with open(args.out, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["src", "dst", "interval_bin", "count"])
            for (src, dst, b), r in counts.most_common():
                w.writerow([ipaddress.IPv4Address(src), ipaddress.IPv4Address(dst), b, r])


if __name__ == "__main__":
    main()
