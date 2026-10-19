# Remaining work plan

Everything in this file still needs doing. The cloud container used for development had no Android
SDK, no access to Google's Maven repository, no GPU, no USB devices, no GNOME desktop and no
tablet, so these items could not be built, downloaded or tested there.

What *was* verified in the container is listed at the end, so you know what to trust and what to
suspect first.

Target setup: Galaxy Tab S9+ (Android 16), Ubuntu 24.04 **X11** session, NVIDIA RTX 3050 8 GB.

---

## 0. First session at the PC (do in this order)

- [ ] 1. Build and install the host: `cd pen-host && ./install.sh` (section 3)
- [ ] 2. Check NVENC: `ffmpeg -hide_banner -encoders | grep h264_nvenc` (section 4)
- [ ] 3. `virtualpen-host --list-monitors`, then note the connector of the writing monitor (section 6)
- [ ] 4. Build the APK in Android Studio and install it on the tablet (section 1)
- [ ] 5. Turn off Air command on the tablet (Settings → Advanced features → S Pen)
- [ ] 6. Run `virtualpen-host --monitor <connector> --encoder nvenc -v --stats`, plug in the tablet,
        accept the "open Virtual Pen" prompt with "always"
- [ ] 7. Walk through sections 5–10 and tick what works
- [ ] 8. Send the logs listed in section 13 for anything that doesn't

---

## 1. Build the Android app (blocked: Android SDK download)

**Why blocked:** `dl.google.com` (Android SDK, build tools, Google Maven) was refused by the
container's network policy. The new Java classes were only type-checked against the Android 14
framework jar from Maven Central (`org.robolectric:android-all:14-robolectric-10818077`). That
catches API mistakes, but not resource, manifest or Gradle problems.

**Do:**
- [ ] Open `android-virtual-pen/` in Android Studio and let it sync (AGP 8.3.2, compileSdk 34,
      minSdk 34, Java 17). Accept a Gradle/AGP upgrade if Studio insists; nothing depends on the
      exact version.
- [ ] Build → *Build APK(s)*, and install it on the tablet (`adb install -r app-debug.apk`).
- [ ] Fix whatever the real build reports. Likely spots:
  - The theme `Theme.VirtualPen.Fullscreen` in `res/values/themes.xml`, especially the
    `android:windowLayoutInDisplayCutoutMode` value `always`.
  - Unused old classes that still need AndroidX (see section 12).

**Done when:** the app launches fullscreen in landscape with the toolbar on the left and "Waiting
for PC…" in the toolbar status.

---

## 2. Front-buffered "wet ink" with Jetpack graphics-core (blocked: Google Maven)

**Why blocked:** `androidx.graphics:graphics-core` lives only on Google Maven, which was
unreachable, and so were its mirrors. The library could not be downloaded or compiled against, so
`InkLayer.java` uses a platform-only fallback instead: a transparent `SurfaceView` drawn from its
own thread with `lockHardwareCanvas()`. That's roughly one 120 Hz frame (~8 ms) to the glass.
Front-buffer rendering should cut it to a few ms.

**Do:**
- [ ] First evaluate the current `InkLayer` on the device (section 8). If the ink already feels
      attached to the nib, this item is optional.
- [ ] Add `implementation("androidx.graphics:graphics-core:<latest stable 1.0.x>")` to
      `app/build.gradle.kts`. Check `minCompileSdk` in the AAR metadata; raise `compileSdk` if
      required.
- [ ] Write `FrontBufferInkLayer` with the same public methods as `InkLayer`: `getView`,
      `setStyle`, `setEnabled`, `beginStroke`, `addPoint`, `clear`, `release`. Use
      `CanvasFrontBufferedRenderer<Segment>` on the existing transparent `SurfaceView`
      (`setZOrderMediaOverlay(true)`, `PixelFormat.TRANSLUCENT`):
  - `addPoint` → `renderFrontBufferedLayer(segment)` (draws only the new segment)
  - the 250 ms tail fade → every ~8 ms, `renderMultiBufferedLayer(young segments)`, or
    `clear()` plus re-render. Avoid redrawing the whole surface into the front buffer; the
    library docs warn that causes tearing.
  - Keep `InkLayer`'s ring buffer and the `TAIL_MS` rule, so the visual behaviour is identical.
- [ ] Pick the implementation in `MainActivity` with a constant or a long-press on the Ink button,
      so the two can be compared side by side.

**Done when:** a fast scribble shows no visible gap between nib and ink, without tearing. Compare
by filming both versions with a phone in slow motion.

---

## 3. Build the host on the PC (needs your machine)

- [ ] `cd pen-host && ./install.sh`. This installs build dependencies, builds, runs `pen-test`, and
      installs the binary, udev rules and systemd user unit. It also runs `setcap
      cap_sys_nice+ep` so the pen thread can run real-time.
- [ ] Replug the tablet after the udev rules are installed.
- [ ] `virtualpen-host --help` runs.

**Done when:** `pen-test` prints `all tests passed`, and `virtualpen-host` starts without
`cannot open /dev/uinput`.

---

## 4. NVENC encoding on the RTX 3050 (blocked: no GPU)

**Why blocked:** the container has no NVIDIA GPU, so every preview test ran through the x264
fallback. The NVENC path is the same libavcodec code with different options (`preset p1`,
`tune ull`, `rc cbr`, `zerolatency 1`, `delay 0`, `rc-lookahead 0`, `forced-idr 1`, BGR0 input
converted on the GPU). It has never actually run.

**Do:**
- [ ] `nvidia-smi` works, and `ffmpeg -hide_banner -encoders | grep h264_nvenc` lists the encoder.
      If not, install the encode library matching your driver, e.g.
      `sudo apt install libnvidia-encode-<driver-major>`.
- [ ] Run with `--encoder nvenc -v`. The log must say `preview: WxH at 60 fps with h264_nvenc`.
- [ ] If opening fails, run once with FFmpeg logging and note which option is rejected. Fallbacks
      to try in `preview.cpp`, in order:
  1. Drop `tune ull`.
  2. Replace `preset p1` with `preset llhp`.
  3. Feed NV12 instead of BGR0: force the swscale path by not selecting `AV_PIX_FMT_0RGB32`.
- [ ] Check encode cost. With `-v`, per-second frame counts are printed. Expected: 60 fps
      while the patch content changes, and a few % of one CPU core.
- [ ] Text sharpness: if small handwriting looks soft, try `--preview-mbps 20`. If that's still
      soft, consider HEVC (`hevc_nvenc` plus `video/hevc` on the tablet) as a later change.

**Done when:** the preview runs on NVENC at 60 fps without dropped frames while writing.

---

## 5. Two-way USB accessory link (blocked: no USB devices)

**Why blocked:** the bulk OUT downlink was never exercised on real hardware. It uses 16 KiB
transfers, `LIBUSB_TRANSFER_ADD_ZERO_PACKET`, and a pool of 96 transfers that queues whole frames
or none. On the tablet, the Android accessory driver is read in full 16 KiB chunks. Only the TCP
path was tested end to end.

**Do:**
- [ ] The pen works, and the host log shows `accessory connected` and no
      `usb downlink transfer ended` errors.
- [ ] The preview appears. `adb logcat -s PreviewDecoder PenLink` shows `decoding WxH with
      c2.…` and no `bad downlink message length`.
- [ ] Unplug and replug the cable: the host prints `accessory disconnected` and reconnects; the app
      shows Disconnected, then Connected.
- [ ] Restart only the host while the tablet stays plugged in: the pen and preview come back.
- [ ] If accessory mode misbehaves on Android 16 / One UI, use the fallback:
      `adb reverse tcp:47474 tcp:47474` and `virtualpen-host --tcp 47474 ...`. Note which one
      worked.

**Watch for:** a stalled preview while the pen still works. That points at the downlink. The
`downlink congested` messages and dropped counts in `-v` output show whether the OUT pool fills
up.

---

## 6. Monitor selection and pen mapping on GNOME X11 with NVIDIA (blocked: no GNOME)

**Why blocked:**
- `--monitor` reads monitors from GNOME's display service and pins the tablet via the GNOME
  setting `org.gnome.desktop.peripherals.tablet ... output`.
- The patch geometry comes from X11's RandR display extension, matched by connector name.
- Both were only tested against a stubbed `gdbus` and `gsettings` and a virtual X server whose
  only output is called `screen`.

**Do:**
- [ ] `virtualpen-host --list-monitors` lists your monitors with NVIDIA-style names (`DP-0`,
      `HDMI-0`, …).
- [ ] The host log line `patch mapping on DP-0: WxH px at X,Y, AxB mm` shows the same name, the
      right resolution, and a non-zero millimetre size. If it shows `0x0 mm`, pass
      `--monitor-size-mm 597x336` (a 27" QHD).
- [ ] Pen alignment test: hover the pen over something visible in the preview, such as a word in
      OneNote. The PC cursor must be exactly on that word. Repeat at all four corners of the
      writing area.
  - If it's offset or scaled, check
    `gsettings get org.gnome.desktop.peripherals.tablet:/org/gnome/desktop/peripherals/tablets/1701:1702/ output`
  - and check `xinput list-props "Virtual Pen Stylus"`, looking at the Coordinate Transformation
    Matrix.
- [ ] Multi-monitor: the cursor stays on the chosen monitor.

**Done when:** the cursor sits under the nib everywhere on the writing area.

---

## 7. Preview decoding on the S9+ (blocked: no device)

**Why blocked:** `PreviewDecoder.java` was only type-checked. It configures the decoder with the
SPS/PPS (start codes included), `KEY_LOW_LATENCY`, priority 0, renders on output immediately, and
asks for a keyframe when frames are lost.

**Do:**
- [ ] The preview is upright, not mirrored, and fills the writing area without stretching; it has
      the same aspect ratio as the patch.
- [ ] Change the patch size (1×/1.5×/2×): the preview reconfigures within a fraction of a second.
- [ ] Move the patch (Next, arrows, Move mode): the preview follows immediately.
- [ ] Write fast and check `logcat` for repeated keyframe requests or `decoder rejected frame`.
- [ ] If the decoder refuses the configuration, try removing the start codes from csd-0/csd-1 in
      `PreviewDecoder.onCodecConfig`. Some decoders want them stripped, though most accept both.

---

## 8. Wet ink, layering and the toolbar on the real screen (blocked: no device)

- [ ] Layer order: preview video at the bottom, wet ink above it, invisible pen layer on top. If
      the ink is hidden behind the video, swap to `setZOrderOnTop(true)` for the ink surface in
      `InkLayer`. That puts it above the window too, which is fine because the toolbar is a
      separate column.
- [ ] Wet ink follows the nib, and its tail disappears roughly when OneNote's ink shows up under it.
      Tune `InkLayer.TAIL_MS` (default 250): shorter if both inks are visible at once, longer if
      there's a gap.
- [ ] Ink colour and width buttons are readable on the dark toolbar. The toolbar fits the 1752 px
      height without scrolling at the default display size; if not, shrink the button height
      (`dp(44)`).
- [ ] No wet ink while the eraser is active or the S Pen button is held.

---

## 9. Toolbar commands in OneNote (blocked: no PC session)

- [ ] **Undo / Redo** (Ctrl+Z / Ctrl+Y via the "Virtual Pen Keys" device) work in OneNote. On a
      non-QWERTY keyboard layout the keys may differ; then change `KEY_Z`/`KEY_Y` in
      `controls.cpp`.
- [ ] **Eraser**: does OneNote web erase with the pen's eraser tool (`BTN_TOOL_RUBBER`)? If not,
      choose OneNote's eraser in its Draw tab and use our button only for the wet-ink suppression.
      Record the result here.
- [ ] **Two-finger scroll**:
  - Direction: content should follow the fingers. If it's inverted, flip the signs in
    `Controls::scroll`.
  - Speed: tune `--scroll-px` until the page moves under the fingers 1:1.
- [ ] **Move mode**: one-finger drag moves the patch like dragging a map.
- [ ] **Next ⤓**: the last lines stay visible at the top after the jump (15% overlap,
      `kNextOverlap` in `patch.cpp`).

---

## 10. Pressure, tilt and the browser (blocked: no PC session)

- [ ] `sudo libinput debug-events --verbose` (package `libinput-tools`) shows `tip down`,
      pressure and tilt for "Virtual Pen Stylus".
- [ ] `virtualpen-host -v`: tilting the top of the pen right gives a positive first tilt value,
      and tilting it towards you gives a positive second value. Otherwise use `--tilt-flip-x`,
      `--tilt-flip-y` or `--tilt-swap`.
- [ ] In OneNote (browser on X11): harder pressure gives thicker lines. Record which browser passes
      pressure through on your setup (Chrome/Edge/Firefox).
- [ ] Tune `--pressure-max` / `--gamma` to taste.

---

## 11. Latency measurement (blocked: no hardware)

Expected (estimates, not measured): pen → PC a few ms; wet ink about one 120 Hz frame; OneNote's
real ink in the preview about 30–50 ms plus OneNote's own drawing time.

- [ ] Film the tablet in slow motion (240 fps) while drawing fast circles. Count frames between
      nib and wet ink, and between nib and the preview's ink.
- [ ] `--stats` on the host: samples/s should be about the S Pen report rate, and the "max gap"
      numbers should stay small while writing.
- [ ] If the preview lags more than expected, try `--preview-fps 120` (only helps on a 120 Hz+
      monitor), and confirm NVENC is in use rather than x264.

---

## 12. Remove the old code (blocked: deletion refused in the container)

Bulk deletion of the old code was refused by the container's safety check, so it is still in the
repo but unused.

- [ ] Delete `virtual-pen-linux-host/` (the old Qt host) and `deb/` (old packaging).
- [ ] Delete `TouchListener.java`, `UsbStreamService.java` and `UsbConnectedActivity.java`, plus
      `res/layout/activity_main.xml` and `res/layout/popup_window.xml` if nothing references them.
- [ ] Then remove the AndroidX dependencies (`appcompat`, `material`, `activity`,
      `constraintlayout`) from `app/build.gradle.kts`. Switch `Theme.Peninput` in `themes.xml`
      (both `values/` and `values-night/`) to a platform parent such as
      `android:Theme.DeviceDefault.NoActionBar`, because Material3 themes come from those libraries.
- [ ] Rebuild and check the APK still installs.

---

## 13. Logs to send back if something fails

- Host: the full terminal output of `virtualpen-host --monitor <connector> --encoder nvenc -v --stats`
- Tablet: `adb logcat -s PreviewDecoder PenLink AndroidRuntime`
- Input: `sudo libinput debug-events --verbose` while writing (for pen/pressure issues)
- Mapping: the `gsettings get …/tablets/1701:1702/ output` result and
  `xinput list-props "Virtual Pen Stylus"`
- Environment: `echo $XDG_SESSION_TYPE`, `nvidia-smi`, `ffmpeg -hide_banner -encoders | grep nvenc`

---

## Later (not blocked, just not started)

- Selecting OneNote pens, highlighter, colours and sizes from the toolbar. OneNote web has no
  keyboard shortcuts for these, so it would mean click automation calibrated to the Draw ribbon,
  which is fragile.
- Remembering the last patch position and zoom between runs.
- Wayland support for the preview: capture through GNOME's ScreenCast portal instead of XShm.

---

## Already verified in the container

- Host builds cleanly with `-Wall -Wextra`, and `pen-test` passes. It covers: protocol decoding
  with resync; pen state machine; pressure curve and libinput tip threshold; tilt maths; patch
  geometry, presets, moves and edge clamping; monitor parsing and matching; H.264 NAL and
  SPS/PPS extraction.
- End-to-end over TCP against a virtual 2560×1440 X server with x264:
  - geometry, codec configuration and keyframes on move, resize and request;
  - unchanged frames are skipped;
  - the output decodes with ffprobe;
  - the captured patch is pixel-exact in position and colour.
- The Android downlink parser (`PenLink.readDownlink`), run in a plain JVM on 5 MB of real host
  output split into random read sizes, reproduces all 55 messages byte for byte, including a
  652 KB keyframe.
- Java and C++ agree on the byte layout of samples, HELLO and command records.
- All new Android classes compile against the Android 14 framework without warnings.
- libinput 1.25 and mutter 46.2 source (Ubuntu 24.04's versions) were read to confirm:
  - the 5%/1% tip thresholds;
  - tilt units in units per radian;
  - the tablet settings path `tablets/1701:1702`;
  - the 4-value `output` key, including the connector.
