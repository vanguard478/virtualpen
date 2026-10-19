package com.virtual_pen;

/**
 * Binary wire format shared with pen-host/protocol.h (protocol v2): fixed
 * 16-byte little-endian records, no allocation on the hot path.
 */
final class PenProtocol {
    static final int VERSION = 2;
    static final int RECORD_SIZE = 16;
    // Keep a batch below 512 bytes so it always fits a single high-speed USB packet.
    static final int MAX_BATCH = 31;
    static final int MAX_BATCH_BYTES = MAX_BATCH * RECORD_SIZE;

    static final int FLAG_IN_RANGE = 1;
    static final int FLAG_TOUCH = 1 << 1;
    static final int FLAG_BTN_PRIMARY = 1 << 2;
    static final int FLAG_BTN_SECONDARY = 1 << 3;
    static final int FLAG_ERASER = 1 << 4;

    private static final byte TYPE_SAMPLE = 'S';
    private static final byte TYPE_HELLO = 'H';

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

    static int putHello(byte[] b, int o, int width, int height) {
        b[o] = TYPE_HELLO;
        b[o + 1] = (byte) VERSION;
        put16(b, o + 2, width);
        put16(b, o + 4, height);
        for (int i = 6; i < RECORD_SIZE; i++) b[o + i] = 0;
        return o + RECORD_SIZE;
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
