// The "patch": the rectangle of the monitor that the tablet's writing area
// is mapped to and that is streamed back as the preview. Pen mapping and
// video always use this one rectangle, so what the tablet shows is exactly
// what the pen writes on.
#pragma once

#include <string>

namespace vp {

struct MonitorGeom {
    std::string name;      // RandR output / connector, e.g. "DP-0"
    int x = 0, y = 0;      // position on the X screen
    int w = 0, h = 0;      // pixels
    double mm_w = 0, mm_h = 0;
    bool valid() const { return w > 0 && h > 0; }
};

class Patch {
public:
    // Magnification of handwriting on the monitor: 1 = ink appears the size
    // it was written; 2 = twice as large, covering more of the screen.
    static constexpr double kPresets[] = {1.0, 1.5, 2.0};
    static constexpr int kPresetCount = 3;

    void setMonitor(const MonitorGeom &m);
    // Writing area in pixels and its density. Returns true if the patch changed.
    bool setTablet(int w_px, int h_px, double xdpi, double ydpi);
    bool ready() const { return mon_.valid() && tab_w_ > 0 && tab_h_ > 0; }

    void setPreset(int index);
    void step(int dx, int dy); // in half patches
    void next();               // one block down, keeping the last lines visible
    void drag(double dx_tab, double dy_tab); // content follows the finger

    // Monitor-px per writing-area px (for scrolling).
    double monitorPxPerTabletPx() const { return tab_w_ > 0 ? double(w) / tab_w_ : 1.0; }
    // Patch in normalized monitor coordinates (x0, y0, x1, y1).
    void normalized(double r[4]) const;

    const MonitorGeom &monitor() const { return mon_; }
    int preset() const { return preset_; }
    double zoom() const { return kPresets[preset_]; }

    // Current patch in monitor pixels (even sizes, always inside the monitor).
    int x = 0, y = 0, w = 0, h = 0;

private:
    MonitorGeom mon_;
    int tab_w_ = 0, tab_h_ = 0;
    double tab_xdpi_ = 0, tab_ydpi_ = 0;
    int preset_ = 0;
    double cx_ = -1, cy_ = -1; // center in monitor px; -1 = not placed yet

    void recompute();
};

} // namespace vp
