// Translates protocol samples into evdev events for a uinput tablet.
#pragma once

#include <linux/input.h>
#include <vector>

#include "protocol.h"

namespace vp {

// Axis ranges exposed by the virtual tablet.
constexpr int ABS_XY_MAX = 65535;
constexpr int ABS_PRESSURE_MAX = 65535;
constexpr int TILT_MAX = 9000;           // centi-degrees
constexpr int TILT_RESOLUTION = 5730;    // units per radian (kernel/libinput convention)

enum class ButtonMode { Eraser, RightClick, None };

struct PenConfig {
    // Target rectangle inside the area the compositor maps the tablet to,
    // in normalized coordinates (x0, y0, x1, y1).
    double area[4] = {0.0, 0.0, 1.0, 1.0};
    // If set, shrink the area so the tablet's aspect ratio is preserved on a
    // screen of this pixel size (handwriting is not distorted).
    int screen_w = 0, screen_h = 0;

    // Pressure curve: input is clamped to [p_min, p_max], normalized, raised to
    // gamma and lifted onto [tip_floor, 1]. tip_floor must stay above libinput's
    // 5% tip threshold, otherwise the start of light strokes is dropped.
    double p_min = 0.0, p_max = 1.0, gamma = 1.0, tip_floor = 0.06;

    ButtonMode button = ButtonMode::Eraser;
    bool tilt_flip_x = false, tilt_flip_y = false, tilt_swap = false;
    bool verbose = false;
};

class PenTranslator {
public:
    explicit PenTranslator(const PenConfig &cfg);

    void onHello(const Hello &h);
    // Appends the evdev frames for one sample. Each frame ends in SYN_REPORT.
    void onSample(const Sample &s, std::vector<input_event> &out);
    // Takes the pen out of proximity (link lost, shutdown).
    void release(std::vector<input_event> &out);

    // Exposed for tests and debugging.
    int mapPressure(uint16_t raw) const;
    void mapTilt(int16_t tilt, int16_t orientation, int &tx, int &ty) const;
    const double *mappedArea() const { return rect_; }

private:
    PenConfig cfg_;
    double rect_[4];
    int tablet_w_ = 2800, tablet_h_ = 1752;  // Galaxy Tab S9+ landscape

    bool in_prox_ = false;
    bool touching_ = false;
    int tool_ = BTN_TOOL_PEN;

    void updateRect();
    void proximityOut(std::vector<input_event> &out);
};

class UinputPen {
public:
    // Returns false and prints the reason on failure.
    bool open(const char *name);
    void write(const std::vector<input_event> &events);
    void close();
    ~UinputPen() { close(); }

private:
    int fd_ = -1;
};

} // namespace vp
