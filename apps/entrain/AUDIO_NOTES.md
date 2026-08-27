# Entrain — Phase 0 audio capability notes

What the iPod nano 7G's OS audio path can and cannot do, and what that forces
on the design of a real-time entrainment tone player.

**Status of this document: partial.** Every claim below is tagged with how it
was established. Nothing here was measured on hardware yet — no iPod was
attached to the development machine during Phase 0, so the questions that need
a device are marked `OPEN` and are answered by `harnesses/audio_spike`, which
is written, builds clean (14 KB), and is ready to run.

| Tag | Meaning |
| --- | --- |
| `SOURCE` | Directly stated by, or plainly readable from, code in this repo |
| `INFERRED` | Follows from the source with a stated argument; high confidence |
| `OPEN` | Needs the device. The harness test that settles it is named |

---

## The API we have

`sdk/hb_audio.c` wraps four firmware entry points in the OS's sound-effect
player, the thing that plays the volume beep and the shake chime:

```c
sfxDesc::ctor      (0x08417efc)  desc -> desc
sfxDesc::loadFile  (0x08417f78)  (desc, path, volumeId, offset, size) -> int rc
sfxPlayer::instance(0x08417eb8)  () -> player
sfxPlayer::play    (0x0841828c)  (player, desc, callback, cbdata) -> void
```

The descriptor is 0x78 bytes. Four fields are identified; **0x74 of the 0x78
bytes are unmapped**, which is where most of the open questions live.

| Offset | Name | Known values |
| --- | --- | --- |
| 0x24 | volume | `0..0x7fff` |
| 0x51 | playmode | `1` = play. Other values unknown |
| 0x52 | flags | `0` = none. Other bits unknown |
| 0x54 | next-sfx | pointer, always set to `NULL` by the SDK |

Two details in that table are the most interesting things in the whole spike,
and both are currently thrown away by the SDK:

- **`play()` takes a callback and a cbdata.** The SDK passes `NULL, NULL`. If
  that is a completion callback, gapless chaining becomes straightforward.
- **The descriptor has a `next` pointer.** A "next" field in a playable object
  is the signature of an intrusive queue. If the player follows it, we get
  gapless sequencing for free — and a self-referential `next` might give an
  infinite loop for free.

Neither is proven. Both are cheap to test, and `harnesses/audio_spike` T3b and
T5 test exactly them.

---

## Question by question

### 0. Does the mixer preserve left/right? `OPEN` — this one is make-or-break

Not on the original question list, but it outranks all of them: **if the SFX
path downmixes to mono, binaural mode is impossible**, not merely degraded. A
binaural beat exists only as the difference between what each ear receives.

This path exists to play system beeps. Nothing guarantees it is stereo, and
nothing in the SDK comments mentions channels at all.

- Harness: **T0**, plays `lr.wav` — 440 Hz left only, gap, 880 Hz right only.
- If it downmixes: binaural mode is cut from v1; **isochronic** (single carrier,
  amplitude-gated) and **monaural** (both tones both ears) still work correctly
  on a mono path, and isochronic is the speaker-friendly mode anyway. The UI
  should then not offer a binaural toggle rather than offering a broken one.

### 1. What WAV formats load? `OPEN`

Nothing in the repo documents sample rate, bit depth, or channel count — the
SDK header calls them "handled by opaque OS pointers", and the only known-good
files are the OS's own `Resources/Sounds/*.wav`, whose headers we cannot read
without a device.

- Harness: **T1** loads eleven containers (44.1/48/32/22.05/16 kHz, 16/8/24-bit,
  float32, mono, `WAVE_FORMAT_EXTENSIBLE`, and a file with a `LIST` chunk ahead
  of `data`) and reports `loadFile` rc for each. `LIST` is included because a
  chunk-walking bug is the single commonest reason a hand-rolled parser rejects
  a valid WAV, and our own writer must avoid tripping it.
- **Accepted ≠ played at the stated rate.** A loader that ignores the header
  rate plays a 48 kHz file 8.8% fast. **T2** measures true output rate by timing
  a file of exactly known duration, which catches that.

Working assumption until measured: **44.1 kHz, 16-bit, stereo, PCM, canonical
44-byte header**. It is what the hardware natively runs and what the renderer
targets; if T1 says otherwise, only `wavout.c` changes.

### 2. Does play block or return immediately? `INFERRED` — returns immediately

Two independent arguments from `sdk/hb_audio.c`:

1. `hb_audio_play_wav` returns the result of `pthread_create`. It is
   non-blocking by construction. `SOURCE`
2. The worker calls `sfxPlayer::play` and immediately returns, and the job
   struct carries the comment *"the descriptor must outlive playback"*. A
   descriptor only needs to outlive a call that has already returned. So
   `sfxPlayer::play` hands the descriptor to an OS audio task and returns.
   `INFERRED`, high confidence.

Confirmed by **T2**, which times call→return against a 5.000 s file.

### 3. Can we tell when playback finishes? `OPEN` — two candidates

No callback, status field, or completion API is exposed today. Two places to
look, both tested:

- **The `play()` callback slot.** Harness **T3b** passes a real function pointer
  that timestamps itself. If it fires at ~5000 ms on a 5 s file, we have a true
  completion signal. If it reboots, the argument is not a plain C callback (a
  C++ member thunk, say) and the idea is dead — the trace ring records how far
  we got either way.
- **The descriptor.** Harness **T3** polls all 0x78 bytes for 9 s across a 5 s
  playback and reports, per byte, its first and last change time. A byte that
  flips at ~5000 ms is the completion marker. This is the highest-value single
  test in the harness: it either hands us a poll-based "is it done" or proves
  none exists.

If both come up empty, completion must be **timed**. We render the audio
ourselves, so we know each file's duration to the sample; `hb_time_uptime_ms()`
gives us the clock. That is workable — see the fallback ladder below.

### 4. Does a second call queue, cut off, or overlap? `OPEN`

`SOURCE`: the SDK header says *"Single-shot only: the descriptor is a static
buffer. Don't call again until the previous sound has finished."* That is a
constraint on the SDK's one static descriptor, **not** a statement about the
player. With two descriptors the behaviour is unknown.

`INFERRED`, weak: a system-beep player probably has one voice, so cut-off is
the most likely answer.

- Harness: **T4** plays a 4 s left-only tone, waits 1.5 s, then starts a
  right-only tone **on a second descriptor** — cut / mix / queue are trivially
  distinguishable by ear. It then repeats the test reusing one descriptor.

This determines whether crossfaded loop joins are possible at all. If the
player mixes, seams can be hidden with an overlap; if it cuts, they cannot.

### 5. Is there a loop flag? `OPEN` — three candidates

- `playmode` (0x51) is an enum whose only known value is `1 = play`. An enum
  with a named value implies siblings. **T5** sweeps it one deliberate tap at a
  time, tracing before each attempt so a reboot is still attributable.
- `flags` (0x52), swept bit by bit the same way.
- **`next` (0x54)** — the most promising. **T5** loads a second descriptor,
  points A's `next` at B, and plays A; then tries A→A. If the player follows the
  chain, we have gapless sequencing, and possibly a free infinite loop.

T5 is the risky screen. Every probe is a separate deliberate tap, every one is
traced first, and there is a SILENCE button as an escape hatch in case a
self-chain does loop forever.

### 6. Latency from call to first sample? `OPEN`

Not directly observable — nothing reports "first sample out". Contributors we
can measure separately, in **T6**: pthread spawn, `ctor`, `loadFile` (which does
the file read, on a ~21 KB stack), `setfields`, and `play`. The best proxy for
actual audio onset is T3's *first* descriptor-byte change time.

Latency matters less here than in most audio apps — nothing is triggered by a
gesture in real time. It matters in exactly one place: the size of the join
when one loop hands over to the next.

### 7. Max file size; does it stream or load whole? `OPEN`

`SOURCE`: `loadFile` takes `(offset, size)` beyond the path, which is how you
reference a region inside a resource bundle. That hints the loader may
reference rather than copy — but hints are not answers.

- Harness: **T7** loads 176 KB / 1 MB / 4 MB / 8 MB / 17.6 MB / 32 MB files and
  reports, for each, `hb_os_heap_free()` before and after plus the load time.
  A heap delta tracking file size means the whole file is buffered in RAM (and
  there is a hard ceiling); a small flat delta means it streams off disk.
- Harness: **T8** passes a non-zero `(offset, size)` — a 5 s window ten seconds
  into the 100 s loop. If a window plays, **one rendered file can hold every
  segment of a program and we seek instead of re-rendering**, which would be a
  significant architectural simplification.

The number that matters: our primary asset is a **17.6 MB** 100-second loop
(100 s × 44100 × 4 B). If loads are whole-file into a shared OS heap, that is a
lot to hold, and `hb_os_heap_largest()` becomes a real constraint we must check
at runtime before rendering.

---

## What this forces on the architecture

### Render-then-play, not stream

There is no PCM callback, no ring buffer, and no streaming entry point in the
SDK — `SOURCE`, and confirmed by reading every audio symbol in `sdk/`. "Real
time" on this device therefore means **render a file, then hand it to the OS**.
The brief already anticipates this and the design follows it:

- **Steady presets → one seamlessly-looping WAV.** Zero CPU while playing.
- **Programs that ramp → a chain of segments**, phase carried across joins.
- Render segment N+1 while N plays; cache by realised parameters.

### The loop-length math holds, and it is exact

Verified numerically against the generated corpus, not just on paper. For the
Schumann preset at `SR = 44100`, `T = 100 s`:

```
f_L = 200.00 Hz -> 20000 cycles in T   (integer)
f_R = 207.83 Hz -> 20783 cycles in T   (integer)
beat = 7.83 Hz exactly, T*SR = 4410000 samples (integer)
```

No carrier nudge was even needed for this one. Measured on the rendered file,
the last-sample → first-sample step is `[466, 485]` (L, R) against a typical
adjacent-sample step of `[467, 484]` — the wrap is **indistinguishable from an
interior step**, which is precisely the click test the host suite will assert.

This confirms the rule from the brief and it is the rule `render.c` implements:
**pick `T` from the beat first, then nudge the carrier, never the beat**, and
store and display the *realised* frequencies.

### The gapless fallback ladder

We do not yet know whether gapless is achievable. The design must therefore not
depend on it. In descending order of quality, with the trigger for each:

1. **Chained descriptors** (T5 `next` works) — true gapless. Pre-load B, point
   A→B. Best case; also likely gives looping via a self-chain.
2. **Completion-signalled re-arm** (T3 or T3b finds a signal) — pre-load the
   next descriptor fully while the current one plays, then issue only the
   `play()` call on completion. The gap is one `play()` call, likely low
   single-digit ms. Inaudible under a tone, and maskable.
3. **Timed re-arm** (no signal at all) — we know the duration to the sample,
   so schedule the next `play()` at `T − ε` off `hb_time_uptime_ms()`. Drift and
   scheduling jitter give a small gap *or* a small overlap. Mitigation: render
   loops with a short (~20 ms) equal-power crossfade region at the seam so a few
   ms either way is masked. **This is the fallback we should assume when
   estimating quality**, and it is good enough to ship.
4. **Worst case** — the player cuts off, loads are slow, and a seam is audible.
   Then: make `T` long (the beat math already forces ≥ 100 s), and render the
   loop to fade to true zero at both ends so the seam is a brief silence rather
   than a click. A 30 ms dip every 100 s is not premium, but it is honest and
   it is not a click.

Rungs 1 and 2 are worth having; **rung 3 is the one to build against**, so that
whatever the harness reports, v1 ships.

### Two constraints that reach into the UI

- **The MIPI-activity rule.** `sdk/hb_audio.c` carries a FIXME: calling the four
  audio steps back to back without display activity between them *reboots the
  device*, and empirically only a scale-3 text draw was reliable. `SOURCE`.
  Note that `apps/files/files.c` calls `hb_audio_play_wav` from an LVGL app with
  no such interleaving and it works — because the LVGL runtime is compositing
  continuously and supplying the traffic. **Consequence for Entrain: the
  screen-blank-during-playback feature must blank the *backlight*
  (`hb_brightness_power(false)`), never stop the LVGL render loop.** Killing
  rendering to save battery could take the audio subsystem down with it. This
  needs an explicit on-device check before that feature ships.
- **Never leave audio running after exit.** If a loop or chain mode is found,
  there is no known stop API. Exit must cut playback — currently only possible
  by starting something short and silent, if cut-off is the semantic (T4). If
  T4 says "queue" and T5 says "loops forever", we need a stop primitive before
  shipping loop mode at all.

### Follow-up for the SDK, not a v1 blocker

The README invites SDK expansion. Three additive candidates, all deliberately
out of scope for v1:

- `hb_audio_loadfile_range(desc, path, vol, offset, size)` — exposing the two
  args the SDK currently hardcodes to zero (pending T8).
- `hb_audio_play_cb(desc, cb, cbdata)` — exposing the callback slot (pending T3b).
- A real streaming path. Would require finding the OS's PCM sink below the SFX
  player and is a reverse-engineering project of its own. **v1 does not wait on
  it.**

---

## Running the spike

```bash
cd harnesses/audio_spike
python3 gen_spike_wavs.py        # writes ./wavs/, ~43 MB, 22 files
# copy ./wavs/ to the iPod main volume as /WAV/spike/
cd ../.. && ./start run audio_spike
```

Headphones on — T0 and T4 are judged by ear. Results print to the screen and to
the DRAM trace ring, so `./start trace` recovers them even if a probe reboots
the device. T5 is the risky screen; read its notes above before using it.

When the numbers come back, this document gets rewritten with `OPEN` replaced by
measurements, and that is what the rest of the app is built against.
