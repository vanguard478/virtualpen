package com.virtual_pen;

/**
 * Binary wire format shared with pen-host/protocol.h (protocol v3). Uplink:
 * fixed 16-byte little-endian records, no allocation on the hot path.
 * Downlink: messages with an 8-byte header (type, flags, 2 reserved, u32 length).
 */
final class PenProtocol {
    static final int VERSION = 3;
    static final int RECORD_SIZE = 16;
    // Keep a batch below 512 bytes so it always fits a single high-speed USB packet.
    static final int MAX_BATCH = 31;
    static final int MAX_BATCH_BYTES = MAX_BATCH * RECORD_SIZE;

    static final int FLAG_IN_RANGE = 1;
    static final int FLAG_TOUCH = 1 << 1;
    static final int FLAG_BTN_PRIMARY = 1 << 2;
    static final int FLAG_BTN_SECONDARY = 1 << 3;
    static final int FLAG_ERASER = 1 << 4;

    static final int CMD_UNDO = 1;
    static final int CMD_REDO = 2;
    static final int CMD_ERASER = 3;
    static final int CMD_PATCH_STEP = 4;
    static final int CMD_PATCH_NEXT = 5;
    static final int CMD_PATCH_PRESET = 6;
    static final int CMD_PATCH_DRAG = 7;
    static final int CMD_SCROLL = 8;
    static final int CMD_KEYFRAME = 9;

    static final int DL_HEADER_SIZE = 8;
    static final int DL_GEOMETRY = 'G';
    static final int DL_CODEC = 'C';
    static final int DL_FRAME = 'V';
    static final int DL_FLAG_KEYFRAME = 1;
    static final int GEO_ERASER = 1;
    static final int GEO_PREVIEW = 2;

    private static final byte TYPE_SAMPLE = 'S';
    private static final byte TYPE_HELLO = 'H';
    private static final byte TYPE_COMMAND = 'K';

    private PenProtocol() {}

    /**
     * @param nx          x in 0..1 across the view
     * @param ny          y in 0..1 across the view
     * @param pressure    MotionEvent pressure, 0..1
     * @param tilt        AXIS_TILT in radians
     * @param orientation AXIS_ORIENTATION in radians
     * @return offset after the record
     */
    static int putSample(byte[] b, int o, int flags, float nx, float ny, float pressure,
                         float tilt, float orientation, long timeNanos) {
        b[o] = TYPE_SAMPLE;
        b[o + 1] = (byte) flags;
        put16(b, o + 2, unit16(nx));
        put16(b, o + 4, unit16(ny));
        put16(b, o + 6, unit16(pressure));
        put16(b, o + 8, Math.round(tilt * 10000f));
        put16(b, o + 10, Math.round(orientation * 10000f));
        int us = (int) (timeNanos / 1000);
        b[o + 12] = (byte) us;
        b[o + 13] = (byte) (us >> 8);
        b[o + 14] = (byte) (us >> 16);
        b[o + 15] = (byte) (us >> 24);
        return o + RECORD_SIZE;
    }

    static int putHello(byte[] b, int o, int width, int height, float xdpi, float ydpi) {
        b[o] = TYPE_HELLO;
        b[o + 1] = (byte) VERSION;
        put16(b, o + 2, width);
        put16(b, o + 4, height);
        put16(b, o + 6, Math.round(xdpi * 10f));
        put16(b, o + 8, Math.round(ydpi * 10f));
        for (int i = 10; i < RECORD_SIZE; i++) b[o + i] = 0;
        return o + RECORD_SIZE;
    }

    static int putCommand(byte[] b, int o, int command, int a, int bArg) {
        b[o] = TYPE_COMMAND;
        b[o + 1] = (byte) command;
        put16(b, o + 2, clamp16(a));
        put16(b, o + 4, clamp16(bArg));
        for (int i = 6; i < RECORD_SIZE; i++) b[o + i] = 0;
        return o + RECORD_SIZE;
    }

    static int rd16(byte[] b, int o) {
        return (b[o] & 0xff) | ((b[o + 1] & 0xff) << 8);
    }

    static int rd32(byte[] b, int o) {
        return rd16(b, o) | (rd16(b, o + 2) << 16);
    }

    private static int clamp16(int v) {
        return Math.max(Short.MIN_VALUE, Math.min(Short.MAX_VALUE, v));
    }

    private static int unit16(float v) {
        if (!(v > 0f)) return 0; // also catches NaN
        if (v >= 1f) return 65535;
        return (int) (v * 65535f + 0.5f);
    }

    private static void put16(byte[] b, int o, int v) {
        b[o] = (byte) v;
        b[o + 1] = (byte) (v >> 8);
    }
}
