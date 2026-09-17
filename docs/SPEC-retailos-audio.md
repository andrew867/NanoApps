# SPEC: RetailOS Audio Sink

## Purpose

Define how TinyGB streams the Game Boy's sound on RetailOS, where the SDK offers no PCM API at all. The mechanism is the chained SoundEffect descriptor path that Entrain reverse-engineered and ships in `apps/entrain/platform/audio_device.c`; this spec adapts it to an emulator, whose needs (low latency, sink-paced emulation, silence in a frame) are the opposite of Entrain's (long buffers, gentle ramps).

This is the highest-risk part of the port. Every number in it is a starting point to be tuned on the device, and the tuning knobs are named so the tuning is a constants change, not a rewrite.

## Scope

- `platform/tg_audio_hb.c` implementing a small sink interface the RetailOS front end uses: start, queue depth, submit, pause, resume, stop.
- The descriptor layout, the chaining rule, the duration field, and the completion polling, as established by Entrain and its harness (`harnesses/audio_spike`).

## Non-Goals

- Sharing code with Entrain's `audio_device.c`. Entrain paces against the monotonic clock, renders 1.28 s blocks, and fades in over a second; none of that fits. What is shared is the knowledge, and this spec cites where it came from.
- Resampling. The APU is compiled for 22050 and the descriptor says 22050; the mixer resamples to its own rate if it must (its output rate is learned from the voice, not assumed).
- Volume UI. The OS volume buttons are A and B in this app; the descriptor volume is fixed at a comfortable default with a settings row later if wanted.

## Background (what the firmware does)

- A SoundEffect descriptor is a 0x78-byte object. Its constructor lives at a fixed address; `sfxPlayer::play(player, desc, NULL, NULL)` starts a voice that reads PCM live from the buffer the descriptor points at.
- `desc+0x54` is a NEXT pointer. When the voice drains a buffer it re-arms from `+0x54` inside its own completion handler, on the audio task, sample-exact, and re-applies rate/channels/bits/volume from the chained descriptor. So only the first descriptor needs `play()`.
- `desc+0x64` is a byte that is 1 while a voice holds the descriptor and 0 once it has played out. There is no callback in use; completion is polled.
- `desc+0x34` is a duration in milliseconds that the voice converts to a sample count using the mixer's output rate. Zero costs a faint click per join; a wrong value costs a block of silence. It must be exact or zero.
- Voices mix; they never cut. There is no stop. Silence is achieved by rewriting the part of the buffer the voice has not reached and unlinking the chain.
- Entrain's proven format: type 0 (LPCM), 2 channels, 16 bits, rate 22050, both buffer slots (+0x04, +0x08) pointing at the same PCM, length in bytes at +0x0C, trims 0, volume 0..0x7fff at +0x24.

## Functional Requirements

### Shape

- **REQ-AUD-001**: The sink shall present the emulator with a queue of fixed-length slots. Constants, all in one place: `TG_AUD_RATE 22050`, `TG_AUD_SLOT_FRAMES 1470` (66.7 ms, about four video frames), `TG_AUD_SLOTS 6` (400 ms of buffer), `TG_AUD_TARGET_LEAD 2` slots queued beyond the one sounding (about 130 to 200 ms of latency).
- **REQ-AUD-002**: Slot PCM buffers shall be allocated once per launch with `hb_os_alloc` after `hb_os_heap_largest()` reports at least the total plus 128 KiB. The total is `TG_AUD_SLOTS * TG_AUD_SLOT_FRAMES * 4` = 35,280 bytes. Descriptors (`uint8_t desc[0x80]`) live in `.bss`.
- **REQ-AUD-003**: `tg_aud_frames_queued()` shall return the number of PCM frames not yet played, computed from slots in the QUEUED state plus the unplayed remainder of the sounding one (wall-clock estimate from `hb_time_uptime_ms` since that slot started, clamped). This is the number the front end paces on.

### Feeding

- **REQ-AUD-010**: The front end shall push audio in emulated-frame units (369 or 370 frames) into the slot being built; when a slot reaches `TG_AUD_SLOT_FRAMES` it shall be sealed: descriptor fields written, `+0x34` set to the exact millisecond duration only when `frames * 1000 % out_rate == 0` (else 0), then linked to the tail with a single aligned 32-bit store to the tail's `+0x54`.
- **REQ-AUD-011**: The first slot of a stream shall be started with `play()`; the mixer's output rate shall then be read from the voice (`desc+0x48 -> voice+0x0C`) and used for every later duration computation. The first slot's `voice+0x48` shall be patched to its frame count directly, as Entrain does, because no rate is known before the voice exists.
- **REQ-AUD-012**: Reclaim shall be strictly FIFO: a slot is FREE again only after its `+0x64` has been observed 1 and then 0. A slot never observed playing is not reclaimed on the strength of `+0x64 == 0`.
- **REQ-AUD-013**: Each tick shall do at most: reclaim, seal at most one slot, link or play it. All from `hb_raw_frame`; nothing from any other context.

### Pause, resume, stop, silence

- **REQ-AUD-020**: Pause (menu open, media re-pause) shall unlink every queued slot, zero the ones behind the head, and in the sounding slot apply an 80 ms linear ramp from `play_cursor + 250 ms` then zeros. Then chain a self-linked 320 ms silent block so the voice stays alive and resume is a ramp, not a cold start.
- **REQ-AUD-021**: Resume shall release the silent block (clear its own `+0x54`) and fade in the first 80 ms of the next sealed slot.
- **REQ-AUD-022**: Cartridge change shall stop the stream as in pause and leave the silent block looping until the next start, so the descriptors and buffers are reused and nothing is allocated twice per launch.
- **REQ-AUD-023**: Starvation (no slot reclaimed for `3 * TG_AUD_SLOTS * slot_ms`) shall be treated as the OS having taken the mixer (Music app in front): drop the queue, count it, and restart with a fade on the next seal.
- **REQ-AUD-024**: The sink shall count underruns (a tick that found zero slots queued while playing) and joins with `+0x34 == 0`, for the overlay and the manual test.

### Hygiene

- **REQ-AUD-030**: PCM buffers handed to a voice shall never be freed while any descriptor could reference them; since there is no exit callback they are never freed at all within a launch, and a relaunch allocates its own (the size is small enough to accept; see `SPEC-retailos-frontend.md` REQ-ROS-053).
- **REQ-AUD-031**: The descriptor constructor's own defaults (rate 44100, 1 channel) shall be overwritten on every seal; nothing relies on ctor state.
- **REQ-AUD-032**: The pull side shall never allocate, block, or touch the filesystem. It is `core->audio_pull` into a slot, nothing else.

## State and Data

```
slot[6]: { state FREE|BUILD|QUEUED, desc[0x80], int16 *pcm, frames_filled, started_ms, seen_playing }
stream: { head, tail, out_rate, playing, paused, underruns, zero_duration_joins, restarts }
quiet: { desc[0x80], pcm[7056*2] }   /* 320 ms of silence, self-chained while paused */
```

## Main Flows

Start: allocate (once) -> seal a first slot as the front end fills it -> `play()` -> learn `out_rate`.

Tick: reclaim -> front end runs frames into BUILD slot -> seal when full -> link to tail (or play if the chain ran dry).

Pause / resume / stop: as above.

## Edge Cases

- The mixer runs at 44100 rather than 22050: durations are computed against the learned rate; the exactness test rejects inexact conversions and falls back to 0 (click-free enough, measured by the join counter).
- `hb_os_heap_largest()` too small at cartridge start: sink reports failure, front end plays silent and paces on the tick.
- RetailOS starts Music under the game: starvation path restarts the stream; the front end's media re-pause stops the music.

## Failure Handling

Every sink failure is non-fatal to the app: it reports through a state enum and a counter, and the front end degrades to silent, one-frame-per-tick operation.

## Security / Privacy Notes

The sink calls three firmware entry points (constructor, player instance, play) at the fixed addresses NanoApps already uses in `sdk/hb_audio.c`. The buffers it hands the mixer are its own.

## Acceptance Criteria

- **AC-AUD-001**: Given a 440 Hz test tone generated in place of the APU (a build-time switch), when it plays for 60 s through headphones and is recorded, then the capture shows no gap, click, or pitch step (compare against the N31 capture method in `host/build/` history).
- **AC-AUD-002**: Given Tetris's title music for five minutes, then `underruns == 0` and `restarts == 0` on the overlay.
- **AC-AUD-003**: Given the menu opened and closed ten times during music, then no click is audible at any transition and the game's pitch is unchanged afterwards.
- **AC-AUD-004**: Given the audio started on a fresh launch, when the queue is at target lead, then the measured latency (a button press to its sound effect, by ear against the N31 build) is under 250 ms.

## Test Coverage Notes

The slot state machine and the duration exactness function shall be unit-tested on the host with a fake descriptor and a fake `+0x64` writer (AUTO-AUD-*). Everything else is device-only (MAN-AUD-*).

## Open Questions

OQ-010 (slot size and depth after measurement), OQ-011 (whether the completion callback that harness T3b proved works is worth using instead of polling, to reclaim within a tick rather than at the next one).
