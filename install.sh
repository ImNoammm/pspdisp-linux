#!/usr/bin/env sh
# PSPdisp Linux installer.
#
# Run it with no arguments and it asks a few questions (install location,
# whether to build the PSP homebrew, and which PSP app). Defaults: per-user
# install, build the homebrew, minimal USB-only app. Just press Enter to accept.
#
# Non-interactive overrides (skip the prompts):
#   --user / --system    install to ~/.local/bin  /  /usr/local/bin
#   --with-psp / --no-psp build the PSP homebrew (and flash) or not
#   --wifi / --no-wifi   full WiFi-capable app / minimal USB-only app (default)
#   --no-sudo            skip anything needing root (build only)
#
# Works on Debian/Ubuntu/PikaOS (apt), Fedora (dnf), Arch (pacman),
# openSUSE (zypper). Wayland (wlroots) and X11 both supported.
set -eu

HERE=$(CDPATH= cd "$(dirname "$0")" && pwd)

# tri-state choices: -1 = ask interactively, 0/1 = forced by a flag
USER_INSTALL=-1     # 1 = ~/.local/bin, 0 = /usr/local/bin
WITH_PSP=-1         # 1 = build+flash homebrew, 0 = host only
WIFI=-1             # 1 = full app (USB+WiFi), 0 = minimal USB-only app
FLASH=1; USE_SUDO=1
for a in "$@"; do
  case "$a" in
    --user)     USER_INSTALL=1 ;;
    --system)   USER_INSTALL=0 ;;
    --with-psp) WITH_PSP=1 ;;
    --no-psp)   WITH_PSP=0 ;;
    --wifi)     WIFI=1 ;;                 # install the full (WiFi-capable) app
    --no-wifi)  WIFI=0 ;;                 # build the minimal USB-only app
    --flash)    WITH_PSP=1; FLASH=1 ;;   # implies --with-psp
    --no-flash) FLASH=0 ;;
    --no-sudo)  USE_SUDO=0 ;;
    -h|--help)  sed -n '2,15p' "$0"; exit 0 ;;
    *) echo "unknown option: $a" >&2; exit 1 ;;
  esac
done

# --- interactive prompts (only for choices not forced by a flag) -----------
ask() {  # ask "Question" default(y/n) -> 0 = yes, 1 = no.
         # Reads a single keypress (no Enter needed); Enter alone = default.
  _q="$1"; _def="$2"
  if [ ! -t 0 ]; then [ "$_def" = y ] && return 0 || return 1; fi   # non-tty: default
  _hint=$([ "$_def" = y ] && echo "[Y/n]" || echo "[y/N]")
  printf '%s %s ' "$_q" "$_hint"
  _old=$(stty -g 2>/dev/null)
  stty -icanon -echo min 1 time 0 2>/dev/null
  while :; do
    _k=$(dd bs=1 count=1 2>/dev/null | tr -d '\r\n')   # one keypress, strip Enter
    case "$_k" in
      y|Y) stty "$_old" 2>/dev/null; printf 'y\n'; return 0 ;;
      n|N) stty "$_old" 2>/dev/null; printf 'n\n'; return 1 ;;
      '')  stty "$_old" 2>/dev/null; printf '%s\n' "$_def"   # Enter -> default
           [ "$_def" = y ] && return 0 || return 1 ;;
      *)   printf '\nkey invalid please select y, n or enter for default\n%s %s ' "$_q" "$_hint" ;;
    esac
  done
}

if [ "$USER_INSTALL" = -1 ]; then
  if ask "Install just for you (~/.local/bin)? Choose 'n' for system-wide (/usr/local/bin). (default n)" n
  then USER_INSTALL=1; else USER_INSTALL=0; fi
fi
if [ "$WITH_PSP" = -1 ]; then
  if ask "Also build the PSP homebrew (fetches the pspdev toolchain, ~165 MB)? (default y)" y
  then WITH_PSP=1; else WITH_PSP=0; fi
fi
# Which PSP app: the minimal USB-only rewrite (smaller, faster boot, no menu
# bloat) or the original full app that also supports WiFi/TCP.
if [ "$WITH_PSP" = 1 ] && [ "$WIFI" = -1 ]; then
  if ask "Are you planning to use a WiFi connection? Press n if not, y if you do (n compiles the minimal USB-only app; y installs the full app). (default n)" n
  then WIFI=1; else WIFI=0; fi
fi

SUDO=""
if [ "$USE_SUDO" = 1 ] && [ "$(id -u)" != 0 ]; then
  command -v sudo >/dev/null 2>&1 && SUDO="sudo" || SUDO=""
fi
run_root() { if [ -n "$SUDO" ]; then $SUDO sh -c "$*"; else sh -c "$*"; fi; }

# --- 1. install host build dependencies ------------------------------------
PSPDEV_PKGS=""
detect_and_install_deps() {
  # portal deps (libpipewire + glib/gio) enable the KDE/GNOME Wayland backend;
  # optional — the build auto-detects them.
  if command -v apt-get >/dev/null 2>&1; then
    PKGS="build-essential pkg-config libusb-1.0-0-dev libjpeg-dev libx11-dev libxext-dev libwayland-dev wayland-protocols libpulse-dev libpipewire-0.3-dev libglib2.0-dev"
    PSPDEV_PKGS="curl tar"
    run_root "apt-get update && apt-get install -y $PKGS $PSPDEV_PKGS"
  elif command -v dnf >/dev/null 2>&1; then
    PKGS="gcc make pkgconf-pkg-config libusbx-devel libjpeg-turbo-devel libX11-devel libXext-devel wayland-devel wayland-protocols-devel pulseaudio-libs-devel pipewire-devel glib2-devel"
    run_root "dnf install -y $PKGS curl tar"
  elif command -v pacman >/dev/null 2>&1; then
    PKGS="base-devel pkgconf libusb libjpeg-turbo libx11 libxext wayland wayland-protocols libpulse libpipewire glib2"
    run_root "pacman -Sy --needed --noconfirm $PKGS curl tar"
  elif command -v zypper >/dev/null 2>&1; then
    PKGS="gcc make pkg-config libusb-1_0-devel libjpeg8-devel libX11-devel libXext-devel wayland-devel wayland-protocols-devel libpulse-devel"
    run_root "zypper install -y $PKGS curl tar"
  else
    echo "Unknown package manager. Install manually: libusb-1.0, libjpeg, libx11," >&2
    echo "libxext, wayland-client, wayland-protocols (wayland-scanner), libpulse, gcc, make, pkg-config." >&2
  fi
}

if [ "$USE_SUDO" = 1 ]; then
  echo ">> Installing host build dependencies ..."
  detect_and_install_deps
else
  echo ">> --no-sudo: skipping dependency install (assuming deps present)."
fi

# --- 2. build + install the host daemon ------------------------------------
echo ">> Building host daemon ..."
make -C "$HERE/linux-host" clean >/dev/null 2>&1 || true
make -C "$HERE/linux-host"

if [ "$USER_INSTALL" = 1 ]; then
  echo ">> Installing host daemon to ~/.local/bin (per-user) ..."
  make -C "$HERE/linux-host" install-user
  # udev rule still needs root for USB/uinput access without sudo
  if [ "$USE_SUDO" = 1 ]; then
    run_root "make -C '$HERE/linux-host' install-udev" || true
  else
    echo "   (skip udev rule; for USB without sudo: sudo make -C linux-host install-udev)"
  fi
elif [ "$USE_SUDO" = 1 ]; then
  echo ">> Installing host daemon + udev rule (system-wide) ..."
  run_root "make -C '$HERE/linux-host' install"
  run_root "udevadm control --reload && udevadm trigger" || true
  echo "   installed: $(command -v pspdisp || echo /usr/local/bin/pspdisp)"
else
  echo "   built: $HERE/linux-host/pspdisp  (run 'sudo make -C linux-host install', or ./install.sh --user)"
fi

# --- 2b. X11: give the framebuffer headroom so pspdisp can make a new display -
# sway/Hyprland get an on-the-fly virtual output for free. On X11 pspdisp makes
# the PSP a real extra monitor by growing the framebuffer past the desktop and
# carving the off-screen strip into a head (xrandr --setmonitor) — this needs
# the X screen's "Virtual" size to exceed the desktop, set once via xorg.conf.
# This is driver-agnostic (works on amdgpu / NVIDIA / Intel — unlike VirtualHeads,
# which amdgpu and the NVIDIA DDX don't support). We REUSE the existing GPU
# Device (no driver swap, so display tweaks like overscan are untouched).
# Trigger on an X11 session, OR a DISPLAY with no Wayland, OR a running Xorg with
# no Wayland session (covers installing over SSH onto an X11 box).
if [ "${XDG_SESSION_TYPE:-}" = x11 ] \
   || { [ -n "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; } \
   || { [ -z "${WAYLAND_DISPLAY:-}" ] && [ "${XDG_SESSION_TYPE:-}" != wayland ] && pgrep -x Xorg >/dev/null 2>&1; }; then
  XCONF=/etc/X11/xorg.conf.d/20-pspdisp-virtual.conf
  if [ -f "$XCONF" ]; then
    echo ">> X11 virtual framebuffer already set up ($XCONF)."
  elif ask "X11 detected. Let the PSP be a real extra screen (adds framebuffer headroom; one-time, needs an X restart)?" y; then
    # Reuse the user's existing GPU Device if they have one (don't swap drivers).
    DEVID=$(awk '
      /[Ss]ection[ \t]+"[Dd]evice"/ {ind=1}
      ind && /[Ii]dentifier/ {if (match($0, /"[^"]+"/)) {print substr($0, RSTART+1, RLENGTH-2); exit}}
    ' /etc/X11/xorg.conf.d/*.conf /etc/X11/xorg.conf 2>/dev/null | head -1)
    if [ -n "$DEVID" ]; then
      BODY="Section \"Screen\"
    Identifier \"PSPdisp Screen\"
    Device     \"$DEVID\"
    SubSection \"Display\"
        Virtual 5120 2160
    EndSubSection
EndSection"
    else
      # No explicit Device — detect the running DDX so we match the right driver.
      DDX=modesetting
      for L in "$HOME"/.local/share/xorg/Xorg.*.log /var/log/Xorg.*.log; do
        [ -f "$L" ] || continue
        case "$(grep -oE '\((II|--)\) (AMDGPU|modeset|NVIDIA|intel|NOUVEAU|RADEON)\(0\)' "$L" 2>/dev/null | head -1)" in
          *AMDGPU*) DDX=amdgpu ;; *NVIDIA*) DDX=nvidia ;; *intel*) DDX=intel ;;
          *NOUVEAU*) DDX=nouveau ;; *RADEON*) DDX=radeon ;; *modeset*) DDX=modesetting ;;
        esac
        break
      done
      BODY="Section \"Device\"
    Identifier \"PSPdisp GPU\"
    Driver     \"$DDX\"
EndSection
Section \"Screen\"
    Identifier \"PSPdisp Screen\"
    Device     \"PSPdisp GPU\"
    SubSection \"Display\"
        Virtual 5120 2160
    EndSubSection
EndSection"
    fi
    run_root "mkdir -p /etc/X11/xorg.conf.d && cat > $XCONF <<EOF
# Added by PSPdisp: enlarge the X framebuffer (Virtual) so pspdisp can place the
# PSP as an off-screen monitor. Driver/overscan untouched. Delete to undo.
$BODY
EOF" && echo "   Wrote $XCONF — restart X (log out/in), then just run 'pspdisp'." \
       || echo "   (could not write $XCONF; pspdisp will mirror instead)"
  else
    echo "   Skipped — pspdisp will mirror your screen on X11."
  fi
fi

# --- 3. optional: pspdev toolchain + PSP homebrew --------------------------
if [ "$WITH_PSP" = 1 ]; then
  PSPDEV_DIR="/usr/local/pspdev"
  [ -x "$PSPDEV_DIR/bin/psp-gcc" ] || PSPDEV_DIR="$HOME/pspdev"
  if [ ! -x "$PSPDEV_DIR/bin/psp-gcc" ]; then
    echo ">> Fetching pspdev toolchain ..."
    case "$(uname -m)" in
      x86_64|amd64) ASSET="pspdev-debian-latest.tar.gz" ;;
      aarch64|arm64) ASSET="pspdev-ubuntu-24.04-arm-arm64.tar.gz" ;;
      *) echo "no prebuilt pspdev for $(uname -m); build from github.com/pspdev/pspdev" >&2; ASSET="" ;;
    esac
    if [ -n "$ASSET" ]; then
      URL=$(curl -sL https://api.github.com/repos/pspdev/pspdev/releases/latest \
            | grep browser_download_url | grep "$ASSET" | head -1 | cut -d'"' -f4)
      TMP=$(mktemp -d)
      echo "   downloading $ASSET ..."
      curl -L "$URL" -o "$TMP/pspdev.tar.gz"
      # integrity: must be a valid gzip archive before we extract
      gzip -t "$TMP/pspdev.tar.gz" 2>/dev/null || { rm -rf "$TMP"; echo "pspdev download is corrupt (bad gzip). Re-run." >&2; exit 1; }
      if [ -w /usr/local ] || [ -n "$SUDO" ]; then
        PSPDEV_DIR="/usr/local/pspdev"
        run_root "rm -rf $PSPDEV_DIR && tar xzf '$TMP/pspdev.tar.gz' -C /usr/local"
      else
        PSPDEV_DIR="$HOME/pspdev"
        rm -rf "$PSPDEV_DIR"; tar xzf "$TMP/pspdev.tar.gz" -C "$HOME"
      fi
      rm -rf "$TMP"
    fi
    # sanity: the toolchain must actually run
    "$PSPDEV_DIR/bin/psp-gcc" --version >/dev/null 2>&1 || { echo "pspdev install looks broken ($PSPDEV_DIR)" >&2; exit 1; }
  fi
  if [ "$WIFI" = 1 ]; then
    echo ">> Building PSP homebrew (full app, USB + WiFi) ..."
    PSPDEV="$PSPDEV_DIR" "$HERE/build-psp.sh"
    PSP_PAYLOAD="$HERE/linux-host/psp-payload"
  else
    echo ">> Building PSP homebrew (minimal USB-only app) ..."
    PSPDEV="$PSPDEV_DIR" "$HERE/psp/source-min/build.sh" "$HERE/psp/source-min/payload"
    PSP_PAYLOAD="$HERE/psp/source-min/payload"
  fi

  if [ "$FLASH" = 1 ]; then
    echo ">> Copy the homebrew onto the PSP."
    echo "   On the PSP: Settings -> USB Connection (mounts it as a USB drive)."
    # wait for a mounted PSP, retrying, so the user has time to connect it
    tries=0
    while [ "$tries" -lt 30 ]; do
      for base in /run/media/"$USER"/* /media/"$USER"/* /media/* /mnt/*; do
        [ -d "$base/PSP" ] && { PSP_MNT="$base"; break; }
      done
      [ -n "${PSP_MNT:-}" ] && break
      if [ "$tries" = 0 ] && [ -t 0 ]; then
        printf '   Connect + mount the PSP, then press Enter (or Ctrl-C to skip flashing)... '
        read _ || break
      else
        sleep 2
      fi
      tries=$((tries + 1))
    done
    if [ -n "${PSP_MNT:-}" ]; then
      "$HERE/linux-host/install-psp.sh" "$PSP_PAYLOAD" "$PSP_MNT" || true
    else
      echo "   No PSP found. Flash it later: ./linux-host/install-psp.sh"
    fi
  fi
fi

cat <<EOF

============================================================
PSPdisp installed.

1. On the PSP: launch PSPdisp from the Game menu, pick USB (or WLAN).
2. On this PC, run the host:

   pspdisp               # USB; makes a new PSP display (sway/Hyprland/X11)
   pspdisp -i            # ...and expose PSP buttons as a gamepad
   pspdisp -t tcp -k PW  # WLAN instead of USB (PSP connects to this PC)
   pspdisp --no-display  # mirror an existing screen instead of a new one
   pspdisp -o HDMI-A-1   # mirror one monitor by name (Wayland and X11)
   pspdisp --background  # run detached (logs to \$XDG_RUNTIME_DIR/pspdisp.log)
   pspdisp --kill        # stop the host cleanly
   pspdisp --help        # all options

Re-flash the PSP later:  ./linux-host/install-psp.sh
============================================================
EOF
