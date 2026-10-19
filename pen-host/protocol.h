// Wire protocol between the Android app and the host (protocol v3).
//
// Uplink (tablet -> host) is a sequence of fixed 16-byte little-endian
// records. The Android side writes all samples of one MotionEvent
// (historical + current) in a single write, so one USB transfer usually
// carries one complete batch.
//
//   SAMPLE  [0]  'S'
//           [1]  flags (FLAG_*)
//           [2]  u16 x          0..65535 across the writing area width
//           [4]  u16 y          0..65535 across the writing area height
//           [6]  u16 pressure   0..65535 (MotionEvent pressure 0..1)
//           [8]  i16 tilt       1e-4 rad, 0 = perpendicular, ~15708 = flat
//           [10] i16 orient     1e-4 rad, -pi..pi, 0 = top of pen points up
//           [12] u32 time_us    event time, low 32 bits in microseconds
//
//   HELLO   [0]  'H'
//           [1]  protocol version
//           [2]  u16 writing area width  (px)
//           [4]  u16 writing area height (px)
//           [6]  u16 xdpi * 10
//           [8]  u16 ydpi * 10
//           [10..15] reserved
//
//   COMMAND [0]  'K'
//           [1]  command (CMD_*)
//           [2]  i16 a
//           [4]  i16 b
//           [6..15] reserved
//
// Downlink (host -> tablet) is a stream of messages with an 8-byte header:
//   [0] type (DL_*)  [1] flags  [2..3] reserved  [4..7] u32 payload length
//
//   DL_GEOMETRY  u16 monitor w, h; u16 patch x, y, w, h (monitor px);
//                u8 preset; u8 flags (GEO_*); u16 zoom * 100
//   DL_CODEC     u16 video w, h; u16 sps length; u16 pps length; SPS; PPS
//                (both with Annex-B start codes)
//   DL_FRAME     one H.264 access unit, Annex-B. flags bit 0 = keyframe
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vp {

constexpr uint8_t PKT_SAMPLE = 'S';
constexpr uint8_t PKT_HELLO = 'H';
constexpr uint8_t PKT_COMMAND = 'K';
constexpr uint8_t PROTOCOL_VERSION = 3;
constexpr size_t PKT_SIZE = 16;

enum : uint8_t {
    FLAG_IN_RANGE = 1 << 0,   // pen is hovering or touching
    FLAG_TOUCH = 1 << 1,      // pen tip is in contact
    FLAG_BTN_PRIMARY = 1 << 2,
    FLAG_BTN_SECONDARY = 1 << 3,
    FLAG_ERASER = 1 << 4,     // Android reported TOOL_TYPE_ERASER
};

enum : uint8_t {
    CMD_UNDO = 1,
    CMD_REDO = 2,
    CMD_ERASER = 3,       // a = 1 on, 0 off
    CMD_PATCH_STEP = 4,   // a, b = steps of half a patch (right, down)
    CMD_PATCH_NEXT = 5,   // next block below, keeping a little overlap
    CMD_PATCH_PRESET = 6, // a = preset index
    CMD_PATCH_DRAG = 7,   // a, b = finger movement in writing-area px (map-style)
    CMD_SCROLL = 8,       // a, b = finger movement in writing-area px
    CMD_KEYFRAME = 9,     // decoder lost sync
};

enum : uint8_t { DL_GEOMETRY = 'G', DL_CODEC = 'C', DL_FRAME = 'V' };
enum : uint8_t { DL_FLAG_KEYFRAME = 1 };
enum : uint8_t { GEO_ERASER = 1, GEO_PREVIEW = 2 };
constexpr size_t DL_HEADER_SIZE = 8;

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
    double xdpi, ydpi; // 0 if unknown
};

struct Command {
    uint8_t cmd;
    int16_t a, b;
};

inline uint16_t rd16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline void wr16(uint8_t *p, unsigned v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
inline void wr32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i));
}

// Builds a downlink message: header followed by the payload.
inline std::vector<uint8_t> downlinkMessage(uint8_t type, uint8_t flags, const uint8_t *payload, size_t len) {
    std::vector<uint8_t> m(DL_HEADER_SIZE + len);
    m[0] = type;
    m[1] = flags;
    wr32(&m[4], uint32_t(len));
    if (len) memcpy(&m[DL_HEADER_SIZE], payload, len);
    return m;
}

// Reassembles records from an arbitrary byte stream (USB packets or TCP).
class Decoder {
public:
    template <typename OnSample, typename OnHello, typename OnCommand>
    void feed(const uint8_t *data, size_t len, OnSample &&onSample, OnHello &&onHello, OnCommand &&onCommand) {
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
            if (!dispatch(buf_, onSample, onHello, onCommand)) resync(onSample, onHello, onCommand);
        }
        if (pending_ > 0) return;  // input exhausted while carrying a partial record
        while (len >= PKT_SIZE) {
            if (dispatch(data, onSample, onHello, onCommand)) {
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

    template <typename OnSample, typename OnHello, typename OnCommand>
    bool dispatch(const uint8_t *p, OnSample &onSample, OnHello &onHello, OnCommand &onCommand) {
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
            h.xdpi = rd16(p + 6) / 10.0;
            h.ydpi = rd16(p + 8) / 10.0;
            onHello(h);
            return true;
        }
        if (p[0] == PKT_COMMAND) {
            Command c;
            c.cmd = p[1];
            c.a = int16_t(rd16(p + 2));
            c.b = int16_t(rd16(p + 4));
            onCommand(c);
            return true;
        }
        return false;
    }

    template <typename OnSample, typename OnHello, typename OnCommand>
    void resync(OnSample &onSample, OnHello &onHello, OnCommand &onCommand) {
        ++dropped_;
        uint8_t tmp[PKT_SIZE - 1];
        memcpy(tmp, buf_ + 1, sizeof(tmp));
        pending_ = 0;
        feed(tmp, sizeof(tmp), onSample, onHello, onCommand);
    }
};

} // namespace vp
