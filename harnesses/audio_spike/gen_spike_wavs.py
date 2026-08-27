#!/usr/bin/env python3
"""
gen_spike_wavs.py — build the WAV corpus the audio_spike harness loads.

The harness answers "what can the OS SFX player actually do?" by loading
files with known, deliberately varied properties and reporting what the
loader says. This script writes that corpus.

    python3 gen_spike_wavs.py              # format matrix + size ladder to 17.6 MB
    python3 gen_spike_wavs.py --big        # also write the 32 MB probe
    python3 gen_spike_wavs.py --out DIR    # default: ./wavs

Copy the result to the iPod's main volume as /WAV/spike/ (put the iPod in
disk mode, or mount it on the Linux box), then `./start run audio_spike`.

Everything is written with the stdlib only — no numpy, no scipy.
"""

import argparse
import math
import os
import struct
import sys

# ---- WAV writing -----------------------------------------------------------

FMT_PCM = 0x0001
FMT_FLOAT = 0x0003
FMT_EXTENSIBLE = 0xFFFE

SUBFMT_PCM_GUID = bytes.fromhex("0100000000001000800000aa00389b71")


def _chunk(cid, payload):
    body = cid + struct.pack("<I", len(payload)) + payload
    if len(payload) & 1:
        body += b"\x00"  # RIFF chunks are word-aligned
    return body


def write_wav(path, frames, sample_rate, channels, bits, fmt=FMT_PCM,
              extra_chunks=()):
    """frames: bytes of already-packed interleaved sample data."""
    block_align = channels * (bits // 8)
    byte_rate = sample_rate * block_align

    if fmt == FMT_EXTENSIBLE:
        fmt_body = struct.pack(
            "<HHIIHH", FMT_EXTENSIBLE, channels, sample_rate,
            byte_rate, block_align, bits)
        # cbSize=22, validBits, channelMask (FL|FR), subformat GUID
        fmt_body += struct.pack("<HHI", 22, bits, 0x3) + SUBFMT_PCM_GUID
    else:
        fmt_body = struct.pack(
            "<HHIIHH", fmt, channels, sample_rate,
            byte_rate, block_align, bits)

    payload = b"WAVE" + _chunk(b"fmt ", fmt_body)
    for cid, data in extra_chunks:
        payload += _chunk(cid, data)
    payload += _chunk(b"data", frames)

    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(payload)) + payload)
    return os.path.getsize(path)


# ---- sample generation -----------------------------------------------------

def pack_s16(samples):
    return struct.pack("<%dh" % len(samples), *samples)


def gen_stereo_s16(sample_rate, seconds, f_left, f_right, amp=0.5,
                   fade_ms=0):
    """Integer-cycle-safe only if f*seconds is integral; callers pick that."""
    n = int(round(sample_rate * seconds))
    out = []
    fade = int(sample_rate * fade_ms / 1000.0)
    peak = amp * 32767.0
    for i in range(n):
        g = 1.0
        if fade:
            if i < fade:
                g = 0.5 - 0.5 * math.cos(math.pi * i / fade)
            elif i >= n - fade:
                j = n - 1 - i
                g = 0.5 - 0.5 * math.cos(math.pi * j / fade)
        l = int(peak * g * math.sin(2.0 * math.pi * f_left * i / sample_rate))
        r = int(peak * g * math.sin(2.0 * math.pi * f_right * i / sample_rate))
        out.append(max(-32768, min(32767, l)))
        out.append(max(-32768, min(32767, r)))
    return pack_s16(out)


def gen_mono_s16(sample_rate, seconds, freq, amp=0.5):
    n = int(round(sample_rate * seconds))
    peak = amp * 32767.0
    out = [int(peak * math.sin(2.0 * math.pi * freq * i / sample_rate))
           for i in range(n)]
    return pack_s16(out)


def gen_hard_panned_s16(sample_rate, f_left, f_right, seconds_each, amp=0.5):
    """Left tone alone, silence, right tone alone. Proves L/R separation
    survives the OS mixer — if this comes out of both cups, binaural is
    off the table and only isochronic/monaural work."""
    n = int(round(sample_rate * seconds_each))
    gap = int(round(sample_rate * 0.5))
    peak = amp * 32767.0
    out = []
    for i in range(n):
        out.append(int(peak * math.sin(2.0 * math.pi * f_left * i / sample_rate)))
        out.append(0)
    out.extend([0, 0] * gap)
    for i in range(n):
        out.append(0)
        out.append(int(peak * math.sin(2.0 * math.pi * f_right * i / sample_rate)))
    return pack_s16(out)


def s16_to_u8(frames):
    vals = struct.unpack("<%dh" % (len(frames) // 2), frames)
    return bytes(((v >> 8) + 128) & 0xFF for v in vals)


def s16_to_s24(frames):
    vals = struct.unpack("<%dh" % (len(frames) // 2), frames)
    out = bytearray()
    for v in vals:
        w = v << 8
        out += struct.pack("<i", w)[0:3]
    return bytes(out)


def s16_to_f32(frames):
    vals = struct.unpack("<%dh" % (len(frames) // 2), frames)
    return struct.pack("<%df" % len(vals), *(v / 32768.0 for v in vals))


# ---- the corpus ------------------------------------------------------------

def build(out_dir, big):
    os.makedirs(out_dir, exist_ok=True)
    made = []

    def emit(name, *a, **kw):
        p = os.path.join(out_dir, name)
        sz = write_wav(p, *a, **kw)
        made.append((name, sz))

    # --- T0: channel separation. 440 Hz left only, then 880 Hz right only.
    emit("lr.wav", gen_hard_panned_s16(44100, 440, 880, 2.0), 44100, 2, 16)

    # --- T1: format matrix. Same 5.000 s content, varied container.
    #     440 L / 880 R so a downmix or a rate change is audible.
    base44 = gen_stereo_s16(44100, 5.0, 440, 880)
    emit("s44s16.wav", base44, 44100, 2, 16)
    emit("s48s16.wav", gen_stereo_s16(48000, 5.0, 440, 880), 48000, 2, 16)
    emit("s32s16.wav", gen_stereo_s16(32000, 5.0, 440, 880), 32000, 2, 16)
    emit("s22s16.wav", gen_stereo_s16(22050, 5.0, 440, 880), 22050, 2, 16)
    emit("s16s16.wav", gen_stereo_s16(16000, 5.0, 440, 880), 16000, 2, 16)
    # Low rates matter more than anything else here: loadFile caps a load at
    # 1 MiB, so the loop length we can ship is set by the lowest rate the
    # decoder accepts. 200 Hz carriers need nothing above ~8 kHz.
    emit("s11s16.wav", gen_stereo_s16(11025, 5.0, 440, 880), 11025, 2, 16)
    emit("s08s16.wav", gen_stereo_s16(8000, 5.0, 440, 880), 8000, 2, 16)
    emit("s44m16.wav", gen_mono_s16(44100, 5.0, 440), 44100, 1, 16)
    emit("s44s08.wav", s16_to_u8(base44), 44100, 2, 8)
    emit("s44s24.wav", s16_to_s24(base44), 44100, 2, 24)
    emit("s44s32f.wav", s16_to_f32(base44), 44100, 2, 32, fmt=FMT_FLOAT)
    emit("s44sxt.wav", base44, 44100, 2, 16, fmt=FMT_EXTENSIBLE)
    # A LIST/INFO chunk ahead of `data` — the commonest reason a hand-rolled
    # parser rejects an otherwise fine file. Our own writer must not trip it.
    emit("s44slst.wav", base44, 44100, 2, 16,
         extra_chunks=[(b"LIST", b"INFOISFT" + struct.pack("<I", 8)
                        + b"entrain\x00")])

    # --- T2/T3/T6: timing references. Exact durations, so a measured
    #     playback length pins down the real output sample rate.
    emit("t1s.wav", gen_stereo_s16(44100, 1.0, 440, 880, fade_ms=5), 44100, 2, 16)
    emit("t5s.wav", base44, 44100, 2, 16)
    emit("t10s.wav", gen_stereo_s16(44100, 10.0, 440, 880), 44100, 2, 16)

    # --- T4: overlap probe. A is left-only 440, B is right-only 880, both
    #     4 s. Playing B over A tells us cut-off vs. mix vs. queue by ear.
    emit("ovl_a.wav", gen_stereo_s16(44100, 4.0, 440, 0), 44100, 2, 16)
    emit("ovl_b.wav", gen_stereo_s16(44100, 4.0, 0, 880), 44100, 2, 16)

    # --- T5: loop-seam reference. 1.000 s of 200/210 Hz — every partial
    #     completes a whole number of cycles, so a correct loop is silent
    #     at the seam and a click means the player restarted badly.
    emit("seam1s.wav", gen_stereo_s16(44100, 1.0, 200, 210), 44100, 2, 16)

    # --- the real thing. loadFile refuses anything over 1 MiB, so the 100 s
    #     44.1k loop is impossible; this is the shipping shape instead.
    #     SR=11025, N=122500 samples -> T = 100/9 s exactly.
    #     n_L=2222 cycles -> 199.98 Hz, n_R=2309 -> 207.81 Hz.
    #     Beat = 87 cycles / (100/9 s) = 7.83 Hz EXACTLY, in 490 KB.
    emit("loop783.wav", gen_stereo_s16(11025, 100.0 / 9.0, 2222 * 9 / 100.0,
                                       2309 * 9 / 100.0), 11025, 2, 16)
    # Same preset at 44.1k, to show what the cap costs if low rates are refused:
    # T is forced down to 5.875 s and the file lands within 12 KB of the limit.
    emit("loop783_44k.wav", gen_stereo_s16(44100, 259078 / 44100.0,
                                           2000 * 44100 / 259078.0,
                                           2046 * 44100 / 259078.0), 44100, 2, 16)

    # --- T7: size ladder. Content is a quiet 100 Hz tone; only the byte
    #     count matters. 44.1k/16/stereo = 176400 B/s.
    # Static RE of SoundEffectDescriptor::loadFile says loads over 1 MiB are
    # rejected outright. Bracket that boundary tightly so the run either
    # confirms the constant or refutes it, then a couple of coarse probes.
    ladder = [("szhalf.wav", 512 * 1024),
              ("sz0900k.wav", 900 * 1024),
              ("sz1023k.wav", 1023 * 1024),      # just under 1 MiB -> expect ok
              ("sz1025k.wav", 1025 * 1024),      # just over  1 MiB -> expect rc=1
              ("sz04m.wav", 4 * 1024 * 1024)]
    if big:
        ladder.append(("sz32m.wav", 32 * 1024 * 1024))
    for name, nbytes in ladder:
        secs = nbytes / 176400.0
        emit(name, gen_stereo_s16(44100, secs, 100, 100, amp=0.2), 44100, 2, 16)

    return made


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "wavs"))
    ap.add_argument("--big", action="store_true",
                    help="also write the 32 MB probe (slow, ~40 s)")
    args = ap.parse_args()

    made = build(args.out, args.big)
    total = 0
    for name, sz in made:
        print("  %-14s %10d bytes" % (name, sz))
        total += sz
    print("wrote %d files, %.1f MB -> %s" % (len(made), total / 1048576.0, args.out))
    print("copy this folder to the iPod as /WAV/spike/, then: ./start run audio_spike")
    return 0


if __name__ == "__main__":
    sys.exit(main())
