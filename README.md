# Virtual Pen: Galaxy Tab S9+ → Linux pen tablet (for OneNote)

This fork turns a **Samsung Galaxy Tab S9+ (Android 16)** into a low-latency pressure and tilt
pen tablet for an **Ubuntu 24.04** PC. It is built for writing in OneNote (web or an Electron
wrapper) and is deliberately not generic.

The tablet shows a live view of a **patch** of your monitor and you write straight into it: you
see the OneNote page under the nib and can continue on the same line.

```
S Pen ─► Android app (raw, unbatched MotionEvents, wet ink drawn locally)
        ─► 16-byte binary records over USB accessory mode (AOA), or adb reverse TCP
        ─► virtualpen-host (async libusb, one uinput write per USB packet)
        ─► /dev/uinput "Virtual Pen Stylus" ─► libinput ─► X11/GNOME ─► browser/OneNote

Patch on the monitor ─► XShm capture ─► NVENC H.264 ─► same USB cable
        ─► tablet hardware decoder ─► shown under the pen
```

Status: the code is complete, but has not yet run on the real PC and tablet. See
[plan.md](plan.md) for what still needs testing, and what to do if something fails.

---

## Requirements

| | |
|---|---|
| Tablet | Galaxy Tab S9+ with S Pen, Android 14 or newer (built for Android 16) |
| PC | Ubuntu 24.04, GNOME on **X11** (needed for the live preview; check with `echo $XDG_SESSION_TYPE`) |
| GPU | NVIDIA with the proprietary driver for hardware encoding (falls back to the CPU otherwise) |
| Cable | USB-C data cable between tablet and PC |
| To build the app | Android Studio, or JDK 17 + Android SDK (platform 34) |

---

## 1. Build and install the PC host

```sh
git clone https://github.com/vanguard478/virtualpen.git
cd virtualpen
git checkout claude/virtual-pen-onenote-linux-22zfyd   # until merged
cd pen-host
./install.sh
```

`install.sh` does all of this:
- installs the build dependencies (asks for sudo);
- builds and runs the unit tests (`all tests passed`);
- installs `/usr/local/bin/virtualpen-host`, with permission to run the pen thread at real-time
  priority;
- installs udev rules (`/etc/udev/rules.d/70-virtualpen.rules`) that give you access to the tablet
  and `/dev/uinput`;
- installs a systemd user unit (`~/.config/systemd/user/virtualpen-host.service`).

**Unplug and replug the tablet afterwards**, so the new udev rules apply.

Check the NVIDIA encoder is available (optional, for the hardware preview):

```sh
nvidia-smi
ffmpeg -hide_banner -encoders | grep h264_nvenc    # should list h264_nvenc
```

If `h264_nvenc` is missing, install the encode library for your driver version, for example
`sudo apt install libnvidia-encode-550`. Use the version shown by `nvidia-smi`.

<details>
<summary>Building by hand instead of install.sh</summary>

```sh
sudo apt install build-essential cmake pkg-config libusb-1.0-0-dev libglib2.0-dev \
    libavcodec-dev libavutil-dev libswscale-dev libx11-dev libxext-dev libxrandr-dev
cmake -S pen-host -B pen-host/build -DCMAKE_BUILD_TYPE=Release
cmake --build pen-host/build -j"$(nproc)"
pen-host/build/pen-test
sudo install -m 0755 pen-host/build/virtualpen-host /usr/local/bin/
sudo setcap cap_sys_nice+ep /usr/local/bin/virtualpen-host
sudo install -m 0644 pen-host/70-virtualpen.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
```
</details>

---

## 2. Build and install the tablet app

### Prepare the tablet (once)

1. Enable developer options: Settings → About tablet → Software information → tap **Build number**
   seven times.
2. Settings → Developer options → turn on **USB debugging**. This is only needed to install the app
   and for the TCP fallback.
3. **Turn off Air command**: Settings → Advanced features → S Pen → Air command. Otherwise the S
   Pen button opens Samsung's menu instead of reaching the app.
4. Install adb on the PC: `sudo apt install adb`. Then connect the tablet, accept the "Allow USB
   debugging" prompt, and check that `adb devices` lists it.

### Option A: Android Studio

1. Install Android Studio: `sudo snap install android-studio --classic`, then start it and let it
   download the SDK.
2. *Open* → select the `android-virtual-pen` folder → wait for the Gradle sync.
3. Pick the tablet in the device list and press **Run ▶**. This builds, installs and starts the app.

### Option B: command line

Needs JDK 17 and an Android SDK. The easiest way to get the SDK is to install Android Studio once.

```sh
sudo apt install openjdk-17-jdk adb
cd virtualpen/android-virtual-pen
echo "sdk.dir=$HOME/Android/Sdk" > local.properties     # where Android Studio put the SDK
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

The app is called **Android Virtual Pen** in the launcher.

---

## 3. Use it

### Start

```sh
virtualpen-host --list-monitors
#  #  connector  resolution  position  scale  name
#  1  DP-0       2560x1440   0,0       1.00   Dell Inc. 27" [primary]
#  2  HDMI-0     1920x1080   2560,0    1.00   LG Electronics 24"

virtualpen-host --monitor DP-0
```

1. Pick the monitor you write on. With the NVIDIA driver on X11 the connectors are usually named
   `DP-0`, `HDMI-0`, …; use what `--list-monitors` prints. `--monitor` also accepts the number from
   the list or part of the display name. Always pass it if you have more than one monitor, so the
   pen and the preview line up.
2. Plug in the tablet. The host switches it into accessory mode and Android asks whether to open
   Virtual Pen. Tick **always** and accept.
3. Open OneNote on that monitor (F11 for fullscreen) and pick a pen in OneNote's **Draw** tab.
4. Write on the tablet. The toolbar status shows `Connected via USB accessory` and the patch size.

Stop the host with **Ctrl+C**.

### Write

- **Writing:** the pen writes into the patch shown on the tablet. Your stroke appears instantly as
  local "wet ink" and is replaced by OneNote's real ink a moment later.
- **Next paragraph or page:** tap **Next ⤓** to move the patch down a block. The last ~15% stays
  visible, so you keep your line. Or scroll OneNote under the patch with **two fingers**.
- **Bigger or smaller handwriting on screen:** **1× / 1.5× / 2×**. At 1×, ink on the monitor is the
  size you wrote it.

### Toolbar (left edge)

| Button | Does |
|---|---|
| ↶ Undo / ↷ Redo | Ctrl+Z / Ctrl+Y on the PC |
| Eraser | pen erases until you tap it again (holding the S Pen button also erases) |
| Ink / Width | colour and width of the instant local ink; match it to your OneNote pen |
| Next ⤓ | patch jumps one block down, keeping the last lines visible |
| ▲ ▼ ◀ ▶ | move the patch by half its size |
| 1× / 1.5× / 2× | patch size (handwriting magnification on the monitor) |
| Move | while on, one finger drags the page like a map |
| ⟳ | request a fresh video frame if the preview looks wrong |

Fingers are ignored while the pen is near the glass, so a resting palm does nothing.

### Common commands

```sh
virtualpen-host --monitor DP-0                       # normal use
virtualpen-host --monitor DP-0 --zoom 1.5            # start with a larger patch
virtualpen-host --monitor DP-0 --pressure-max 0.7    # full pressure with a lighter hand
virtualpen-host --monitor DP-0 -v --stats            # debug output: every sample, rates, preview fps
virtualpen-host --monitor DP-0 --encoder nvenc       # fail instead of falling back to the CPU encoder
virtualpen-host --monitor DP-0 --no-preview          # pen only, no video
virtualpen-host --monitor auto                       # hand the monitor choice back to GNOME
```

### Run in the background (systemd)

Edit the `ExecStart` line in `~/.config/systemd/user/virtualpen-host.service`, e.g.
`ExecStart=/usr/local/bin/virtualpen-host --monitor DP-0`, then:

```sh
systemctl --user daemon-reload
systemctl --user enable --now virtualpen-host
journalctl --user -u virtualpen-host -f        # follow the log
systemctl --user stop virtualpen-host          # stop it
```

The preview needs the X display. If the log says it cannot open the X display, add
`Environment=DISPLAY=:0` under `[Service]` in the unit.

### Fallback: adb reverse instead of accessory mode

If accessory mode doesn't work on your firmware (USB debugging must be on):

```sh
adb reverse tcp:47474 tcp:47474
virtualpen-host --tcp 47474 --monitor DP-0
```

The app tries the accessory first and then this connection automatically. It adds roughly a
millisecond of delay.

---

## Host options

| Option | Default | Purpose |
|---|---|---|
| `--list-monitors` | | list attached monitors (connector, resolution, position, name) and exit |
| `--monitor ID` | GNOME's choice | pin the tablet to a monitor; `auto` hands the choice back to GNOME |
| `--zoom Z` | `1` | starting patch size: `1`, `1.5` or `2` |
| `--monitor-size-mm WxH` | from EDID | physical monitor size, if the monitor doesn't report it (affects 1× sizing) |
| `--no-preview` | preview on | map the patch but don't stream it |
| `--preview-fps N` | `60` | preview frame rate; unchanged frames are never sent |
| `--preview-mbps N` | `12` | preview bitrate; raise it if small text looks soft |
| `--encoder NAME` | `auto` | `nvenc`, `x264` or `auto` (NVENC first) |
| `--scroll-px N` | `53` | screen pixels per wheel notch; tunes two-finger scroll speed |
| `--area X0,Y0,X1,Y1` | | fixed mapping instead of the patch, e.g. `0.5,0,1,1` = right half (no preview) |
| `--screen WxH` | from `--monitor` | with `--area`: monitor pixel size, so the 16:10 tablet isn't stretched |
| `--pressure-max F` | `1.0` | lower it (e.g. `0.7`) to reach full pressure with a lighter hand |
| `--pressure-min F` | `0.0` | ignore the bottom of the raw range |
| `--gamma F` | `1.0` | `<1` makes light strokes thicker, `>1` thinner |
| `--tip-floor F` | `0.06` | pressure reported on first contact; must stay above libinput's 5% threshold |
| `--button MODE` | `eraser` | S Pen button: `eraser`, `right-click` or `none` |
| `--tilt-flip-x`, `--tilt-flip-y`, `--tilt-swap` | off | correct tilt direction if it comes out mirrored |
| `--tcp PORT` | off | use TCP via `adb reverse` instead of USB accessory mode |
| `--usb VID[:PID]` | `04e8` | which USB device to switch into accessory mode |
| `--no-rt` | off | don't request real-time scheduling |
| `--stats` | off | per-second sample rate and the largest gaps (tablet-side and USB-side) |
| `-v` | off | print every sample, raw and translated, plus preview statistics |
| `--dry-run` | off | decode and translate without creating input devices |

---

## Troubleshooting

| Symptom | Check |
|---|---|
| `cannot open /dev/uinput` | udev rules not active: replug the tablet, or log out and in after `install.sh` |
| `found 04e8:… but cannot open it` | same: udev rules; replug the tablet |
| `waiting for the tablet` forever | cable is charge-only, or another Samsung device is connected (use `--usb 04e8:<pid>`) |
| App says "Waiting for PC…" | host not running, or the "open Virtual Pen" prompt was dismissed: replug |
| Cursor not under the nib | pass `--monitor`; see section 6 of [plan.md](plan.md) |
| Patch size wrong at 1× | `--monitor-size-mm 597x336` (27" QHD) if the log shows `0x0 mm` |
| No preview, toolbar shows "(no preview)" | host log line starting `preview disabled:` says why |
| Preview uses `libx264` | NVENC library missing, see section 1 |
| Strokes have no pressure in OneNote | `sudo libinput debug-events --verbose` (package `libinput-tools`) shows whether pressure reaches the desktop; then try another browser |
| Tilt mirrored | `virtualpen-host -v`, tilt the pen, then use `--tilt-flip-x` / `--tilt-flip-y` / `--tilt-swap` |

Logs that help: the host output with `-v --stats`, and from the tablet
`adb logcat -s PreviewDecoder PenLink AndroidRuntime`.

---

## How it works

### Mapping

GNOME maps the virtual tablet onto one monitor, and the host maps the tablet's writing area onto
the patch, a rectangle of that monitor with exactly the writing area's shape. The pen moves the PC
cursor, and whatever window is under it gets the pen, OneNote included.

`--monitor` reads the monitors from GNOME's display service (`org.gnome.Mutter.DisplayConfig`).
It writes the monitor's EDID vendor, product, serial and connector to
`org.gnome.desktop.peripherals.tablet:/org/gnome/desktop/peripherals/tablets/1701:1702/ output`.
The setting persists between runs. Without it, GNOME 46 spreads an unknown tablet across the whole
desktop. The patch position and size come from X11 RandR in physical millimetres, so 1× really is
the size you wrote.

### Preview

The host copies only the patch from the X screen (XShm), skips unchanged frames, and encodes
H.264 with NVENC at ultra-low-latency settings, or x264 as a fallback. Frames go back over the same
USB cable. Video frames are dropped rather than queued when the link is busy, so they never delay
pen samples. The tablet decodes in hardware in low-latency mode and renders each frame
immediately.

### Wet ink

The tablet draws the last 250 ms of the stroke itself, on a transparent layer above the video, at
the panel's 120 Hz. By the time the trail fades, OneNote's real ink has arrived in the preview
underneath. It is only a visual bridge; OneNote holds the real ink.

### Latency

- **Pen to PC:** a few ms. The S Pen digitizer and Android's input reader add a few ms;
  unbuffered dispatch removes up to one frame of vsync batching; USB plus the host adds well under
  a millisecond.
- **Wet ink:** about one 120 Hz frame.
- **OneNote's real ink in the preview:** OneNote's own drawing time plus an estimated 30–50 ms
  (capture at the next screen refresh, encode, USB, decode, one tablet frame).

### Pressure and tilt

- Contact pressure always starts above libinput's 5% tip threshold, so light strokes aren't lost.
- The pen enters proximity with zero pressure, so libinput doesn't apply a "worn pen" offset.
- Tilt is reported in centidegrees at 5730 units per radian, as libinput expects.

### Compared with upstream

| Area | Upstream | This fork |
|---|---|---|
| Input dispatch | vsync-batched `onTouch`, history dropped | `requestUnbufferedDispatch`, every historical sample forwarded |
| Wire format | `String.format` CSV; host parser breaks when two events share a transfer | fixed 16-byte binary records, stream decoder with resync, two-way link |
| Android threading | USB write on the UI thread | lock-free ring → dedicated writer thread |
| USB host | synchronous 512 B reads, 200 ms timeout | async transfers always queued, separate video downlink |
| uinput | one `write()` per event, memory leaked per sample, `BTN_TOUCH` never released | one `write()` per batch, no allocation |
| Hover | not supported | real hover/proximity |
| Pressure / tilt | linear, light strokes lost; tilt never sent | curve with tip floor; tilt from `AXIS_TILT` + `AXIS_ORIENTATION` |
| Mapping | stretched or 1:1 on the primary screen | movable patch with live preview, per-monitor pinning |
| Host app | Qt GUI, manual resolution entry | headless daemon, auto-switch, reconnect, systemd user unit |
| Permissions | `MODE=0777` on every USB device | `uaccess` for Samsung, AOA and uinput only |

The original Qt host (`virtual-pen-linux-host/`) and the old Android classes (`TouchListener`,
`UsbStreamService`, `UsbConnectedActivity`) are no longer used; [plan.md](plan.md) section 12
covers removing them.

## Credits

Based on [Android Virtual Pen](https://github.com/androidvirtualpen/virtualpen), inspired by
[Weylus](https://github.com/H-M-H/Weylus) and [Linux ADK](https://github.com/gibsson/linux-adk).
