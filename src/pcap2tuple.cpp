// Converts a pcap trace (Raw IP or Ethernet link type, us or ns timestamps) into 21-byte tuple21
// records {srcIP, dstIP, srcPort, dstPort, proto, uint64 timestamp_ns} for the --fmt tuple21 reader (src/pbo_common.h).
// IPv4 only; ports are zero for non-TCP/UDP packets and non-first fragments.
//   pcap2tuple in.pcap out.dat [max_packets]
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static uint32_t bswap32(uint32_t x) { return __builtin_bswap32(x); }
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s in.pcap out.dat [max_packets]\n", argv[0]);
        return 1;
    }
    uint64_t max_n = argc > 3 ? std::stoull(argv[3]) : UINT64_MAX;
    FILE* in = std::fopen(argv[1], "rb");
    FILE* out = std::fopen(argv[2], "wb");
    if (!in || !out) {
        std::perror("open");
        return 1;
    }

    uint8_t gh[24];
    if (std::fread(gh, 1, 24, in) != 24) {
        std::fprintf(stderr, "short pcap header\n");
        return 1;
    }
    uint32_t magic;
    std::memcpy(&magic, gh, 4);
    bool swap = false, nano = false;
    if (magic == 0xa1b2c3d4) {
    } else if (magic == 0xd4c3b2a1) {
        swap = true;
    } else if (magic == 0xa1b23c4d) {
        nano = true;
    } else if (magic == 0x4d3cb2a1) {
        swap = nano = true;
    } else {
        std::fprintf(stderr, "not a pcap file (pcapng is not supported)\n");
        return 1;
    }
    auto rd32 = [&](const uint8_t* p) {
        uint32_t v;
        std::memcpy(&v, p, 4);
        return swap ? bswap32(v) : v;
    };
    uint32_t link = rd32(gh + 20) & 0x0fffffff;
    size_t l2;
    if (link == 1) l2 = 14;                                    // Ethernet
    else if (link == 101 || link == 12 || link == 228) l2 = 0;  // Raw IP / IPv4
    else {
        std::fprintf(stderr, "unsupported link type %u\n", link);
        return 1;
    }

    std::vector<uint8_t> pkt(1 << 18);
    std::vector<uint8_t> obuf;
    obuf.reserve(21 * 65536);
    uint64_t n_read = 0, n_out = 0, n_skip = 0;
    uint8_t ph[16];
    while (n_out < max_n && std::fread(ph, 1, 16, in) == 16) {
        uint32_t sec = rd32(ph), frac = rd32(ph + 4), caplen = rd32(ph + 8);
        if (caplen > pkt.size()) pkt.resize(caplen);
        if (std::fread(pkt.data(), 1, caplen, in) != caplen) break;
        ++n_read;

        const uint8_t* p = pkt.data();
        size_t len = caplen, off = l2;
        if (link == 1) {
            if (len < 14) { ++n_skip; continue; }
            uint16_t et = be16(p + 12);
            if (et == 0x8100 && len >= 18) { et = be16(p + 16); off = 18; }
            if (et != 0x0800) { ++n_skip; continue; }
        }
        if (len < off + 20 || (p[off] >> 4) != 4) { ++n_skip; continue; }
        const uint8_t* ip = p + off;
        size_t ihl = (size_t)(ip[0] & 0x0f) * 4;
        uint8_t proto = ip[9];
        uint32_t src = be32(ip + 12), dst = be32(ip + 16);
        uint16_t sport = 0, dport = 0;
        bool first_frag = (be16(ip + 6) & 0x1fff) == 0;
        if ((proto == 6 || proto == 17) && first_frag && len >= off + ihl + 4) {
            sport = be16(ip + ihl);
            dport = be16(ip + ihl + 2);
        }
        uint64_t ts = (uint64_t)sec * 1000000000ull + (nano ? frac : (uint64_t)frac * 1000ull);

        uint8_t rec[21];
        std::memcpy(rec, &src, 4);
        std::memcpy(rec + 4, &dst, 4);
        std::memcpy(rec + 8, &sport, 2);
        std::memcpy(rec + 10, &dport, 2);
        rec[12] = proto;
        std::memcpy(rec + 13, &ts, 8);
        obuf.insert(obuf.end(), rec, rec + 21);
        if (obuf.size() >= 21 * 65536) {
            std::fwrite(obuf.data(), 1, obuf.size(), out);
            obuf.clear();
        }
        ++n_out;
    }
    std::fwrite(obuf.data(), 1, obuf.size(), out);
    std::fclose(out);
    std::fclose(in);
    std::fprintf(stderr, "read %llu packets, wrote %llu IPv4 records, skipped %llu\n",
                 (unsigned long long)n_read, (unsigned long long)n_out, (unsigned long long)n_skip);
    return 0;
}
