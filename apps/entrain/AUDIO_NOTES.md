# Entrain — Phase 0 audio capability notes

What the iPod nano 7G's OS audio path can and cannot do, and what that forces
on the design of an entrainment tone player.

**How these answers were obtained.** No iPod was attached during Phase 0, so
nothing here is a device measurement. Most of it is instead **static
disassembly of RetailOS 1.1.2** (`osos.live.bin`, `39A10023`, flat load base
`0x08000000`, so VA = file offset + `0x08000000`), which turned out to answer
more, and more precisely, than the on-device probe would have. Every claim
cites the virtual address it came from so it can be re-checked.

`harnesses/audio_spike` is written, builds clean, and now serves a second
purpose: several of its tests are framed as **predictions** that either confirm
the disassembly or refute it.

| Tag | Meaning |
| --- | --- |
| `DISASM` | Read directly out of RetailOS 1.1.2 machine code, with the VA cited |
| `SOURCE` | Stated by code in this repo |
| `INFERRED` | Follows from the above with a stated argument |
| `OPEN` | Still needs the device. The harness test that settles it is named |

---

## Headline: loads are capped at 1 MiB

`SoundEffectDescriptor::loadFile` (VA `0x08417f78`) at VA `0x08418018`:

```
ldr   r0, [sp, #12]          ; byte count to load
cmp.w r0, #1048576           ; 1 MiB
bls   continue
movs  r5, #1                 ; -> return 1
b     bail
```

`DISASM`. **Any load over 1,048,576 bytes is rejected outright with rc=1.**

This is the single most consequential fact in Phase 0. The brief's primary
asset — a 100-second 44.1 kHz stereo loop, 17.6 MB — **cannot be loaded at
all**. The whole preset budget has to be rebuilt around this number, and it is
rebuilt below. It survives, but only because sample rate is a free variable.

Harness **T7** brackets the constant with a 1,047,596-byte file (predict: loads)
and a 1,049,644-byte file (predict: rc=1). If those two come back as predicted,
the constant is confirmed on hardware.

---

## The descriptor, from the constructor

`SoundEffectDescriptor::ctor` (VA `0x08417efc`) initialises all 0x78 bytes.
`DISASM`. The SDK knew four fields; this is the rest of the map that matters:

| Offset | Ctor default | What it is |
| --- | --- | --- |
| 0x00 | `0x087ae660` | vtable |
| 0x04 / 0x08 | 0 / 0 | decoded PCM buffer pointer (both slots) |
| 0x0C | 0 | decoded buffer size |
| 0x10 | 0 | container-type enum, set by `loadFile` |
| **0x14** | **44100** | **sample rate**, overwritten from the decoded stream |
| **0x18** | **1** | **channel count**, overwritten from the decoded stream |
| **0x1C** | **16** | **bits per sample — `loadFile` hard-writes 16 here** |
| 0x20 | 1024 | frame/block size |
| 0x24 | 0x7fff | volume (this is the one the SDK sets) |
| 0x28 | 1000 | — |
| 0x51 | 0 | "playmode" — **never read by `play()`** |
| 0x52 | 0 | flags — bit `0x04` and bit `0x02` are read by `play()` |
| 0x54 | 0 | the SDK calls this "next sfx" — **never read by `play()`** |
| **0x58 / 0x5C / 0x60** | **0x7fff / 0 / 0** | **volume-envelope triple**, skipped when 0x58 == 0x7fff |
| 0x68 / 0x6C | −1 / −1 | routing ids, consulted under flags bit 0x04 |
| 0x74 | heap ptr | 8-byte lock/mutex object |

Two corrections to the SDK's picture:

- **`0x54` is not a chain pointer.** It is zeroed by the constructor and never
  read by `play()`. My first pass through the disassembly appeared to find list
  splices writing to `[rX, #84]` — those turned out to be the ASCII bytes of the
  error strings being mis-decoded as Thumb. There is **no evidence of descriptor
  chaining**, so the "free gapless sequencing" idea is dead.
- **`0x58/0x5C/0x60` is a real volume envelope** the SDK never touches, and it
  looks like mixer-side fades for free. See "what to try next".

---

## Question by question

### 1. What formats load? `DISASM` in part, `OPEN` on the accepted list

- **Bit depth is not a variable.** `loadFile` writes a literal 16 to `desc+0x1C`
  (VA `0x08418094`). Everything is converted to 16-bit. `DISASM`
- **Sample rate and channel count come from the decoded stream** and are stored
  to `desc+0x14` / `desc+0x18` (VA `0x08418090`, `0x08418098`). `DISASM`
  A channel count that propagates end to end is good evidence that **stereo is
  carried, not downmixed** — see question 0.
- **It is not a WAV loader.** `loadFile` gets a parser from a factory and calls
  through a vtable, then maps a container enum through four accepted values
  (2, 0, 5, 4 → stored at `desc+0x10`), rejecting anything else with
  *"SoundEffectDescriptor::loadFile Format not supported/recognized"* (string at
  VA `0x0866f9c0`). Type 4 gets an extra packet-table walk, which is what a
  compressed format needs. `DISASM`
- **Which rates are accepted is still `OPEN`**, and it is now the most important
  open question in the project — see the budget below. Harness **T1**, which now
  includes 11025 Hz and 8000 Hz cases specifically because of this.

### 2. Does play block? `INFERRED` → no

Three converging arguments. `hb_audio_play_wav` returns `pthread_create`'s
result (`SOURCE`). The SDK's job struct comments that the descriptor "must
outlive playback" — only true of a call that already returned (`SOURCE`). And
`sfxPlayer::play` ends by calling `voice->start()` and immediately unlocking and
returning `void` (VA `0x08418300`, `DISASM`). It hands off to the mixer.

Confirmed on device by **T2**.

### 3. Can we tell when playback finishes? `DISASM` → yes, the callback is real

`sfxPlayer::play(player, desc, cb, cbdata)` at VA `0x084182f0`:

```
cmp.w r9, #0          ; r9 = cb
beq   skip
ldr   r2, [sp, #12]   ; cbdata (the saved 4th argument)
mov   r1, r9          ; cb
mov   r0, r7          ; the voice
blx   0x0862dfd4      ; voice->setCallback(cb, cbdata)
skip:
mov   r0, r7
blx   0x08630154      ; voice->start()
```

`DISASM`. **The callback argument the SDK always passes as NULL is a real,
plain `(function, context)` pair**, registered on the voice immediately before
it starts. It is not a C++ member thunk — there is no `this`-adjustment and no
second pointer — so passing an ordinary C function is safe.

That is the completion signal Phase 0 was looking for, and it means the design
does not have to fall back to blind timing. What remains `OPEN` is only *when*
it fires (end-of-buffer, or every buffer) and its exact argument order —
harness **T3b** timestamps it against a 5.000 s file, and **T3** independently
watches all 0x78 descriptor bytes as a cross-check.

### 4. Queue, cut off, or overlap? `DISASM` → up to 8 voices, they mix

`play()` acquires a voice from a pool via VA `0x0804d1bc`, and bails silently if
it gets NULL. That routine:

- scans **8 voice slots**, taking the first idle one;
- if all 8 are busy, scores every voice (VA `0x0863028c`) and **steals the
  highest-scoring one** — an LRU/priority victim;
- bumps a 16-bit counter at `voice+0x5C` on each busy voice (VA `0x0862e024`,
  which is a three-instruction increment, *not* a stop).

`DISASM`. So: **two sounds started together mix, they do not cut**. Cut-off only
happens at the ninth simultaneous voice, which we will never approach.

This is the good news that rescues the seam problem — see below.

### 5. Is there a loop flag? `DISASM` → no, and `0x51` is dead

`play()` never reads `desc+0x51` ("playmode") and never reads `desc+0x54`
("next"). It reads only `desc+0x52`, twice: bit `0x04` gates a routing branch
involving `desc+0x6C`, and bit `0x02` posts event id 1250. Neither is a loop.
`DISASM`

**There is no loop flag on this path.** Looping must be re-issued by us. The
harness keeps its T5 sweep anyway — it is cheap, and a mode byte consumed
further down the mixer would not show up in `play()` — but the expectation is
now negative, and the design does not depend on it.

### 6. Latency? `OPEN`, but the shape is known

`loadFile` reserves 20,992 bytes of stack (VA `0x08417f7e`) and does a full
decode into a fresh heap buffer, so **load cost scales with file size** and is
the dominant term. `DISASM`. `play()` itself is a lock, a pool scan, three
virtual calls and an unlock — microseconds. The practical consequence: **do the
`loadFile` early, keep the `play()` for the moment you need the sound.**
Harness **T6** puts numbers on it.

### 7. Whole file into RAM? `DISASM` → yes, ×1.2 plus 8 KB

At VA `0x084180ea`:

```
buffer = max(fileSize * 1.2, 8192) & ~3  +  8192      ; the 1.2 is an f64 constant
                                                       ; at VA 0x08418284
desc+0x0C = buffer size
desc+0x08 = desc+0x04 = alloc(buffer)                  ; VA 0x0842d694
```

`DISASM`. The entire file is decoded into one heap allocation, and the previous
buffer is freed first (VA `0x08418030`). So at the 1 MiB ceiling a single sound
costs roughly **1.26 MB of the shared OS heap**.

`hb_heap.h` warns that the OS's panicking allocator *reboots the device* when an
allocation fails, and `0x0842d694` sits right beside the allocator it names.
**Entrain must call `hb_os_heap_largest()` and refuse to load if headroom is
thin**, rather than letting a failed allocation take the device down.

### 8. Does `(offset, size)` give a window into a file? `DISASM` → yes

`loadFile`'s 4th and 5th arguments, which the SDK hardcodes to zero:

- `size` (5th, read at VA `0x08417f8a`) **replaces the stat'd file length** at VA
  `0x0841800e` — and is then subject to the same 1 MiB cap;
- `offset` (4th, held in `fp`) is forwarded to the parser's open call at VA
  `0x08418070`.

`DISASM`. **A byte window into a larger file is a real capability.** Whether the
parser treats the offset as a raw byte seek or expects a container header there
is `OPEN` — harness **T8**.

### 0. Does the mixer preserve L/R? `INFERRED` → probably, still `OPEN`

Not on the original list, but it outranks the rest: if the path downmixes,
**binaural is impossible, not degraded**, since the beat exists only as the
difference between the ears.

The evidence is now encouraging rather than unknown: the descriptor carries a
channel count that `loadFile` populates from the stream (`desc+0x18`), and a
mono-only path would not need the field. But a channel count in a descriptor is
not proof that the mixer keeps the channels apart. Harness **T0** settles it in
ten seconds with headphones on. Isochronic and monaural modes work correctly
either way.

---

## What this forces on the design

### The preset budget, rebuilt around 1 MiB

Bit depth is fixed at 16 and stereo is required for binaural, so the only free
variable is **sample rate**, and it buys loop length directly:

| Sample rate | Bytes/s | Max loop in 1 MiB | Nyquist |
| --- | --- | --- | --- |
| 44100 | 176,400 | 5.94 s | 22.05 kHz |
| 22050 | 88,200 | 11.89 s | 11.02 kHz |
| 11025 | 44,100 | 23.78 s | 5.51 kHz |
| 8000 | 32,000 | 32.77 s | 4.00 kHz |

Carriers live at 100–400 Hz and the renderer emits pure sines, so **11025 Hz is
already luxurious** — its Nyquist is more than an order of magnitude above the
highest carrier. Dropping the rate costs nothing audible and quadruples the loop
length. This is why T1's low-rate cases matter more than anything else it tests.

### The beat math still comes out exact

The brief's rule — *pick T from the beat, nudge the carrier, never the beat,
and display the realised beat* — is what makes the cap survivable. For a beat
`b = p/q` in lowest terms we need integer cycle counts and an integer sample
count, i.e. `Δn / T = b` with `T · SR` integral. Worked for Schumann:

```
SR = 11025, N = 122500 samples  ->  T = 100/9 s exactly
n_L = 2222 cycles ->  f_L = 199.98 Hz
n_R = 2309 cycles ->  f_R = 207.81 Hz
beat = 87 cycles / (100/9 s) = 7.830000000 Hz   EXACTLY
file = 490,044 bytes — less than half the cap
```

Verified against the rendered file, not just on paper: the last-sample →
first-sample step is `[1863, 1935]` (L, R) against a typical interior step of
`[1839, 1909]` — the wrap is continuous, which is the click test the host suite
will assert. The file is `wavs/loop783.wav`.

For contrast, the same preset at 44.1 kHz needs `N = 259078` — a 1,036,356-byte
file, **within 12 KB of the hard cap** and needing a 1.25 MB heap allocation.
That is the fallback if T1 says low rates are refused, and it is uncomfortable.

### Seams: overlap, don't chain

Assembling the picture: no loop flag, no descriptor chaining, but a **real
completion callback** and a **mixer that sums up to 8 voices**. So the loop
strategy is:

1. Render the loop with a short (~20 ms) equal-power crossfade region at its
   ends.
2. Keep two descriptors. While A plays, B is already `loadFile`d — the expensive
   step is done well in advance.
3. Fire B from A's completion callback, or a few ms early off
   `hb_time_uptime_ms()`. **Because voices mix, a few ms of overlap is not a
   glitch** — it is the crossfade doing its job. Undershoot is the only bad
   case, so deliberately overlap.

Loops are now ~12–24 s rather than 100 s, so seams arrive far more often and
this has to be right. The saving grace is that mixing makes overlap safe, which
is exactly the property the 100-second design would not have needed and this one
does. Render with ~3 dB of headroom so two overlapping voices cannot clip.

### Nothing can be stopped once it is playing

This falls out of the two findings above and is the most awkward consequence of
the whole spike. There is no loop flag, so we re-arm each buffer ourselves —
fine. But the voice pool **mixes** rather than cutting, which is what makes
overlapping seams safe, and it also means a newly played buffer cannot silence
one already in flight. No stop entry point was found on the player or the
descriptor.

So on the device:

- `en_audio_stop()` stops re-arming and lets the current buffer run out. A stop
  therefore takes up to one loop length — 11 to 18 seconds — to fall silent.
- Pause has the same shape: it takes effect at the next buffer boundary.
- The sleep timer's fade lands at a loop boundary rather than exactly on the
  second.

Three ways out, in order of preference, none of them v1 work:

1. **Find the stop.** The voice object has methods either side of the ones
   `play()` uses — `0x08630154` starts it, `0x08630188` resets it,
   `0x0863028c` scores it. One of its neighbours is very likely a stop, and
   the descriptor's vtable at `0x087ae660` is the other place to look. This is
   a contained RE job and it is the right fix.
2. **Steal the voice.** The allocator recycles the highest-scoring voice once
   all eight are busy (VA `0x0804d1bc`), so playing eight short silent buffers
   would evict a stuck one. Crude, and it depends on the scoring function
   picking the victim we want, which is unverified.
3. **Shorten what is playing.** `loadFile` accepts a byte window into a file,
   so one cached loop could be played as a sequence of four-second windows,
   bounding a stop to four seconds without needing more files on disk. Cheap to
   build, and harness T8 is what says whether the window works at all.

### Two constraints that reach into the UI

- **The MIPI-activity rule.** `sdk/hb_audio.c` carries a FIXME: calling the
  audio steps back to back without display activity between them *reboots the
  device* (`SOURCE`). `apps/files/files.c` gets away with it because LVGL
  composites continuously and supplies the traffic. **Consequence: blank the
  backlight (`hb_brightness_power(false)`), never stop the LVGL render loop.**
  Killing rendering to save battery could take audio down with it. Needs an
  explicit on-device check before that feature ships.
- **Heap discipline.** Every load allocates 1.2× the file plus 8 KB from the
  shared OS heap through an allocator that panics on failure. Check
  `hb_os_heap_largest()` before loading, keep files well under the cap (the
  490 KB Schumann preset is a good model), and free promptly.

### Follow-ups for the SDK — additive, and not v1 blockers

The disassembly makes three of these concrete rather than speculative:

- `hb_audio_play_cb(desc, cb, cbdata)` — expose the callback slot. **The
  disassembly says this works**; it just needs the on-device confirmation from
  T3b.
- `hb_audio_loadfile_range(desc, path, vol, offset, size)` — expose the two
  arguments hardcoded to zero.
- `hb_audio_set_envelope(desc, ...)` — `desc+0x58/0x5C/0x60`, skipped when
  `0x58 == 0x7fff`, is passed to the voice at VA `0x08418328` and looks like a
  mixer-side volume ramp. If it is, **the 1–2 s raised-cosine start/stop fades
  could be done by the mixer instead of baked into the PCM**, which would also
  give a clean way to stop audio on exit. Worth a probe.

The SDK is not widened in v1: `apps/entrain/platform/audio_device.c` calls the
firmware directly, the same way the harness does, and the wrappers get proposed
upstream once the device confirms them.

---

## Running the spike

```bash
cd harnesses/audio_spike
python3 gen_spike_wavs.py        # writes ./wavs/, 27 files, ~23 MB
# copy ./wavs/ to the iPod main volume as /WAV/spike/
cd ../.. && ./start run audio_spike
```

Headphones on — T0 and T4 are judged by ear. Results print to the screen and to
the DRAM trace ring, so `./start trace` recovers them even if a probe reboots
the device.

**Predictions to check first**, since these are what the design now rests on:

| Test | Prediction |
| --- | --- |
| T7 | `sz1023k.wav` loads; `sz1025k.wav` returns rc=1 |
| T7 | heap delta ≈ 1.2 × file size + 8 KB |
| T1 | `s11s16.wav` and `s08s16.wav` load — **the one that matters most** |
| T3b | the callback fires, once, at ≈ 5000 ms |
| T4 | A and B play **together**; neither is cut |
| T5 | no playmode or flags value produces a loop |
| T0 | left tone then right tone, not both in both cups |

A refutation of any of these is more valuable than a confirmation, and this
document gets rewritten around it.
