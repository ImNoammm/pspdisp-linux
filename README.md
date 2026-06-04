# PSPdisp for Linux

Turn a **PSP into a USB or Wi‑Fi second monitor (and gamepad) for a Linux PC.**

This is a clean‑room Linux port of [PSPdisp](https://github.com/PSP-Archive/PSPdisp)
by Jochen Schleu (JJS). The original was Windows‑only (a Delphi app + a kernel
display‑mirror driver). This project replaces that entire host stack with a
small Linux daemon, and modernises the PSP homebrew so it builds with a current
`pspdev` toolchain. **The on‑wire protocol is unchanged**, so the same PSP
homebrew talks to either host.

> Status: **working on real hardware.** Capture → JPEG → USB/Wi‑Fi → PSP
> display, and PSP buttons/analog back to the PC, all verified on a PSP Slim
> over sway/Wayland. ~57–60 fps (the PSP is vblank‑locked at 60).

## ⚠️ Disclaimer — what's tested

This was developed and verified on **one setup**: a PSP Slim + **sway/Wayland**
on an **NVIDIA** PC. Treat anything outside that as "should work, not proven."

| Feature | Status |
|---|---|
| USB transport, Wi‑Fi (TCP) transport | ✅ verified on hardware |
| Capture on **sway / wlroots** (wlr‑screencopy) | ✅ verified |
| Capture on **X11** (XShm) | ✅ verified |
| Auto virtual display (sway) | ✅ verified |
| Gamepad (uinput), rotation, dirty‑skip | ✅ verified |
| Auto virtual display on **Hyprland** | ⚠️ written, **not tested** (no Hyprland here) |
| Auto virtual display on **X11** (VirtualHeads) | ⚠️ written, **not tested** (dev box is Wayland) |
| **KDE/GNOME Wayland** capture (PipeWire portal, `-b portal`) | ⚠️ **experimental** — full pipeline runs, but DMA‑BUF readback is unverified on Mesa and **garbles on NVIDIA** (driver limitation). See the [compositor table](#compositor-support). |
| Audio (`-a`) | ⚠️ best‑effort, A/V sync **not validated** |

No warranty — it talks to your USB devices, creates virtual outputs, and (with
`-i`) injects input. Read before running on something you care about. Bug
reports with a `-v` log welcome.

```
capture (wlroots screencopy / X11)  ─▶ scale 480×272 ─▶ baseline JPEG
                                    ─▶ transport (USB libusb / TCP "WLAN") ─▶ PSP
PSP buttons ◀── uinput gamepad ◀── transport ◀── PSP
PC audio   ──▶ PulseAudio ──▶ PSP            (optional)
```

## Quick start

```bash
git clone https://github.com/ImNoammm/pspdisp-linux.git && cd pspdisp-linux
./install.sh
```

`install.sh` asks two questions (where to install, and whether to also build +
flash the PSP homebrew — defaults: per‑user, yes) then does everything. Skip the
prompts with flags: `--user`/`--system`, `--with-psp`/`--no-psp`, `--flash`,
`--no-flash`, `--no-sudo`.

Then on the PSP: launch **PSPdisp** from the Game menu → pick **USB** or **WLAN**.

### Running it

```bash
pspdisp               # USB; new display on sway/Hyprland/X11, mirror on KDE/GNOME
pspdisp -i            # also expose PSP buttons + stick as a uinput gamepad
pspdisp --no-display  # mirror an existing screen instead of a new one
pspdisp -o HDMI-A-1   # mirror one specific output
pspdisp -t tcp        # Wi‑Fi: PSP connects to this PC (port 17584)
pspdisp --kill        # stop a running host cleanly
pspdisp --help        # all options, grouped, with examples
```

> **Where you get a real new display vs. a mirror:**
> - **sway / Hyprland** — dedicated virtual monitor, created automatically and
>   removed on exit. No flag, no setup.
> - **X11** (any desktop, incl. KDE/GNOME on X11) — same, after a one‑time setup
>   the installer offers (a VirtualHeads snippet + one X restart). Then plain
>   `pspdisp` makes the new display itself.
> - **KDE / GNOME on Wayland** — mirror only; there's no portable way to make a
>   virtual output there yet.
>
> See [Compositor support](#compositor-support) for the full table.

See [`linux-host/README.md`](linux-host/README.md) for all options.

## Compositor support

| Session | Capture | New virtual monitor |
|---------|---------|---------------------|
| **sway / Hyprland** (wlroots) | ✅ wlr‑screencopy | ✅ **automatic** — created on start, removed on stop |
| **KDE / GNOME** (Wayland) | ⚠️ PipeWire portal — **experimental**¹ | ⚠️ mirror only (no portable virtual‑output API) |
| Other wlroots (river/Wayfire/labwc) | ✅ wlr‑screencopy | use `swaymsg`/manual or mirror |
| **X11** (any desktop) | ✅ XShm | ✅ automatic after a one‑time VirtualHeads setup (installer offers it) |

¹ The PipeWire‑portal backend (`-b portal`) does the full xdg‑desktop‑portal
handshake and reads frames over PipeWire, including **DMA‑BUF** (negotiated with
SPA modifiers, imported via EGL/GLES and read back). It works on **Mesa
(Intel/AMD)**. On the **NVIDIA proprietary driver** the producer only offers the
implicit modifier, which NVIDIA's EGL can't detile, so frames come out garbled —
a known NVIDIA/wlroots limitation, not a pspdisp bug. KDE/GNOME‑Wayland users on
NVIDIA should use an **X11 session** (full support) for now. wlroots, X11, USB
and Wi‑Fi all work everywhere.

## Real extra monitor

Just run `pspdisp` — it creates a dedicated virtual monitor for the PSP and
removes it when you stop it. Drag windows onto it:

```bash
pspdisp -i                                   # new PSP screen + gamepad
swaymsg 'move window to output HEADLESS-1'   # (sway) send a window to it
```

This is automatic on **sway and Hyprland**. On **X11** it also just works after
a one-time setup: the installer writes a `modesetting` `VirtualHeads` snippet (or
do it yourself — see below) and you restart X once; from then on `pspdisp` enables
the virtual output, captures it, and turns it off on exit, no flags needed.

<details><summary>Manual X11 setup (if you skipped the installer prompt)</summary>

```bash
sudo tee /etc/X11/xorg.conf.d/20-pspdisp-virtual.conf >/dev/null <<'EOF'
Section "Device"
    Identifier "PSPdisp modesetting"
    Driver     "modesetting"
    Option     "VirtualHeads" "1"
EndSection
EOF
# log out and back in (restart X), then just run: pspdisp
```
(`evdi` — the hot-pluggable kernel virtual display, `evdi-dkms` — is the other
option, but pspdisp doesn't drive it directly yet.)
</details>

Use `--no-display` to mirror an existing screen instead, or `-o NAME` to capture
a specific output.

On **KDE/GNOME Wayland** there's no portable virtual-output API, so `pspdisp`
mirrors a screen there (no dedicated monitor yet).

## Repo layout

| Path | What |
|------|------|
| `linux-host/` | the Linux host daemon (C): capture + JPEG + USB/TCP + uinput + audio |
| `psp/source/` | the PSP homebrew, patched to build on modern `pspdev` |
| `install.sh` | one‑shot installer (apt/dnf/pacman/zypper); `--with-psp` builds the EBOOT |
| `build-psp.sh` | builds `EBOOT.PBP` + `usbhostfs.prx` + `kernel.prx` → `linux-host/psp-payload/` |
| `linux-host/install-psp.sh` | copies the payload to a USB‑mounted PSP |

## Building the PSP homebrew

`./install.sh --with-psp` fetches the [pspdev](https://github.com/pspdev/pspdev)
toolchain and runs `build-psp.sh`. Manually:

```bash
PSPDEV=/usr/local/pspdev ./build-psp.sh           # -> linux-host/psp-payload/
./linux-host/install-psp.sh                        # copy to a mounted PSP
```

The PSP must run **custom firmware** (CFW) to launch unsigned homebrew.

## Running it reliably (systemd)

For clean start/stop/auto‑restart that never `kill -9`s the device, use the
provided user service ([`linux-host/systemd/pspdisp.service`](linux-host/systemd/pspdisp.service)):

```bash
mkdir -p ~/.config/systemd/user
cp linux-host/systemd/pspdisp.service ~/.config/systemd/user/
echo 'OPTS=-t usb -o HEADLESS-1 -q 65 -f 60' > ~/.config/pspdisp.conf   # your options
systemctl --user daemon-reload
systemctl --user enable --now pspdisp
systemctl --user restart pspdisp     # restart cleanly anytime
```

## Troubleshooting

- **Black screen during use** — this is *not* one bug. The host now prevents the
  two self‑inflicted causes: a **single‑instance lock** stops two hosts fighting
  over the USB device, and stopping it with `SIGTERM` (systemd, or
  `pkill -TERM -x pspdisp`) lets libusb release the device cleanly. **Never
  `kill -9`** — that leaves the interface claimed and the kernel force‑resets the
  PSP into a reconnect loop. The remaining cause is the **USB cable**: a
  marginal/charge‑only cable shows `error -71` and constant disconnects in
  `dmesg`. Use a real **data** cable on a direct port, or use **Wi‑Fi**
  (`-t tcp`) which is cable‑independent. The host auto‑reconnects through brief
  drops either way.
- **Wayland but not wlroots (GNOME/KDE)** — use Xwayland + `-b x11` for now; a
  PipeWire‑portal backend isn't implemented yet.
- **`/dev/uinput` permission** (for `-i` gamepad) — `make install` adds a udev
  rule; re‑login or run as root.

## Credits & license

- Original **PSPdisp** © Jochen Schleu (JJS) — BSD. See [`LICENSE`](LICENSE).
- Bundled **usbhostfs** from PSPLINK (BSD).
- **intraFont** and **danzeff** on‑screen keyboard by their respective authors
  (see `psp/source/library/`); danzeff keyboard skins from
  [PSP-Archive/Danzeff](https://github.com/PSP-Archive/Danzeff).
- Linux host + homebrew modernization: this repo (BSD, same spirit).
