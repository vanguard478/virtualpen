// virtualpen-host: turns a Galaxy Tab S9+ running the Virtual Pen app into a
// pressure/tilt tablet on Linux via uinput.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <signal.h>
#include <string>
#include <sys/resource.h>
#include <time.h>

#include "link.h"
#include "pen.h"

namespace vp {
volatile sig_atomic_t g_stop = 0;
}

using namespace vp;

namespace {

void usage() {
    fprintf(stderr,
            "usage: virtualpen-host [options]\n"
            "  --usb VID[:PID]        tablet USB id to switch to accessory mode (default 04e8 = Samsung)\n"
            "  --tcp PORT             use TCP via `adb reverse` instead of USB accessory mode\n"
            "  --screen WxH           monitor size in px; keeps the tablet aspect ratio (no stretched ink)\n"
            "  --area X0,Y0,X1,Y1     map the tablet to this part of the output, normalized 0..1\n"
            "  --pressure-min F       raw pressure treated as zero (default 0)\n"
            "  --pressure-max F       raw pressure treated as full (default 1; lower = lighter touch)\n"
            "  --gamma F              pressure curve exponent (<1 = softer, >1 = firmer; default 1)\n"
            "  --tip-floor F          output pressure at first contact (default 0.06, keep > 0.05)\n"
            "  --button MODE          S Pen button: eraser (default), right-click, none\n"
            "  --tilt-flip-x / --tilt-flip-y / --tilt-swap   fix tilt direction if it is mirrored\n"
            "  --no-rt                do not request real-time scheduling\n"
            "  --stats                print sample rate and gaps once per second\n"
            "  --dry-run              decode and translate without creating the uinput device\n"
            "  -v, --verbose          print every sample\n");
}

bool parseDoubles(const char *s, double *out, int n) {
    for (int i = 0; i < n; i++) {
        char *end;
        out[i] = strtod(s, &end);
        if (end == s) return false;
        s = end;
        if (i < n - 1) {
            if (*s != ',') return false;
            s++;
        }
    }
    return *s == 0;
}

void onSignal(int) { g_stop = 1; }

void raisePriority() {
    sched_param sp{};
    sp.sched_priority = 20;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) == 0) {
        fprintf(stderr, "running with SCHED_FIFO priority %d\n", sp.sched_priority);
        return;
    }
    if (setpriority(PRIO_PROCESS, 0, -10) == 0) {
        fprintf(stderr, "running with nice -10 (SCHED_FIFO not permitted)\n");
        return;
    }
    fprintf(stderr, "running with normal priority (see README to allow real-time scheduling)\n");
}

uint64_t nowUs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000 + uint64_t(ts.tv_nsec) / 1000;
}

struct Stats {
    uint64_t windowStart = 0, lastArrival = 0;
    uint32_t lastTime = 0;
    bool haveLast = false;
    int samples = 0, batches = 0;
    uint32_t maxGapUs = 0;
    uint64_t maxArrivalGapUs = 0;

    void sample(const Sample &s) {
        if (haveLast && s.time_us - lastTime > maxGapUs && s.time_us - lastTime < 1000000)
            maxGapUs = s.time_us - lastTime;
        lastTime = s.time_us;
        haveLast = s.flags & FLAG_IN_RANGE;
        samples++;
    }
    void batch() {
        uint64_t now = nowUs();
        if (lastArrival && now - lastArrival < 1000000 && now - lastArrival > maxArrivalGapUs)
            maxArrivalGapUs = now - lastArrival;
        lastArrival = now;
        batches++;
        if (!windowStart) windowStart = now;
        if (now - windowStart >= 1000000) {
            fprintf(stderr, "%4d samples/s in %4d transfers, max gap: tablet %.1f ms, usb %.1f ms\n",
                    samples, batches, maxGapUs / 1000.0, maxArrivalGapUs / 1000.0);
            *this = Stats{};
            windowStart = now;
            lastArrival = now;
        }
    }
};

} // namespace

int main(int argc, char **argv) {
    PenConfig cfg;
    UsbOptions usb;
    int tcpPort = 0;
    bool rt = true, stats = false, dryRun = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char * {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for %s\n", a.c_str());
                exit(2);
            }
            return argv[++i];
        };
        if (a == "-v" || a == "--verbose") cfg.verbose = true;
        else if (a == "--stats") stats = true;
        else if (a == "--no-rt") rt = false;
        else if (a == "--dry-run") dryRun = true;
        else if (a == "--tcp") tcpPort = atoi(next());
        else if (a == "--usb") {
            const char *v = next();
            char *end;
            usb.vid = int(strtol(v, &end, 16));
            usb.pid = (*end == ':') ? int(strtol(end + 1, nullptr, 16)) : -1;
        } else if (a == "--screen") {
            if (sscanf(next(), "%dx%d", &cfg.screen_w, &cfg.screen_h) != 2) {
                usage();
                return 2;
            }
        } else if (a == "--area") {
            if (!parseDoubles(next(), cfg.area, 4) || cfg.area[2] <= cfg.area[0] || cfg.area[3] <= cfg.area[1]) {
                usage();
                return 2;
            }
        } else if (a == "--pressure-min") cfg.p_min = atof(next());
        else if (a == "--pressure-max") cfg.p_max = atof(next());
        else if (a == "--gamma") cfg.gamma = atof(next());
        else if (a == "--tip-floor") cfg.tip_floor = atof(next());
        else if (a == "--button") {
            std::string m = next();
            if (m == "eraser") cfg.button = ButtonMode::Eraser;
            else if (m == "right-click") cfg.button = ButtonMode::RightClick;
            else if (m == "none") cfg.button = ButtonMode::None;
            else {
                usage();
                return 2;
            }
        } else if (a == "--tilt-flip-x") cfg.tilt_flip_x = true;
        else if (a == "--tilt-flip-y") cfg.tilt_flip_y = true;
        else if (a == "--tilt-swap") cfg.tilt_swap = true;
        else {
            usage();
            return a == "-h" || a == "--help" ? 0 : 2;
        }
    }

    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    UinputPen pen;
    if (!dryRun && !pen.open("Virtual Pen Stylus")) return 1;
    if (rt) raisePriority();

    PenTranslator translator(cfg);
    Decoder decoder;
    Stats st;
    std::vector<input_event> out;
    out.reserve(4096);

    auto onConnect = [&] { decoder.reset(); };
    auto onData = [&](const uint8_t *data, size_t len) {
        out.clear();
        decoder.feed(
            data, len,
            [&](const Sample &s) {
                translator.onSample(s, out);
                if (stats) st.sample(s);
            },
            [&](const Hello &h) { translator.onHello(h); });
        pen.write(out);
        if (stats) st.batch();
    };
    auto onDisconnect = [&] {
        out.clear();
        translator.release(out);
        pen.write(out);
    };

    if (tcpPort > 0) runTcp(tcpPort, onConnect, onData, onDisconnect);
    else runUsb(usb, onConnect, onData, onDisconnect, cfg.verbose);

    onDisconnect();
    if (decoder.dropped()) fprintf(stderr, "%llu malformed bytes skipped\n", (unsigned long long)decoder.dropped());
    return 0;
}
