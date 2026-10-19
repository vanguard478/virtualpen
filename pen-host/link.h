// Transports that deliver the Android byte stream to the host and carry
// messages (patch geometry, preview video) back to the tablet.
#pragma once

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>

namespace vp {

using DataFn = std::function<void(const uint8_t *data, size_t len)>;

extern volatile sig_atomic_t g_stop;

// Host -> tablet. send() is thread-safe and never splits a message.
class Downlink {
public:
    virtual ~Downlink() = default;
    // droppable: may be refused instead of queued when the link is busy
    // (video frames); returns false if the message was not queued.
    virtual bool send(const uint8_t *data, size_t len, bool droppable) = 0;
};

// Gives other threads safe access to the current downlink, which comes and
// goes with the connection.
class DownlinkSlot {
public:
    void set(Downlink *d) {
        std::lock_guard<std::mutex> lock(m_);
        d_ = d;
    }
    bool send(const uint8_t *data, size_t len, bool droppable) {
        std::lock_guard<std::mutex> lock(m_);
        return d_ && d_->send(data, len, droppable);
    }
    bool connected() {
        std::lock_guard<std::mutex> lock(m_);
        return d_ != nullptr;
    }

private:
    std::mutex m_;
    Downlink *d_ = nullptr;
};

struct LinkEvents {
    std::function<void(Downlink *)> up; // connection established
    DataFn data;                        // uplink bytes
    std::function<void()> down;         // called before the downlink is destroyed
};

struct UsbOptions {
    int vid = 0x04e8;  // Samsung
    int pid = -1;      // any
    int transfers = 8; // bulk IN transfers kept in flight
};

// USB Android Open Accessory link. Switches the tablet into accessory mode,
// streams until it disconnects, reconnects; returns on g_stop.
void runUsb(const UsbOptions &opt, const LinkEvents &ev, bool verbose);

// TCP link (use with `adb reverse tcp:PORT tcp:PORT`). Accepts one client at
// a time on 127.0.0.1:port.
void runTcp(int port, const LinkEvents &ev);

} // namespace vp
