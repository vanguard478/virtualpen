// Unit tests for the decoder and the evdev translation. Run: ./pen-test
#include <cmath>
#include <cstdio>
#include <vector>

#include "monitors.h"
#include "pen.h"
#include "protocol.h"

using namespace vp;

static int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void put16(uint8_t *p, int v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

static std::vector<uint8_t> sample(uint8_t flags, int x, int y, int p, int tilt = 0, int orient = 0,
                                   uint32_t t = 0) {
    std::vector<uint8_t> b(PKT_SIZE, 0);
    b[0] = PKT_SAMPLE;
    b[1] = flags;
    put16(&b[2], x);
    put16(&b[4], y);
    put16(&b[6], p);
    put16(&b[8], tilt);
    put16(&b[10], orient);
    for (int i = 0; i < 4; i++) b[12 + i] = uint8_t(t >> (8 * i));
    return b;
}

static Sample S(uint8_t flags, int x = 32768, int y = 32768, int p = 0, int tilt = 0, int orient = 0) {
    return Sample{flags, uint16_t(x), uint16_t(y), uint16_t(p), int16_t(tilt), int16_t(orient), 0};
}

// Value of the last event with this type/code, or -999 if absent.
static int last(const std::vector<input_event> &ev, int type, int code) {
    int v = -999;
    for (auto &e : ev)
        if (e.type == type && e.code == code) v = e.value;
    return v;
}

static int frames(const std::vector<input_event> &ev) {
    int n = 0;
    for (auto &e : ev) n += e.type == EV_SYN && e.code == SYN_REPORT;
    return n;
}

static void testDecoder() {
    std::vector<uint8_t> stream;
    auto a = sample(FLAG_IN_RANGE, 1, 2, 3, -4, 5, 0xdeadbeef);
    auto b = sample(FLAG_IN_RANGE | FLAG_TOUCH, 65535, 0, 40000);
    std::vector<uint8_t> hello(PKT_SIZE, 0);
    hello[0] = PKT_HELLO;
    hello[1] = PROTOCOL_VERSION;
    put16(&hello[2], 2800);
    put16(&hello[4], 1752);
    stream.insert(stream.end(), hello.begin(), hello.end());
    stream.insert(stream.end(), a.begin(), a.end());
    stream.push_back(0x00);  // garbage byte forces a resync
    stream.insert(stream.end(), b.begin(), b.end());

    // Feed in awkward chunk sizes.
    for (size_t chunk : {size_t(1), size_t(7), size_t(16), size_t(33), stream.size()}) {
        Decoder d;
        std::vector<Sample> got;
        int hellos = 0;
        for (size_t off = 0; off < stream.size(); off += chunk) {
            size_t n = std::min(chunk, stream.size() - off);
            d.feed(stream.data() + off, n, [&](const Sample &s) { got.push_back(s); },
                   [&](const Hello &h) {
                       hellos++;
                       CHECK(h.width == 2800 && h.height == 1752);
                   });
        }
        CHECK(hellos == 1);
        CHECK(got.size() == 2);
        if (got.size() == 2) {
            CHECK(got[0].x == 1 && got[0].y == 2 && got[0].pressure == 3);
            CHECK(got[0].tilt == -4 && got[0].orientation == 5 && got[0].time_us == 0xdeadbeef);
            CHECK(got[1].x == 65535 && got[1].pressure == 40000 && (got[1].flags & FLAG_TOUCH));
        }
        CHECK(d.dropped() == 1);
    }
}

static void testStroke() {
    PenConfig cfg;
    PenTranslator t(cfg);
    std::vector<input_event> ev;

    // Hover in: tool in proximity, zero pressure, no touch.
    t.onSample(S(FLAG_IN_RANGE), ev);
    CHECK(last(ev, EV_KEY, BTN_TOOL_PEN) == 1);
    CHECK(last(ev, EV_ABS, ABS_PRESSURE) == 0);
    CHECK(last(ev, EV_KEY, BTN_TOUCH) == 0);

    // Contact.
    ev.clear();
    t.onSample(S(FLAG_IN_RANGE | FLAG_TOUCH, 32768, 32768, 1), ev);
    CHECK(frames(ev) == 1);
    CHECK(last(ev, EV_KEY, BTN_TOUCH) == 1);
    // Even the lightest contact must clear libinput's 5% tip threshold.
    CHECK(last(ev, EV_ABS, ABS_PRESSURE) > ABS_PRESSURE_MAX * 5 / 100);
    CHECK(last(ev, EV_KEY, BTN_TOOL_PEN) == -999);

    // Lift: stays in proximity.
    ev.clear();
    t.onSample(S(FLAG_IN_RANGE), ev);
    CHECK(last(ev, EV_KEY, BTN_TOUCH) == 0);
    CHECK(last(ev, EV_ABS, ABS_PRESSURE) == 0);
    CHECK(last(ev, EV_KEY, BTN_TOOL_PEN) == -999);

    // Out of range.
    ev.clear();
    t.onSample(S(0), ev);
    CHECK(last(ev, EV_KEY, BTN_TOOL_PEN) == 0);
    ev.clear();
    t.onSample(S(0), ev);
    CHECK(ev.empty());
}

static void testDirectContact() {
    // Contact without a preceding hover: proximity-in frame with zero pressure first.
    PenTranslator t{PenConfig{}};
    std::vector<input_event> ev;
    t.onSample(S(FLAG_IN_RANGE | FLAG_TOUCH, 100, 100, 30000), ev);
    CHECK(frames(ev) == 2);
    bool sawZeroBeforeSyn = false;
    for (auto &e : ev) {
        if (e.type == EV_ABS && e.code == ABS_PRESSURE && e.value == 0) sawZeroBeforeSyn = true;
        if (e.type == EV_SYN) break;
    }
    CHECK(sawZeroBeforeSyn);
    CHECK(last(ev, EV_ABS, ABS_PRESSURE) > 0);
}

static void testEraserButton() {
    PenTranslator t{PenConfig{}};
    std::vector<input_event> ev;
    t.onSample(S(FLAG_IN_RANGE), ev);
    ev.clear();
    // Button while hovering switches to the eraser tool.
    t.onSample(S(FLAG_IN_RANGE | FLAG_BTN_PRIMARY), ev);
    CHECK(last(ev, EV_KEY, BTN_TOOL_PEN) == 0);
    CHECK(last(ev, EV_KEY, BTN_TOOL_RUBBER) == 1);
    ev.clear();
    t.onSample(S(FLAG_IN_RANGE | FLAG_TOUCH | FLAG_BTN_PRIMARY, 1, 1, 30000), ev);
    // Releasing the button mid-stroke keeps erasing.
    ev.clear();
    t.onSample(S(FLAG_IN_RANGE | FLAG_TOUCH, 2, 2, 30000), ev);
    CHECK(last(ev, EV_KEY, BTN_TOOL_RUBBER) == -999);
    CHECK(last(ev, EV_KEY, BTN_TOOL_PEN) == -999);
    // After lifting, hovering returns to the pen.
    ev.clear();
    t.onSample(S(FLAG_IN_RANGE), ev);
    ev.clear();
    t.onSample(S(FLAG_IN_RANGE), ev);
    CHECK(last(ev, EV_KEY, BTN_TOOL_RUBBER) == 0);
    CHECK(last(ev, EV_KEY, BTN_TOOL_PEN) == 1);

    PenConfig rc;
    rc.button = ButtonMode::RightClick;
    PenTranslator r(rc);
    ev.clear();
    r.onSample(S(FLAG_IN_RANGE | FLAG_BTN_PRIMARY), ev);
    CHECK(last(ev, EV_KEY, BTN_STYLUS) == 1);
    CHECK(last(ev, EV_KEY, BTN_TOOL_RUBBER) == -999);
}

static void testPressureCurve() {
    PenConfig cfg;
    cfg.gamma = 2.0;
    cfg.p_max = 0.5;
    PenTranslator t(cfg);
    CHECK(t.mapPressure(0) == int(std::lround(0.06 * ABS_PRESSURE_MAX)));
    CHECK(t.mapPressure(32768) == ABS_PRESSURE_MAX);  // saturates at p_max
    CHECK(t.mapPressure(65535) == ABS_PRESSURE_MAX);
    int quarter = t.mapPressure(16384);  // t = 0.5 -> 0.25 after gamma
    CHECK(std::abs(quarter - int(std::lround((0.06 + 0.94 * 0.25) * ABS_PRESSURE_MAX))) < 50);
}

static void testTilt() {
    PenTranslator t{PenConfig{}};
    int tx, ty;
    const int q = 15708 / 2;  // 45 degrees
    t.mapTilt(0, 12345, tx, ty);
    CHECK(tx == 0 && ty == 0);
    t.mapTilt(q, 15708, tx, ty);  // leaning right
    CHECK(std::abs(tx - 4500) <= 2 && std::abs(ty) <= 2);
    t.mapTilt(q, 0, tx, ty);  // leaning up / away from the user
    CHECK(std::abs(tx) <= 2 && std::abs(ty + 4500) <= 2);
    t.mapTilt(q, 31416, tx, ty);  // leaning down / towards the user
    CHECK(std::abs(tx) <= 2 && std::abs(ty - 4500) <= 2);
    t.mapTilt(15708, 15708, tx, ty);  // flat
    CHECK(tx == TILT_MAX);

    PenConfig flip;
    flip.tilt_flip_x = true;
    PenTranslator f(flip);
    f.mapTilt(q, 15708, tx, ty);
    CHECK(std::abs(tx + 4500) <= 2);
}

static void testAspect() {
    PenConfig cfg;
    cfg.screen_w = 1920;
    cfg.screen_h = 1080;
    PenTranslator t(cfg);
    Hello h{PROTOCOL_VERSION, 2800, 1752};
    t.onHello(h);
    const double *r = t.mappedArea();
    double w = (r[2] - r[0]) * 1920, hgt = (r[3] - r[1]) * 1080;
    CHECK(std::abs(w / hgt - 2800.0 / 1752.0) < 1e-6);
    CHECK(std::abs(r[1]) < 1e-9 && std::abs(r[3] - 1.0) < 1e-9);  // full height on 16:9
    CHECK(std::abs(r[0] - (1.0 - r[2])) < 1e-9);                  // centered

    std::vector<input_event> ev;
    t.onSample(S(FLAG_IN_RANGE, 0, 65535), ev);
    CHECK(last(ev, EV_ABS, ABS_X) == int(std::lround(r[0] * ABS_XY_MAX)));
    CHECK(last(ev, EV_ABS, ABS_Y) == ABS_XY_MAX);
}

// Shaped like `gdbus call ... DisplayConfig.GetCurrentState` output on GNOME 46:
// a scaled 4K primary, a portrait monitor and a laptop panel that is turned off.
static const char *kDisplayState = R"GV((uint32 7, [(('DP-1', 'DEL', 'DELL U2720Q', 'CN0ABC'), [('3840x2160@59.997', 3840, 2160, 59.997, 2.0, [1.0, 2.0], {'is-current': <true>, 'is-preferred': <true>}), ('2560x1440@59.951', 2560, 1440, 59.951, 1.0, [1.0], @a{sv} {})], {'is-underscanning': <false>, 'display-name': <'Dell Inc. 27"'>, 'is-builtin': <false>}), (('HDMI-1', 'GSM', 'LG HDR 4K', '0x0001'), [('2560x1440@60.000', 2560, 1440, 60.0, 1.0, [1.0], {'is-current': <true>})], {'display-name': <'LG Electronics 27"'>}), (('eDP-1', 'BOE', '0x0a1b', '0x00000000'), [('1920x1200@60.000', 1920, 1200, 60.0, 1.0, [1.0], {'is-current': <true>})], {'display-name': <'Built-in display'>, 'is-builtin': <true>})], [(0, 0, 2.0, uint32 0, true, [('DP-1', 'DEL', 'DELL U2720Q', 'CN0ABC')], @a{sv} {}), (1920, 0, 1.0, uint32 1, false, [('HDMI-1', 'GSM', 'LG HDR 4K', '0x0001')], @a{sv} {})], {'layout-mode': <uint32 2>, 'supports-changing-layout-mode': <true>}))GV";

static void testMonitors() {
    std::vector<Monitor> mons;
    std::string err;
    CHECK(parseDisplayState(kDisplayState, mons, err));
    CHECK(mons.size() == 3);
    if (mons.size() != 3) return;

    const Monitor &dell = mons[0];
    CHECK(dell.connector == "DP-1" && dell.vendor == "DEL" && dell.product == "DELL U2720Q" &&
          dell.serial == "CN0ABC");
    CHECK(dell.name == "Dell Inc. 27\"");
    CHECK(dell.width == 3840 && dell.height == 2160);  // current mode, not the first listed
    CHECK(dell.enabled && dell.primary && dell.scale == 2.0);

    const Monitor &lg = mons[1];
    CHECK(lg.enabled && !lg.primary && lg.x == 1920);
    CHECK(lg.width == 1440 && lg.height == 2560);  // rotated 90 degrees

    CHECK(!mons[2].enabled && mons[2].builtin);

    CHECK(findMonitor(mons, "hdmi-1", err) == &mons[1]);
    CHECK(findMonitor(mons, "1", err) == &mons[0]);
    CHECK(findMonitor(mons, "Built-in display", err) == &mons[2]);
    CHECK(findMonitor(mons, "lg", err) == &mons[1]);
    CHECK(findMonitor(mons, "u2720q", err) == &mons[0]);
    CHECK(findMonitor(mons, "27", err) == nullptr);  // ambiguous
    CHECK(findMonitor(mons, "DP-9", err) == nullptr);
    CHECK(findMonitor(mons, "4", err) == nullptr);

    CHECK(!parseDisplayState("not a variant", mons, err));
}

int main() {
    testMonitors();
    testDecoder();
    testStroke();
    testDirectContact();
    testEraserButton();
    testPressureCurve();
    testTilt();
    testAspect();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
