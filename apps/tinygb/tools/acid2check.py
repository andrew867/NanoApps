#!/usr/bin/env python3
"""Compare a dmg-acid2 frame against Matt Currie's reference image.

acid2 is a single frame that depends on most of the PPU at once - sprite
priority, the 10-per-line limit, window behaviour, the LCDC bits that change
meaning mid-frame. It is designed so that a bug is visible as a wrong face,
which is a lovely property for a person and a useless one for a build. This
turns it back into an equality test.

The reference ships as 2-bit greyscale at 160x144, which is the same
information as our shade indices, so the comparison is exact rather than a
tolerance. The only thing to get right is polarity: in greyscale 0 is black,
and a Game Boy calls shade 0 the lightest.

    acid2check.py <frame.raw> <reference.png>

Exits 0 when every pixel matches.
"""
import sys

W, H = 160, 144


def load_reference(path):
    """The reference, as shade indices in Game Boy order (0 lightest)."""
    from PIL import Image

    img = Image.open(path)
    if img.size != (W, H):
        sys.exit(f"reference is {img.size[0]}x{img.size[1]}, expected {W}x{H}")

    # Whatever it is stored as, ask for 8-bit grey and quantise back to four
    # levels; that keeps this working if the reference is ever re-saved in a
    # different but equivalent format.
    grey = img.convert("L").tobytes()
    # 0,85,170,255 -> 0..3, then invert: greyscale 0 is black, shade 3 is.
    return bytes(3 - min(3, (g + 42) // 85) for g in grey)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)

    got = open(sys.argv[1], "rb").read()
    if len(got) != W * H:
        sys.exit(f"frame is {len(got)} bytes, expected {W * H}")

    want = load_reference(sys.argv[2])

    bad = [i for i in range(W * H) if got[i] != want[i]]
    if not bad:
        print(f"acid2: exact match, all {W * H} pixels")
        return 0

    # Where the differences are matters more than how many: acid2's failures
    # are localised to the feature that broke, so the bounding box usually
    # names the bug on its own.
    xs = [i % W for i in bad]
    ys = [i // W for i in bad]
    print(f"acid2: {len(bad)} of {W * H} pixels differ "
          f"({100.0 * len(bad) / (W * H):.2f}%)")
    print(f"       within x {min(xs)}..{max(xs)}, y {min(ys)}..{max(ys)}")

    # A row-by-row sketch, so a glance says whether it is one feature or the
    # whole picture.
    print("       rows with differences:")
    rows = {}
    for i in bad:
        rows[i // W] = rows.get(i // W, 0) + 1
    for y in sorted(rows)[:12]:
        print(f"         y={y:3d}  {rows[y]:4d} px")
    if len(rows) > 12:
        print(f"         ... and {len(rows) - 12} more rows")
    return 1


if __name__ == "__main__":
    sys.exit(main())
