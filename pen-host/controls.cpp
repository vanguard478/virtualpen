#include "controls.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace vp {

namespace {

int createDevice(const char *name, uint16_t product, const std::vector<int> &keys, const std::vector<int> &rels) {
    int fd = ::open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    bool ok = ioctl(fd, UI_SET_EVBIT, EV_SYN) == 0 && ioctl(fd, UI_SET_EVBIT, EV_KEY) == 0;
    for (int k : keys) ok = ok && ioctl(fd, UI_SET_KEYBIT, k) == 0;
    if (!rels.empty()) ok = ok && ioctl(fd, UI_SET_EVBIT, EV_REL) == 0;
    for (int r : rels) ok = ok && ioctl(fd, UI_SET_RELBIT, r) == 0;
    uinput_setup setup{};
    snprintf(setup.name, sizeof(setup.name), "%s", name);
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0x1701;
    setup.id.product = product;
    setup.id.version = 1;
    ok = ok && ioctl(fd, UI_DEV_SETUP, &setup) == 0 && ioctl(fd, UI_DEV_CREATE) == 0;
    if (!ok) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void writeEvents(int fd, const std::vector<input_event> &ev) {
    if (fd < 0 || ev.empty()) return;
    ssize_t n = ::write(fd, ev.data(), ev.size() * sizeof(input_event));
    (void)n;
}

void push(std::vector<input_event> &ev, int type, int code, int value) {
    input_event e{};
    e.type = uint16_t(type);
    e.code = uint16_t(code);
    e.value = value;
    ev.push_back(e);
}

} // namespace

bool Controls::open() {
    kbd_ = createDevice("Virtual Pen Keys", 0x1703, {KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_Z, KEY_Y}, {});
    // A wheel without REL_X/REL_Y and a button is not classified as a mouse.
    mouse_ = createDevice("Virtual Pen Wheel", 0x1704, {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE},
                          {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES});
    if (kbd_ < 0 || mouse_ < 0) {
        fprintf(stderr, "warning: cannot create toolbar keyboard/wheel devices: %s\n", strerror(errno));
        return false;
    }
    return true;
}

void Controls::close() {
    for (int *fd : {&kbd_, &mouse_}) {
        if (*fd < 0) continue;
        ioctl(*fd, UI_DEV_DESTROY);
        ::close(*fd);
        *fd = -1;
    }
}

void Controls::chord(int modifier, int key) {
    std::vector<input_event> ev;
    push(ev, EV_KEY, modifier, 1);
    push(ev, EV_SYN, SYN_REPORT, 0);
    push(ev, EV_KEY, key, 1);
    push(ev, EV_SYN, SYN_REPORT, 0);
    push(ev, EV_KEY, key, 0);
    push(ev, EV_SYN, SYN_REPORT, 0);
    push(ev, EV_KEY, modifier, 0);
    push(ev, EV_SYN, SYN_REPORT, 0);
    writeEvents(kbd_, ev);
}

void Controls::undo() { chord(KEY_LEFTCTRL, KEY_Z); }
void Controls::redo() { chord(KEY_LEFTCTRL, KEY_Y); }

void Controls::scroll(double dx_px, double dy_px) {
    // 120 hi-res units = one notch. Wheel up (positive) moves content down,
    // so content follows the fingers when value = +dy; horizontal wheel right
    // (positive) moves content left, so value = -dx.
    double units_per_px = 120.0 / wheel_px_per_notch;
    int v = int(std::lround(dy_px * units_per_px));
    int hz = int(std::lround(-dx_px * units_per_px));
    if (v == 0 && hz == 0) return;
    std::vector<input_event> ev;
    if (v) {
        push(ev, EV_REL, REL_WHEEL_HI_RES, v);
        acc_v_ += v;
        int notches = int(acc_v_ / 120);
        if (notches) {
            push(ev, EV_REL, REL_WHEEL, notches);
            acc_v_ -= notches * 120;
        }
    }
    if (hz) {
        push(ev, EV_REL, REL_HWHEEL_HI_RES, hz);
        acc_h_ += hz;
        int notches = int(acc_h_ / 120);
        if (notches) {
            push(ev, EV_REL, REL_HWHEEL, notches);
            acc_h_ -= notches * 120;
        }
    }
    push(ev, EV_SYN, SYN_REPORT, 0);
    writeEvents(mouse_, ev);
}

} // namespace vp
