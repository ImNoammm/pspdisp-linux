#!/usr/bin/env sh
# PSPdisp: copy the PSP homebrew to a USB-connected PSP, with verification.
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
# The PSP must be connected in USB storage mode (PSP menu: Settings -> USB
# Connection), not the PSPdisp app's USB mode.
set -eu

PAYLOAD="${1:-./psp-payload}"
DEST="${2:-}"

say()  { printf '%s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

# 1. locate the PSP
# Identify the PSP by its USB identity (a Sony "PSP" Memory Stick), NOT by
# "some mounted drive that happens to contain a PSP/ folder". That loose match
# has flashed people's SD cards instead of the PSP. If the PSP is plugged in but
# not mounted, mount it ourselves (and unmount on exit).
PSP_DEV=""; WE_MOUNTED=""
cleanup_mount() {
  [ -n "$WE_MOUNTED" ] || return 0
  sync
  udisksctl unmount -b "$PSP_DEV" >/dev/null 2>&1 \
    || umount "$WE_MOUNTED" 2>/dev/null \
    || sudo umount "$WE_MOUNTED" 2>/dev/null || true
  case "$WE_MOUNTED" in /tmp/*) rmdir "$WE_MOUNTED" 2>/dev/null || true ;; esac
}
trap cleanup_mount EXIT INT TERM

# First USB block device that identifies as a Sony PSP memory stick -> its
# first partition (or the whole disk if unpartitioned).
find_psp_partition() {
  for d in /sys/block/sd*; do
    [ -e "$d" ] || continue
    n=$(basename "$d")
    _m=$(cat "$d/device/model" 2>/dev/null); _v=$(cat "$d/device/vendor" 2>/dev/null)
    case "$_v $_m" in
      *PSP*|*Sony*|*SONY*)
        [ -b "/dev/${n}1" ] && { echo "/dev/${n}1"; return 0; }
        echo "/dev/$n"; return 0 ;;
    esac
  done
  return 1
}

if [ -z "$DEST" ]; then
  PSP_DEV=$(find_psp_partition || true)
  if [ -n "$PSP_DEV" ]; then
    DEST=$(lsblk -no MOUNTPOINT "$PSP_DEV" 2>/dev/null | grep -m1 .) || true
    if [ -z "$DEST" ]; then
      say "PSP detected at $PSP_DEV but not mounted, mounting it ..."
      # Prefer udisksctl (rootless on a desktop). It fails on headless boxes with
      # no polkit agent, so fall back to a plain/sudo mount on a temp dir.
      if command -v udisksctl >/dev/null 2>&1; then
        _out=$(udisksctl mount -b "$PSP_DEV" 2>&1) \
          && DEST=$(printf '%s\n' "$_out" | sed -n 's/^Mounted .* at \(.*\)$/\1/p' | sed 's/\.$//')
      fi
      if [ -z "$DEST" ]; then
        DEST=$(mktemp -d)
        if mount "$PSP_DEV" "$DEST" 2>/dev/null || sudo mount "$PSP_DEV" "$DEST" 2>/dev/null; then
          :
        else
          rmdir "$DEST" 2>/dev/null || true
          die "could not mount $PSP_DEV. Mount it manually, then re-run:
  sudo mkdir -p /mnt/psp && sudo mount $PSP_DEV /mnt/psp
  ./install-psp.sh \"$PAYLOAD\" /mnt/psp"
        fi
      fi
      WE_MOUNTED="$DEST"
      say "mounted at: $DEST"
    fi
  else
    # Fallback for odd setups: scan automount locations for a PSP/ folder.
    for base in /run/media/"$USER"/* /media/"$USER"/* /media/* /mnt/*; do
      [ -d "$base/PSP" ] && DEST="$base" && break
    done
  fi
fi

[ -n "$DEST" ] || die "no PSP found.
  Put the PSP in USB storage mode (Settings -> USB Connection), then re-run,
  or pass the mountpoint:  ./install-psp.sh \"$PAYLOAD\" /run/media/$USER/PSP"

# Safety: unless we positively identified the PSP by USB id, require a PSP/ dir
# so we never write to the wrong drive.
if [ -z "$PSP_DEV" ] && [ ! -d "$DEST/PSP" ]; then
  die "'$DEST' doesn't look like a PSP (no PSP/ folder). Refusing to write.
  Pass the correct mountpoint explicitly if you're sure."
fi

# 2. check the payload
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
    # Indicator printed before the copy (no newline) so the user sees it in
    # progress; the result completes the same line. Copies to flash + sync can
    # take a few seconds on a slow PSP cable.
    printf '  copying %s (%s bytes) ... ' "$(basename "$src")" "$(wc -c < "$src" | tr -d ' ')"
    cp "$src" "$dst"; sync
    if cmp -s "$src" "$dst"; then
      printf 'done, verified (sha256=%s…)\n' "$(sha "$dst" | cut -c1-12)"
      return 0
    fi
    printf 'checksum mismatch (cable?), retry %s\n' "$i"
    i=$((i + 1))
  done
  die "FAILED to copy $(basename "$src") intact after 3 tries. Use a better USB cable."
}

# 3. copy + verify
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
