# Kobo hardware notes

Everything in this file was needed to write CrossKobo, and none of it is
documented by Kobo. The sources are Kobo's own published kernel trees, the
firmware's own shell scripts, and two projects that have already done the
archaeology: [FBInk](https://github.com/NiLuJe/FBInk) and
[KOReader](https://github.com/koreader/koreader). It is recorded here so
the next person does not have to rediscover it.

## The device

Kobo Libra Colour, codename **`monza`**:

| | |
| --- | --- |
| SoC | MediaTek MT8113T, dual Cortex-A53 |
| RAM | 1 GB |
| Storage | 32 GB eMMC (no card slot) |
| Panel | 7" E Ink Kaleido 3, 1264x1680, 300 dpi mono / 150 dpi colour |
| Input | capacitive touch, Wacom-style digitiser (Kobo Stylus 2), two page buttons, power |
| Userland | 32-bit ARM hard-float (armv7) on a 64-bit capable SoC |

The 2024 colour generation (Clara BW `spaBW`, Clara Colour `spaColour`,
Libra Colour `monza`) and the Elipsa 2E (`condor`) are MediaTek. Everything
older is NXP i.MX. That distinction decides which display-controller
interface to use, so CrossKobo detects it at runtime
(`src/platform/device.cpp`) from the platform directory, the presence of
`/proc/hwtcon`, or `/usr/local/Kobo/pickel-mtk`.

Whether a panel has a colour filter array is not guessable from the model
alone; the firmware's own scripts ask the hardware:

```sh
ntx_hwconfig -S 1 -p /dev/mmcblk0p6 EPD_Flags CFA   # "ON" on Kaleido panels
```

## Display: MediaTek `hwtcon`

The framebuffer is `/dev/fb0` as usual, but updates are pushed with
MediaTek's ioctls rather than the i.MX `mxcfb` ones. The ABI is in
`src/platform/eink_ioctl.h`; the essentials:

- `HWTCON_SEND_UPDATE` with a `hwtcon_update_data` (region, waveform mode,
  update mode, marker, flags, dither mode).
- `HWTCON_WAIT_FOR_UPDATE_COMPLETE` with the marker, to block until the
  panel has finished — needed before sleeping, so the last page is actually
  on screen.
- `HWTCON_SET_PWRDOWN_DELAY` keeps the controller powered between updates;
  CrossKobo sets 2000 ms so page turns do not pay the power-up cost.

Waveform modes, and what they are for:

| Mode | Use |
| --- | --- |
| `DU` (1) | two-tone, fastest: menus, highlights |
| `GL16` (3) | 16 greys, no flash: **the reading default** |
| `GC16` (2) | full tonal range: images, and flashing refreshes |
| `A2` (6) | bilevel, lowest latency: **pen strokes** |
| `GCK16` (8), `GLKW16` (9) | "eclipse": night mode, inverted, in hardware |
| `GCC16` (10) | Kaleido-tuned: **colour images** |
| `GLRC16` (11) | Kaleido REAGL: colour over text, no flash |

Two rules matter. Anything Kaleido-tuned (`GCC16`, `GLRC16`) must be paired
with `UPDATE_MODE_FULL`, not partial. And the eclipse modes misbehave
unless the framebuffer is 32 bpp — which is moot on colour devices, since
they only support 32 bpp anyway. CrossKobo forces 32 bpp at startup if it
finds anything else, and restores the previous mode on exit.

Colour rendering itself is done by the kernel: at 32 bpp, every update runs
the colour-filter-array pass, and the flags select how aggressive it is.

| Flag | Effect |
| --- | --- |
| `CFA_EINK_G1` | standard (the default) |
| `CFA_EINK_G2` | gentle saturation boost, roughly +50% HSP |
| `CFA_EINK_AIE_S4/S7/S9` | E Ink's own enhancement, increasingly strong and increasingly prone to banding |
| `CFA_EINK_G0` | desaturate |
| `CFA_SKIP` | no colour processing: render as greyscale |

CrossKobo exposes these as *Settings → Display → Colour rendering*.

Pen updates use `A2` together with `HWTCON_FLAG_FORCE_A2_OUTPUT`, which
clamps output to two tones and skips the slow transitions. It is what makes
ink appear under the nib instead of a frame later; the cost is a slightly
grainy line, so the page is settled with a `GL16` refresh when the pen
lifts.

Older i.MX devices take the `mxcfb` path in the same file: `MXCFB_SEND_UPDATE`,
waveform modes `DU`/`GL16`/`GC16`/`A2`/`REAGL`, and `TEMP_USE_AMBIENT`.

## Input

Input devices are enumerated rather than hard-coded, because the numbering
moves between models and firmware versions. `src/platform/input.cpp` opens
every `/dev/input/event*` and classifies it with `EVIOCGBIT`:

- **touch panel**: `EV_ABS` with `ABS_MT_POSITION_X`, or `ABS_X`/`ABS_Y`
  with `BTN_TOUCH`. Multitouch protocol B (`ABS_MT_SLOT`,
  `ABS_MT_TRACKING_ID`) and the older single-touch protocol are both
  handled.
- **digitiser**: reports `BTN_TOOL_PEN` or `BTN_TOOL_RUBBER`, plus
  `ABS_PRESSURE`. The rubber tool is the stylus's eraser end.
- **buttons**: key codes 193 and 194 are the page-turn buttons on the
  Libra/Forma/Sage bodies, 102 is Home, 90 the light button, 116 power, and
  35/59 the sleep cover.

Axis ranges come from `EVIOCGABS` and are normalised, so a digitiser whose
resolution differs from the panel still lands in the right place. Whether
an axis needs mirroring or transposing varies by model — the Libra Colour
needs Y mirrored on touch — so the transform is a setting with a
calibration screen behind it, rather than a table of guesses.

## Power

- Front light: `/sys/class/backlight/mxc_msp430.0/brightness`, with
  `max_brightness` alongside it for the scale.
- Warmth on natural-light devices: `/sys/class/backlight/lm3630a_led/color`,
  0-10, counting down from warmest.
- Battery: `/sys/class/power_supply/bd71827_bat/{capacity,status}`.
- Suspend is a sequence, not a single write:

  ```sh
  echo 1 > /sys/power/state-extended   # flag subsystems
  sleep 2                              # the stock software waits too
  sync
  echo mem > /sys/power/state          # returns on wake
  echo 0 > /sys/power/state-extended
  ```

  On MediaTek devices, suspending while plugged in hangs the kernel, so
  CrossKobo refuses to try (the same workaround KOReader carries).

## Boot and the stock UI

The stock interface is `nickel`, a Qt application, with helpers
(`hindenburg`, `sickel`, `fickel`, …). Stopping it cleanly means
`killall -TERM` on the whole family, waiting for it to go, and removing
`/tmp/nickel-hardware-status` — a FIFO that udev and the DHCP scripts write
to, and which blocks their `open()` forever once nickel is gone.

Before stopping it, CrossKobo reads `/proc/<pid>/environ` and keeps
`PLATFORM`, `PRODUCT`, `DBUS_SESSION_BUS_ADDRESS`, `WIFI_MODULE`,
`INTERFACE`, `NICKEL_HOME` and `LANG`. Those are exported by the firmware's
`rcS` *after* the boot hook runs, so the only way to get them is from the
running process, and restarting nickel later needs them.

Restarting the stock UI differs by firmware generation:

- **4.x**: recreate the FIFO, then
  `LD_LIBRARY_PATH=/usr/local/Kobo /usr/local/Kobo/nickel -platform kobo
  -skipFontLoad` with `hindenburg` alongside it, and `udevadm trigger`.
- **5.x**: run the system's own `/etc/init.d/z-nickel-hardware-status`
  followed by `/etc/rc.local`.

### Getting started at boot

Two mechanisms, matching the two firmware layouts:

- **4.x** runs `/etc/init.d/on-animator.sh` to draw the boot animation.
  Replacing it is the long-established way to start a third-party program
  (fmon, KFMon, Kobo Start Menu all do it). CrossKobo's replacement starts
  its launcher with `setsid` — nickel kills `on-animator.sh` by name when
  it starts, and a detached child survives that — and then draws the same
  animation frames the stock script does.
- **5.x** uses `/etc/rcS.d`, so CrossKobo installs `/etc/init.d/crosskobo`
  and a `S99crosskobo` symlink, and sources
  `/usr/libexec/platform/nickel-env.sh` for the environment.

### Installing

- **4.x**: `/mnt/onboard/.kobo/KoboRoot.tgz` is extracted to `/` as root at
  the next boot, then deleted, then the device reboots. This is the stock
  update mechanism and needs no cooperation from anything.
- **5.x**: `/mnt/onboard/.kobo/update.tar` containing `driver.sh`, which the
  updater calls with the archive path and a stage name. Returning non-zero
  from stage 1 avoids a reboot into recovery.

A firmware update rewrites the root filesystem, so anything installed this
way has to be reinstalled afterwards. Nothing on the user partition is
affected, which is why CrossKobo keeps books, notebooks, settings and
reading state there.
