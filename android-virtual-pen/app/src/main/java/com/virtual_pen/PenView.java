package com.virtual_pen;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.PointerIcon;
import android.view.View;

import java.util.Collections;

/**
 * Full-screen surface that forwards S Pen samples to the PC. It renders
 * nothing per event; the only drawing is the status line.
 */
final class PenView extends View {
    // Android sends HOVER_EXIT right before the pen touches down and may stop
    // reporting after lift-off, so leaving proximity is only reported if no
    // further pen event follows within these delays.
    private static final long HOVER_EXIT_GRACE_MS = 80;
    private static final long LIFT_EXIT_GRACE_MS = 200;

    private final PenLink link;
    private final byte[] batch = new byte[PenProtocol.MAX_BATCH_BYTES];
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Runnable leaveRange = this::sendOutOfRange;
    private boolean inRange;
    private String status = "";

    PenView(Context context, PenLink link) {
        super(context);
        this.link = link;
        setBackgroundColor(Color.BLACK);
        textPaint.setColor(Color.DKGRAY);
        textPaint.setTextSize(28f);
        textPaint.setTextAlign(Paint.Align.CENTER);
        setFocusable(true);
        setFocusableInTouchMode(true);
    }

    void setStatus(String text) {
        status = text;
        invalidate();
    }

    /** A new link starts with the pen out of proximity, so the next sample re-sends HELLO. */
    void onLinkConnected() {
        inRange = false;
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
        return PointerIcon.getSystemIcon(getContext(), PointerIcon.TYPE_NULL);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        canvas.drawText(status, getWidth() / 2f, getHeight() - 40f, textPaint);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int idx = stylusIndex(e);
        if (idx < 0) return true; // swallow finger/palm touches
        int flags;
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                requestUnbufferedDispatch(e);
                flags = PenProtocol.FLAG_IN_RANGE | PenProtocol.FLAG_TOUCH;
                break;
            case MotionEvent.ACTION_MOVE:
                flags = PenProtocol.FLAG_IN_RANGE | PenProtocol.FLAG_TOUCH;
                break;
            case MotionEvent.ACTION_POINTER_DOWN:
                if (e.getActionIndex() != idx) return true;
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
        int flags = stateFlags;
        if (e.getToolType(idx) == MotionEvent.TOOL_TYPE_ERASER) flags |= PenProtocol.FLAG_ERASER;
        int buttons = e.getButtonState();
        if ((buttons & (MotionEvent.BUTTON_STYLUS_PRIMARY | MotionEvent.BUTTON_SECONDARY)) != 0)
            flags |= PenProtocol.FLAG_BTN_PRIMARY;
        if ((buttons & (MotionEvent.BUTTON_STYLUS_SECONDARY | MotionEvent.BUTTON_TERTIARY)) != 0)
            flags |= PenProtocol.FLAG_BTN_SECONDARY;

        float sx = 1f / Math.max(1, getWidth());
        float sy = 1f / Math.max(1, getHeight());
        int off = 0;
        if (!inRange) {
            // Re-announce the surface on every proximity-in so a restarted host
            // learns the aspect ratio without a reconnect.
            off = PenProtocol.putHello(batch, off, getWidth(), getHeight());
            inRange = true;
        }
        for (int h = 0, n = e.getHistorySize(); h < n; h++) {
            off = PenProtocol.putSample(batch, off, flags,
                    e.getHistoricalX(idx, h) * sx, e.getHistoricalY(idx, h) * sy,
                    e.getHistoricalPressure(idx, h),
                    e.getHistoricalAxisValue(MotionEvent.AXIS_TILT, idx, h),
                    e.getHistoricalOrientation(idx, h),
                    e.getHistoricalEventTimeNanos(h));
            if (off == batch.length) {
                link.send(batch, off);
                off = 0;
            }
        }
        off = PenProtocol.putSample(batch, off, flags, e.getX(idx) * sx, e.getY(idx) * sy,
                e.getPressure(idx), e.getAxisValue(MotionEvent.AXIS_TILT, idx),
                e.getOrientation(idx), e.getEventTimeNanos());
        link.send(batch, off);
    }

    private void sendOutOfRange() {
        removeCallbacks(leaveRange);
        if (!inRange) return;
        inRange = false;
        int off = PenProtocol.putSample(batch, 0, 0, 0f, 0f, 0f, 0f, 0f, System.nanoTime());
        link.send(batch, off);
    }
}
