package com.virtual_pen;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.hardware.usb.UsbAccessory;
import android.hardware.usb.UsbManager;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.util.Log;

import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.locks.LockSupport;

/**
 * Connection to the PC. Prefers the USB accessory (AOA) link and falls back to
 * TCP on localhost, which reaches the PC through {@code adb reverse}.
 *
 * <p>The UI thread hands batches to a dedicated writer thread through a
 * lock-free ring, so a stalled host can never block input dispatch.
 */
final class PenLink {
    // Must match the identity sent by pen-host/usb.cpp and res/xml/accessory_filter.xml.
    static final String MANUFACTURER = "virtualpen";
    static final String MODEL = "S9PenHost";
    static final int TCP_PORT = 47474;

    interface Listener {
        /** Called on the UI thread. */
        void onLinkState(boolean connected, String description);
    }

    private static final String TAG = "PenLink";
    private static final int SLOTS = 64;

    private final Context context;
    private final UsbManager usbManager;
    private final Listener listener;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final Object lock = new Object();

    private final byte[][] slots = new byte[SLOTS][PenProtocol.MAX_BATCH_BYTES];
    private final int[] lengths = new int[SLOTS];
    private final AtomicLong head = new AtomicLong();
    private final AtomicLong tail = new AtomicLong();

    private volatile boolean running;
    private volatile OutputStream out;
    private ParcelFileDescriptor accessoryFd;
    private Socket socket;
    private boolean permissionRequested;
    private Thread writer;
    private Thread connector;

    PenLink(Context context, Listener listener) {
        this.context = context.getApplicationContext();
        this.usbManager = context.getSystemService(UsbManager.class);
        this.listener = listener;
    }

    boolean isConnected() {
        return out != null;
    }

    void start() {
        running = true;
        writer = new Thread(this::writeLoop, "pen-writer");
        connector = new Thread(this::connectLoop, "pen-connector");
        writer.start();
        connector.start();
    }

    void stop() {
        running = false;
        disconnect(null);
        LockSupport.unpark(writer);
    }

    /** Wakes the connector, e.g. after an accessory was attached. */
    void kick() {
        synchronized (lock) {
            lock.notifyAll();
        }
    }

    /** UI thread only. Returns false if the batch was dropped. */
    boolean send(byte[] src, int len) {
        if (out == null || len == 0) return false;
        long h = head.get();
        if (h - tail.get() >= SLOTS) return false; // host is not reading
        int i = (int) (h % SLOTS);
        System.arraycopy(src, 0, slots[i], 0, len);
        lengths[i] = len;
        head.set(h + 1);
        LockSupport.unpark(writer);
        return true;
    }

    void disconnect(String reason) {
        boolean wasConnected;
        synchronized (lock) {
            wasConnected = out != null;
            out = null;
            closeQuietly();
            lock.notifyAll();
        }
        if (wasConnected && reason != null) {
            Log.i(TAG, "disconnected: " + reason);
            postState(false, "Disconnected (" + reason + "). Waiting for PC…");
        }
    }

    private void writeLoop() {
        try {
            Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY);
        } catch (RuntimeException ignored) {
        }
        while (running) {
            long t = tail.get();
            if (t == head.get()) {
                LockSupport.park(this);
                continue;
            }
            int i = (int) (t % SLOTS);
            OutputStream o = out;
            if (o != null) {
                try {
                    o.write(slots[i], 0, lengths[i]);
                } catch (IOException e) {
                    if (out == o) disconnect("write failed");
                }
            }
            tail.set(t + 1);
        }
    }

    private void connectLoop() {
        postState(false, "Waiting for PC… start virtualpen-host");
        while (running) {
            if (out == null) {
                OutputStream o = openAccessory();
                String via = "USB accessory";
                if (o == null) {
                    o = openTcp();
                    via = "adb reverse tcp:" + TCP_PORT;
                }
                if (o != null) {
                    synchronized (lock) {
                        if (!running) {
                            closeQuietly();
                            return;
                        }
                        out = o;
                    }
                    postState(true, "Connected via " + via);
                }
            }
            Socket s = socket;
            if (out != null && s != null) {
                // TCP: block until the host closes the connection.
                try {
                    InputStream in = s.getInputStream();
                    while (running && in.read() >= 0) { /* host never sends data */ }
                } catch (IOException ignored) {
                }
                disconnect("host closed");
                continue;
            }
            synchronized (lock) {
                try {
                    lock.wait(out == null ? 500 : 0);
                } catch (InterruptedException e) {
                    return;
                }
            }
        }
    }

    private OutputStream openAccessory() {
        UsbAccessory[] accessories = usbManager.getAccessoryList();
        if (accessories == null) return null;
        for (UsbAccessory a : accessories) {
            if (!MANUFACTURER.equals(a.getManufacturer()) || !MODEL.equals(a.getModel())) continue;
            if (!usbManager.hasPermission(a)) {
                if (!permissionRequested) {
                    permissionRequested = true;
                    Intent intent = new Intent(context.getPackageName() + ".USB_PERMISSION")
                            .setPackage(context.getPackageName());
                    usbManager.requestPermission(a, PendingIntent.getBroadcast(
                            context, 0, intent, PendingIntent.FLAG_IMMUTABLE));
                }
                return null;
            }
            ParcelFileDescriptor fd = usbManager.openAccessory(a);
            if (fd == null) return null;
            synchronized (lock) {
                accessoryFd = fd;
            }
            return new FileOutputStream(fd.getFileDescriptor());
        }
        return null;
    }

    private OutputStream openTcp() {
        Socket s = new Socket();
        try {
            s.setTcpNoDelay(true);
            s.connect(new InetSocketAddress("127.0.0.1", TCP_PORT), 300);
            synchronized (lock) {
                socket = s;
            }
            return s.getOutputStream();
        } catch (IOException e) {
            try {
                s.close();
            } catch (IOException ignored) {
            }
            return null;
        }
    }

    // Caller holds lock.
    private void closeQuietly() {
        try {
            if (accessoryFd != null) accessoryFd.close();
        } catch (IOException ignored) {
        }
        try {
            if (socket != null) socket.close();
        } catch (IOException ignored) {
        }
        accessoryFd = null;
        socket = null;
    }

    private void postState(boolean connected, String text) {
        ui.post(() -> listener.onLinkState(connected, text));
    }
}
