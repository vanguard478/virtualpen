package com.virtual_pen;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbManager;
import android.os.Bundle;
import android.view.Display;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

/**
 * Single full-screen activity: launched manually or by the system when the PC
 * switches the tablet into accessory mode.
 */
public class MainActivity extends Activity implements PenLink.Listener {

    private PenLink link;
    private PenView penView;

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
        penView = new PenView(this, link);
        setContentView(penView);
        penView.requestFocus();

        registerReceiver(detachReceiver, new IntentFilter(UsbManager.ACTION_USB_ACCESSORY_DETACHED),
                Context.RECEIVER_NOT_EXPORTED);
        link.start();
    }

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
        super.onDestroy();
    }

    @Override
    public void onLinkState(boolean connected, String description) {
        if (connected) penView.onLinkConnected();
        penView.setStatus(description);
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
