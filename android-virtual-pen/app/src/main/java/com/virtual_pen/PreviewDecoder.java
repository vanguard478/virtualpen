package com.virtual_pen;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * Hardware H.264 decoder for the patch preview, configured for low latency:
 * every decoded frame is rendered immediately. Frames are fed from the link's
 * connector thread; output is released on a private handler thread.
 */
final class PreviewDecoder {
    interface KeyframeRequester {
        void requestKeyframe();
    }

    private static final String TAG = "PreviewDecoder";
    private static final String MIME = MediaFormat.MIMETYPE_VIDEO_AVC;

    private final KeyframeRequester requester;
    private final HandlerThread outputThread = new HandlerThread("preview-output");
    private final Handler outputHandler;
    private final ArrayBlockingQueue<Integer> freeInputs = new ArrayBlockingQueue<>(64);

    private Surface surface;
    private MediaCodec codec;
    private byte[] sps, pps;
    private int width, height;
    private boolean waitingForKeyframe = true;
    private long lastKeyframeRequestMs;
    private long ptsUs;

    PreviewDecoder(KeyframeRequester requester) {
        this.requester = requester;
        outputThread.start();
        outputHandler = new Handler(outputThread.getLooper());
    }

    /** Called when the output surface appears or goes away (null). */
    synchronized void setSurface(Surface s) {
        surface = s;
        release();
        if (s != null && sps != null) start();
    }

    /** DL_CODEC payload: u16 w, h, sps length, pps length, SPS, PPS. */
    synchronized void onCodecConfig(byte[] b, int off, int len) {
        if (len < 8) return;
        int w = PenProtocol.rd16(b, off), h = PenProtocol.rd16(b, off + 2);
        int spsLen = PenProtocol.rd16(b, off + 4), ppsLen = PenProtocol.rd16(b, off + 6);
        if (8 + spsLen + ppsLen > len) return;
        byte[] newSps = new byte[spsLen], newPps = new byte[ppsLen];
        System.arraycopy(b, off + 8, newSps, 0, spsLen);
        System.arraycopy(b, off + 8 + spsLen, newPps, 0, ppsLen);
        if (codec != null && w == width && h == height
                && java.util.Arrays.equals(newSps, sps) && java.util.Arrays.equals(newPps, pps)) {
            return;
        }
        sps = newSps;
        pps = newPps;
        width = w;
        height = h;
        release();
        if (surface != null) start();
    }

    /** Called on the connector thread for each DL_FRAME. */
    void onFrame(byte[] b, int off, int len, boolean keyframe) {
        MediaCodec c;
        synchronized (this) {
            c = codec;
            if (c == null || (waitingForKeyframe && !keyframe)) {
                maybeRequestKeyframe();
                return;
            }
        }
        Integer index;
        try {
            // Wait briefly; past that the frame is stale anyway.
            index = freeInputs.poll(20, TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            return;
        }
        synchronized (this) {
            if (c != codec) return; // reconfigured meanwhile
            if (index == null) {
                // Dropping a frame breaks the reference chain until the next keyframe.
                waitingForKeyframe = true;
                maybeRequestKeyframe();
                return;
            }
            try {
                ByteBuffer in = c.getInputBuffer(index);
                if (in == null || in.capacity() < len) {
                    c.queueInputBuffer(index, 0, 0, 0, 0);
                    waitingForKeyframe = true;
                    maybeRequestKeyframe();
                    return;
                }
                in.clear();
                in.put(b, off, len);
                ptsUs += 16_666;
                c.queueInputBuffer(index, 0, len, ptsUs, keyframe ? MediaCodec.BUFFER_FLAG_KEY_FRAME : 0);
                if (keyframe) waitingForKeyframe = false;
            } catch (IllegalStateException e) {
                Log.w(TAG, "decoder rejected frame", e);
                waitingForKeyframe = true;
            }
        }
    }

    synchronized void close() {
        release();
        outputThread.quitSafely();
    }

    private void maybeRequestKeyframe() {
        long now = System.currentTimeMillis();
        if (now - lastKeyframeRequestMs < 250) return;
        lastKeyframeRequestMs = now;
        requester.requestKeyframe();
    }

    private void start() {
        try {
            MediaFormat f = MediaFormat.createVideoFormat(MIME, width, height);
            f.setByteBuffer("csd-0", ByteBuffer.wrap(sps));
            f.setByteBuffer("csd-1", ByteBuffer.wrap(pps));
            f.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
            f.setInteger(MediaFormat.KEY_PRIORITY, 0); // real-time
            f.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, Math.max(width * height * 3 / 2, 256 * 1024));
            MediaCodec c = MediaCodec.createDecoderByType(MIME);
            freeInputs.clear();
            c.setCallback(new MediaCodec.Callback() {
                @Override
                public void onInputBufferAvailable(MediaCodec mc, int index) {
                    freeInputs.offer(index);
                }

                @Override
                public void onOutputBufferAvailable(MediaCodec mc, int index, MediaCodec.BufferInfo info) {
                    try {
                        mc.releaseOutputBuffer(index, true); // render now
                    } catch (IllegalStateException ignored) {
                    }
                }

                @Override
                public void onError(MediaCodec mc, MediaCodec.CodecException e) {
                    Log.w(TAG, "decoder error", e);
                    synchronized (PreviewDecoder.this) {
                        waitingForKeyframe = true;
                    }
                    maybeRequestKeyframe();
                }

                @Override
                public void onOutputFormatChanged(MediaCodec mc, MediaFormat format) {
                }
            }, outputHandler);
            c.configure(f, surface, null, 0);
            c.setVideoScalingMode(MediaCodec.VIDEO_SCALING_MODE_SCALE_TO_FIT);
            c.start();
            codec = c;
            waitingForKeyframe = true;
            maybeRequestKeyframe();
            Log.i(TAG, "decoding " + width + "x" + height + " with " + c.getName());
        } catch (Exception e) {
            Log.e(TAG, "cannot start decoder", e);
            codec = null;
        }
    }

    private void release() {
        if (codec == null) return;
        try {
            codec.stop();
        } catch (IllegalStateException ignored) {
        }
        codec.release();
        codec = null;
        freeInputs.clear();
    }
}
