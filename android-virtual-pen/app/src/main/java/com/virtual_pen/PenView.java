package com.virtual_pen;

import android.content.Context;
import android.graphics.Rect;
import android.os.SystemClock;
import android.util.DisplayMetrics;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.PointerIcon;
import android.view.View;

import java.util.Collections;

/**
 * Transparent input layer over the writing area. Forwards S Pen samples to
 * the PC, feeds the wet-ink layer, and turns finger gestures into commands:
 * two fingers scroll the page, one finger drags the patch in move mode.
 */
final class PenView extends View {
    // Android sends HOVER_EXIT right before the pen touches down and may stop
    // reporting after lift-off, so leaving proximity is only reported if no
    // further pen event follows within these delays.
    private static final long HOVER_EXIT_GRACE_MS = 80;
    private static final long LIFT_EXIT_GRACE_MS = 200;
    // Fingers are ignored this long after the pen was near: that is the palm.
    private static final long PALM_GUARD_MS = 400;

    interface GestureListener {
        /** Content under the ink is about to move. */
        void onContentMoving();
    }

    private final PenLink link;
    private final InkLayer ink;
    private final GestureListener gestures;
    private final byte[] batch = new byte[PenProtocol.MAX_BATCH_BYTES];
    private final Runnable leaveRange = this::sendOutOfRange;
    private final PointerIcon hiddenPointer;
    private boolean inRange;
    private boolean eraserMode;
    private boolean moveMode;
    private long lastPenMs;

    // Finger gesture state.
    private boolean gestureActive;
    private float lastFx, lastFy, carryX, carryY;

    PenView(Context context, PenLink link, InkLayer ink, GestureListener gestures) {
        super(context);
        this.link = link;
        this.ink = ink;
        this.gestures = gestures;
        setFocusable(true);
        setFocusableInTouchMode(true);
        // Resolved once: onResolvePointerIcon runs on every hover event.
        hiddenPointer = PointerIcon.getSystemIcon(context, PointerIcon.TYPE_NULL);
    }

    /** A new link starts with the pen out of proximity, so the next sample re-sends HELLO. */
    void onLinkConnected() {
        inRange = false;
    }

    void setEraserMode(boolean on) {
        eraserMode = on;
    }

    void setMoveMode(boolean on) {
        moveMode = on;
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        // Deliver every pointer sample as it arrives instead of batching to vsync.
        requestUnbufferedDispatch(InputDevice.SOURCE_CLASS_POINTER);
    }

    @Override
    protected void onLayout(boolean changed, int l, int t, int r, int b) {
        super.onLayout(changed, l, t, r, b);
        // Keep edge swipes from opening back/home gestures mid-stroke (the
        // system caps how much of each edge it honours).
        setSystemGestureExclusionRects(Collections.singletonList(new Rect(0, 0, r - l, b - t)));
    }

    @Override
    public PointerIcon onResolvePointerIcon(MotionEvent event, int pointerIndex) {
        return hiddenPointer;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int idx = stylusIndex(e);
        if (idx < 0) return onFingers(e);
        endGesture();
        int flags;
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                requestUnbufferedDispatch(e);
                startStroke();
                flags = PenProtocol.FLAG_IN_RANGE | PenProtocol.FLAG_TOUCH;
                break;
            case MotionEvent.ACTION_MOVE:
                flags = PenProtocol.FLAG_IN_RANGE | PenProtocol.FLAG_TOUCH;
                break;
            case MotionEvent.ACTION_POINTER_DOWN:
                if (e.getActionIndex() != idx) return true;
                startStroke();
                flags = PenProtocol.FLAG_IN_RANGE | PenProtocol.FLAG_TOUCH;
                break;
            case MotionEvent.ACTION_UP:
                forward(e, idx, PenProtocol.FLAG_IN_RANGE);
                postLeaveRange(LIFT_EXIT_GRACE_MS);
                return true;
            case MotionEvent.ACTION_POINTER_UP:
                if (e.getActionIndex() != idx) return true;
                forward(e, idx, PenProtocol.FLAG_IN_RANGE);
                postLeaveRange(LIFT_EXIT_GRACE_MS);
                return true;
            case MotionEvent.ACTION_CANCEL:
                sendOutOfRange();
                return true;
            default:
                return true;
        }
        removeCallbacks(leaveRange);
        forward(e, idx, flags);
        return true;
    }

    @Override
    public boolean onHoverEvent(MotionEvent e) {
        int idx = stylusIndex(e);
        if (idx < 0) return false;
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_HOVER_ENTER:
            case MotionEvent.ACTION_HOVER_MOVE:
                removeCallbacks(leaveRange);
                forward(e, idx, PenProtocol.FLAG_IN_RANGE);
                return true;
            case MotionEvent.ACTION_HOVER_EXIT:
                postLeaveRange(HOVER_EXIT_GRACE_MS);
                return true;
            default:
                return false;
        }
    }

    private void startStroke() {
        ink.beginStroke();
    }

    // ---------------------------------------------------------------------
    // Fingers

    private boolean onFingers(MotionEvent e) {
        int action = e.getActionMasked();
        boolean palmCancel = action == MotionEvent.ACTION_CANCEL
                || (action == MotionEvent.ACTION_POINTER_UP
                    && (e.getFlags() & MotionEvent.FLAG_CANCELED) != 0);
        if (palmCancel || action == MotionEvent.ACTION_UP) {
            endGesture();
            return true;
        }
        // Palm rejection: a hand resting on the glass while writing.
        if (inRange || SystemClock.uptimeMillis() - lastPenMs < PALM_GUARD_MS) {
            endGesture();
            return true;
        }
        int fingers = e.getPointerCount();
        int needed = moveMode ? 1 : 2;
        if (fingers != needed) {
            endGesture();
            return true;
        }
        float fx = 0, fy = 0;
        for (int i = 0; i < fingers; i++) {
            fx += e.getX(i);
            fy += e.getY(i);
        }
        fx /= fingers;
        fy /= fingers;
        if (!gestureActive || action == MotionEvent.ACTION_POINTER_DOWN || action == MotionEvent.ACTION_DOWN) {
            gestureActive = true;
            lastFx = fx;
            lastFy = fy;
            carryX = carryY = 0;
            return true;
        }
        if (action != MotionEvent.ACTION_MOVE) return true;
        carryX += fx - lastFx;
        carryY += fy - lastFy;
        lastFx = fx;
        lastFy = fy;
        int dx = (int) carryX, dy = (int) carryY;
        if (dx == 0 && dy == 0) return true;
        carryX -= dx;
        carryY -= dy;
        gestures.onContentMoving();
        link.sendCommand(moveMode ? PenProtocol.CMD_PATCH_DRAG : PenProtocol.CMD_SCROLL, dx, dy);
        return true;
    }

    private void endGesture() {
        gestureActive = false;
    }

    // ---------------------------------------------------------------------
    // Pen

    private void postLeaveRange(long delayMs) {
        removeCallbacks(leaveRange);
        postDelayed(leaveRange, delayMs);
    }

    private static int stylusIndex(MotionEvent e) {
        for (int i = 0, n = e.getPointerCount(); i < n; i++) {
            int tool = e.getToolType(i);
            if (tool == MotionEvent.TOOL_TYPE_STYLUS || tool == MotionEvent.TOOL_TYPE_ERASER) return i;
        }
        return -1;
    }

    /** Sends all historical samples of the event plus the current one in one write. */
    private void forward(MotionEvent e, int idx, int stateFlags) {
        lastPenMs = SystemClock.uptimeMillis();
        int flags = stateFlags;
        if (e.getToolType(idx) == MotionEvent.TOOL_TYPE_ERASER) flags |= PenProtocol.FLAG_ERASER;
        int buttons = e.getButtonState();
        if ((buttons & (MotionEvent.BUTTON_STYLUS_PRIMARY | MotionEvent.BUTTON_SECONDARY)) != 0)
            flags |= PenProtocol.FLAG_BTN_PRIMARY;
        if ((buttons & (MotionEvent.BUTTON_STYLUS_SECONDARY | MotionEvent.BUTTON_TERTIARY)) != 0)
            flags |= PenProtocol.FLAG_BTN_SECONDARY;
        // No wet ink while erasing: the trail would show ink that is being removed.
        boolean wet = (stateFlags & PenProtocol.FLAG_TOUCH) != 0 && !eraserMode
                && (flags & (PenProtocol.FLAG_ERASER | PenProtocol.FLAG_BTN_PRIMARY)) == 0;

        float sx = 1f / Math.max(1, getWidth());
        float sy = 1f / Math.max(1, getHeight());
        int off = 0;
        if (!inRange) {
            // Re-announce the surface on every proximity-in so a restarted host
            // learns the writing area without a reconnect.
            DisplayMetrics dm = getResources().getDisplayMetrics();
            off = PenProtocol.putHello(batch, off, getWidth(), getHeight(), dm.xdpi, dm.ydpi);
            inRange = true;
        }
        for (int h = 0, n = e.getHistorySize(); h < n; h++) {
            float x = e.getHistoricalX(idx, h), y = e.getHistoricalY(idx, h);
            float p = e.getHistoricalPressure(idx, h);
            if (wet) ink.addPoint(x, y, p);
            off = PenProtocol.putSample(batch, off, flags, x * sx, y * sy, p,
                    e.getHistoricalAxisValue(MotionEvent.AXIS_TILT, idx, h),
                    e.getHistoricalOrientation(idx, h),
                    e.getHistoricalEventTimeNanos(h));
            if (off == batch.length) {
                link.send(batch, off);
                off = 0;
            }
        }
        float x = e.getX(idx), y = e.getY(idx), p = e.getPressure(idx);
        if (wet) ink.addPoint(x, y, p);
        off = PenProtocol.putSample(batch, off, flags, x * sx, y * sy, p,
                e.getAxisValue(MotionEvent.AXIS_TILT, idx), e.getOrientation(idx), e.getEventTimeNanos());
        link.send(batch, off);
    }

    private void sendOutOfRange() {
        removeCallbacks(leaveRange);
        if (!inRange) return;
        inRange = false;
        lastPenMs = SystemClock.uptimeMillis();
        int off = PenProtocol.putSample(batch, 0, 0, 0f, 0f, 0f, 0f, 0f, System.nanoTime());
        link.send(batch, off);
    }
}
