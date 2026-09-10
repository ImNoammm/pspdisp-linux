# PSPdisp, Linux host

This captures a screen, scales it to 480x272, encodes a baseline JPEG and pushes
it to the PSP over USB or TCP, with button and analog state coming back on the
same link. It is a clean-room replacement for the old Windows host (a Delphi app
plus an XPDM mirror driver).

The wire protocol is the original one, so the host does not care which PSP app
is running: the patched full homebrew in `psp/source`, the stripped USB-only
build in `psp/source-min`, or a stock PSPdisp EBOOT. WLAN mode and `-a` audio
need the full app; the minimal one is USB only.

Built and validated against sway 1.11 (wlroots) on Wayland, first with a
protocol-level test client and then on a real PSP Slim.

```
capture (wlroots / X11 / portal) -> scale 480x272 -> baseline JPEG
                                 -> transport (USB libusb / TCP "WLAN") -> PSP
PSP buttons <- uinput gamepad <- transport <- PSP
PC audio    -> PulseAudio -> PSP             (optional, -a)
```

## Capture backends

| Backend | Where it applies | How |
|---------|------------------|-----|
| `wlr` (default on Wayland) | wlroots compositors: sway, Hyprland, river, Wayfire, labwc | `wlr-screencopy-unstable-v1`, copies an output into shm |
| `x11` | Xorg and Xwayland | MIT-SHM region grab off the root window |
| `portal` | KDE and GNOME on Wayland | xdg-desktop-portal plus PipeWire, including DMA-BUF via EGL/GLES |

The backend is picked automatically from your session. Override it with
`-b wlr|x11|portal`.

`portal` only gets compiled in when its dependencies are installed at build
time (libpipewire-0.3, gio-2.0, gio-unix-2.0, gbm, libdrm, egl, glesv2), and it
is still experimental. Builds without it fall back to `wlr`, so KDE and GNOME
users on such a build need `-b x11` through Xwayland. It goes through the Mesa
path on Intel and AMD. On the NVIDIA proprietary driver the producer offers only
the implicit modifier, which NVIDIA's EGL cannot detile, so frames come out
garbled; use an X11 session there instead.

## Getting a second screen instead of a mirror

By default the host makes its own virtual output and captures that, so the PSP
behaves like a real extra monitor and the output disappears again when you stop.
It handles this on sway and Hyprland by asking the compositor, and on X11 by
growing the framebuffer and carving the off-screen strip into a monitor with
`xrandr --setmonitor` (which needs the one-time `Virtual` xorg headroom that
`install.sh` sets up).

```bash
pspdisp -i                                   # new PSP screen + gamepad
swaymsg 'move window to output HEADLESS-1'   # (sway) drag a window over
```

Pass `--no-display` to mirror something that already exists, or `-o NAME` to
mirror one particular output. List names with `swaymsg -t get_outputs`,
`wlr-randr`, or `xrandr`.

On the wlroots compositors the host does not drive for you (river, Wayfire,
labwc), make the output yourself and capture it by name:

```bash
swaymsg create_output                        # creates HEADLESS-1
swaymsg output HEADLESS-1 mode 480x272 position 1920 0
pspdisp -o HEADLESS-1 -i                     # capture it, buttons as a gamepad
```

Rotation: the host only scales the captured output into the wire image, and the
PSP does the geometric rotation itself, same as the original. So for an upright
`-r 90` or `-r 270` view, make the captured output portrait first
(`swaymsg output HEADLESS-1 mode 272x480`), otherwise it comes out squished.

## Options

```
Connection
  -t usb|tcp           how to reach the PSP (default usb; tcp = Wi-Fi)
  -P PORT              TCP port (default 17584)
  -k PASSWORD          Wi-Fi password, has to match the PSP

What to show
  --no-display         mirror an existing screen instead of making a new one
  -o NAME              mirror this monitor by name
  -b wlr|x11|portal    capture backend (default: auto)
  -x -y -w -h N        capture an explicit region (x11 only)
  -r 0|90|180|270      rotate (default 0)

Quality and speed
  -q 1..100            JPEG quality (default 100; lower is faster)
  -f N                 fps cap (default 60; the PSP caps at 60 anyway)

Extras
  -i                   expose PSP buttons as a uinput gamepad
  -a                   stream PC audio to the PSP (experimental)
  -v                   verbose: fps and button data
  --background, -D     run detached, logs to $XDG_RUNTIME_DIR/pspdisp.log
  --kill               stop a running host cleanly
  --help, -H           this list, with examples
```

## Build and install

From the repo root, `./install.sh` does the lot on Debian/Ubuntu/PikaOS (apt),
Fedora (dnf), Arch (pacman) and openSUSE (zypper). Add `--with-psp --flash` to
also fetch pspdev, build the homebrew and copy it onto a connected PSP. By hand:

```bash
sudo apt-get install build-essential libusb-1.0-0-dev libjpeg-dev \
     libx11-dev libxext-dev libwayland-dev wayland-protocols libpulse-dev pkg-config
make
sudo make install        # binary + udev rule (rule optional)
sudo udevadm control --reload && sudo udevadm trigger
```

Fedora: `libusb1-devel libjpeg-turbo-devel libX11-devel libXext-devel wayland-devel wayland-protocols-devel pulseaudio-libs-devel`
Arch: `libusb libjpeg-turbo libx11 libxext wayland wayland-protocols libpulse`

For the portal backend add `pipewire-devel`/`libpipewire-0.3-dev`, `libgbm-dev`,
`libdrm-dev` and the EGL/GLES dev packages for your distro.

The udev rule (`99-pspdisp.rules`) gives a normal user access to the PSP and to
`/dev/uinput`. Without it you need `sudo` for USB and for `-i`.

### PSP homebrew

`../build-psp.sh` builds the full app's `EBOOT.PBP`, `usbhostfs.prx` and
`kernel.prx` into `psp-payload/`, using the pspdev toolchain that
`install.sh --with-psp` fetches. For the minimal USB-only app the equivalent is
`../psp/source-min/build.sh OUTDIR`, which is what the installer runs by
default. `./install-psp.sh [PAYLOAD_DIR]` then copies a payload to a USB-mounted PSP under
`PSP/GAME/PSPdisp/`, checksum-verifying every file, since a flaky cable can
corrupt a copy silently. Launch PSPdisp from the PSP Game menu afterwards.

## Running it

USB is the default. Launch PSPdisp on the PSP, pick USB, then:

```bash
pspdisp                 # new virtual screen
pspdisp -o HEADLESS-1   # capture a specific output
pspdisp -r 90 -i        # rotate, and expose the buttons as a gamepad
pspdisp -q 60 -f 25     # trade quality for speed
```

For Wi-Fi the PSP connects to this PC. This needs the full PSP app; the minimal
one has no WLAN. Pick WLAN on the PSP, give it this PC's IP and the password,
then:

```bash
pspdisp -t tcp -k MYPASSWORD          # default port 17584
```

Stop it with `--kill` or `SIGTERM`. Never `kill -9`: that leaves the USB
interface claimed and the kernel resets the PSP into a reconnect loop.

## Audio

`-a` grabs PC audio through PulseAudio and streams it to the PSP, which again
means the full PSP app. Point it at a
*monitor* source so you capture playback rather than the microphone:

```bash
PSPDISP_AUDIO_SOURCE=$(pactl get-default-sink).monitor pspdisp -a
```

Framing matches the Windows app (22050 Hz, chunk 2240, so 4480-byte S16 stereo
frames), but I never validated A/V sync on hardware. Treat it as experimental,
and it behaves best at the default fps.

## Files

```
main.c            arg parse + frame pump (dirty-skip, audio mux, response)
proto.h           wire protocol, mirrors psp/source/shared.h
pspdisp.h         module interfaces
transport_usb.c   libusb bulk + usbhostfs hello handshake
transport_tcp.c   TCP server + the 32-byte WLAN password exchange
capture_wlr.c     wlr-screencopy (Wayland)
capture_x11.c     MIT-SHM region (X11)
capture_portal.c  xdg-desktop-portal + PipeWire, incl. DMA-BUF (optional build)
display_auto.c    make/remove the virtual output (sway, Hyprland, X11)
frame.c           scale + rotate + baseline JPEG + content hash
input.c           uinput virtual gamepad
audio_pulse.c     PulseAudio capture
protocol/         vendored wlr-screencopy XML, run through wayland-scanner
```

## Not ported

- A/V sync hardening for audio.
- The WLAN Wake-on-LAN helper.
- On-PSP settings and OSK menu sync. The PSP can still drive itself; the host
  just ignores the settings block it sends back.
- The Windows SideShow gadget, which was dead Microsoft tech anyway.
