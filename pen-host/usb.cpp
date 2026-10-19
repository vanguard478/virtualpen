// Android Open Accessory (AOA) transport built on asynchronous libusb.
//
// Several bulk IN transfers of exactly wMaxPacketSize are kept queued, so a
// packet from the tablet completes a transfer immediately (no reliance on
// short packets / ZLPs) and there is never a gap without a pending request.
//
// The downlink uses a pool of bulk OUT transfers. A message is only queued
// if the pool can take all of it, so a busy link drops whole video frames
// and never blocks the pen.
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <libusb-1.0/libusb.h>
#include <mutex>
#include <thread>
#include <vector>

#include "link.h"

namespace vp {

namespace {

constexpr uint16_t AOA_VID = 0x18d1;
constexpr uint16_t AOA_PID = 0x2d00;     // accessory
constexpr uint16_t AOA_ADB_PID = 0x2d01; // accessory + adb

constexpr uint8_t AOA_GET_PROTOCOL = 51;
constexpr uint8_t AOA_SEND_STRING = 52;
constexpr uint8_t AOA_START = 53;

// Must match res/xml/accessory_filter.xml in the Android app.
const char *const kIdentity[] = {
    "virtualpen",               // manufacturer
    "S9PenHost",                // model
    "Virtual pen host for OneNote", // description
    "2",                        // version
    "https://github.com/vanguard478/virtualpen", // uri
    "0000000000000002",         // serial
};

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

libusb_device_handle *openAccessory(libusb_context *ctx) {
    for (uint16_t pid : {AOA_ADB_PID, AOA_PID}) {
        libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, AOA_VID, pid);
        if (h) return h;
    }
    return nullptr;
}

// Asks every matching device to restart in accessory mode. Returns true if
// at least one accepted.
bool switchToAccessory(libusb_context *ctx, const UsbOptions &opt, bool verbose) {
    libusb_device **list = nullptr;
    ssize_t n = libusb_get_device_list(ctx, &list);
    bool switched = false;
    for (ssize_t i = 0; i < n; i++) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
        if (desc.idVendor != opt.vid || (opt.pid >= 0 && desc.idProduct != opt.pid)) continue;

        libusb_device_handle *h = nullptr;
        int err = libusb_open(list[i], &h);
        if (err != 0) {
            fprintf(stderr, "found %04x:%04x but cannot open it (%s); install the udev rules\n",
                    desc.idVendor, desc.idProduct, libusb_error_name(err));
            continue;
        }
        uint8_t ver[2] = {0, 0};
        int r = libusb_control_transfer(h, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR,
                                        AOA_GET_PROTOCOL, 0, 0, ver, sizeof(ver), 1000);
        int aoa = (r == 2) ? (ver[0] | (ver[1] << 8)) : 0;
        if (aoa < 1) {
            if (verbose) fprintf(stderr, "%04x:%04x does not speak AOA\n", desc.idVendor, desc.idProduct);
            libusb_close(h);
            continue;
        }
        bool ok = true;
        for (uint16_t idx = 0; idx < 6 && ok; idx++) {
            const char *s = kIdentity[idx];
            ok = libusb_control_transfer(h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
                                         AOA_SEND_STRING, 0, idx, (uint8_t *)s,
                                         uint16_t(strlen(s) + 1), 1000) >= 0;
        }
        ok = ok && libusb_control_transfer(h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
                                           AOA_START, 0, 0, nullptr, 0, 1000) >= 0;
        fprintf(stderr, "%04x:%04x (AOA v%d): %s\n", desc.idVendor, desc.idProduct, aoa,
                ok ? "switching to accessory mode" : "accessory request failed");
        switched |= ok;
        libusb_close(h);
    }
    libusb_free_device_list(list, 1);
    return switched;
}

struct Stream {
    const DataFn *onData;
    int inFlight = 0;
    bool dead = false;
};

void LIBUSB_CALL onTransfer(libusb_transfer *t) {
    auto *st = static_cast<Stream *>(t->user_data);
    if (t->status == LIBUSB_TRANSFER_COMPLETED && !st->dead && !g_stop) {
        if (t->actual_length > 0) (*st->onData)(t->buffer, size_t(t->actual_length));
        if (libusb_submit_transfer(t) == 0) return;
        st->dead = true;
    } else if (t->status != LIBUSB_TRANSFER_CANCELLED) {
        if (!st->dead && !g_stop)
            fprintf(stderr, "usb transfer ended: status %d\n", t->status);
        st->dead = true;
    }
    st->inFlight--;
}

// The Android accessory driver reads at most 16 KiB per request.
constexpr int OUT_CHUNK = 16384;
constexpr int OUT_TRANSFERS = 96; // 1.5 MiB in flight: room for the largest keyframes

class UsbDownlink : public Downlink {
public:
    UsbDownlink(libusb_device_handle *h, uint8_t ep) {
        for (int i = 0; i < OUT_TRANSFERS; i++) {
            libusb_transfer *t = libusb_alloc_transfer(0);
            auto *buf = new uint8_t[OUT_CHUNK];
            // ADD_ZERO_PACKET ends transfers that are a multiple of the packet
            // size, so the tablet's read returns without waiting for more data.
            libusb_fill_bulk_transfer(t, h, ep, buf, OUT_CHUNK, onDone, this, 0);
            t->flags = LIBUSB_TRANSFER_ADD_ZERO_PACKET;
            all_.push_back(t);
            free_.push_back(t);
        }
    }

    ~UsbDownlink() override {
        for (libusb_transfer *t : all_) {
            delete[] t->buffer;
            libusb_free_transfer(t);
        }
    }

    bool send(const uint8_t *data, size_t len, bool droppable) override {
        std::lock_guard<std::mutex> lock(m_);
        if (dead_) return false;
        size_t need = (len + OUT_CHUNK - 1) / OUT_CHUNK;
        if (free_.size() < need) {
            if (!droppable) fprintf(stderr, "downlink congested, control message dropped\n");
            return false;
        }
        for (size_t off = 0; off < len; off += OUT_CHUNK) {
            libusb_transfer *t = free_.back();
            free_.pop_back();
            int n = int(std::min<size_t>(OUT_CHUNK, len - off));
            memcpy(t->buffer, data + off, size_t(n));
            t->length = n;
            if (libusb_submit_transfer(t) != 0) {
                free_.push_back(t);
                dead_ = true;
                return false;
            }
            inFlight_++;
        }
        return true;
    }

    // Stops accepting messages and cancels what is in flight.
    void shutdown() {
        std::lock_guard<std::mutex> lock(m_);
        dead_ = true;
        for (libusb_transfer *t : all_) libusb_cancel_transfer(t);
    }

    int inFlight() {
        std::lock_guard<std::mutex> lock(m_);
        return inFlight_;
    }

    bool dead() {
        std::lock_guard<std::mutex> lock(m_);
        return dead_;
    }

private:
    std::mutex m_;
    std::vector<libusb_transfer *> all_, free_;
    int inFlight_ = 0;
    bool dead_ = false;

    static void LIBUSB_CALL onDone(libusb_transfer *t) {
        auto *self = static_cast<UsbDownlink *>(t->user_data);
        std::lock_guard<std::mutex> lock(self->m_);
        if (t->status != LIBUSB_TRANSFER_COMPLETED && t->status != LIBUSB_TRANSFER_CANCELLED) {
            if (!self->dead_ && !g_stop) fprintf(stderr, "usb downlink transfer ended: status %d\n", t->status);
            self->dead_ = true;
        }
        self->free_.push_back(t);
        self->inFlight_--;
    }
};

// Finds the bulk endpoints of interface 0 (the accessory interface).
bool findBulkEndpoints(libusb_device *dev, uint8_t &in, int &maxPacket, uint8_t &out) {
    libusb_config_descriptor *cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0) return false;
    bool foundIn = false;
    if (cfg->bNumInterfaces > 0 && cfg->interface[0].num_altsetting > 0) {
        const libusb_interface_descriptor &alt = cfg->interface[0].altsetting[0];
        for (int i = 0; i < alt.bNumEndpoints; i++) {
            const libusb_endpoint_descriptor &e = alt.endpoint[i];
            if ((e.bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK) continue;
            if (e.bEndpointAddress & 0x80) {
                in = e.bEndpointAddress;
                maxPacket = e.wMaxPacketSize;
                foundIn = true;
            } else {
                out = e.bEndpointAddress;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return foundIn;
}

void stream(libusb_context *ctx, libusb_device_handle *h, const UsbOptions &opt, const LinkEvents &ev,
            bool verbose) {
    uint8_t ep = 0x81, epOut = 0x02;
    int maxPacket = 512;
    if (!findBulkEndpoints(libusb_get_device(h), ep, maxPacket, epOut))
        fprintf(stderr, "no bulk endpoints in descriptor, assuming 0x81/0x02\n");
    libusb_set_auto_detach_kernel_driver(h, 1);
    int err = libusb_claim_interface(h, 0);
    if (err != 0) {
        fprintf(stderr, "cannot claim accessory interface: %s\n", libusb_error_name(err));
        return;
    }
    if (verbose) fprintf(stderr, "bulk IN ep %#x (max packet %d), OUT ep %#x\n", ep, maxPacket, epOut);

    Stream st;
    st.onData = &ev.data;
    std::vector<libusb_transfer *> transfers;
    std::vector<std::vector<uint8_t>> buffers(size_t(opt.transfers), std::vector<uint8_t>(size_t(maxPacket)));
    for (int i = 0; i < opt.transfers; i++) {
        libusb_transfer *t = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(t, h, ep, buffers[size_t(i)].data(), maxPacket, onTransfer, &st, 0);
        transfers.push_back(t);
        if (libusb_submit_transfer(t) == 0) st.inFlight++;
    }

    UsbDownlink downlink(h, epOut);
    ev.up(&downlink);

    while (!g_stop && !st.dead && st.inFlight > 0 && !downlink.dead()) {
        timeval tv{0, 200000};
        int r = libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
        if (r != 0 && r != LIBUSB_ERROR_INTERRUPTED) break;
    }
    ev.down(); // after this nobody sends on the downlink any more
    st.dead = true;
    downlink.shutdown();
    for (libusb_transfer *t : transfers) libusb_cancel_transfer(t);
    for (int spins = 0; (st.inFlight > 0 || downlink.inFlight() > 0) && spins < 100; spins++) {
        timeval tv{0, 20000};
        libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
    }
    for (libusb_transfer *t : transfers) libusb_free_transfer(t);
    libusb_release_interface(h, 0);
}

} // namespace

void runUsb(const UsbOptions &opt, const LinkEvents &ev, bool verbose) {
    libusb_context *ctx = nullptr;
    if (int err = libusb_init(&ctx)) {
        fprintf(stderr, "libusb_init failed: %s\n", libusb_error_name(err));
        return;
    }
    auto lastSwitch = std::chrono::steady_clock::time_point{};
    bool waitingNoted = false;
    while (!g_stop) {
        if (libusb_device_handle *h = openAccessory(ctx)) {
            fprintf(stderr, "accessory connected, open the Virtual Pen app on the tablet\n");
            stream(ctx, h, opt, ev, verbose);
            libusb_close(h);
            fprintf(stderr, "accessory disconnected\n");
            waitingNoted = false;
            continue;
        }
        // Give a freshly switched device time to re-enumerate before poking again.
        auto now = std::chrono::steady_clock::now();
        if (now - lastSwitch > std::chrono::seconds(5)) {
            if (switchToAccessory(ctx, opt, verbose)) {
                lastSwitch = now;
            } else if (!waitingNoted) {
                char pid[12] = "*";
                if (opt.pid >= 0) snprintf(pid, sizeof(pid), "%04x", opt.pid);
                fprintf(stderr, "waiting for the tablet (usb %04x:%s)...\n", opt.vid, pid);
                waitingNoted = true;
            }
        }
        sleepMs(250);
    }
    libusb_exit(ctx);
}

} // namespace vp
