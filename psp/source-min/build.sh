#!/usr/bin/env sh
# Build pspdisp-min: EBOOT.PBP + usbhostfs.prx, then assemble a flash payload.
#
# Usage: ./build.sh [OUTPUT_DIR]   (default: ./payload)
set -eu

HERE=$(CDPATH= cd "$(dirname "$0")" && pwd)
OUT="${1:-$HERE/payload}"

# Locate the pspdev toolchain.
if [ -z "${PSPDEV:-}" ]; then
  for d in /usr/local/pspdev "$HOME/pspdev" /opt/pspdev; do
    [ -x "$d/bin/psp-gcc" ] && PSPDEV="$d" && break
  done
fi
if [ -z "${PSPDEV:-}" ] || [ ! -x "$PSPDEV/bin/psp-gcc" ]; then
  echo "error: pspdev toolchain not found. Set PSPDEV." >&2
  exit 1
fi
export PSPDEV
export PSPSDK="$PSPDEV/psp/sdk"
export PATH="$PSPDEV/bin:$PSPDEV/psp/bin:$PATH"
echo "Using pspdev at $PSPDEV ($(psp-gcc --version | head -1))"

echo "Building EBOOT.PBP ..."
make -C "$HERE" clean >/dev/null 2>&1 || true
make -C "$HERE"

echo "Building usbhostfs.prx ..."
make -C "$HERE/usbhostfs" clean >/dev/null 2>&1 || true
make -C "$HERE/usbhostfs"

mkdir -p "$OUT"
cp "$HERE/EBOOT.PBP"            "$OUT/"
cp "$HERE/usbhostfs/usbhostfs.prx" "$OUT/"

echo "Payload ready in $OUT:"
ls -la "$OUT"
