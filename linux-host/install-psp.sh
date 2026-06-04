#!/usr/bin/env sh
# PSPdisp — copy the PSP homebrew to a USB-connected PSP, with verification.
#
# Replaces the Windows "CopyToPsp" installer step. Copies the payload to the
# PSP's PSP/GAME/PSPdisp/ and CHECKSUM-VERIFIES every file (byte-exact). On a
# flaky USB cable a copy can silently corrupt, so each file is re-copied up to
# 3 times until it matches the source.
#
# Usage:
#   ./install-psp.sh [PAYLOAD_DIR] [PSP_MOUNTPOINT]
#     PAYLOAD_DIR     default: ./psp-payload  (made by ../build-psp.sh)
#     PSP_MOUNTPOINT  default: auto-detected (a mounted drive with a PSP/ dir)
#
# The PSP must be connected in USB **storage** mode (PSP menu: Settings → USB
# Connection), not the PSPdisp app's USB mode.
set -eu

PAYLOAD="${1:-./psp-payload}"
DEST="${2:-}"

say()  { printf '%s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

# --- 1. locate the PSP ------------------------------------------------------
if [ -z "$DEST" ]; then
  for base in /run/media/"$USER"/* /media/"$USER"/* /media/* /mnt/*; do
    [ -d "$base/PSP" ] && DEST="$base" && break
  done
fi
[ -n "$DEST" ] && [ -d "$DEST/PSP" ] || die "no mounted PSP found.
  Put the PSP in USB storage mode (Settings -> USB Connection) and mount it,
  or pass the mountpoint:  ./install-psp.sh \"$PAYLOAD\" /run/media/$USER/PSP"

# --- 2. check the payload ---------------------------------------------------
[ -f "$PAYLOAD/EBOOT.PBP" ] || die "no EBOOT.PBP in '$PAYLOAD'.
  Build it first:  PSPDEV=/usr/local/pspdev ../build-psp.sh
  (or run the top-level ./install.sh --with-psp)"

# checksum helper (sha256sum, or shasum -a 256 on systems without it)
sha() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
        else shasum -a 256 "$1" | cut -d' ' -f1; fi; }

# copy one file and verify byte-exact, retrying on mismatch (flaky cable)
copy_verified() {
  src="$1"; dst="$2"
  [ -f "$src" ] || { say "  skip (missing): $(basename "$src")"; return 0; }
  i=1
  while [ "$i" -le 3 ]; do
    cp "$src" "$dst"; sync
    if cmp -s "$src" "$dst"; then
      say "  OK   $(basename "$src")  sha256=$(sha "$dst" | cut -c1-12)…"
      return 0
    fi
    say "  retry $i: checksum mismatch on $(basename "$src") (cable?)"
    i=$((i + 1))
  done
  die "FAILED to copy $(basename "$src") intact after 3 tries. Use a better USB cable."
}

# --- 3. copy + verify -------------------------------------------------------
TARGET="$DEST/PSP/GAME/PSPdisp"
say "Installing to: $TARGET"
mkdir -p "$TARGET/graphics"

copy_verified "$PAYLOAD/EBOOT.PBP"      "$TARGET/EBOOT.PBP"
copy_verified "$PAYLOAD/usbhostfs.prx"  "$TARGET/usbhostfs.prx"
copy_verified "$PAYLOAD/kernel.prx"     "$TARGET/kernel.prx"
[ -f "$PAYLOAD/PSPDISP.CFG" ] && copy_verified "$PAYLOAD/PSPDISP.CFG" "$TARGET/PSPDISP.CFG"

if [ -d "$PAYLOAD/graphics" ]; then
  for png in "$PAYLOAD"/graphics/*.png; do
    [ -e "$png" ] || break
    copy_verified "$png" "$TARGET/graphics/$(basename "$png")"
  done
fi

sync
say ""
say "All files copied and verified."
say ""
say "Next:"
say "  1. Unmount/eject the PSP, unplug USB."
say "  2. PSP Game menu -> Memory Stick -> PSPdisp -> launch (needs CFW)."
say "  3. Pick USB or WLAN mode, then run 'pspdisp' (USB) or 'pspdisp -t tcp' (WLAN) on the PC."
