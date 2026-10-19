// Wire protocol between the Android app and the host (protocol v2).
//
// The stream is a sequence of fixed 16-byte little-endian records. The
// Android side writes all samples of one MotionEvent (historical + current)
// in a single write, so one USB transfer usually carries one complete batch.
//
//   SAMPLE  [0]  'S'
//           [1]  flags (FLAG_*)
//           [2]  u16 x          0..65535 across the view width
//           [4]  u16 y          0..65535 across the view height
//           [6]  u16 pressure   0..65535 (MotionEvent pressure 0..1)
//           [8]  i16 tilt       1e-4 rad, 0 = perpendicular, ~15708 = flat
//           [10] i16 orient     1e-4 rad, -pi..pi, 0 = top of pen points up
//           [12] u32 time_us    event time, low 32 bits in microseconds
//
//   HELLO   [0]  'H'
//           [1]  protocol version
//           [2]  u16 view width  (px)
//           [4]  u16 view height (px)
//           [6..15] reserved
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace vp {

constexpr uint8_t PKT_SAMPLE = 'S';
constexpr uint8_t PKT_HELLO = 'H';
constexpr uint8_t PROTOCOL_VERSION = 2;
constexpr size_t PKT_SIZE = 16;

enum : uint8_t {
    FLAG_IN_RANGE = 1 << 0,   // pen is hovering or touching
    FLAG_TOUCH = 1 << 1,      // pen tip is in contact
    FLAG_BTN_PRIMARY = 1 << 2,
    FLAG_BTN_SECONDARY = 1 << 3,
    FLAG_ERASER = 1 << 4,     // Android reported TOOL_TYPE_ERASER
};

struct Sample {
    uint8_t flags;
    uint16_t x, y;
    uint16_t pressure;
    int16_t tilt;
    int16_t orientation;
    uint32_t time_us;
};

struct Hello {
    uint8_t version;
    uint16_t width, height;
};

inline uint16_t rd16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Reassembles records from an arbitrary byte stream (USB packets or TCP).
class Decoder {
public:
    template <typename OnSample, typename OnHello>
    void feed(const uint8_t *data, size_t len, OnSample &&onSample, OnHello &&onHello) {
        // Finish a record split across reads.
        while (pending_ > 0 && len > 0) {
            size_t take = PKT_SIZE - pending_;
            if (take > len) take = len;
            memcpy(buf_ + pending_, data, take);
            pending_ += take;
            data += take;
            len -= take;
            if (pending_ < PKT_SIZE) return;
            pending_ = 0;
            if (!dispatch(buf_, onSample, onHello)) resync(onSample, onHello);
        }
        if (pending_ > 0) return;  // input exhausted while carrying a partial record
        while (len >= PKT_SIZE) {
            if (dispatch(data, onSample, onHello)) {
                data += PKT_SIZE;
                len -= PKT_SIZE;
            } else {
                // Unknown type byte: slide forward one byte until we lock on again.
                ++dropped_;
                ++data;
                --len;
            }
        }
        memcpy(buf_, data, len);
        pending_ = len;
    }

    void reset() { pending_ = 0; }
    uint64_t dropped() const { return dropped_; }

private:
    uint8_t buf_[PKT_SIZE];
    size_t pending_ = 0;
    uint64_t dropped_ = 0;

    template <typename OnSample, typename OnHello>
    bool dispatch(const uint8_t *p, OnSample &onSample, OnHello &onHello) {
        if (p[0] == PKT_SAMPLE) {
            Sample s;
            s.flags = p[1];
            s.x = rd16(p + 2);
            s.y = rd16(p + 4);
            s.pressure = rd16(p + 6);
            s.tilt = int16_t(rd16(p + 8));
            s.orientation = int16_t(rd16(p + 10));
            s.time_us = rd32(p + 12);
            onSample(s);
            return true;
        }
        if (p[0] == PKT_HELLO) {
            Hello h;
            h.version = p[1];
            h.width = rd16(p + 2);
            h.height = rd16(p + 4);
            onHello(h);
            return true;
        }
        return false;
    }

    template <typename OnSample, typename OnHello>
    void resync(OnSample &onSample, OnHello &onHello) {
        ++dropped_;
        uint8_t tmp[PKT_SIZE - 1];
        memcpy(tmp, buf_ + 1, sizeof(tmp));
        pending_ = 0;
        feed(tmp, sizeof(tmp), onSample, onHello);
    }
};

} // namespace vp
