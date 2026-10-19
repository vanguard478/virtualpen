// Live preview of the patch: X11 screen capture -> H.264 -> tablet.
//
// Runs on its own thread and only ever hands complete frames to the
// downlink as droppable messages, so a slow link costs video frames, never
// pen latency.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "link.h"
#include "patch.h"

namespace vp {

struct PreviewOptions {
    int fps = 60;
    int bitrate_kbps = 12000;
    std::string encoder = "auto"; // auto | nvenc | x264
    bool verbose = false;
};

// Finds a monitor on the X screen ($DISPLAY) by RandR output name (as in
// --list-monitors); an empty name picks the primary monitor.
bool queryX11Monitor(const std::string &name, MonitorGeom &out, std::string &err);

// Byte ranges (start code included) of the NAL units in an Annex-B buffer.
std::vector<std::pair<size_t, size_t>> splitNals(const uint8_t *data, size_t len);
// Copies the SPS and PPS out of an access unit; false if either is missing.
bool extractParameterSets(const uint8_t *au, size_t len, std::vector<uint8_t> &sps, std::vector<uint8_t> &pps);

class Preview {
public:
    Preview(DownlinkSlot &slot, const PreviewOptions &opt) : slot_(slot), opt_(opt) {}
    ~Preview() { stop(); }

    // Captures from the given monitor; the region is relative to it.
    bool start(const MonitorGeom &monitor, std::string &err);
    void stop();
    // True while frames can be produced (capture and encoder working).
    bool running() const { return active_; }

    void setRegion(int x, int y, int w, int h);
    void requestKeyframe() { keyframe_ = true; }
    // A new connection needs the codec configuration again.
    void linkUp() { resendConfig_ = true; keyframe_ = true; }

private:
    DownlinkSlot &slot_;
    PreviewOptions opt_;
    MonitorGeom mon_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> keyframe_{true};
    std::atomic<bool> resendConfig_{true};
    std::atomic<bool> active_{false};

    std::mutex m_;
    int rx_ = 0, ry_ = 0, rw_ = 0, rh_ = 0;
    unsigned regionGen_ = 0;

    void run();
};

} // namespace vp
