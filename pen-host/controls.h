// Virtual keyboard and wheel for toolbar actions (undo/redo) and two-finger
// scrolling. They act on whatever window is under the pen's cursor.
#pragma once

namespace vp {

class Controls {
public:
    bool open();
    void close();
    ~Controls() { close(); }

    void undo(); // Ctrl+Z
    void redo(); // Ctrl+Y
    // Content follows the fingers: dy < 0 (fingers move up) scrolls down.
    // Distances are in monitor pixels.
    void scroll(double dx_px, double dy_px);

    double wheel_px_per_notch = 53.0; // Chromium's scroll distance per wheel notch

private:
    int kbd_ = -1, mouse_ = -1;
    double acc_v_ = 0, acc_h_ = 0; // hi-res units not yet sent as whole notches

    void chord(int modifier, int key);
};

} // namespace vp
