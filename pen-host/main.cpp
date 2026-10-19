// virtualpen-host: turns a Galaxy Tab S9+ running the Virtual Pen app into a
// pressure/tilt tablet on Linux via uinput.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <signal.h>
#include <string>
#include <sys/resource.h>
#include <time.h>

#include "controls.h"
#include "link.h"
#include "monitors.h"
#include "patch.h"
#include "pen.h"
#include "preview.h"

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
            "  --list-monitors        show the attached monitors (GNOME) and exit\n"
            "  --monitor ID           pin the tablet to a monitor: connector (DP-1), number from\n"
            "                         --list-monitors, or display name; 'auto' = GNOME decides\n"
            "  --screen WxH           monitor size in px; keeps the tablet aspect ratio (no stretched ink).\n"
            "                         Taken from --monitor when not given\n"
            "  --area X0,Y0,X1,Y1     fixed mapping to this part of the monitor, normalized 0..1\n"
            "                         (turns off the movable patch and the preview)\n"
            "  --zoom Z               initial patch size: 1, 1.5 or 2 = handwriting magnification (default 1)\n"
            "  --monitor-size-mm WxH  physical monitor size if the monitor does not report it\n"
            "  --no-preview           do not stream the patch to the tablet\n"
            "  --preview-fps N        preview frame rate (default 60)\n"
            "  --preview-mbps N       preview bitrate in Mbit/s (default 12)\n"
            "  --encoder NAME         auto (default), nvenc or x264\n"
            "  --scroll-px N          screen pixels per wheel notch for two-finger scrolling (default 53)\n"
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
    PreviewOptions popt;
    int tcpPort = 0, zoomPreset = 0;
    double monMmW = 0, monMmH = 0, scrollPx = 53.0;
    bool rt = true, stats = false, dryRun = false, listMonitors = false, areaGiven = false, previewOn = true;
    std::string monitorId;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char * {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for %s\n", a.c_str());
                exit(2);
            }
            return argv[++i];
        };
        if (a == "-v" || a == "--verbose") cfg.verbose = popt.verbose = true;
        else if (a == "--stats") stats = true;
        else if (a == "--no-rt") rt = false;
        else if (a == "--dry-run") dryRun = true;
        else if (a == "--list-monitors") listMonitors = true;
        else if (a == "--monitor") monitorId = next();
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
            areaGiven = true;
        } else if (a == "--zoom") {
            double z = atof(next());
            zoomPreset = 0;
            for (int p = 0; p < Patch::kPresetCount; p++)
                if (std::fabs(Patch::kPresets[p] - z) < std::fabs(Patch::kPresets[zoomPreset] - z)) zoomPreset = p;
        } else if (a == "--monitor-size-mm") {
            if (sscanf(next(), "%lfx%lf", &monMmW, &monMmH) != 2) {
                usage();
                return 2;
            }
        } else if (a == "--no-preview") previewOn = false;
        else if (a == "--preview-fps") popt.fps = std::max(1, atoi(next()));
        else if (a == "--preview-mbps") popt.bitrate_kbps = int(atof(next()) * 1000);
        else if (a == "--encoder") popt.encoder = next();
        else if (a == "--scroll-px") scrollPx = std::max(1.0, atof(next()));
        else if (a == "--pressure-min") cfg.p_min = atof(next());
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

    std::string connector; // monitor the tablet is pinned to, if any
    if (listMonitors || !monitorId.empty()) {
        std::vector<Monitor> monitors;
        std::string err;
        if (!queryMonitors(monitors, err)) {
            fprintf(stderr, "cannot list monitors: %s\n", err.c_str());
            return 1;
        }
        if (listMonitors) {
            printMonitors(monitors);
            return 0;
        }
        const Monitor *m = nullptr;
        if (monitorId != "auto") {
            m = findMonitor(monitors, monitorId, err);
            if (!m) {
                fprintf(stderr, "%s. Attached monitors:\n", err.c_str());
                printMonitors(monitors);
                return 2;
            }
            if (!m->enabled) {
                fprintf(stderr, "monitor %s is turned off in display settings\n", m->connector.c_str());
                return 2;
            }
        }
        if (!pinTabletToMonitor(m, err)) {
            fprintf(stderr, "cannot pin the tablet: %s\n", err.c_str());
            return 1;
        }
        if (m) {
            connector = m->connector;
            if (cfg.screen_w <= 0) {
                cfg.screen_w = m->width;
                cfg.screen_h = m->height;
            }
            fprintf(stderr, "tablet pinned to %s (%s), %dx%d\n", m->connector.c_str(), m->name.c_str(),
                    m->width, m->height);
        } else {
            fprintf(stderr, "tablet mapping returned to GNOME's automatic choice\n");
        }
    }

    // The patch needs the monitor's position and size on the X screen.
    Patch patch;
    bool patchMode = !areaGiven;
    MonitorGeom geom;
    if (patchMode) {
        std::string err;
        if (queryX11Monitor(connector, geom, err)) {
            if (monMmW > 0 && monMmH > 0) {
                geom.mm_w = monMmW;
                geom.mm_h = monMmH;
            }
            patch.setMonitor(geom);
            patch.setPreset(zoomPreset);
            fprintf(stderr, "patch mapping on %s: %dx%d px at %d,%d, %.0fx%.0f mm\n", geom.name.c_str(), geom.w,
                    geom.h, geom.x, geom.y, geom.mm_w, geom.mm_h);
            if (geom.mm_w <= 0)
                fprintf(stderr, "warning: monitor size unknown, pass --monitor-size-mm for true 1:1 zoom\n");
            if (connector.empty())
                fprintf(stderr, "note: without --monitor GNOME may spread the tablet over all monitors; "
                                "pass --monitor %s so pen and preview line up\n",
                        geom.name.c_str());
        } else {
            fprintf(stderr, "patch mapping off (%s); using a fixed mapping\n", err.c_str());
            patchMode = false;
        }
    }

    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    UinputPen pen;
    if (!dryRun && !pen.open("Virtual Pen Stylus")) return 1;
    Controls controls;
    controls.wheel_px_per_notch = scrollPx;
    if (!dryRun) controls.open();

    // Started before raising priority so capture and encoding stay at normal
    // priority; only the pen thread runs real-time.
    DownlinkSlot slot;
    Preview preview(slot, popt);
    if (patchMode && previewOn) {
        std::string err;
        if (!preview.start(geom, err)) fprintf(stderr, "preview disabled: %s\n", err.c_str());
    }
    if (rt) raisePriority();

    PenTranslator translator(cfg);
    Decoder decoder;
    Stats st;
    std::vector<input_event> out;
    out.reserve(4096);

    auto sendGeometry = [&] {
        if (!patchMode || !patch.ready()) return;
        uint8_t p[16] = {};
        wr16(p + 0, unsigned(geom.w));
        wr16(p + 2, unsigned(geom.h));
        wr16(p + 4, unsigned(patch.x));
        wr16(p + 6, unsigned(patch.y));
        wr16(p + 8, unsigned(patch.w));
        wr16(p + 10, unsigned(patch.h));
        p[12] = uint8_t(patch.preset());
        p[13] = uint8_t((translator.stickyEraser() ? GEO_ERASER : 0) | (preview.running() ? GEO_PREVIEW : 0));
        wr16(p + 14, unsigned(std::lround(patch.zoom() * 100)));
        std::vector<uint8_t> msg = downlinkMessage(DL_GEOMETRY, 0, p, sizeof(p));
        slot.send(msg.data(), msg.size(), false);
    };
    auto applyPatch = [&] {
        double r[4];
        patch.normalized(r);
        translator.setRect(r);
        preview.setRegion(patch.x, patch.y, patch.w, patch.h);
        sendGeometry();
        fprintf(stderr, "patch %dx%d at %d,%d (zoom %.1f)\n", patch.w, patch.h, patch.x, patch.y, patch.zoom());
    };
    auto onHello = [&](const Hello &h) {
        translator.onHello(h);
        if (!patchMode) return;
        if (patch.setTablet(h.width, h.height, h.xdpi, h.ydpi)) applyPatch();
        else sendGeometry();
    };
    auto onCommand = [&](const Command &c) {
        if (cfg.verbose) fprintf(stderr, "command %d (%d, %d)\n", c.cmd, c.a, c.b);
        switch (c.cmd) {
        case CMD_UNDO: controls.undo(); break;
        case CMD_REDO: controls.redo(); break;
        case CMD_ERASER:
            translator.setStickyEraser(c.a != 0);
            sendGeometry();
            break;
        case CMD_SCROLL: {
            double k = patchMode ? patch.monitorPxPerTabletPx() : 1.0;
            controls.scroll(c.a * k, c.b * k);
            break;
        }
        case CMD_KEYFRAME: preview.requestKeyframe(); break;
        case CMD_PATCH_STEP:
        case CMD_PATCH_NEXT:
        case CMD_PATCH_PRESET:
        case CMD_PATCH_DRAG:
            if (!patchMode || !patch.ready()) break;
            if (c.cmd == CMD_PATCH_STEP) patch.step(c.a, c.b);
            else if (c.cmd == CMD_PATCH_NEXT) patch.next();
            else if (c.cmd == CMD_PATCH_PRESET) patch.setPreset(c.a);
            else patch.drag(c.a, c.b);
            applyPatch();
            break;
        default: break;
        }
    };

    LinkEvents events;
    events.up = [&](Downlink *d) {
        decoder.reset();
        slot.set(d);
        preview.linkUp();
    };
    events.data = [&](const uint8_t *data, size_t len) {
        out.clear();
        decoder.feed(
            data, len,
            [&](const Sample &s) {
                translator.onSample(s, out);
                if (stats) st.sample(s);
            },
            onHello, onCommand);
        pen.write(out);
        if (stats) st.batch();
    };
    events.down = [&] {
        slot.set(nullptr);
        out.clear();
        translator.release(out);
        pen.write(out);
    };

    if (tcpPort > 0) runTcp(tcpPort, events);
    else runUsb(usb, events, cfg.verbose);

    preview.stop();
    events.down();
    if (decoder.dropped()) fprintf(stderr, "%llu malformed bytes skipped\n", (unsigned long long)decoder.dropped());
    return 0;
}
