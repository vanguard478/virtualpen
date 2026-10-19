# Virtual Pen: Galaxy Tab S9+ → Linux pen tablet (for OneNote)

This fork turns a **Samsung Galaxy Tab S9+ (Android 16)** into a low-latency pressure and tilt
pen tablet for an **Ubuntu 24.04** PC. It is built for writing in OneNote (web or an Electron
wrapper) and is deliberately not generic.

```
S Pen ─► Android app (raw, unbatched MotionEvents)
        ─► 16-byte binary records over USB accessory mode (AOA), or adb reverse TCP
        ─► virtualpen-host (async libusb, one uinput write per USB packet)
        ─► /dev/uinput "Virtual Pen Stylus" ─► libinput ─► compositor ─► browser/OneNote
```

## What changed compared to upstream

| Area | Upstream | This fork |
|---|---|---|
| Input dispatch | vsync-batched `onTouch`, history dropped | `requestUnbufferedDispatch`, every historical sample forwarded |
| Wire format | `String.format` CSV, one write per event; host parser breaks when two events share a transfer | fixed 16-byte binary records, one write per MotionEvent batch, stream decoder with resync |
| Android threading | USB write on the UI thread (freezes when the host stops reading) | lock-free ring → dedicated writer thread at `URGENT_DISPLAY` priority |
| USB host | synchronous 512 B reads with 200 ms timeout | 8 async transfers of `wMaxPacketSize` always queued |
| uinput | one `write()` per event, `new Error()` leaked per sample, `BTN_TOUCH` never released | one `write()` per batch, no allocation |
| Hover | not supported (pen leaves proximity on every lift) | real hover/proximity, with grace timers for Android's HOVER_EXIT-before-DOWN quirk |
| Pressure | linear, could exceed axis max, lost light strokes under libinput's 5% tip threshold | clamp, gamma, saturation; contact always above libinput's threshold; zero-pressure proximity-in so libinput doesn't apply a pressure offset |
| Tilt | axes declared with wrong resolution, never sent | `AXIS_TILT` + `AXIS_ORIENTATION` → `ABS_TILT_X/Y` in centidegrees, 5730 units/rad |
| Mapping | stretched or 1:1 on "primary screen" | aspect-preserving fit (`--screen`), custom sub-area (`--area`) |
| S Pen button | ignored | eraser (default), right-click (`BTN_STYLUS`) or off |
| Host app | Qt GUI, manual resolution entry | headless daemon, auto-switches the tablet, reconnects, systemd user unit |
| Permissions | `MODE=0777` on **every** USB device | `uaccess` for Samsung, AOA and uinput only |
| Tablet UI | windowed, rotates | fullscreen immersive, landscape-locked, screen kept on, max refresh rate, edge-gesture exclusion |

The original Qt host (`virtual-pen-linux-host/`) and the old Android classes (`TouchListener`,
`UsbStreamService`, `UsbConnectedActivity`) are no longer used and can be deleted.

## Setup

### PC (Ubuntu 24.04)

```sh
cd pen-host
./install.sh          # builds, runs tests, installs binary + udev rules + systemd user unit
```

Then pick the monitor you will write on and start the host:

```sh
virtualpen-host --list-monitors
#  #  connector  resolution  position  scale  name
#  1  DP-1       2560x1440   0,0       1.00   Dell Inc. 27" [primary]
#  2  eDP-1      1920x1200   2560,0    1.00   Built-in display [built-in]

virtualpen-host --monitor DP-1
```

The names are the ones GNOME Settings → Displays shows. `--monitor` accepts the connector
(`DP-1`), the number from the list, or the display name (or any unique part of it). It pins the
tablet to that monitor and fits the tablet's 16:10 shape inside it, so handwriting isn't stretched.

To run it in the background, add your options (e.g. `--monitor DP-1`) to the `ExecStart` line in
`~/.config/systemd/user/virtualpen-host.service` and run
`systemctl --user enable --now virtualpen-host`.

### Tablet

1. Build and install the app from `android-virtual-pen/` in Android Studio (minSdk 34).
2. **Turn off Air command** (Settings → Advanced features → S Pen → Air command). Otherwise the
   S Pen button opens Samsung's menu instead of reaching the app.
3. Connect the USB cable. The host switches the tablet into accessory mode and Android asks to
   open Virtual Pen. Tick "always" and accept.

### Fallback: adb reverse

If accessory mode doesn't work on your firmware, enable USB debugging and run:

```sh
adb reverse tcp:47474 tcp:47474
virtualpen-host --tcp 47474 --screen 2560x1440
```

The app tries the accessory first and then localhost:47474 automatically. This path adds
roughly a millisecond.

## Host options

| Option | Default | Purpose |
|---|---|---|
| `--list-monitors` | | list attached monitors (connector, resolution, position, name) and exit |
| `--monitor ID` | GNOME's choice | pin the tablet to a monitor; `auto` hands the choice back to GNOME |
| `--screen WxH` | from `--monitor`, else off (stretch) | monitor pixel size, so the 16:10 tablet maps to a 16:10 area |
| `--area X0,Y0,X1,Y1` | `0,0,1,1` | map to part of the monitor, e.g. `0.5,0,1,1` = right half |
| `--pressure-max F` | `1.0` | lower it (e.g. `0.7`) to reach full pressure with a lighter hand |
| `--pressure-min F` | `0.0` | ignore the bottom of the raw range |
| `--gamma F` | `1.0` | `<1` makes light strokes thicker, `>1` thinner |
| `--tip-floor F` | `0.06` | pressure reported on first contact; must stay above libinput's 5% threshold |
| `--button MODE` | `eraser` | `eraser`, `right-click` or `none` |
| `--tilt-flip-x`, `--tilt-flip-y`, `--tilt-swap` | off | correct tilt direction if it comes out mirrored |
| `--usb VID[:PID]` | `04e8` | which USB device to switch into accessory mode |
| `--stats` | off | per-second sample rate and the largest gaps (tablet-side and USB-side) |
| `-v` | off | print every sample, raw and translated |
| `--dry-run` | off | decode and translate without creating the uinput device |

## Verifying pressure and tilt

1. `virtualpen-host -v`: hover and write. `p=` should track pen force. Tilting the top of the pen
   to the right should make the first tilt value positive, and tilting it towards you the second.
   If not, use the flip/swap flags.
2. `sudo libinput debug-events --verbose` (package `libinput-tools`) shows what the compositor
   receives: `tip down`, pressure and tilt.
3. In the browser, check that OneNote's pen draws thicker lines when you press harder. Whether a
   browser passes pressure and tilt through depends on the browser and on whether it runs natively on
   Wayland or through XWayland. If pressure doesn't come through, try the other mode
   (`--ozone-platform=wayland` / `--ozone-platform=x11` for Chrome and Edge; Firefox on Ubuntu 24.04
   runs natively on Wayland).

## How the tablet maps to the screen

The tablet doesn't mirror the PC: its surface stands for the chosen monitor, the pen moves the PC
cursor, and whatever window is under the cursor gets the pen, OneNote included. Put OneNote on
that monitor (F11 for fullscreen) and write.

How `--monitor` works: the host reads the monitors from GNOME's display service
(`org.gnome.Mutter.DisplayConfig`) and writes the monitor's EDID vendor, product, serial and
connector to the tablet's GNOME setting
`org.gnome.desktop.peripherals.tablet:/org/gnome/desktop/peripherals/tablets/1701:1702/ output`.
GNOME then confines the tablet to that monitor. The setting persists, so later runs without
`--monitor` keep the same monitor. Without any setting, GNOME 46 spreads an unknown tablet like
this one across the whole desktop. `--monitor` needs a GNOME session (the Ubuntu 24.04 default,
Wayland or Xorg); on other desktops use `--screen` and `--area`.

## Where the latency goes

The pipeline now adds very little on top of the hardware. The S Pen digitizer and Android's input
reader add a few ms. Unbuffered dispatch removes up to one frame of vsync batching. USB plus the
host adds well under a millisecond. Most of what you see is the compositor and the browser
painting the stroke, and that part is out of this project's hands.

## Building manually

```sh
sudo apt install build-essential cmake pkg-config libusb-1.0-0-dev
cmake -S pen-host -B pen-host/build && cmake --build pen-host/build && pen-host/build/pen-test
```

## Credits

Based on [Android Virtual Pen](https://github.com/androidvirtualpen/virtualpen), inspired by
[Weylus](https://github.com/H-M-H/Weylus) and [Linux ADK](https://github.com/gibsson/linux-adk).
