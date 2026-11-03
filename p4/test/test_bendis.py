"""PTF functional test for bendis.p4 on tofino-model.

Build with small count windows so the test can drive window boundaries with packet counts:

    bf-p4c -DWINDOW_BY_COUNT -DWIN_COUNT_BITS=8 -DCM_IDX_BITS=6 -DTAB_IDX_BITS=6 -DBURST_H=20 \
        -o bendis.tofino bendis.p4

A window is 256 packets of this pipe (all packets are sent on one port). Background flows send a
few packets per window and never reach H; the periodic flow sends 40 packets in windows
2, 9, 16, 23 and nothing else. Expected digests: three, all for the periodic flow, interval 7
(the first onset only claims an onset-table entry).
"""
import ipaddress
import os
import sys

import ptf.testutils as testutils
from bfruntime_client_base_tests import BfRuntimeTest
import bfrt_grpc.client as gc

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cp"))
from onset_collector import StaleReset  # noqa: E402

P4_NAME = "bendis"
WINDOW = 256
IN_PORT = 0
OUT_PORT = 1
PERIODIC = ("10.0.0.1", "10.0.1.1")
BURST_WINDOWS = (2, 9, 16, 23)
BURST_PKTS = 40
N_WINDOWS = 26
N_BACKGROUND = 60


def pkt(src, dst):
    return testutils.simple_udp_packet(ip_src=src, ip_dst=dst, udp_sport=1234, udp_dport=80)


def window_schedule(first=0, last=N_WINDOWS):
    """Yields (src, dst) for every packet, window by window, exactly WINDOW packets per window."""
    background = [(f"10.1.{i // 250}.{i % 250 + 1}", "10.2.0.1") for i in range(N_BACKGROUND)]
    for w in range(first, last):
        flows = [PERIODIC] * (BURST_PKTS if w in BURST_WINDOWS else 0)
        i = 0
        while len(flows) < WINDOW:
            flows.append(background[i % N_BACKGROUND])
            i += 1
        # Interleave the burst with background traffic.
        flows.sort(key=lambda f: hash((f, w)) & 0xFFFF)
        for f in flows:
            yield f


class PeriodicOnsetTest(BfRuntimeTest):
    def setUp(self):
        BfRuntimeTest.setUp(self, 0, P4_NAME)
        self.bfrt_info = self.interface.bfrt_info_get(P4_NAME)
        self.target = gc.Target(device_id=0, pipe_id=0xFFFF)
        fwd = self.bfrt_info.table_get("pipe.SwitchIngress.forward")
        fwd.entry_add(
            self.target,
            [fwd.make_key([gc.KeyTuple("ig_intr_md.ingress_port", IN_PORT)])],
            [fwd.make_data([gc.DataTuple("port", OUT_PORT)], "SwitchIngress.set_port")],
        )
        self.learn = self.bfrt_info.learn_get("pipe.SwitchIngressDeparser.onset_digest")

    def runTest(self):
        for src, dst in window_schedule():
            testutils.send_packet(self, IN_PORT, pkt(src, dst))

        digests = []
        while True:
            try:
                d = self.interface.digest_get(timeout=2)
            except Exception:
                break
            digests += [x.to_dict() for x in self.learn.make_data_list(d)]

        src = int(ipaddress.IPv4Address(PERIODIC[0]))
        dst = int(ipaddress.IPv4Address(PERIODIC[1]))
        got = [(x["src_addr"], x["dst_addr"], x["interval"]) for x in digests]
        expected = [(src, dst, 7)] * (len(BURST_WINDOWS) - 1)
        self.assertEqual(sorted(got), expected, f"digests: {got}")

    def tearDown(self):
        self.bfrt_info.table_get("pipe.SwitchIngress.forward").entry_del(self.target)
        BfRuntimeTest.tearDown(self)


class StaleResetTest(BfRuntimeTest):
    """The control plane's StaleReset: an entry that does not change between two scans is reset to the
    unused state, so the next onset of its flow is a first onset (no digest) and the one after reports
    the interval again."""

    def setUp(self):
        BfRuntimeTest.setUp(self, 0, P4_NAME)
        self.bfrt_info = self.interface.bfrt_info_get(P4_NAME)
        self.target = gc.Target(device_id=0, pipe_id=0xFFFF)
        fwd = self.bfrt_info.table_get("pipe.SwitchIngress.forward")
        fwd.entry_add(
            self.target,
            [fwd.make_key([gc.KeyTuple("ig_intr_md.ingress_port", IN_PORT)])],
            [fwd.make_data([gc.DataTuple("port", OUT_PORT)], "SwitchIngress.set_port")],
        )
        self.learn = self.bfrt_info.learn_get("pipe.SwitchIngressDeparser.onset_digest")
        self.reset = StaleReset(self.bfrt_info, pipes=1)

    def digests(self):
        got = []
        while True:
            try:
                d = self.interface.digest_get(timeout=2)
            except Exception:
                return got
            got += [(x["src_addr"], x["dst_addr"], x["interval"]) for x in
                    (y.to_dict() for y in self.learn.make_data_list(d))]

    def runTest(self):
        flow = (int(ipaddress.IPv4Address(PERIODIC[0])), int(ipaddress.IPv4Address(PERIODIC[1])))
        self.reset.scan()
        self.reset.scan()  # clears the entries left by earlier tests
        self.digests()
        for src, dst in window_schedule(0, 10):  # bursts in windows 2 and 9
            testutils.send_packet(self, IN_PORT, pkt(src, dst))
        self.assertEqual(self.digests(), [flow + (7,)])

        self.reset.scan()
        n = self.reset.scan()  # nothing changed in between: every used entry is reset
        self.assertGreater(n, 0)
        for name, table in self.reset.tables:
            entries = self.reset.read(name, table, self.reset.targets[0][1])
            self.assertTrue(all(e == StaleReset.UNUSED for e in entries.values()))

        for src, dst in window_schedule(10, 24):  # bursts in windows 16 (first onset again) and 23
            testutils.send_packet(self, IN_PORT, pkt(src, dst))
        self.assertEqual(self.digests(), [flow + (7,)])

    def tearDown(self):
        self.bfrt_info.table_get("pipe.SwitchIngress.forward").entry_del(self.target)
        BfRuntimeTest.tearDown(self)
