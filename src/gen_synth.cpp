// Synthetic packet trace generator (16-byte {uint64 timestamp, uint64 id} records, like CAIDA files
// used by BurstSketch/PBSketch). Each count-based window holds exactly W packets:
//   - periodic burst flows (planted ground truth): period, phase, jitter, missed bursts, and either
//     a sudden drop or a gradual decay after each burst
//   - aperiodic bursty flows: bursts separated by geometric gaps
//   - level-shift flows: jump to a high rate for longer than L windows, 1-3 times
//   - background: Zipf(alpha) over bg_flows flows fills the rest of every window
#include <random>

#include "pbo_common.h"

using namespace pbo;

struct Args {
    uint64_t packets = 30000000;
    uint32_t W = 30000;
    double alpha = 1.0;
    uint32_t bg_flows = 1000000;
    uint32_t pb_flows = 1000, pb_pmin = 3, pb_pmax = 30, pb_amin = 90, pb_amax = 200;
    uint32_t pb_dmin = 1, pb_dmax = 1, pb_base_max = 3;
    double pb_jitter = 0.3, pb_miss = 0.05, pb_decay = 0.3;
    uint32_t ap_flows = 150, ap_gap_mean = 40, ap_amin = 100, ap_amax = 400, ap_dmin = 1, ap_dmax = 4;
    uint32_t ls_flows = 20;
    uint64_t seed = 1;
    std::string out, planted_out;
};

static Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--packets") a.packets = std::stoull(v);
        else if (k == "--window") a.W = std::stoul(v);
        else if (k == "--alpha") a.alpha = std::stod(v);
        else if (k == "--bg-flows") a.bg_flows = std::stoul(v);
        else if (k == "--pb-flows") a.pb_flows = std::stoul(v);
        else if (k == "--pb-period") { a.pb_pmin = std::stoul(v.substr(0, v.find(','))); a.pb_pmax = std::stoul(v.substr(v.find(',') + 1)); }
        else if (k == "--pb-amp") { a.pb_amin = std::stoul(v.substr(0, v.find(','))); a.pb_amax = std::stoul(v.substr(v.find(',') + 1)); }
        else if (k == "--pb-dur") { a.pb_dmin = std::stoul(v.substr(0, v.find(','))); a.pb_dmax = std::stoul(v.substr(v.find(',') + 1)); }
        else if (k == "--pb-base") a.pb_base_max = std::stoul(v);
        else if (k == "--pb-decay") a.pb_decay = std::stod(v);
        else if (k == "--pb-jitter") a.pb_jitter = std::stod(v);
        else if (k == "--ap-flows") a.ap_flows = std::stoul(v);
        else if (k == "--ls-flows") a.ls_flows = std::stoul(v);
        else if (k == "--seed") a.seed = std::stoull(v);
        else if (k == "--out") a.out = v;
        else if (k == "--planted") a.planted_out = v;
        else throw std::runtime_error("unknown option " + k);
    }
    if (a.out.empty()) throw std::runtime_error("--out is required");
    return a;
}

int main(int argc, char** argv) {
    Args a = parse(argc, argv);
    std::mt19937_64 rng(a.seed);
    auto uni = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
    auto uint = [&](uint32_t lo, uint32_t hi) { return std::uniform_int_distribution<uint32_t>(lo, hi)(rng); };

    const uint32_t nwin = (uint32_t)((a.packets + a.W - 1) / a.W);
    std::vector<std::vector<double>> rate;  // expected packets per window for each planted flow
    std::vector<uint64_t> pid;
    FILE* planted = a.planted_out.empty() ? nullptr : std::fopen(a.planted_out.c_str(), "w");
    if (planted) std::fprintf(planted, "kind,id,period,phase,duration,amplitude,decay\n");

    auto new_flow = [&](uint32_t kind, double base) {
        pid.push_back(splitmix64(((uint64_t)kind << 40) ^ (pid.size() + 1) ^ (a.seed << 20)) | 1);
        rate.emplace_back(nwin, base);
        return rate.size() - 1;
    };
    auto put_burst = [&](size_t f, int64_t start, uint32_t dur, double amp, bool decay) {
        for (uint32_t d = 0; d < dur; ++d)
            if (start + d >= 0 && start + d < nwin) rate[f][start + d] = amp * uni(0.8, 1.2);
        if (decay)
            for (uint32_t d = 0; d < 3; ++d) {
                int64_t w = start + dur + d;
                if (w >= 0 && w < nwin) rate[f][w] = std::max(rate[f][w], amp * std::pow(0.6, d + 1));
            }
    };

    // periodic burst flows
    for (uint32_t i = 0; i < a.pb_flows; ++i) {
        uint32_t v = uint(a.pb_pmin, a.pb_pmax);
        bool decay = uni(0, 1) < a.pb_decay && v >= 7;
        uint32_t room = v >= 4 + (decay ? 3 : 0) ? v - 3 - (decay ? 3 : 0) : 1;
        uint32_t dur = uint(a.pb_dmin, std::max(a.pb_dmin, std::min(a.pb_dmax, room)));
        double amp = uint(a.pb_amin, a.pb_amax), base = uni(0, a.pb_base_max);
        uint32_t phase = uint(0, v - 1);
        size_t f = new_flow(1, base);
        for (int64_t s = phase; s < nwin; s += v) {
            if (uni(0, 1) < a.pb_miss) continue;
            int64_t j = 0;
            double u = uni(0, 1);
            if (u < a.pb_jitter / 2) j = -1;
            else if (u < a.pb_jitter) j = 1;
            put_burst(f, s + j, dur, amp, decay);
        }
        if (planted) std::fprintf(planted, "periodic,%llu,%u,%u,%u,%.0f,%d\n", (unsigned long long)pid[f], v, phase, dur, amp, decay);
    }
    // aperiodic bursty flows
    std::geometric_distribution<uint32_t> gap(1.0 / a.ap_gap_mean);
    for (uint32_t i = 0; i < a.ap_flows; ++i) {
        size_t f = new_flow(2, uni(0, a.pb_base_max));
        double amp = uint(a.ap_amin, a.ap_amax);
        for (int64_t s = gap(rng); s < nwin; s += gap(rng) + a.ap_dmax + 2)
            put_burst(f, s, uint(a.ap_dmin, a.ap_dmax), amp, false);
        if (planted) std::fprintf(planted, "aperiodic,%llu,0,0,0,%.0f,0\n", (unsigned long long)pid[f], amp);
    }
    // level-shift flows
    for (uint32_t i = 0; i < a.ls_flows; ++i) {
        size_t f = new_flow(3, uni(0, a.pb_base_max));
        double amp = uint(100, 300);
        uint32_t shifts = uint(1, 3);
        for (uint32_t s = 0; s < shifts; ++s) {
            uint32_t start = uint(0, nwin - 1), len = uint(50, 300);
            for (uint32_t w = start; w < std::min(nwin, start + len); ++w) rate[f][w] = amp * uni(0.9, 1.1);
        }
        if (planted) std::fprintf(planted, "levelshift,%llu,0,0,0,%.0f,0\n", (unsigned long long)pid[f], amp);
    }
    if (planted) std::fclose(planted);

    // Zipf background via inverse CDF
    std::vector<double> cdf(a.bg_flows);
    double acc = 0;
    for (uint32_t r = 0; r < a.bg_flows; ++r) cdf[r] = (acc += std::pow(r + 1.0, -a.alpha));
    for (auto& x : cdf) x /= acc;

    FILE* out = std::fopen(a.out.c_str(), "wb");
    if (!out) throw std::runtime_error("cannot write " + a.out);
    std::vector<uint64_t> win;
    std::vector<Record> buf;
    uint64_t ts = 0, planted_total = 0;
    for (uint32_t w = 0; w < nwin && ts < a.packets; ++w) {
        uint32_t target = (uint32_t)std::min<uint64_t>(a.W, a.packets - ts);
        win.clear();
        for (size_t f = 0; f < rate.size(); ++f) {
            uint32_t n = std::poisson_distribution<uint32_t>(rate[f][w])(rng);
            for (uint32_t x = 0; x < n && win.size() < target; ++x) win.push_back(pid[f]);
        }
        planted_total += win.size();
        while (win.size() < target) {
            size_t r = std::lower_bound(cdf.begin(), cdf.end(), uni(0, 1)) - cdf.begin();
            win.push_back(splitmix64(r + 0xB6A9ULL * (a.seed + 7)) | 1);
        }
        std::shuffle(win.begin(), win.end(), rng);
        buf.resize(win.size());
        for (size_t x = 0; x < win.size(); ++x) buf[x] = {ts++, win[x]};
        std::fwrite(buf.data(), sizeof(Record), buf.size(), out);
    }
    std::fclose(out);
    std::fprintf(stderr, "wrote %llu packets, %u windows, planted share %.1f%%\n", (unsigned long long)ts, nwin,
                 100.0 * planted_total / ts);
    return 0;
}
