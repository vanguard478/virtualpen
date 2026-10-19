package com.virtual_pen;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.PorterDuff;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/**
 * "Wet ink": the tablet draws the last moments of the pen's path itself, on
 * a transparent surface above the preview video, so a stroke appears under
 * the nib immediately. Only points younger than {@link #TAIL_MS} are drawn;
 * by then the real ink from OneNote has arrived in the video, so the local
 * trail hands over to it without a visible seam.
 *
 * <p>Drawing runs on its own thread straight into the surface, without
 * waiting for the UI thread's next frame.
 */
final class InkLayer implements SurfaceHolder.Callback {
    static final long TAIL_MS = 250;
    private static final int CAPACITY = 4096;
    private static final long FRAME_MS = 8; // 120 Hz panel

    private final SurfaceView view;
    private final HandlerThread thread = new HandlerThread("wet-ink");
    private final Handler handler;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Object lock = new Object();

    // Ring of points: x, y, pressure, time, stroke id.
    private final float[] xs = new float[CAPACITY], ys = new float[CAPACITY], ps = new float[CAPACITY];
    private final long[] ts = new long[CAPACITY];
    private final int[] ids = new int[CAPACITY];
    private int head, count, strokeId;
    private boolean drawPending, enabled = true;
    private float baseWidth = 4f;
    private volatile SurfaceHolder holder;

    private final Runnable draw = this::drawFrame;

    InkLayer(Context context) {
        view = new SurfaceView(context);
        view.setZOrderMediaOverlay(true); // above the video, below the window
        view.getHolder().setFormat(PixelFormat.TRANSLUCENT);
        view.getHolder().addCallback(this);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeCap(Paint.Cap.ROUND);
        paint.setColor(Color.rgb(20, 20, 30));
        thread.start();
        handler = new Handler(thread.getLooper());
    }

    SurfaceView getView() {
        return view;
    }

    void setStyle(int color, float widthPx) {
        synchronized (lock) {
            paint.setColor(color);
            baseWidth = widthPx;
        }
    }

    void setEnabled(boolean on) {
        synchronized (lock) {
            enabled = on;
            if (!on) count = 0;
        }
        requestDraw();
    }

    /** UI thread: starts a new stroke so it is not joined to the previous one. */
    void beginStroke() {
        synchronized (lock) {
            strokeId++;
        }
    }

    /** UI thread: adds a point in view coordinates. */
    void addPoint(float x, float y, float pressure) {
        synchronized (lock) {
            if (!enabled) return;
            int i = (head + count) % CAPACITY;
            if (count == CAPACITY) head = (head + 1) % CAPACITY;
            else count++;
            xs[i] = x;
            ys[i] = y;
            ps[i] = pressure;
            ts[i] = SystemClock.uptimeMillis();
            ids[i] = strokeId;
        }
        requestDraw();
    }

    /** The content under the ink moved (scroll, patch change): drop the trail. */
    void clear() {
        synchronized (lock) {
            count = 0;
        }
        requestDraw();
    }

    void release() {
        thread.quitSafely();
    }

    private void requestDraw() {
        synchronized (lock) {
            if (drawPending) return;
            drawPending = true;
        }
        handler.post(draw);
    }

    private void drawFrame() {
        SurfaceHolder h = holder;
        boolean more;
        synchronized (lock) {
            drawPending = false;
            long cutoff = SystemClock.uptimeMillis() - TAIL_MS;
            while (count > 0 && ts[head] < cutoff) {
                head = (head + 1) % CAPACITY;
                count--;
            }
            more = count > 0;
        }
        if (h == null) return;
        Canvas c;
        try {
            c = h.lockHardwareCanvas();
        } catch (IllegalStateException e) {
            return;
        }
        if (c == null) return;
        c.drawColor(Color.TRANSPARENT, PorterDuff.Mode.CLEAR);
        synchronized (lock) {
            for (int k = 1; k < count; k++) {
                int a = (head + k - 1) % CAPACITY, b = (head + k) % CAPACITY;
                if (ids[a] != ids[b]) continue;
                paint.setStrokeWidth(baseWidth * (0.35f + 0.65f * Math.min(1f, ps[b])));
                c.drawLine(xs[a], ys[a], xs[b], ys[b], paint);
            }
        }
        h.unlockCanvasAndPost(c);
        // Keep redrawing while the trail is shrinking.
        if (more) {
            synchronized (lock) {
                if (drawPending) return;
                drawPending = true;
            }
            handler.postDelayed(draw, FRAME_MS);
        }
    }

    @Override
    public void surfaceCreated(SurfaceHolder h) {
        holder = h;
        requestDraw(); // starts transparent
    }

    @Override
    public void surfaceChanged(SurfaceHolder h, int format, int width, int height) {
        requestDraw();
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder h) {
        holder = null;
        // Make sure the render thread is not inside lock/unlock when the surface goes.
        handler.removeCallbacks(draw);
        final Object done = new Object();
        synchronized (done) {
            handler.post(() -> {
                synchronized (done) {
                    done.notify();
                }
            });
            try {
                done.wait(100);
            } catch (InterruptedException ignored) {
            }
        }
        synchronized (lock) {
            drawPending = false;
        }
    }
}
