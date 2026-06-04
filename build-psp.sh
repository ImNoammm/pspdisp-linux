#!/usr/bin/env sh
# Build the PSP homebrew (EBOOT.PBP + usbhostfs.prx + kernel.prx) and assemble
# the payload directory for install-psp.sh.
#
# Needs the pspdev toolchain. If PSPDEV is unset it is auto-detected in the
# usual places; install.sh can fetch it for you.
#
# Usage: ./build-psp.sh [OUTPUT_DIR]   (default: linux-host/psp-payload)
set -eu

HERE=$(CDPATH= cd "$(dirname "$0")" && pwd)
SRC="$HERE/psp/source"
OUT="${1:-$HERE/linux-host/psp-payload}"

# --- locate pspdev ----------------------------------------------------------
if [ -z "${PSPDEV:-}" ]; then
  for d in /usr/local/pspdev "$HOME/pspdev" /opt/pspdev; do
    [ -x "$d/bin/psp-gcc" ] && PSPDEV="$d" && break
  done
fi
if [ -z "${PSPDEV:-}" ] || [ ! -x "$PSPDEV/bin/psp-gcc" ]; then
  echo "error: pspdev toolchain not found. Set PSPDEV, or run: ./install.sh --with-psp" >&2
  exit 1
fi
export PSPDEV
export PSPSDK="$PSPDEV/psp/sdk"
export PATH="$PSPDEV/bin:$PSPDEV/psp/bin:$PATH"
echo "Using pspdev at $PSPDEV ($(psp-gcc --version | head -1))"

# --- build ------------------------------------------------------------------
echo "Building EBOOT.PBP ..."
( cd "$SRC" && make )
echo "Building usbhostfs.prx ..."
( cd "$SRC/usbhostfs" && make )
echo "Building kernel.prx ..."
( cd "$SRC/kernel" && make )

# --- assemble payload -------------------------------------------------------
mkdir -p "$OUT/graphics"
cp "$SRC/EBOOT.PBP"                "$OUT/"
cp "$SRC/usbhostfs/usbhostfs.prx" "$OUT/"
cp "$SRC/kernel/kernel.prx"       "$OUT/"
# danzeff keyboard skins + default config are not in this source repo; the OSK
# feature is unavailable without them, core display works regardless.
[ -d "$SRC/graphics" ] && cp "$SRC"/graphics/*.png "$OUT/graphics/" 2>/dev/null || true

echo "Payload ready in $OUT:"
ls -la "$OUT"
echo "Flash it with:  ./linux-host/install-psp.sh \"$OUT\""
