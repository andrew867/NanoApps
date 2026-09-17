# SPEC: Emulator Core

## Purpose

Define the contract between the Game Boy emulator (`core/`) and every front end that runs it, so a front end can be written against `core/tg_core.h` without knowing which emulator is behind it, and a second core (Game Boy Color) can be added without touching a front end.

This spec describes what exists today. The core is done and gated; the spec exists so that the RetailOS port and the repo split cannot quietly change it.

## Scope

- `core/tg_core.h`: the core table, the registry, the cartridge probe.
- `core/tg_core.c`: registry, header parsing, core selection.
- `core/tg_peanut.c`: the Peanut-GB adapter, including the save-state serialiser.
- `vendor/peanut_gb.h`, `vendor/minigb_apu.c`: vendored upstream, unmodified.
- The portable helpers a front end is expected to use with the core: `platform/tg_scale.*`, `platform/tg_audio_clock.c`, `platform/tg_tilt.*`, `platform/tg_palette.*`.

## Non-Goals

- Game Boy Color. Peanut-GB has no CGB code. This is a second core (deferred, see `IMPLEMENTATION-PLAN.md`, Deferred Work).
- Link cable between two devices. The serial sink exists for test ROMs only.
- Any file, time, thread, or device access inside `core/`.
- Cycle-exact accuracy beyond what the gate proves. TinyGB passes Blargg cpu_instrs, instr_timing, and dmg-acid2 pixel-exact; it does not claim more.

## User / Actor Stories

- As a front-end author, I allocate `ctx_size` bytes, hand the core a ROM and an SRAM buffer, and call `run_frame` once per emulated frame. I never call malloc on the core's behalf.
- As a front-end author, I pull exactly the audio frames the sink needs for this video frame, and the core hands me 16-bit stereo at the rate agreed at open.
- As a tester, I run the core on a desktop with no device attached and get the same bytes out that the device draws.
- As the maintainer, I add a second core by filling in one `tg_core` table and appending it to the registry.

## Functional Requirements

### Interface shape

- **REQ-CORE-001**: The core shall perform no heap allocation. It shall publish `ctx_size` and operate entirely inside a caller-provided buffer of at least that size, which the caller keeps alive and unmoved for the session.
- **REQ-CORE-002**: The core shall perform no file I/O. ROM and SRAM arrive as caller-owned byte ranges that outlive the session; the core stores pointers and never copies the ROM.
- **REQ-CORE-003**: The core shall have no notion of wall-clock time. `run_frame` advances exactly one Game Boy frame (70224 T-cycles) and returns.
- **REQ-CORE-004**: The core shall touch no audio device. `audio_pull(ctx, dst, frames)` fills interleaved signed 16-bit stereo at the `audio_rate` given to `open`, and returns the number of frames written.
- **REQ-CORE-005**: A front end shall reach the emulator only through `tg_core.h`. No front end may include `vendor/peanut_gb.h` or call a Peanut-GB function directly.

### Registry and selection

- **REQ-CORE-010**: `tg_core_count()` / `tg_core_at(i)` / `tg_core_by_id(id)` shall enumerate every compiled-in core. Today the registry holds exactly one entry, id `"peanut"`.
- **REQ-CORE-011**: `tg_core_for_rom(rom, len)` shall return the first registered core whose `caps` cover the cartridge. A DMG-only core shall accept a colour-aware cartridge (header byte 0x143 == 0x80) and shall refuse a colour-only one (0xC0).
- **REQ-CORE-012**: The Peanut-GB core shall report `TG_CAP_DMG | TG_CAP_SOUND | TG_CAP_STATE` and shall not report `TG_CAP_CGB`. (The unit test `peanut does not claim colour` pins this; if it ever fails, Phase 07 has changed shape.)

### Cartridge probe

- **REQ-CORE-020**: `tg_rom_probe` shall parse the header without starting a session: title (15 bytes when the CGB flag is set, else 16, trailing spaces and NULs trimmed), CGB / CGB-only / SGB flags, mapper byte 0x147 verbatim, declared ROM size, SRAM size, and whether the 0x134..0x14C header checksum agrees.
- **REQ-CORE-021**: SRAM size shall come from the lookup `{0, 2K, 8K, 32K, 128K, 64K}` indexed by byte 0x149 (note 0x05 is smaller than 0x04), except that MBC2 cartridges (0x05, 0x06) shall get 512 bytes despite declaring none.
- **REQ-CORE-022**: A wrong header checksum shall be reported in `header_ok`, not treated as fatal. The global checksum at 0x14E shall be ignored.
- **REQ-CORE-023**: A buffer too short to hold a header shall return `TG_ERR_ROM`.
- **REQ-CORE-024**: `open` shall return `TG_ERR_CAPACITY` when the caller's SRAM buffer is smaller than the cartridge needs, and `TG_ERR_MAPPER` for a memory bank controller the core lacks (Peanut-GB: MBC7, Pocket Camera, TAMA5, HuC1, HuC3 unsupported; MMM01 and MBC6 untested upstream).

### Frames and pixels

- **REQ-CORE-030**: `pixels(ctx)` shall return `TG_W * TG_H` (160 x 144) bytes, one per pixel, valid until the next `run_frame`.
- **REQ-CORE-031**: The low two bits of a pixel (`TG_PX_SHADE`) shall be the shade, 0 lightest to 3 darkest. Bit `TG_PX_OBJ` shall mark a pixel drawn by an object and `TG_PX_OBJ1` that it used OBJ palette 1. Consumers shall mask with `TG_PX_SHADE` and never assume the upper bits are zero.
- **REQ-CORE-032**: `palette(ctx, &n)` shall return at least 4 entries of `0x00RRGGBB`, lightest first. Front ends may substitute a `tg_palette` entry; the core's own palette is the default only.
- **REQ-CORE-033**: The frame rate constant shall be expressed only as the rational `TG_FPS_NUM / TG_FPS_DEN` (4194304 / 70224). No code shall use 60 or 59.73 as the Game Boy frame rate.

### Input

- **REQ-CORE-040**: `set_buttons(ctx, held)` shall take a level, not an edge: the bitmask of buttons currently held, in Game Boy joypad register order (`TG_A`=1, `TG_B`=2, `TG_SELECT`=4, `TG_START`=8, `TG_RIGHT`=16, `TG_LEFT`=32, `TG_UP`=64, `TG_DOWN`=128). The value latches until the next call.

### Audio

- **REQ-CORE-050**: `open(..., audio_rate)` with `audio_rate == 0` shall disable sound; `audio_pull` then need not be called. The gate uses this to run faster than real time.
- **REQ-CORE-051**: The APU sample rate is a compile-time constant (`AUDIO_SAMPLE_RATE`, baked into minigb_apu). A build is for exactly one rate and never resamples. The host and N31 builds use 48000; the RetailOS build uses 22050.
- **REQ-CORE-052**: `audio_pull` shall never return more frames than asked, and a short return shall mean "nothing more is available yet", which the front end pads with silence, never with a repeat.
- **REQ-CORE-053**: `tg_audio_clock_next` shall hand out frames-per-video-frame by exact rational accumulation (`acc += rate * TG_FPS_DEN; n = acc / TG_FPS_NUM`), so that over any interval the total equals `rate * frames * TG_FPS_DEN / TG_FPS_NUM` to within one frame. At 48000 this alternates 803 and 804; at 22050 it alternates 369 and 370.

### Save states

- **REQ-CORE-060**: `state_size(ctx)` shall be asked before every save; it depends on the cartridge's RAM size, which is a property of the game.
- **REQ-CORE-061**: `state_save` with a buffer smaller than `state_size` shall return `TG_ERR_CAPACITY` and write nothing.
- **REQ-CORE-062**: `state_load` shall return `TG_ERR_STATE` for a blob written by another build, another core, a corrupted first byte, or a length that is not a state, and shall leave the running machine untouched in that case.
- **REQ-CORE-063**: A state restored after N further frames shall reproduce the frame that followed the save exactly (the unit test runs 30 frames, saves, runs 60 more, loads, runs 30, and compares the frame byte for byte).

### Serial

- **REQ-CORE-070**: `set_serial_sink(ctx, fn, user)` shall deliver every byte the cartridge writes to the link port. This is how Blargg's test ROMs report, and it is the gate's pass/fail channel.

### Portable helpers

- **REQ-CORE-080**: `tg_scale_15` shall scale 160 x 144 shade indices to exactly 240 x 216 32-bit pixels (`0x00RRGGBB`) into a caller-provided destination with a caller-provided stride in pixels, writing nothing outside those rows and columns.
- **REQ-CORE-081**: With `smooth == true` the horizontal middle sample of each source pair shall be the exact average of its neighbours (`(a & b) + (((a ^ b) & 0xFEFEFE) >> 1)`); with `smooth == false` it shall repeat the left pixel. The vertical middle row is always the average of the rows above and below (this asymmetry is existing behaviour and is kept).
- **REQ-CORE-082**: The scaler shall skip writing any pair of source rows whose 320 bytes are unchanged since the previous frame, and shall repaint everything after `tg_scaler_invalidate` or a palette change.
- **REQ-CORE-083**: `tg_tilt_feed` shall map accelerometer readings to d-pad bits with hysteresis (press at `on_pct`, release at `off_pct` of half-scale), shall exclude the axis closest to gravity at calibration, and shall report a diagonal as two bits.
- **REQ-CORE-084**: `tg_palette_at(i)` shall clamp an out-of-range index to 0; `tg_palette_index(name)` shall return 0 for an unknown name rather than failing.

## State and Data

The core's entire state is the caller's context buffer (`peanut_ctx`, about 43.5 KiB: a 16.5 KiB `gb_s` with 8 KiB WRAM, 8 KiB VRAM, 160 B OAM, 256 B HRAM/IO; a 23040 B frame; an 814-frame audio spill; the APU context). The ROM (32 KiB to 8 MiB) and SRAM (0 to 128 KiB) are the caller's. A `tg_scaler` adds 23 KiB for the previous-frame copy.

Peanut-GB reads the ROM through a callback (`rom_read`), which bounds-checks and returns 0xFF past the end. A front end that cannot hold a whole ROM in RAM can substitute a paged `rom_read` without touching anything else; none does today.

## Main Flows

Session start: `tg_rom_probe` -> `tg_core_for_rom` -> allocate `ctx_size` and `sram_size` -> load battery save into SRAM (before `open`, because a cartridge reads its save at boot) -> `open(ctx, rom, len, sram, sram_len, audio_rate)`.

Per frame: `set_buttons` -> `run_frame` -> `pixels` -> scale -> `audio_pull(tg_audio_clock_next())` -> sink.

Session end: flush SRAM -> `close`.

## Edge Cases

- ROM shorter than the header declares: reads past the file return 0xFF; a truncated dump glitches rather than faults.
- Cartridge declares no RAM but is MBC2: 512 bytes are provided anyway (REQ-CORE-021).
- Colour-aware cartridge on the DMG core: runs in four shades (REQ-CORE-011).
- `audio_pull` asked for more than one APU callback's worth (814 frames at 48 kHz): the excess is unfilled and padded by the caller.

## Failure Handling

Every entry point that can fail returns `enum tg_result`; `tg_strerror` names it. A front end shows the string and returns to its library. Nothing in the core aborts, prints, or exits.

## Security / Privacy Notes

The core reads untrusted bytes (a ROM file). Every ROM and SRAM access is bounds-checked against the lengths given at `open`; a malformed header cannot index outside those ranges. Save-state loading validates length and a leading marker before touching machine state.

## Acceptance Criteria

- **AC-CORE-001**: Given the fetched test ROMs, when `tools/gate.sh` runs against a fresh host build, then cpu_instrs and instr_timing report "Passed" over the serial sink and dmg-acid2 matches `reference-dmg.png` on all 23040 pixels.
- **AC-CORE-002**: Given `host/tests.c`, when `make -f Makefile.host test` runs, then every check prints `ok` and the process exits 0.
- **AC-CORE-003**: Given `tg_headless -l`, when it lists cores, then it prints `peanut`, its caps, and a context size under 64 KiB.
- **AC-CORE-004**: Given any front end's object files, when `nm` is run over them, then no symbol from `peanut_gb.h` (`gb_init`, `gb_run_frame`, `gb_get_save_size`, ...) is referenced outside `core/tg_peanut.o`.
- **AC-CORE-005**: Given a build at 22050 Hz, when the audio clock is stepped 60 times, then the sum is 22151 (not 22050), and over one hour of frames the sum is within one frame of `22050 * 215019 * 70224 / 4194304`.

## Test Coverage Notes

Covered today by `host/tests.c` (header, registry, run, state, scale, audio clock, tilt) and `tools/gate.sh` (Blargg, acid2). Not covered: `tg_palette.c` is not linked into the host tests; AC-CORE-004 has no automated check yet. Both are listed in `TEST-PLAN-automated.md`.

## Open Questions

See `OPEN-QUESTIONS.md` OQ-006 (SameBoy as the CGB core) and OQ-007 (whether vendored upstream versions should be pinned by hash).
