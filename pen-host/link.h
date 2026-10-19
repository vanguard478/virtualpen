// Transports that deliver the Android byte stream to the host.
#pragma once

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace vp {

using DataFn = std::function<void(const uint8_t *data, size_t len)>;

extern volatile sig_atomic_t g_stop;

struct UsbOptions {
    int vid = 0x04e8;  // Samsung
    int pid = -1;      // any
    int transfers = 8; // bulk IN transfers kept in flight
};

// USB Android Open Accessory link. Switches the tablet into accessory mode,
// streams until it disconnects, then returns so the caller can reconnect.
// onConnect fires once the accessory endpoint is open.
void runUsb(const UsbOptions &opt, const std::function<void()> &onConnect, const DataFn &onData,
            const std::function<void()> &onDisconnect, bool verbose);

// TCP link (use with `adb reverse tcp:PORT tcp:PORT`). Accepts one client at
// a time on 127.0.0.1:port.
void runTcp(int port, const std::function<void()> &onConnect, const DataFn &onData,
            const std::function<void()> &onDisconnect);

} // namespace vp
