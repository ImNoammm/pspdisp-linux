# PSPdisp for Linux

Use a PSP as a second monitor (and a gamepad) for a Linux PC, over USB or Wi-Fi.

This is a clean-room Linux port of [PSPdisp](https://github.com/PSP-Archive/PSPdisp)
by Jochen Schleu (JJS). The original was Windows only: a Delphi app plus a kernel
display-mirror driver. I replaced that whole host stack with a small Linux daemon
and patched the PSP homebrew so it builds against a current `pspdev`. The on-wire
protocol is untouched, so the same homebrew talks to either host.

It works on real hardware. Capture goes to JPEG, over USB or Wi-Fi, onto the PSP
screen, and the buttons and analog stick come back as a gamepad. Around 57 to 60
fps, since the PSP is vblank-locked at 60.

```
capture (wlroots screencopy / X11)  -> scale 480x272 -> baseline JPEG
                                    -> transport (USB libusb / TCP "WLAN") -> PSP
PSP buttons <- uinput gamepad <- transport <- PSP
PC audio    -> PulseAudio -> PSP             (optional)
```

## Quick start

```bash
git clone https://github.com/ImNoammm/pspdisp-linux.git && cd pspdisp-linux
./install.sh
```

It asks where to install, whether to build and flash the PSP homebrew, which of
the two PSP apps you want, and on X11 whether to set up the framebuffer headroom
for a real second screen. Enter accepts the default each time. To skip the
prompts: `--user`/`--system`, `--with-psp`/`--no-psp`, `--wifi`/`--no-wifi`,
`--flash`/`--no-flash`, `--no-sudo`.

Which PSP app you get matters. The default is `psp/source-min`, a stripped
USB-only build that boots straight into the display and drops the on-PSP menu,
WLAN, the on-screen keyboard and audio. It is the one you want unless you need
Wi-Fi. Pass `--wifi` for the full app in `psp/source`, which is what WLAN mode
and host audio (`-a`) need.

Then launch PSPdisp from the PSP Game menu. On the full app, pick USB or WLAN.

### Running it

```bash
pspdisp               # USB; new display on sway/Hyprland/X11, mirror on KDE/GNOME
pspdisp -i            # also expose PSP buttons + stick as a uinput gamepad
pspdisp --no-display  # mirror an existing screen instead of a new one
pspdisp -o HDMI-A-1   # mirror one specific monitor by name (Wayland and X11)
pspdisp --no-display -x 1920 -y 0 -w 2560 -h 1440  # mirror a region (X11)
pspdisp -t tcp        # Wi-Fi: PSP connects to this PC, port 17584 (full app only)
pspdisp --background  # run detached (logs to $XDG_RUNTIME_DIR/pspdisp.log)
pspdisp --kill        # stop a running host cleanly
pspdisp --help        # all options, grouped, with examples
```

With several monitors, `-o NAME` mirrors one of them. Names come from
`xrandr | grep ' connected'` on X11, or `swaymsg -t get_outputs` / `wlr-randr` on
Wayland. On X11, `-o` looks the geometry up for you, or you can pass an explicit
rectangle with `-x/-y/-w/-h` (use the `WxH+X+Y` that `xrandr` prints). Drop
`--no-display` and you get a new extra screen instead of a mirror.

[`linux-host/README.md`](linux-host/README.md) has the rest of the options.

## What actually got tested

Two setups, both real hardware: a PSP Slim on sway/Wayland (NVIDIA PC), and the
same PSP on X11 (a Steam Deck, amdgpu, via `startx`). Anything outside those
should work but I have not proven it.

Both transports are proven, USB and Wi-Fi alike, as is capture on sway/wlroots
(wlr-screencopy) and on X11 (XShm). On X11 that covers both mirroring and the
automatic second screen, where plain `pspdisp` builds the framebuffer strip and
tears it down again on exit, and `-o NAME` and an explicit `-x/-y/-w/-h` region
both behave. The gamepad, rotation and dirty-frame skipping work, and so does
running detached with `--background` and stopping with `--kill`.

Three things I have not proven. The automatic virtual display on Hyprland is
written and should work, but I have no Hyprland box to try it on. The PipeWire
portal backend for KDE and GNOME is experimental, and the
[compositor section](#compositor-support) explains where it stands. Audio (`-a`)
runs, but I never validated A/V sync, and it needs the full PSP app anyway.

No warranty on any of it. It talks to your USB devices, creates virtual outputs,
and with `-i` it injects input, so read what it does before pointing it at
something you care about. Bug reports with a `-v` log are welcome.

## Compositor support

| Session | Capture | New virtual monitor |
|---------|---------|---------------------|
| sway / Hyprland (wlroots) | wlr-screencopy | automatic, created on start and removed on stop |
| KDE / GNOME (Wayland) | PipeWire portal, experimental (see below) | mirror only, no portable virtual-output API |
| other wlroots (river, Wayfire, labwc) | wlr-screencopy | use `swaymsg` or set one up manually, or mirror |
| X11 (any desktop) | XShm | automatic, after a one-time framebuffer setup the installer offers |

The portal backend (`-b portal`) does the full xdg-desktop-portal handshake and
reads frames over PipeWire, including DMA-BUF, negotiated with SPA modifiers and
imported through EGL/GLES for readback. It is only built when its dependencies
are present at compile time. It goes through the Mesa path on Intel and AMD. On
the NVIDIA proprietary driver the producer only offers the implicit modifier,
which NVIDIA's EGL cannot detile, so frames come out garbled. That is a known
NVIDIA/wlroots limitation rather than a bug here, and the fix is to run an X11
session instead, which supports everything. I have not verified either path on
hardware myself.

## Getting a real extra monitor

Just run `pspdisp`. It makes a dedicated virtual monitor for the PSP and removes
it again when you stop. Then drag windows onto it:

```bash
pspdisp -i                                   # new PSP screen + gamepad
swaymsg 'move window to output HEADLESS-1'   # (sway) send a window to it
```

sway and Hyprland handle this themselves. On X11 it also just works, but only
after a one-time setup: the installer enlarges the X
framebuffer with a `Virtual` xorg snippet and you restart X once. From then on
`pspdisp` grows the framebuffer, carves the off-screen strip into a monitor with
`xrandr --setmonitor`, captures it, and tears the whole thing down on exit, no
flags needed.

None of that is driver-specific, which is the point: it needs no DDX
virtual-output support, so it runs on AMD, Intel and NVIDIA alike, unlike the
`VirtualHeads` option that amdgpu and the NVIDIA DDX simply do not have. The
installer reuses your existing GPU `Device` section, so it does not swap drivers
and leaves your display tweaks (overscan, scaling, TearFree) alone.

<details><summary>Manual X11 setup, if you skipped the installer prompt</summary>

Add a `Screen` with extra `Virtual` headroom pointing at your existing GPU
`Device` (its `Identifier` is in your current xorg config, e.g. `"AMD"`):

```bash
sudo tee /etc/X11/xorg.conf.d/20-pspdisp-virtual.conf >/dev/null <<'EOF'
Section "Screen"
    Identifier "PSPdisp Screen"
    Device     "AMD"          # <- your existing Device Identifier
    SubSection "Display"
        Virtual 5120 2160
    EndSubSection
EndSection
EOF
# log out and back in (restart X), then just run: pspdisp
```

No explicit `Device` section? Create one for the driver you are already using
(`amdgpu`, `nvidia`, `modesetting`, `intel`) alongside the `Screen` above. That
is what the installer does for you.
</details>

Use `--no-display` to mirror an existing screen instead, or `-o NAME` for a
specific output.

## Repo layout

| Path | What |
|------|------|
| `linux-host/` | the Linux host daemon in C: capture, JPEG, USB/TCP, uinput, audio |
| `psp/source/` | the original PSP homebrew, patched to build on modern `pspdev` |
| `psp/source-min/` | stripped USB-only rewrite of the homebrew, wire-compatible |
| `install.sh` | installer for apt/dnf/pacman/zypper; `--with-psp` builds the EBOOT |
| `build-psp.sh` | builds `EBOOT.PBP`, `usbhostfs.prx` and `kernel.prx` into `linux-host/psp-payload/` |
| `linux-host/install-psp.sh` | copies the payload onto a USB-mounted PSP |

## Building the PSP homebrew

`./install.sh --with-psp` pulls the [pspdev](https://github.com/pspdev/pspdev)
toolchain and builds whichever app you picked. By hand, there is one script per
app:

```bash
# minimal USB-only app (what the installer builds by default)
PSPDEV=/usr/local/pspdev ./psp/source-min/build.sh psp/source-min/payload
./linux-host/install-psp.sh psp/source-min/payload

# full app, with WLAN and audio
PSPDEV=/usr/local/pspdev ./build-psp.sh            # -> linux-host/psp-payload/
./linux-host/install-psp.sh
```

Either way the PSP needs custom firmware to launch unsigned homebrew.

## Running it as a service

If you want clean start/stop and auto-restart without anything ever sending
`kill -9` at the device, use the bundled user unit
([`linux-host/systemd/pspdisp.service`](linux-host/systemd/pspdisp.service)):

```bash
mkdir -p ~/.config/systemd/user
cp linux-host/systemd/pspdisp.service ~/.config/systemd/user/
echo 'OPTS=-t usb -o HEADLESS-1 -q 65 -f 60' > ~/.config/pspdisp.conf   # your options
systemctl --user daemon-reload
systemctl --user enable --now pspdisp
systemctl --user restart pspdisp     # restart cleanly anytime
```

## Troubleshooting

Black screen while it is running is not one single bug. Two of the causes were
self-inflicted and the host now blocks them: a single-instance lock stops two
hosts fighting over the same USB device, and shutting down with `SIGTERM`
(systemd, or `pkill -TERM -x pspdisp`) lets libusb hand the device back
properly. Do not `kill -9` it. That leaves the interface claimed, and the kernel
force-resets the PSP into a reconnect loop.

The cause that is left is the cable. A marginal or charge-only one shows up as
`error -71` and constant disconnects in `dmesg`. Use a real data cable on a
direct port, or use Wi-Fi (`-t tcp`), which sidesteps the cable entirely. Either
way the host reconnects itself through brief drops.

On KDE or GNOME under Wayland, try `-b portal`. If your build was compiled
without portal support, or you hit the NVIDIA tiling problem above, an X11
session with `-b x11` is the way out.

For the `-i` gamepad you need access to `/dev/uinput`. `make install` drops in a
udev rule; log back in afterwards, or run as root.

## Credits and license

- Original PSPdisp by Jochen Schleu (JJS), BSD. See [`LICENSE`](LICENSE).
- Bundled usbhostfs from PSPLINK (BSD).
- intraFont and the danzeff on-screen keyboard by their respective authors, see
  `psp/source/library/`. Danzeff keyboard skins from
  [PSP-Archive/Danzeff](https://github.com/PSP-Archive/Danzeff).
- Linux host and homebrew modernization: this repo, BSD, same spirit.
