# PSPdisp — Linux host

Use a PSP as a second display (and gamepad) for a Linux PC. Clean-room Linux
replacement for the original Windows host (Delphi app + XPDM mirror driver).
**The PSP side is unchanged** — it runs the stock PSPdisp homebrew EBOOT.

```
capture (wlroots / X11) ─▶ scale 480×272 ─▶ baseline JPEG
                        ─▶ transport (USB libusb / TCP "WLAN") ─▶ PSP
PSP buttons ◀── uinput gamepad ◀── transport ◀── PSP
PC audio   ──▶ PulseAudio ──▶ PSP            (optional, -a, untested)
```

## Status

Built and validated on **sway 1.11 (wlroots) / Wayland**: capture →
480×272 baseline JPEG → TCP transport with the real 32-byte WLAN handshake,
dirty-frame skipping, and button/analog responses all verified with a
protocol-level test client. The only leg not yet exercised on hardware is the
USB transport to a physical PSP (libusb code mirrors the proven Windows
recipe) and audio A/V sync.

## Capture backends

| Backend | Use | How |
|---------|-----|-----|
| `wlr` (default on Wayland) | wlroots compositors: sway, Hyprland, river, Wayfire, labwc | `wlr-screencopy-unstable-v1`, copies a whole output into shm |
| `x11` | Xorg / Xwayland | MIT-SHM region grab of the root window |

Auto-selected from `$WAYLAND_DISPLAY`; override with `-b wlr|x11`.

### A real second screen (sway)

The host captures an *output*. To get a genuine extra monitor instead of
mirroring an existing one, make a virtual output and capture it:

```bash
swaymsg create_output                       # creates HEADLESS-1
swaymsg output HEADLESS-1 mode 480x272 position 1920 0
pspdisp -o HEADLESS-1 -i                     # capture it, PSP buttons as gamepad
```

Drag windows onto `HEADLESS-1` — they appear on the PSP. `-o` also takes any
real output name (e.g. `HDMI-A-1`) to mirror it. List names with `swaymsg -t get_outputs`.

**Rotation:** the host only scales the captured output into the wire image; the
PSP performs the geometric rotation (matching the original). For an upright
`-r 90`/`-r 270` view, make the captured output *portrait* (e.g.
`swaymsg output HEADLESS-1 mode 272x480`) so it isn't squished.

## Install

One command from the repo root (Debian/Ubuntu/PikaOS, Fedora, Arch, openSUSE):

```bash
./install.sh                      # deps + build + install the host daemon
./install.sh --with-psp --flash   # also fetch pspdev, build the PSP homebrew,
                                  # and copy it onto a connected PSP
```

Or build the host manually:

```bash
sudo apt-get install build-essential libusb-1.0-0-dev libjpeg-dev \
     libx11-dev libxext-dev libwayland-dev wayland-protocols libpulse-dev pkg-config
make
sudo make install        # binary + udev rule (optional)
sudo udevadm control --reload && sudo udevadm trigger
```

### PSP homebrew

`./build-psp.sh` builds `EBOOT.PBP` + `usbhostfs.prx` + `kernel.prx` into
`psp-payload/` (needs the pspdev toolchain — `install.sh --with-psp` fetches
it). `./install-psp.sh` copies the payload to a USB-mounted PSP's
`PSP/GAME/PSPdisp/`. Then launch PSPdisp from the PSP Game menu.

Fedora: `libusb1-devel libjpeg-turbo-devel libX11-devel libXext-devel wayland-devel wayland-protocols-devel pulseaudio-libs-devel`
Arch:   `libusb libjpeg-turbo libx11 libxext wayland wayland-protocols libpulse`

The udev rule (`99-pspdisp.rules`) grants a normal user access to the PSP and
`/dev/uinput`; otherwise run with `sudo`.

## Run

**USB** (default) — on the PSP launch PSPdisp and pick **USB** mode, then:

```bash
pspdisp                 # mirror first output
pspdisp -o HEADLESS-1   # capture a virtual sway output
pspdisp -r 90 -i        # rotate 90°, expose buttons as a gamepad
pspdisp -q 60 -f 25     # quality / fps
```

**WLAN** (TCP) — the PSP connects to this PC. Pick WLAN mode on the PSP, set
this PC's IP and the password, then:

```bash
pspdisp -t tcp -k MYPASSWORD          # default port 17584
```

Options: `-t usb|tcp`, `-b wlr|x11`, `-o OUTPUT`, `-x -y -w -h` (x11 region),
`-r 0|90|180|270`, `-q 1..100`, `-f fps`, `-P port`, `-k password`,
`-i` gamepad, `-a` audio, `-v` verbose.

## Audio (optional, untested)

`-a` captures PC audio via PulseAudio and streams it to the PSP. Pick a
*monitor* source so you capture playback, not the mic:

```bash
PSPDISP_AUDIO_SOURCE=$(pactl get-default-sink).monitor pspdisp -a
```

Framing matches the Windows app (22050 Hz, chunk 2240 → 4480-byte S16 stereo
frames) but A/V sync was not validated on hardware — treat as experimental,
and works best at the default fps.

## Module layout

```
main.c            arg parse + frame pump (dirty-skip, audio mux, response)
proto.h           wire protocol (mirrors psp/source/shared.h)
pspdisp.h         module interfaces
transport_usb.c   libusb bulk + usbhostfs hello handshake
transport_tcp.c   TCP server + 32-byte WLAN password
capture_wlr.c     wlr-screencopy (Wayland)
capture_x11.c     MIT-SHM region (X11)
frame.c           scale + rotate + baseline JPEG + content hash
input.c           uinput virtual gamepad
audio_pulse.c     PulseAudio capture
protocol/         vendored wlr-screencopy XML (wayland-scanner at build)
```

See `../PORT.md` for the full protocol and architecture analysis.

## Not ported

- Audio A/V sync hardening; the WLAN Wake-on-LAN helper.
- On-PSP settings/OSK menu sync (the PSP can still drive itself; the host just
  ignores the settings block it sends back).
- The Windows SideShow gadget (dead Microsoft tech).
- PipeWire portal capture for non-wlroots Wayland (GNOME/KDE) — use `-b x11`
  via Xwayland there for now.
