# Automated Test Plan

## Purpose

Map every requirement that can be checked without a device to a test that runs on a desktop or in CI, and say which ones cannot be. The rule: anything below a front end's `hb_` or Linux syscall line is portable C and gets a host test.

## Framework assumptions

- Plain C test executables, no framework: `host/tests.c` prints `ok`/`FAIL` per check and exits non-zero on any failure. New suites are added to the same file (or split into `host/tests_*.c` linked into one binary) in the same style.
- `make test` builds and runs the unit tests (seconds). `make gate` fetches the test ROMs if missing and runs `tools/gate.sh` (Blargg cpu_instrs, instr_timing, dmg-acid2; a few seconds at `-O2`).
- Test ROMs are downloaded by `tools/fetch-testroms.sh`, never committed. CI fetches them each run; a fetch failure is a CI failure, not a skip.
- Device-facing modules get a **fake platform**: `host/fake_hb.c` implements the handful of `hb_*` functions `tg_fs_hb.c`, `tg_input_hb.c`, and `tg_audio_hb.c` call (an in-memory filesystem, a scripted finger table and button levels, a scripted descriptor `+0x64` writer), so the RetailOS platform files compile and run on the host with `-DTG_FAKE_HB`.
- CI is GitHub Actions on `ubuntu-latest`, two jobs: `host` (build, unit tests, vendor hashes, seam check, fetch test ROMs, gate, acid2 frame as an artifact) and `retailos` (clones `andrew867/NanoApps`, checks this repository out as its `apps/tinygb`, builds the `.hbapp` with `arm-none-eabi-gcc`, runs `check-size`).

## Unit tests (`make test`)

| ID | Suite / test | Covers |
|---|---|---|
| AUTO-CORE-001 | `test_header`: sizes, checksum, title trimming, RAM table incl. 0x05 < 0x04, MBC2 512 B, colour flags, short file | REQ-CORE-020..023 |
| AUTO-CORE-002 | `test_registry`: count, by-id, peanut caps (DMG yes, CGB no, state yes, serial yes), colour-aware finds a core, colour-only finds none | REQ-CORE-010..012 |
| AUTO-CORE-003 | `test_run`: opens, blank frame is uniform, palette >= 4, reset survives, SRAM capacity refused | REQ-CORE-001, -024, -030..032 |
| AUTO-CORE-004 | `test_state`: size sane, capacity refused, 30/60/30 round trip byte-exact, corrupted byte and short blob refused | REQ-CORE-060..063 |
| AUTO-CORE-005 | `test_scale`: exact 240 x 216, no write past right edge or last row (stride 257), smooth average / sharp repeat, vertical rule, palette bits ignored, unchanged frame skipped, invalidate repaints, changed row redrawn | REQ-CORE-080..082 |
| AUTO-CORE-006 | `test_audio_clock`: 803/804 at 48 k, hour within one frame, both values occur, 22050 gives 22151 per 60 | REQ-CORE-053 |
| AUTO-CORE-007 | `test_tilt`: gravity axis excluded, four directions, diagonal, saturation, hysteresis, corrected thresholds, zero range | REQ-CORE-083 |
| AUTO-CORE-008 (new) | `test_palette`: count is 5, index by name, unknown name -> 0, out-of-range clamp -> 0, DMG shades lightest-first | REQ-CORE-084 |
| AUTO-CORE-009 (new) | `test_scale_alpha`: with palette entries carrying 0xFF alpha, every output pixel (solid and mixed) has 0xFF alpha | REQ-ROS-002 |
| AUTO-UI-001 (new) | `test_text`: every glyph 32..126 renders inside its 6 x 8 cell; a string's width is 6 * n; 2x scale doubles both; clipped at the surface edge without writing outside | REQ-ROS-030 |
| AUTO-UI-002 (new) | `test_pad`: hit-test of every d-pad cell (corners give two bits), A/B discs, Start/Select pills, menu pill, gaps give 0, the picture region gives 0; draw with `force` writes only rows >= 216; a repeat draw with the same mask writes nothing | REQ-ROS-003, -020 |
| AUTO-ROS-001 (new) | `test_pacing`: the frames-per-tick decision (queue depth in, 0/1/2 out): below target -> 1, far below -> 2, above -> 0, muted -> always 1 | REQ-ROS-010, -011 |
| AUTO-ROS-002 (new) | `test_fs_hb` over the fake: library scan sorts case-insensitively, skips dot files, caps at 64 with a remainder count; `.sav` path derivation; wrong-size refusal leaves the fake file unchanged; state write reports failure when the fake refuses | REQ-DATA-001..003, -010..011, -022 |
| AUTO-ROS-003 (new) | `test_input_hb` over the fake: three scripted fingers on Right, Down-Right, A produce `RIGHT|DOWN|A`; Vol Up level -> A; Home never sets a bit; Play/Pause never sets a bit | REQ-ROS-020..023 |
| AUTO-AUD-001 | `test_audq` over a fake audio task: FIFO reclaim only after `+0x64` seen 1 then 0; seal at 1323 frames; the first block is played and its voice counter corrected, later ones only linked; queued frames follow the clock; the target stops the emulator being asked; overflow drops and counts | REQ-AUD-010..013 |
| AUTO-AUD-002 | `test_audq`, duration: exact ms only when `frames*1000 % out_rate == 0` and the inverse round-trips: 1323 -> 60 @ 22050, 30 @ 44100, 0 @ 48000; the quiet block 7056 -> 320 | REQ-AUD-010 |
| AUTO-AUD-003 | `test_audq`, silence: pause breaks the chain, zeros the blocks behind the head, self-chains the quiet block on its own voice, drops pushes; resume chains the next seal from the silence with no new voice and fades it in; a long gap drops the queue and counts a restart | REQ-AUD-020..023 |
| AUTO-DATA-001 (new) | `test_settings`: round-trip of all keys; unknown key ignored; misspelled palette -> 0; missing file -> defaults per target | REQ-DATA-030..031 |
| AUTO-DATA-002 (new) | `test_save_posix`: path derivation with dots in directories; size refusal; changed-only write via CRC (count writes with a fake clock) | REQ-DATA-010..012 |

## Gate (`make gate`)

| ID | Check | Pass criterion |
|---|---|---|
| AUTO-GATE-001 | `tg_headless cpu_instrs.gb -f 8000 -s -q` | exit 0 (serial log contains "Passed", no "Failed") |
| AUTO-GATE-002 | `tg_headless instr_timing.gb -f 2000 -s -q` | exit 0 |
| AUTO-GATE-003 | `tg_headless dmg-acid2.gb -f 60 -r acid2.raw` + `acid2check.py` | all 23040 pixels match `reference-dmg.png` |

## Build-time checks

| ID | Check | Where |
|---|---|---|
| AUTO-BUILD-001 | `make check-seam`: `nm` over every non-core object finds no `gb_*` or `audio_*` Peanut symbol | Phase 1; AC-CORE-004 |
| AUTO-BUILD-002 | The CI `retailos` job: this repository checked out as `NanoApps/apps/tinygb`, `make all check-size` with `arm-none-eabi-gcc`; a link failure at `mkrelocapp.py` is how a stray libc call shows up. Runs on every push since Phase 1; grows with `SRCS` in Phase 3 | Phase 1; REQ-ROS-050, -070 |
| AUTO-BUILD-003 | `.hbapp` size printed and asserted under 512 KiB | Phase 3; REQ-ROS-072 |
| AUTO-BUILD-004 | N31 `check-built`: FP arch, no VFP-args tag, no NEEDED | existing; REQ-N31-050 |
| AUTO-BUILD-005 | `grep -rn NanoApps` in the repo finds only provenance and the `NANOAPPS ?=` variables | Phase 2; AC-N31-005 |

## Integration tests (host)

| ID | Test |
|---|---|
| AUTO-INT-001 | `tg_headless -S` scaled output for acid2 at frame 60 has a stable CRC recorded in `tests/expected/acid2-scaled.crc`; a change to the scaler or palette fails this and the CRC is updated deliberately |
| AUTO-INT-002 | A synthetic 32 KiB cartridge with MBC1+RAM+BATTERY writes a known SRAM pattern; `tg_save_tick` with a fake clock writes exactly once; the file content matches |

## Requirement-to-test map (summary)

| Requirement group | Tests |
|---|---|
| REQ-CORE-* | AUTO-CORE-001..009, AUTO-GATE-*, AUTO-BUILD-001 |
| REQ-N31-* | AUTO-BUILD-004, -005; the rest is manual (MAN-N31-*) |
| REQ-ROS-* | AUTO-UI-*, AUTO-ROS-*, AUTO-BUILD-002, -003; the rest manual (MAN-ROS-*) |
| REQ-AUD-* | AUTO-AUD-001..003; the rest manual (MAN-AUD-*) |
| REQ-DATA-* | AUTO-DATA-*, AUTO-ROS-002, AUTO-INT-002; the rest manual (MAN-DATA-*) |

## Not automatable here

Anything that needs the OS: the real touch mailbox, the real mixer, the arena allocator, the panel. Those are in `TEST-PLAN-manual.md`, and the one that matters most (the picture) has a byte-exact check rather than an eyeball (AC-ROS-001, AC-N31-002).
