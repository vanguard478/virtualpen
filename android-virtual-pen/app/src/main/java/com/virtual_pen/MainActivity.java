package com.virtual_pen;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Color;
import android.hardware.usb.UsbManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.Display;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * Single full-screen activity: a toolbar strip on the left and the writing
 * area, which stacks the patch preview video, the wet-ink layer and the
 * transparent pen input layer. Launched manually or by the system when the
 * PC switches the tablet into accessory mode.
 */
public class MainActivity extends Activity implements PenLink.Listener, PenView.GestureListener {

    private static final int[] INK_COLORS = {
            Color.rgb(20, 20, 30), Color.rgb(25, 80, 220), Color.rgb(210, 30, 30),
            Color.rgb(20, 140, 60), Color.argb(110, 255, 230, 0)};
    private static final String[] INK_NAMES = {"black", "blue", "red", "green", "highlight"};
    private static final float[] INK_WIDTHS_DP = {1.5f, 3f, 6f};
    private static final String[] PRESET_LABELS = {"1×", "1.5×", "2×"};

    private final Handler ui = new Handler(Looper.getMainLooper());
    private PenLink link;
    private PenView penView;
    private InkLayer ink;
    private PreviewDecoder decoder;
    private TextView status;
    private Button eraserButton, moveButton, inkButton, widthButton;
    private final Button[] presetButtons = new Button[PRESET_LABELS.length];
    private String linkText = "";
    private String patchText = "";
    private boolean eraserOn, moveOn;
    private int inkColor, inkWidth = 1;
    private int lastPatchX = -1, lastPatchY = -1, lastPatchW = -1;

    private final BroadcastReceiver detachReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            link.disconnect("USB detached");
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Window window = getWindow();
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        window.setDecorFitsSystemWindows(false);
        WindowManager.LayoutParams lp = window.getAttributes();
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        lp.preferredDisplayModeId = highestRefreshModeId();
        window.setAttributes(lp);

        link = new PenLink(this, this);
        ink = new InkLayer(this);
        decoder = new PreviewDecoder(() -> ui.post(() -> link.sendCommand(PenProtocol.CMD_KEYFRAME, 0, 0)));
        penView = new PenView(this, link, ink, this);

        SurfaceView video = new SurfaceView(this);
        video.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                decoder.setSurface(holder.getSurface());
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                decoder.setSurface(null);
            }
        });

        FrameLayout writing = new FrameLayout(this);
        writing.setBackgroundColor(Color.BLACK);
        writing.addView(video, matchParent());
        writing.addView(ink.getView(), matchParent());
        writing.addView(penView, matchParent());

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.HORIZONTAL);
        root.setBackgroundColor(Color.rgb(28, 28, 32));
        root.addView(buildToolbar(), new LinearLayout.LayoutParams(dp(92), ViewGroup.LayoutParams.MATCH_PARENT));
        root.addView(writing, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f));
        setContentView(root);
        penView.requestFocus();
        applyInkStyle();

        registerReceiver(detachReceiver, new IntentFilter(UsbManager.ACTION_USB_ACCESSORY_DETACHED),
                Context.RECEIVER_NOT_EXPORTED);
        link.start();
    }

    private View buildToolbar() {
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.VERTICAL);
        bar.setPadding(dp(4), dp(8), dp(4), dp(8));

        addButton(bar, "↶ Undo", v -> link.sendCommand(PenProtocol.CMD_UNDO, 0, 0));
        addButton(bar, "↷ Redo", v -> link.sendCommand(PenProtocol.CMD_REDO, 0, 0));
        eraserButton = addButton(bar, "Eraser", v -> {
            eraserOn = !eraserOn;
            link.sendCommand(PenProtocol.CMD_ERASER, eraserOn ? 1 : 0, 0);
            refreshToolbar();
        });
        inkButton = addButton(bar, "", v -> {
            inkColor = (inkColor + 1) % INK_COLORS.length;
            applyInkStyle();
        });
        widthButton = addButton(bar, "", v -> {
            inkWidth = (inkWidth + 1) % INK_WIDTHS_DP.length;
            applyInkStyle();
        });

        addLabel(bar, "Patch");
        addButton(bar, "Next ⤓", v -> patchCommand(PenProtocol.CMD_PATCH_NEXT, 0, 0));
        LinearLayout arrows1 = row(bar);
        addButton(arrows1, "▲", v -> patchCommand(PenProtocol.CMD_PATCH_STEP, 0, -1));
        addButton(arrows1, "▼", v -> patchCommand(PenProtocol.CMD_PATCH_STEP, 0, 1));
        LinearLayout arrows2 = row(bar);
        addButton(arrows2, "◀", v -> patchCommand(PenProtocol.CMD_PATCH_STEP, -1, 0));
        addButton(arrows2, "▶", v -> patchCommand(PenProtocol.CMD_PATCH_STEP, 1, 0));
        for (int i = 0; i < PRESET_LABELS.length; i++) {
            final int preset = i;
            presetButtons[i] = addButton(bar, PRESET_LABELS[i],
                    v -> patchCommand(PenProtocol.CMD_PATCH_PRESET, preset, 0));
        }
        moveButton = addButton(bar, "Move", v -> {
            moveOn = !moveOn;
            penView.setMoveMode(moveOn);
            refreshToolbar();
        });
        addButton(bar, "⟳", v -> link.sendCommand(PenProtocol.CMD_KEYFRAME, 0, 0));

        status = new TextView(this);
        status.setTextColor(Color.LTGRAY);
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10);
        status.setPadding(0, dp(8), 0, 0);
        bar.addView(status);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(bar);
        refreshToolbar();
        return scroll;
    }

    private void patchCommand(int command, int a, int b) {
        onContentMoving();
        link.sendCommand(command, a, b);
    }

    @Override
    public void onContentMoving() {
        ink.clear();
    }

    private void applyInkStyle() {
        ink.setStyle(INK_COLORS[inkColor], dp(INK_WIDTHS_DP[inkWidth]));
        inkButton.setText("Ink: " + INK_NAMES[inkColor]);
        // Black ink would be invisible on the dark toolbar.
        inkButton.setTextColor(inkColor == 0 ? Color.WHITE : INK_COLORS[inkColor] | 0xff000000);
        widthButton.setText("Width " + (inkWidth + 1));
    }

    private void refreshToolbar() {
        highlight(eraserButton, eraserOn);
        highlight(moveButton, moveOn);
        penView.setEraserMode(eraserOn);
        status.setText(linkText + (patchText.isEmpty() ? "" : "\n" + patchText));
    }

    // ---------------------------------------------------------------------
    // Link

    @Override
    public void onLinkState(boolean connected, String description) {
        if (connected) penView.onLinkConnected();
        linkText = description;
        refreshToolbar();
    }

    @Override
    public void onMessage(int type, int flags, byte[] b, int off, int len) {
        if (type == PenProtocol.DL_FRAME) {
            decoder.onFrame(b, off, len, (flags & PenProtocol.DL_FLAG_KEYFRAME) != 0);
        } else if (type == PenProtocol.DL_CODEC) {
            decoder.onCodecConfig(b, off, len);
        } else if (type == PenProtocol.DL_GEOMETRY && len >= 16) {
            final int x = PenProtocol.rd16(b, off + 4), y = PenProtocol.rd16(b, off + 6);
            final int w = PenProtocol.rd16(b, off + 8), h = PenProtocol.rd16(b, off + 10);
            final int preset = b[off + 12] & 0xff, geo = b[off + 13] & 0xff;
            ui.post(() -> onGeometry(x, y, w, h, preset, geo));
        }
    }

    private void onGeometry(int x, int y, int w, int h, int preset, int geo) {
        if (x != lastPatchX || y != lastPatchY || w != lastPatchW) {
            ink.clear(); // the page under the trail moved
            lastPatchX = x;
            lastPatchY = y;
            lastPatchW = w;
        }
        eraserOn = (geo & PenProtocol.GEO_ERASER) != 0;
        for (int i = 0; i < presetButtons.length; i++) highlight(presetButtons[i], i == preset);
        patchText = "Patch " + w + "×" + h + "\nat " + x + "," + y
                + ((geo & PenProtocol.GEO_PREVIEW) != 0 ? "" : "\n(no preview)");
        refreshToolbar();
    }

    // ---------------------------------------------------------------------
    // Lifecycle

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if (UsbManager.ACTION_USB_ACCESSORY_ATTACHED.equals(intent.getAction())) link.kick();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    @Override
    protected void onDestroy() {
        unregisterReceiver(detachReceiver);
        link.stop();
        decoder.close();
        ink.release();
        super.onDestroy();
    }

    // ---------------------------------------------------------------------
    // Helpers

    private Button addButton(LinearLayout parent, String text, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        b.setMinHeight(dp(44));
        b.setMinimumHeight(dp(44));
        b.setPadding(0, 0, 0, 0);
        b.setFocusable(false);
        b.setOnClickListener(l);
        LinearLayout.LayoutParams p = parent.getOrientation() == LinearLayout.HORIZONTAL
                ? new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                : new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        parent.addView(b, p);
        return b;
    }

    private LinearLayout row(LinearLayout parent) {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        parent.addView(r, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        return r;
    }

    private void addLabel(LinearLayout parent, String text) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextColor(Color.GRAY);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        t.setGravity(Gravity.CENTER_HORIZONTAL);
        t.setPadding(0, dp(10), 0, dp(2));
        parent.addView(t);
    }

    private static void highlight(Button b, boolean on) {
        b.setAlpha(on ? 1f : 0.6f);
        b.setSelected(on);
        b.setTypeface(null, on ? android.graphics.Typeface.BOLD : android.graphics.Typeface.NORMAL);
    }

    private static FrameLayout.LayoutParams matchParent() {
        return new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
    }

    private int dp(float v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private void hideSystemBars() {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c == null) return;
        c.hide(WindowInsets.Type.systemBars());
        c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    /** Pins the panel at its highest refresh rate so the digitizer is never throttled by idle refresh. */
    private int highestRefreshModeId() {
        Display display = getDisplay();
        if (display == null) return 0;
        Display.Mode best = display.getMode();
        for (Display.Mode m : display.getSupportedModes()) {
            if (m.getPhysicalWidth() == best.getPhysicalWidth()
                    && m.getPhysicalHeight() == best.getPhysicalHeight()
                    && m.getRefreshRate() > best.getRefreshRate()) {
                best = m;
            }
        }
        return best.getModeId();
    }
}
