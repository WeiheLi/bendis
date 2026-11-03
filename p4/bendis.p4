/* -*- P4_16 -*- */
// Bendis: periodic burst detection in the data plane of Tofino 1 (TNA).
//
// Per packet, in one pass with forward-only data flow:
//   1. window number and bank rotation;
//   2. per-window flow counts in a 3-row count-min sketch whose rows each have three 16-bit
//      banks rotating between "counting", "previous window" and "being cleared", updated with
//      pipelined conservative update;
//   3. onset test: c >= BURST_H and c >= k * p (c = this window, p = previous window);
//   4. on an onset, a 2-row table of {fingerprint, last onset window} gives the interval
//      v to the flow's previous onset, and (src, dst, v) is sent to the control plane by digest
//      when MIN_GAP_R < v <= VMAX. The control plane counts (flow, v / delta), reports the top-K.
// See src/bendis.h for the reference model and README.md for the design.

#include <core.p4>
#include <tna.p4>

/*************************************************************************
 *  Parameters (override with -D at compile time)
 *************************************************************************/

// Counting sketch: 3 rows x 3 banks x 2^CM_IDX_BITS x 16 bit.
// Onset table: 2 rows x 2^TAB_IDX_BITS x 32 bit.
// Equal bit widths give a ~70/30 split, e.g. 11/11 -> 36 KB + 16 KB.
#ifndef CM_IDX_BITS
#define CM_IDX_BITS 11
#endif
#ifndef TAB_IDX_BITS
#define TAB_IDX_BITS 11
#endif

// Burst threshold H and burst ratio k = 2^BURST_K_SHIFT. (A ratio that is not a power of two
// needs k * p as a short shift-and-add, e.g. 3p = (p << 1) + p.)
#ifndef BURST_H
#define BURST_H 80
#endif
#ifndef BURST_K_SHIFT
#define BURST_K_SHIFT 1
#endif

// Intervals v are reported iff MIN_GAP_R < v <= VMAX: shorter ones are multi-window ramps, and an
// onset-table entry older than VMAX windows may be claimed by another flow. VMAX - MIN_GAP_R must
// be 2^REPORT_BITS, so the range test is a slice test.
#ifndef MIN_GAP_R
#define MIN_GAP_R 1
#endif
#ifndef REPORT_BITS
#define REPORT_BITS 6
#endif
#define VMAX (MIN_GAP_R + (1 << REPORT_BITS))

// Windows: by default 2^WIN_SHIFT ns of ingress MAC timestamp; with -DWINDOW_BY_COUNT,
// 2^WIN_COUNT_BITS packets of this pipe. The banks are cleared one entry per packet, so a window
// must carry at least 2^CM_IDX_BITS packets. Timestamp windows keep WIN_FLAG and wrap modulo 2^15,
// which the control plane handles (StaleReset). Count windows, meant for tests and trace replays,
// compute WIN_FLAG + window in 16 bits and thus support at most 2^15 windows (WIN_FLAG is lost after
// that); the FPGA design keeps WIN_FLAG instead (fpga/rtl/bendis.v).
#ifndef WIN_SHIFT
#define WIN_SHIFT 23
#endif
#ifndef WIN_COUNT_BITS
#define WIN_COUNT_BITS 15
#endif

const bit<16> WIN_FLAG = 0x8000;  // window numbers carry bit 15, so stored windows are never 0 or 1
const bit<16> TAB_PASSED = 0;     // onset-table return value of a pass
const DigestType_t ONSET_DIGEST = 1;

typedef bit<16> ether_type_t;
const ether_type_t ETHERTYPE_IPV4 = 16w0x0800;

/*************************************************************************
 *  Headers and metadata
 *************************************************************************/

header ethernet_h {
    bit<48> dst_addr;
    bit<48> src_addr;
    ether_type_t ether_type;
}

header ipv4_h {
    bit<4>  version;
    bit<4>  ihl;
    bit<8>  diffserv;
    bit<16> total_len;
    bit<16> identification;
    bit<3>  flags;
    bit<13> frag_offset;
    bit<8>  ttl;
    bit<8>  protocol;
    bit<16> hdr_checksum;
    bit<32> src_addr;
    bit<32> dst_addr;
}

header udp_h {
    bit<16> src_port;
    bit<16> dst_port;
    bit<16> len;
    bit<16> checksum;
}

struct ig_headers_t {
    ethernet_h ethernet;
    ipv4_h     ipv4;
    udp_h      udp;
}

struct ig_metadata_t {
    bit<16> win;   // current window: WIN_FLAG + window number (16-bit arithmetic)
    bit<16> winr;  // win - (MIN_GAP_R + 1)
    bit<16> rot;   // bank rotation: bank rot counts, (rot+2)%3 is the previous window, (rot+1)%3 is cleared
    bit<16> z;     // packet counter; its low CM_IDX_BITS bits address the entry being cleared
    bit<CM_IDX_BITS> idx0;
    bit<CM_IDX_BITS> idx1;
    bit<CM_IDX_BITS> idx2;
    bit<16> c0;    // counting-bank values after the (conservative) update
    bit<16> c1;
    bit<16> c2;
    bit<16> p0;    // previous-window bank values
    bit<16> p1;
    bit<16> p2;
    bit<16> c01;
    bit<16> p01;
    bit<16> c;     // min of c0..c2
    bit<16> p;     // min of p0..p2
    bit<16> ck;    // c >> BURST_K_SHIFT = floor(c / k)
    bit<16> hd;    // H - c, saturating: zero iff c >= H
    bit<16> deficit;  // p - ck, saturating: zero iff p <= floor(c / k) iff c >= k * p
    bit<TAB_IDX_BITS> tidx0;
    bit<TAB_IDX_BITS> tidx1;
    bit<16> fp;
    bit<16> old0;  // row-0 result: old window (match or claim), 0 (pass), or 1 (no onset)
    bit<16> old1;
    bit<16> v;     // win - old: interval to the previous onset, in windows
    bit<16> vr;    // v - (MIN_GAP_R + 1): reported iff vr < 2^REPORT_BITS
}

struct onset_digest_t {
    bit<32> src_addr;
    bit<32> dst_addr;
#ifdef ML_FLOWKEY
    bit<16> src_port;
#endif
#ifdef DIGEST_WINDOW
    bit<16> window;
#endif
    bit<16> interval;
}

// Flow key: (src, dst) by default; with ML_FLOWKEY also the UDP source port, which distinguishes the
// queue pairs of RoCEv2 traffic.
#ifdef ML_FLOWKEY
#define FLOW_KEY {hdr.ipv4.src_addr, hdr.ipv4.dst_addr, hdr.udp.src_port}
#else
#define FLOW_KEY {hdr.ipv4.src_addr, hdr.ipv4.dst_addr}
#endif

struct pair16_t {
    bit<16> lo;
    bit<16> hi;
}

/*************************************************************************
 *  Ingress parser
 *************************************************************************/

parser SwitchIngressParser(packet_in pkt,
                           out ig_headers_t hdr,
                           out ig_metadata_t meta,
                           out ingress_intrinsic_metadata_t ig_intr_md) {
    state start {
        pkt.extract(ig_intr_md);
        pkt.advance(PORT_METADATA_SIZE);
        meta.v = 0;
        meta.old0 = 1;  // no onset: no table access, and old0 != 0 keeps row 1 idle
        transition parse_ethernet;
    }

    state parse_ethernet {
        pkt.extract(hdr.ethernet);
        transition select(hdr.ethernet.ether_type) {
            ETHERTYPE_IPV4: parse_ipv4;
            default: accept;
        }
    }

    state parse_ipv4 {
        pkt.extract(hdr.ipv4);
#ifdef ML_FLOWKEY
        transition select(hdr.ipv4.protocol) {
            17: parse_udp;
            default: accept;
        }
    }

    state parse_udp {
        pkt.extract(hdr.udp);
#endif
        transition accept;
    }
}

/*************************************************************************
 *  Ingress
 *************************************************************************/

// One bank of one CM row: count (row 0 plain, later rows conservative), read, or clear.
// ROW_CU_COND is the conservative-update condition for this row.
#define CM_BANK(R, B, ROW_CU_COND)                                                              \
    Register<bit<16>, bit<CM_IDX_BITS>>(1 << CM_IDX_BITS, 0) cm_r##R##_b##B;                   \
    RegisterAction<bit<16>, bit<CM_IDX_BITS>, bit<16>>(cm_r##R##_b##B) cm_r##R##_b##B##_count = { \
        void apply(inout bit<16> value, out bit<16> rv) {                                       \
            if (ROW_CU_COND) {                                                                  \
                value = value |+| 1;                                                            \
            }                                                                                   \
            rv = value;                                                                         \
        }                                                                                       \
    };                                                                                          \
    RegisterAction<bit<16>, bit<CM_IDX_BITS>, bit<16>>(cm_r##R##_b##B) cm_r##R##_b##B##_read = { \
        void apply(inout bit<16> value, out bit<16> rv) {                                       \
            rv = value;                                                                         \
        }                                                                                       \
    };                                                                                          \
    RegisterAction<bit<16>, bit<CM_IDX_BITS>, bit<16>>(cm_r##R##_b##B) cm_r##R##_b##B##_clear = { \
        void apply(inout bit<16> value) {                                                       \
            value = 0;                                                                          \
        }                                                                                       \
    };

// One CM row. Each register access is its own action (Tofino drives one stateful address per
// table), and the rotation selects the actions in mutually exclusive branches:
// count in bank rot, read bank (rot+2)%3, clear bank (rot+1)%3.
#define CM_ROW_ACTIONS(R, B)                                                                    \
    action cm_r##R##_b##B##_do_count() { meta.c##R = cm_r##R##_b##B##_count.execute(meta.idx##R); } \
    action cm_r##R##_b##B##_do_read() { meta.p##R = cm_r##R##_b##B##_read.execute(meta.idx##R); }   \
    action cm_r##R##_b##B##_do_clear() {                                                        \
        cm_r##R##_b##B##_clear.execute(meta.z[CM_IDX_BITS - 1:0]);                              \
    }

// Every packet zeroes one entry of the clearing bank, so packets of the packet generator (which
// are not IPv4) sweep the bank even when no traffic arrives; only IPv4 packets count and read.
#define CM_ROW_APPLY(R)                                                                         \
    if (meta.rot == 0) {                                                                        \
        if (hdr.ipv4.isValid()) { cm_r##R##_b0_do_count(); cm_r##R##_b2_do_read(); }            \
        cm_r##R##_b1_do_clear();                                                                \
    } else if (meta.rot == 1) {                                                                 \
        if (hdr.ipv4.isValid()) { cm_r##R##_b1_do_count(); cm_r##R##_b0_do_read(); }            \
        cm_r##R##_b2_do_clear();                                                                \
    } else {                                                                                    \
        if (hdr.ipv4.isValid()) { cm_r##R##_b2_do_count(); cm_r##R##_b1_do_read(); }            \
        cm_r##R##_b0_do_clear();                                                                \
    }

// One onset-table row of {fingerprint, last onset window}. A match or a stale entry (last onset
// more than VMAX windows ago) both store {fp, win} and return the old window; otherwise the row
// passes (returns 0). Tofino 1 gives a register action a single output source, so both cases share
// one branch; storing fp again on a match is a no-op. Entries start at window 1, which never equals
// a WIN_FLAG window, so claiming an unused entry returns 1 rather than 0.
#define ONSET_ROW(R)                                                                            \
    Register<pair16_t, bit<TAB_IDX_BITS>>(1 << TAB_IDX_BITS, {0, 1}) otab##R;                  \
    RegisterAction<pair16_t, bit<TAB_IDX_BITS>, bit<16>>(otab##R) otab##R##_access = {        \
        void apply(inout pair16_t e, out bit<16> rv) {                                          \
            if (e.lo == meta.fp || meta.win - e.hi > VMAX) {                                    \
                rv = e.hi;                                                                      \
                e.lo = meta.fp;                                                                 \
                e.hi = meta.win;                                                                \
            }                                                                                   \
        }                                                                                       \
    };                                                                                          \
    action otab##R##_lookup() {                                                                 \
        meta.old##R = otab##R##_access.execute(meta.tidx##R);                                   \
    }

control SwitchIngress(inout ig_headers_t hdr,
                      inout ig_metadata_t meta,
                      in ingress_intrinsic_metadata_t ig_intr_md,
                      in ingress_intrinsic_metadata_from_parser_t ig_prsr_md,
                      inout ingress_intrinsic_metadata_for_deparser_t ig_dprsr_md,
                      inout ingress_intrinsic_metadata_for_tm_t ig_tm_md) {

    // ---------------- forwarding (plain port mapping) ----------------
    action set_port(PortId_t port) {
        ig_tm_md.ucast_egress_port = port;
    }
    action drop() {
        ig_dprsr_md.drop_ctl = 1;
    }
    table forward {
        key = { ig_intr_md.ingress_port : exact; }
        actions = { set_port; drop; NoAction; }
        default_action = NoAction();
        size = 512;
    }

    // ---------------- window, clearing cursor, bank rotation ----------------
    // Packet counter; its low bits address the entry of the clearing banks zeroed by this packet.
    Register<bit<16>, bit<1>>(1, 0) pkt_counter;
    RegisterAction<bit<16>, bit<1>, bit<16>>(pkt_counter) pkt_counter_next = {
        void apply(inout bit<16> value, out bit<16> rv) {
            rv = value;
            value = value + 1;
        }
    };
    action count_packet() {
        meta.z = pkt_counter_next.execute(0);
    }

#ifdef WINDOW_BY_COUNT
    // {lo: packets seen in this window, hi: window}: returns the window of this packet.
    Register<pair16_t, bit<1>>(1, {0, WIN_FLAG}) count_window;
    RegisterAction<pair16_t, bit<1>, bit<16>>(count_window) count_window_next = {
        void apply(inout pair16_t s, out bit<16> rv) {
            rv = s.hi;
            if (s.lo == (1 << WIN_COUNT_BITS) - 1) {
                s.lo = 0;
                s.hi = s.hi + 1;
            } else {
                s.lo = s.lo + 1;
            }
        }
    };
    action set_window() {
        meta.win = count_window_next.execute(0);
    }
#else
    // An identity hash extracts the timestamp bits, so the window needs no PHV realignment.
    Hash<bit<16>>(HashAlgorithm_t.IDENTITY) h_win;
    action set_window() {
        meta.win = h_win.get({1w1, ig_intr_md.ingress_mac_tstamp[WIN_SHIFT + 14:WIN_SHIFT]});
    }
#endif

    // {lo: last window seen, hi: rotation}; advances the rotation on the first packet of a window.
    Register<pair16_t, bit<1>>(1, {0, 0}) rotation;
    RegisterAction<pair16_t, bit<1>, bit<16>>(rotation) rotation_advance = {
        void apply(inout pair16_t s, out bit<16> rv) {
            if (s.lo != meta.win) {
                if (s.hi == 2) {
                    s.hi = 0;
                } else {
                    s.hi = s.hi + 1;
                }
                s.lo = meta.win;
            }
            rv = s.hi;
        }
    };
    action advance_rotation() {
        meta.rot = rotation_advance.execute(0);
    }
    action report_base() {
        meta.winr = meta.win - (MIN_GAP_R + 1);
    }

    // ---------------- hashes of the flow key (src, dst) ----------------
    CRCPolynomial<bit<32>>(0x04C11DB7, true, false, false, 0xFFFFFFFF, 0xFFFFFFFF) crc32;
    CRCPolynomial<bit<32>>(0x1EDC6F41, true, false, false, 0xFFFFFFFF, 0xFFFFFFFF) crc32c;
    CRCPolynomial<bit<32>>(0x741B8CD7, true, false, false, 0xFFFFFFFF, 0xFFFFFFFF) crc32k;
    CRCPolynomial<bit<32>>(0x814141AB, false, false, false, 0x00000000, 0x00000000) crc32q;
    CRCPolynomial<bit<32>>(0xA833982B, true, false, false, 0xFFFFFFFF, 0xFFFFFFFF) crc32d;
    CRCPolynomial<bit<16>>(0x8005, true, false, false, 0x0000, 0x0000) crc16;
    Hash<bit<32>>(HashAlgorithm_t.CUSTOM, crc32) h_cm0;
    Hash<bit<32>>(HashAlgorithm_t.CUSTOM, crc32c) h_cm1;
    Hash<bit<32>>(HashAlgorithm_t.CUSTOM, crc32k) h_cm2;
    Hash<bit<32>>(HashAlgorithm_t.CUSTOM, crc32q) h_tab0;
    Hash<bit<32>>(HashAlgorithm_t.CUSTOM, crc32d) h_tab1;
    Hash<bit<16>>(HashAlgorithm_t.CUSTOM, crc16) h_fp;

    // One hash per action: an action moves at most 32 bits of hash output.
    action hash_cm0() { meta.idx0 = h_cm0.get(FLOW_KEY)[CM_IDX_BITS - 1:0]; }
    action hash_cm1() { meta.idx1 = h_cm1.get(FLOW_KEY)[CM_IDX_BITS - 1:0]; }
    action hash_cm2() { meta.idx2 = h_cm2.get(FLOW_KEY)[CM_IDX_BITS - 1:0]; }
    action hash_tab0() { meta.tidx0 = h_tab0.get(FLOW_KEY)[TAB_IDX_BITS - 1:0]; }
    action hash_tab1() { meta.tidx1 = h_tab1.get(FLOW_KEY)[TAB_IDX_BITS - 1:0]; }
    action hash_fp() { meta.fp = h_fp.get(FLOW_KEY); }
    action fix_zero_fp() {
        meta.fp = 1;  // fingerprint 0 marks an empty entry
    }

    // ---------------- counting sketch ----------------
    // Row 0 always increments; row 1 only below row 0's value; row 2 only below both.
    CM_BANK(0, 0, true)
    CM_BANK(0, 1, true)
    CM_BANK(0, 2, true)
    CM_BANK(1, 0, value < meta.c0)
    CM_BANK(1, 1, value < meta.c0)
    CM_BANK(1, 2, value < meta.c0)
    CM_BANK(2, 0, value < meta.c0 && value < meta.c1)
    CM_BANK(2, 1, value < meta.c0 && value < meta.c1)
    CM_BANK(2, 2, value < meta.c0 && value < meta.c1)
    CM_ROW_ACTIONS(0, 0) CM_ROW_ACTIONS(0, 1) CM_ROW_ACTIONS(0, 2)
    CM_ROW_ACTIONS(1, 0) CM_ROW_ACTIONS(1, 1) CM_ROW_ACTIONS(1, 2)
    CM_ROW_ACTIONS(2, 0) CM_ROW_ACTIONS(2, 1) CM_ROW_ACTIONS(2, 2)

    action min_01() {
        meta.c01 = min(meta.c0, meta.c1);
        meta.p01 = min(meta.p0, meta.p1);
    }
    action min_012() {
        meta.c = min(meta.c01, meta.c2);
        meta.p = min(meta.p01, meta.p2);
    }
    action start_terms() {
        meta.ck = meta.c >> BURST_K_SHIFT;
        meta.hd = BURST_H |-| meta.c;
    }
    action start_deficit() {
        meta.deficit = meta.p |-| meta.ck;
    }

    // ---------------- onset table and report ----------------
    ONSET_ROW(0)
    ONSET_ROW(1)

    action interval_from_row0() {
        meta.v = meta.win - meta.old0;
        meta.vr = meta.winr - meta.old0;
    }
    action interval_from_row1() {
        meta.v = meta.win - meta.old1;
        meta.vr = meta.winr - meta.old1;
    }
    action report_start() {
        ig_dprsr_md.digest_type = ONSET_DIGEST;
    }

    apply {
        forward.apply();
        count_packet();
        set_window();
        advance_rotation();
        report_base();
        if (hdr.ipv4.isValid()) {
            hash_cm0();
            hash_cm1();
            hash_cm2();
            hash_tab0();
            hash_tab1();
            hash_fp();
            if (meta.fp == 0) {
                fix_zero_fp();
            }
        }
        CM_ROW_APPLY(0)
        CM_ROW_APPLY(1)
        CM_ROW_APPLY(2)
        if (hdr.ipv4.isValid()) {
            min_01();
            min_012();
            start_terms();
            start_deficit();
            if (meta.hd == 0 && meta.deficit == 0) {
                otab0_lookup();
            }
            if (meta.old0 == TAB_PASSED) {
                otab1_lookup();
                interval_from_row1();
            } else {
                interval_from_row0();
            }
            // Report iff MIN_GAP_R < v <= VMAX: v = 0 is a later packet of the same window, a
            // claim returns a window older than VMAX (or 1 for an unused entry), and so does a
            // stale match; no onset leaves old0 = 1.
            if (meta.vr[15:REPORT_BITS] == 0) {
                report_start();
            }
        }
    }
}

/*************************************************************************
 *  Ingress deparser
 *************************************************************************/

control SwitchIngressDeparser(packet_out pkt,
                              inout ig_headers_t hdr,
                              in ig_metadata_t meta,
                              in ingress_intrinsic_metadata_for_deparser_t ig_dprsr_md) {
    Digest<onset_digest_t>(ONSET_DIGEST) onset_digest;

    apply {
        if (ig_dprsr_md.digest_type == ONSET_DIGEST) {
            onset_digest.pack({hdr.ipv4.src_addr, hdr.ipv4.dst_addr,
#ifdef ML_FLOWKEY
                               hdr.udp.src_port,
#endif
#ifdef DIGEST_WINDOW
                               meta.win,
#endif
                               meta.v});
        }
        pkt.emit(hdr);
    }
}

/*************************************************************************
 *  Egress (pass-through)
 *************************************************************************/

struct eg_headers_t {}
struct eg_metadata_t {}

parser SwitchEgressParser(packet_in pkt,
                          out eg_headers_t hdr,
                          out eg_metadata_t meta,
                          out egress_intrinsic_metadata_t eg_intr_md) {
    state start {
        pkt.extract(eg_intr_md);
        transition accept;
    }
}

control SwitchEgress(inout eg_headers_t hdr,
                     inout eg_metadata_t meta,
                     in egress_intrinsic_metadata_t eg_intr_md,
                     in egress_intrinsic_metadata_from_parser_t eg_prsr_md,
                     inout egress_intrinsic_metadata_for_deparser_t eg_dprsr_md,
                     inout egress_intrinsic_metadata_for_output_port_t eg_oport_md) {
    apply {}
}

control SwitchEgressDeparser(packet_out pkt,
                             inout eg_headers_t hdr,
                             in eg_metadata_t meta,
                             in egress_intrinsic_metadata_for_deparser_t eg_dprsr_md) {
    apply {
        pkt.emit(hdr);
    }
}

Pipeline(SwitchIngressParser(),
         SwitchIngress(),
         SwitchIngressDeparser(),
         SwitchEgressParser(),
         SwitchEgress(),
         SwitchEgressDeparser()) pipe;

Switch(pipe) main;
