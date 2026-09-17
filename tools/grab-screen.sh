#!/usr/bin/env bash
# Read the device's panel back, as a PNG.
#
# There is no way to look at the iPod's screen from here, and "does it look
# right" is the only question that matters for a front end. /dev/fb0 is
# readable while an app has it mapped, so the answer is one dd away: capture
# the framebuffer mid-run and render it exactly as the panel shows it.
#
# This is how Phase 01 was verified without anyone looking at the device - the
# captured picture came back byte-identical to the host's own scaler output,
# which is a stronger check than a photograph could ever be.
#
#   grab-screen.sh out.png                  capture whatever is on screen now
#   grab-screen.sh out.png /tmp/tinygb ROM  run something, capture, then stop
#
# Needs python3 here (not on the device) and nothing but busybox there.
set -euo pipefail

KEY=${SSH_KEY:-/mnt/c/src/ipod/artifacts/linux-n31/n31_id}
DEV=${DEVICE:-root@192.168.7.2}
SSH="ssh -o BatchMode=yes -o ConnectTimeout=8 -i $KEY $DEV"

out=${1:?usage: grab-screen.sh out.png [command to run while capturing...]}
shift || true

# The panel's geometry, asked for rather than assumed - a wrong stride turns a
# correct picture into a diagonal smear and looks like a rendering bug.
read -r W H <<<"$($SSH 'tr , " " < /sys/class/graphics/fb0/virtual_size')"
STRIDE=$($SSH 'cat /sys/class/graphics/fb0/stride')
echo "panel ${W}x${H}, ${STRIDE} bytes per row"

if [[ $# -gt 0 ]]; then
  # Run it in the background, let it settle, capture, then let it finish. An
  # app that blanks the screen on the way out - which is the polite thing to
  # do - leaves nothing to capture if you wait for it.
  $SSH "$* > /tmp/grab.log 2>&1 &
        sleep 4
        dd if=/dev/fb0 of=/tmp/fb.raw bs=$STRIDE count=$H 2>/dev/null
        wait" || true
  $SSH 'tail -3 /tmp/grab.log' || true
else
  $SSH "dd if=/dev/fb0 of=/tmp/fb.raw bs=$STRIDE count=$H 2>/dev/null"
fi

tmp=$(mktemp)
$SSH 'cat /tmp/fb.raw' > "$tmp"
echo "captured $(stat -c%s "$tmp") bytes"

python3 - "$tmp" "$out" "$W" "$H" "$STRIDE" <<'PY'
import struct, sys, zlib

src, dst, W, H, STRIDE = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
d = open(src, 'rb').read()

# The framebuffer is BGRX; PNG wants RGB, and each scanline needs a filter byte.
rows = []
for y in range(H):
    o = y * STRIDE
    row = bytearray(b'\x00')
    for x in range(W):
        b, g, r = d[o + x*4], d[o + x*4 + 1], d[o + x*4 + 2]
        row += bytes((r, g, b))
    rows.append(bytes(row))

def chunk(tag, body):
    c = tag + body
    return struct.pack('>I', len(body)) + c + struct.pack('>I', zlib.crc32(c))

png = (b'\x89PNG\r\n\x1a\n'
       + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
       + chunk(b'IDAT', zlib.compress(b''.join(rows), 9))
       + chunk(b'IEND', b''))
open(dst, 'wb').write(png)

nz = sum(1 for y in range(H) for x in range(W)
         if d[y*STRIDE + x*4 : y*STRIDE + x*4 + 3] != b'\x00\x00\x00')
print(f"wrote {dst}  {W}x{H}  {nz} non-black of {W*H}")
PY

rm -f "$tmp"
