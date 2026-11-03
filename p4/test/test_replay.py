"""PTF replay of a trace segment through bendis.p4 on tofino-model.

Sends the flows listed in the "ids" file (little-endian uint64 per packet: src << 32 | dst) as UDP
packets on port 0, collects every digest, and writes them to the "out" file as "src,dst,interval".
Both paths are read from the JSON file /tmp/bendis_replay.json (run_p4_tests.sh does not pass
the environment through to PTF).
The digests are compared offline with the P4 golden model (bin/p4_expect) by p4/sde/replay.sh.
Build the program with the same -D parameters as bin/p4_expect (count windows).
"""
import json
import struct
import time

import ptf.testutils as testutils
from bfruntime_client_base_tests import BfRuntimeTest
import bfrt_grpc.client as gc

P4_NAME = "bendis"
IN_PORT = 0
OUT_PORT = 1


def ip(x):
    return ".".join(str((x >> s) & 0xFF) for s in (24, 16, 8, 0))


class ReplayTest(BfRuntimeTest):
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

    def drain(self, out, timeout):
        while True:
            try:
                d = self.interface.digest_get(timeout=timeout)
            except Exception:
                return
            for x in self.learn.make_data_list(d):
                x = x.to_dict()
                out.append((x["src_addr"], x["dst_addr"], x["interval"]))

    def runTest(self):
        conf = json.load(open("/tmp/bendis_replay.json"))
        raw = open(conf["ids"], "rb").read()
        ids = struct.unpack("<%dQ" % (len(raw) // 8), raw)
        gap = float(conf.get("gap", 0.0005))
        digests = []
        t0 = time.time()
        for i, f in enumerate(ids):
            testutils.send_packet(self, IN_PORT, testutils.simple_udp_packet(
                ip_src=ip(f >> 32), ip_dst=ip(f & 0xFFFFFFFF), udp_sport=1234, udp_dport=80))
            if gap:
                time.sleep(gap)
            if i % 5000 == 4999:
                self.drain(digests, 0.01)
                print("sent %d packets, %d digests, %.0f s" % (i + 1, len(digests), time.time() - t0), flush=True)
        time.sleep(5)
        self.drain(digests, 5)
        with open(conf["out"], "w") as f:
            f.write("src,dst,interval\n")
            for s, d, v in digests:
                f.write("%d,%d,%d\n" % (s, d, v))
        # Packets the pipeline processed (pkt_counter wraps at 2^16).
        reg = self.bfrt_info.table_get("pipe.SwitchIngress.pkt_counter")
        for data, _ in reg.entry_get(self.target, [reg.make_key([gc.KeyTuple("$REGISTER_INDEX", 0)])],
                                     {"from_hw": True}):
            print("pkt_counter:", data.to_dict(), flush=True)
        print("replay done: %d packets, %d digests" % (len(ids), len(digests)), flush=True)

    def tearDown(self):
        self.bfrt_info.table_get("pipe.SwitchIngress.forward").entry_del(self.target)
        BfRuntimeTest.tearDown(self)
