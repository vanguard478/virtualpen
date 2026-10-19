#include "patch.h"

#include <algorithm>
#include <cmath>

namespace vp {

constexpr double Patch::kPresets[];

namespace {

// Used when the monitor or tablet does not report its physical size.
constexpr double kFallbackMonitorMmPerPx = 25.4 / 96.0;
constexpr double kFallbackTabletDpi = 266.0; // Galaxy Tab S9+
// "Next block" keeps this fraction of the previous block visible at the top.
constexpr double kNextOverlap = 0.15;

int even(double v) { return std::max(2, int(std::lround(v / 2.0)) * 2); }

} // namespace

void Patch::setMonitor(const MonitorGeom &m) {
    mon_ = m;
    recompute();
}

bool Patch::setTablet(int w_px, int h_px, double xdpi, double ydpi) {
    if (w_px == tab_w_ && h_px == tab_h_ && xdpi == tab_xdpi_ && ydpi == tab_ydpi_) return false;
    tab_w_ = w_px;
    tab_h_ = h_px;
    tab_xdpi_ = xdpi;
    tab_ydpi_ = ydpi;
    recompute();
    return true;
}

void Patch::setPreset(int index) {
    preset_ = std::clamp(index, 0, kPresetCount - 1);
    recompute();
}

void Patch::step(int dx, int dy) {
    cx_ += dx * w / 2.0;
    cy_ += dy * h / 2.0;
    recompute();
}

void Patch::next() {
    cy_ += h * (1.0 - kNextOverlap);
    recompute();
}

void Patch::drag(double dx_tab, double dy_tab) {
    // Dragging the page right reveals what is to its left.
    cx_ -= dx_tab * monitorPxPerTabletPx();
    cy_ -= dy_tab * monitorPxPerTabletPx();
    recompute();
}

void Patch::normalized(double r[4]) const {
    r[0] = double(x) / mon_.w;
    r[1] = double(y) / mon_.h;
    r[2] = double(x + w) / mon_.w;
    r[3] = double(y + h) / mon_.h;
}

void Patch::recompute() {
    if (!ready()) return;
    // Monitor pixels are square, so the width alone sets the scale; the
    // height follows from the writing area's aspect ratio, which keeps the
    // video undistorted on the tablet and the pen aligned with it.
    double mm_px = mon_.mm_w > 0 ? mon_.mm_w / mon_.w : kFallbackMonitorMmPerPx;
    double tab_mm_w = tab_w_ / (tab_xdpi_ > 0 ? tab_xdpi_ : kFallbackTabletDpi) * 25.4;
    double pw = tab_mm_w * zoom() / mm_px;
    double ph = pw * tab_h_ / tab_w_;
    double fit = std::min({1.0, mon_.w / pw, mon_.h / ph});
    pw *= fit;
    ph *= fit;
    w = std::min(even(pw), mon_.w - mon_.w % 2);
    h = std::min(even(ph), mon_.h - mon_.h % 2);

    if (cx_ < 0 || cy_ < 0) {
        cx_ = mon_.w / 2.0;
        cy_ = mon_.h / 2.0;
    }
    x = std::clamp(int(std::lround(cx_ - w / 2.0)), 0, mon_.w - w);
    y = std::clamp(int(std::lround(cy_ - h / 2.0)), 0, mon_.h - h);
    // Pull the center back inside so repeated steps against an edge don't accumulate.
    cx_ = x + w / 2.0;
    cy_ = y + h / 2.0;
}

} // namespace vp
