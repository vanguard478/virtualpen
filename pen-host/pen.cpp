#include "pen.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace vp {

namespace {

inline void emit(std::vector<input_event> &out, int type, int code, int value) {
    input_event ev{};
    ev.type = uint16_t(type);
    ev.code = uint16_t(code);
    ev.value = value;
    out.push_back(ev);
}

inline void syn(std::vector<input_event> &out) { emit(out, EV_SYN, SYN_REPORT, 0); }

inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

} // namespace

PenTranslator::PenTranslator(const PenConfig &cfg) : cfg_(cfg) { updateRect(); }

void PenTranslator::onHello(const Hello &h) {
    if (h.version != PROTOCOL_VERSION)
        fprintf(stderr, "warning: Android app speaks protocol v%d, host expects v%d\n", h.version,
                PROTOCOL_VERSION);
    if (h.width > 0 && h.height > 0) {
        tablet_w_ = h.width;
        tablet_h_ = h.height;
    }
    updateRect();
    fprintf(stderr, "tablet surface %dx%d px -> area [%.4f %.4f %.4f %.4f]\n", tablet_w_, tablet_h_,
            rect_[0], rect_[1], rect_[2], rect_[3]);
}

void PenTranslator::updateRect() {
    std::copy(cfg_.area, cfg_.area + 4, rect_);
    if (cfg_.screen_w <= 0 || cfg_.screen_h <= 0) return;
    // Fit a rectangle with the tablet's aspect ratio, centered in the area.
    double area_w = (rect_[2] - rect_[0]) * cfg_.screen_w;
    double area_h = (rect_[3] - rect_[1]) * cfg_.screen_h;
    double tablet_aspect = double(tablet_w_) / tablet_h_;
    if (area_w / area_h > tablet_aspect) {
        double shrink = (area_w - area_h * tablet_aspect) / cfg_.screen_w / 2.0;
        rect_[0] += shrink;
        rect_[2] -= shrink;
    } else {
        double shrink = (area_h - area_w / tablet_aspect) / cfg_.screen_h / 2.0;
        rect_[1] += shrink;
        rect_[3] -= shrink;
    }
}

int PenTranslator::mapPressure(uint16_t raw) const {
    double p = raw / 65535.0;
    double span = cfg_.p_max - cfg_.p_min;
    double t = span > 0 ? clamp01((p - cfg_.p_min) / span) : 1.0;
    if (cfg_.gamma != 1.0) t = std::pow(t, cfg_.gamma);
    double out = cfg_.tip_floor + (1.0 - cfg_.tip_floor) * t;
    return int(std::lround(clamp01(out) * ABS_PRESSURE_MAX));
}

void PenTranslator::mapTilt(int16_t tilt, int16_t orientation, int &tx, int &ty) const {
    // Android: tilt is the angle from the screen normal, orientation the
    // direction the pen leans (0 = up, +pi/2 = right). Linux/libinput: tilt_x
    // positive when the top of the pen leans right, tilt_y when it leans
    // towards the user (down).
    double theta = tilt * 1e-4;
    double phi = orientation * 1e-4;
    double r = std::sin(theta), z = std::cos(theta);
    double ax = std::atan2(std::sin(phi) * r, z) * (18000.0 / M_PI);
    double ay = std::atan2(-std::cos(phi) * r, z) * (18000.0 / M_PI);
    if (cfg_.tilt_swap) std::swap(ax, ay);
    if (cfg_.tilt_flip_x) ax = -ax;
    if (cfg_.tilt_flip_y) ay = -ay;
    tx = std::clamp(int(std::lround(ax)), -TILT_MAX, TILT_MAX);
    ty = std::clamp(int(std::lround(ay)), -TILT_MAX, TILT_MAX);
}

void PenTranslator::proximityOut(std::vector<input_event> &out) {
    emit(out, EV_KEY, BTN_TOUCH, 0);
    emit(out, EV_ABS, ABS_PRESSURE, 0);
    emit(out, EV_KEY, BTN_STYLUS, 0);
    emit(out, EV_KEY, tool_, 0);
    syn(out);
    in_prox_ = false;
    touching_ = false;
}

void PenTranslator::release(std::vector<input_event> &out) {
    if (in_prox_) proximityOut(out);
}

void PenTranslator::onSample(const Sample &s, std::vector<input_event> &out) {
    if (!(s.flags & FLAG_IN_RANGE)) {
        release(out);
        return;
    }

    bool button = s.flags & (FLAG_BTN_PRIMARY | FLAG_BTN_SECONDARY);
    bool touch = s.flags & FLAG_TOUCH;

    // Never switch tools in the middle of a stroke; the switch happens on the
    // next hover or contact.
    if (!touching_) {
        bool eraser = (s.flags & FLAG_ERASER) || (cfg_.button == ButtonMode::Eraser && button);
        int tool = eraser ? BTN_TOOL_RUBBER : BTN_TOOL_PEN;
        if (in_prox_ && tool != tool_) proximityOut(out);
        tool_ = tool;
    }

    int x = int(std::lround((rect_[0] + s.x / 65535.0 * (rect_[2] - rect_[0])) * ABS_XY_MAX));
    int y = int(std::lround((rect_[1] + s.y / 65535.0 * (rect_[3] - rect_[1])) * ABS_XY_MAX));
    int tx, ty;
    mapTilt(s.tilt, s.orientation, tx, ty);

    if (!in_prox_) {
        // Enter proximity with zero pressure first: libinput derives a
        // "worn pen" pressure offset from the pressure seen at proximity-in.
        emit(out, EV_KEY, tool_, 1);
        emit(out, EV_ABS, ABS_X, x);
        emit(out, EV_ABS, ABS_Y, y);
        emit(out, EV_ABS, ABS_TILT_X, tx);
        emit(out, EV_ABS, ABS_TILT_Y, ty);
        emit(out, EV_ABS, ABS_PRESSURE, 0);
        syn(out);
        in_prox_ = true;
    }

    emit(out, EV_ABS, ABS_X, x);
    emit(out, EV_ABS, ABS_Y, y);
    emit(out, EV_ABS, ABS_TILT_X, tx);
    emit(out, EV_ABS, ABS_TILT_Y, ty);
    emit(out, EV_ABS, ABS_PRESSURE, touch ? mapPressure(s.pressure) : 0);
    emit(out, EV_KEY, BTN_TOUCH, touch ? 1 : 0);
    emit(out, EV_KEY, BTN_STYLUS, (cfg_.button == ButtonMode::RightClick && button) ? 1 : 0);
    syn(out);
    touching_ = touch;

    if (cfg_.verbose) {
        fprintf(stderr, "t=%10u f=%02x x=%5u y=%5u p=%5u tilt=%6d or=%6d -> abs %5d,%5d p=%5d tilt %5d,%5d %s\n",
                s.time_us, s.flags, s.x, s.y, s.pressure, s.tilt, s.orientation, x, y,
                touch ? mapPressure(s.pressure) : 0, tx, ty,
                tool_ == BTN_TOOL_RUBBER ? "eraser" : "pen");
    }
}

// ---------------------------------------------------------------------------

namespace {

bool setupAbs(int fd, int code, int min, int max, int res) {
    uinput_abs_setup abs{};
    abs.code = uint16_t(code);
    abs.absinfo.minimum = min;
    abs.absinfo.maximum = max;
    abs.absinfo.resolution = res;
    return ioctl(fd, UI_SET_ABSBIT, code) == 0 && ioctl(fd, UI_ABS_SETUP, &abs) == 0;
}

} // namespace

bool UinputPen::open(const char *name) {
    fd_ = ::open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (fd_ < 0) {
        fprintf(stderr, "cannot open /dev/uinput: %s (install the udev rules, see README)\n",
                strerror(errno));
        return false;
    }
    bool ok = ioctl(fd_, UI_SET_EVBIT, EV_SYN) == 0 && ioctl(fd_, UI_SET_EVBIT, EV_KEY) == 0 &&
              ioctl(fd_, UI_SET_EVBIT, EV_ABS) == 0 &&
              ioctl(fd_, UI_SET_PROPBIT, INPUT_PROP_DIRECT) == 0;
    for (int key : {BTN_TOOL_PEN, BTN_TOOL_RUBBER, BTN_TOUCH, BTN_STYLUS})
        ok = ok && ioctl(fd_, UI_SET_KEYBIT, key) == 0;
    // Resolution in units/mm describes the Tab S9+ panel (~267 x 167 mm);
    // libinput refuses tablets without a resolution.
    ok = ok && setupAbs(fd_, ABS_X, 0, ABS_XY_MAX, 245) && setupAbs(fd_, ABS_Y, 0, ABS_XY_MAX, 392) &&
         setupAbs(fd_, ABS_PRESSURE, 0, ABS_PRESSURE_MAX, 0) &&
         setupAbs(fd_, ABS_TILT_X, -TILT_MAX, TILT_MAX, TILT_RESOLUTION) &&
         setupAbs(fd_, ABS_TILT_Y, -TILT_MAX, TILT_MAX, TILT_RESOLUTION);

    uinput_setup setup{};
    snprintf(setup.name, sizeof(setup.name), "%s", name);
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = UINPUT_VENDOR;
    setup.id.product = UINPUT_PRODUCT;
    setup.id.version = 2;
    ok = ok && ioctl(fd_, UI_DEV_SETUP, &setup) == 0 && ioctl(fd_, UI_DEV_CREATE) == 0;
    if (!ok) {
        fprintf(stderr, "uinput device setup failed: %s\n", strerror(errno));
        close();
        return false;
    }
    return true;
}

void UinputPen::write(const std::vector<input_event> &events) {
    if (fd_ < 0 || events.empty()) return;
    // One syscall for the whole batch.
    const char *p = reinterpret_cast<const char *>(events.data());
    size_t left = events.size() * sizeof(input_event);
    while (left > 0) {
        ssize_t n = ::write(fd_, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "uinput write failed: %s\n", strerror(errno));
            return;
        }
        p += n;
        left -= size_t(n);
    }
}

void UinputPen::close() {
    if (fd_ < 0) return;
    ioctl(fd_, UI_DEV_DESTROY);
    ::close(fd_);
    fd_ = -1;
}

} // namespace vp
