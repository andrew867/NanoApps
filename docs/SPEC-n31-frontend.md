# SPEC: N31 Linux Front End

## Purpose

Describe the front end that already runs on the N31 Linux port of the iPod nano 7G (`linux/tg_linux.c` plus the Linux platform files), as numbered requirements, so that moving the code to its own repository and building it from there can be checked for parity rather than eyeballed.

Nothing in this spec is new behaviour. Where the current code has a known defect it is noted and given a requirement, not silently fixed.

## Scope

- `linux/tg_linux.c`: main, the play loop, the pause menu, save states.
- `platform/tg_fb_linux.c`, `tg_audio_alsalib.c`, `tg_audio_linux.c`, `tg_input_linux.c`, `tg_pad.c`, `tg_roms.c`, `tg_save.c`.
- `ui/tg_menu.c`: the LVGL menu.
- `host/Makefile.n31`, `host/lv_conf_n31.h`.
- The three files compiled in from the launcher: `drmfb.c`, `fbcon.c`, `touch.c`, and the shared `build_stamp.c` and `lvgl-mirror.mk`.

## Non-Goals

- Changing the menu's look, controls, or pages.
- A Bluetooth gamepad (Phase 06).
- Making the tinyalsa backend work on this device (kept, selectable, known broken).

## User / Actor Stories

- As the player, I open TinyGB from the N31 launcher, pick a cartridge with Vol Up / Vol Down / Play, and it starts in under two seconds.
- As the player, I press Home during a game and get a pause menu; Home again from the top of the menu leaves TinyGB and returns to the launcher.
- As the player, my Pokemon save survives a power-button hold, because the battery RAM is written to disk within seconds of changing.
- As the maintainer, I build from the standalone repo with one make invocation and get a binary that the existing `install-n31os-disk.ps1` and `mk-release-zip.sh` pick up unchanged.

## Functional Requirements

### Display

- **REQ-N31-001**: The front end shall open `/dev/dri/card0` through `n31_drmfb_open` and, when that fails or `N31_DISPLAY=fbdev` is set, fall back to `/dev/fb0`. A missing DRM driver shall never mean a black screen.
- **REQ-N31-002**: The picture shall be 240 x 216 at the top of the 240 x 432 panel (`tg_fb_layout` returns x centred, y = 0), with the touch pad below.
- **REQ-N31-003**: The framebuffer console shall be detached before the first frame and restored on exit, so kernel messages neither draw over the game nor are lost after it.
- **REQ-N31-004**: On the DRM path every frame shall end with one atomic commit carrying a full-surface `FB_DAMAGE_CLIPS` blob and a flip event that is waited on. There is one buffer, written in place, because the scaler's row skipping depends on the destination still holding the previous frame.
- **REQ-N31-005**: The framebuffer shall be rejected unless it is 32 bpp; the mapping length shall come from `line_length * yres` (or `smem_len` if larger), never `w * h * 4`.

### Audio

- **REQ-N31-010**: Sound shall go to ALSA `hw:0,0` through alsa-lib by default, at 48000 Hz, 16-bit interleaved stereo. 44100 shall never be requested: the codec's 12 MHz master clock does not divide to it, and the driver accepts it and then does not clock it.
- **REQ-N31-011**: `tg_audio_write` shall block until the device has taken the frames. This is the master clock of the emulator; video follows audio.
- **REQ-N31-012**: A monotonic deadline (`next += 70224 * 1e9 / 4194304` ns, accumulated from the start time) shall run underneath the blocking write, so that a sink whose write returns without blocking cannot let the emulator free-run. If the loop falls more than half a second behind, the debt is forgiven rather than fast-forwarded.
- **REQ-N31-013**: `TINYGB_ALSA_DEVICE` shall override the device name (used to route through `snd-aloop` to Bluetooth), with no fallback when it fails to open.
- **REQ-N31-014**: `--mute` shall use a null sink that still blocks for the right duration, so pacing is identical with and without sound.
- **REQ-N31-015**: The front-end staging buffer shall hold `rate / 30 + 64` frames so the clock's 803-or-804 never needs a resize mid-run.

### Input

- **REQ-N31-020**: Buttons shall be read as levels from evdev and OR-ed across sources: physical keys (Vol Up = A, Vol Down = B, Play/Pause = Start), the accelerometer as a d-pad (`tg_tilt`), and the touch panel as an on-screen pad when `n31_touch_find` locates one. There is no physical Select.
- **REQ-N31-021**: Home shall never reach the cartridge; it is reported through `tg_input_take_quit`. Power shall be ignored entirely (the launcher owns it).
- **REQ-N31-022**: The evdev event struct shall be declared by hand as `{ long sec; long usec; u16 type; u16 code; s32 value; }`, because on this 32-bit kernel with 64-bit `time_t` the headers and the kernel disagree.
- **REQ-N31-023**: Tilt calibration shall average the first 250 ms of readings at open and exclude the axis nearest gravity. Default thresholds are 22 % press, 12 % release; `--tilt A,B` overrides and `--no-tilt` disables.
- **REQ-N31-024**: The touch panel shall be read as multitouch Protocol B (4 slots), the held mask rebuilt from all live slots at every `SYN_REPORT`, and every contact hit-tested through `tg_pad_hit` so a direction and A can be held at once.

### Library, saves, states

- **REQ-N31-030**: The ROM library is `$TINYGB_ROMS`, else `roms/` beside the binary located via `/proc/self/exe`. `.gb` and `.gbc` in any case are listed, sorted case-insensitively; names starting with `.` are skipped.
- **REQ-N31-031**: A ROM named on the command line shall resolve as: readable path, else exact library filename, else unique case-insensitive substring; two matches shall be refused with a message naming the ambiguity.
- **REQ-N31-032**: Battery RAM shall be loaded from `<rom>.sav` before `open`, checksummed every 5 s during play, written only when changed, and flushed on every exit path including the pause menu's Save state.
- **REQ-N31-033**: A `.sav` whose size differs from the cartridge's declared SRAM shall be refused rather than padded.
- **REQ-N31-034**: Save states shall live at `<rom>.st0`; writing shall check `fclose` and a short state shall be refused on load.

### Menu

- **REQ-N31-040**: With no ROM on the command line the front end shall start on the LVGL ROM picker; Home during a game shall open the pause menu (Resume, Save state, Load state, Restart cartridge, Settings, Choose another game, Quit). Settings offers Palette (5 entries), Scaling (smooth/sharp), Tilt d-pad.
- **REQ-N31-041**: Before the menu draws over a DRM session the front end shall close the DRM surface, and reopen it, refresh the destination pointer, reinitialise the scaler with the possibly-changed palette, and invalidate the pad on return. Without this the menu renders into a buffer nobody scans out.
- **REQ-N31-042**: The menu shall drain queued key events on entry (`flush_keys`), because evdev delivers every event to every reader.
- **REQ-N31-043**: LVGL shall be initialised once and suspended/resumed around the game, not torn down (about 400 KB of allocation each way).

### Build and staging

- **REQ-N31-050**: The N31 binary shall be static, soft-float ABI, `-mcpu=cortex-a5 -mthumb -mfpu=vfpv4`, and the build shall fail if `readelf` shows `Tag_ABI_VFP_args`, an FP arch outside VFPv2..VFPv4, or any `NEEDED` entry.
- **REQ-N31-051**: The build shall bake a UTC stamp and short commit hash via `-DEN_BUILD_STAMP` / `-DEN_BUILD_GIT`, and `build_stamp.o` shall depend on every other object so the stamp moves whenever the binary does, without a FORCE prerequisite (which recursed forever through the `check` target).
- **REQ-N31-052**: The stripped binary shall be copied to `$(IPOD_ROOT)/tinygb` (`-hard` suffix for a hard-float experiment) so `mk-release-zip.sh` and `install-n31os-disk.ps1` find it.
- **REQ-N31-053**: LVGL shall compile from the native mirror (`lvgl-mirror.mk`) into a cache keyed by the `lv_conf` hash and float ABI.
- **REQ-N31-054** (new, from the split): The build shall not reach into a NanoApps checkout. The five files it borrows (`drmfb.c/h`, `fbcon.c/h`, `touch.c/h`, `fbrefresh.h`, `build_stamp.c/h`) and `lvgl-mirror.mk` shall be vendored under `n31/shim/` with a provenance header naming the source path and commit, and `tools/sync-n31-shims.sh` shall refresh and diff them against `$NANOAPPS` when one is present.

### Known defects carried into the spec

- **REQ-N31-060**: `main` in menu-less mode shall return 2 when `play_once` did not reach `TG_MENU_QUIT` (before Phase 2 the expression read `== TG_MENU_QUIT ? 0 : 0`, so a cartridge that would not start still exited 0). Fixed in Phase 2.
- **REQ-N31-061**: `lv_conf_n31.h` and `tg_scale.h` shall describe the SoC as what it is: a Cortex-A5 with VFPv4 and no NEON, not a Cortex-A8. Fixed in Phase 2.

## State and Data

Owned by `play_once`'s stack: `tg_menu_state` (rom path, title, palette, smooth, tilt, have_game, can_state, have_state), the `tg_fb`, `tg_scaler`, `tg_audio_clock`, `tg_save`. Heap: ROM, SRAM, core context, staging buffer, LVGL's 640 KB pool.

## Main Flows

`main` -> (args decide) `play_once` once, or `tg_menu_init` -> loop { `tg_menu_run(picker)` -> `play_once(menu)` }. `play_once` -> resolve ROM -> read file -> probe -> select core -> SRAM + `.sav` -> open sink -> `open` core -> clock, buffer -> fb -> palette/scaler -> input -> warm-up -> loop { poll, pad, quit?, save tick, run_frame, scale+flush, audio, deadline } -> report -> flush save -> close everything.

## Edge Cases

- Cartridge would not start from the menu: `play_once` returns an error code that lands in the action enum; the loop shows "that cartridge would not start" and returns to the picker.
- Audio device refuses every rate: sink is NULL, sound is off, the deadline paces.
- Touch panel absent (Grape fails its handshake on current kernels): the pad is not drawn and not read; keys and tilt still work.
- `.sav` from another emulator of the wrong size: refused, game starts fresh, existing file untouched.

## Failure Handling

Every failure prints one line to stderr naming the path or device and returns 2. SIGINT/SIGTERM set a flag the loop checks, so the save flushes before exit (the launcher sends SIGTERM on Home, SIGKILL only after 4 s).

## Security / Privacy Notes

No network, no writes outside the ROM's own directory. ROMs are the owner's; the release zip deliberately ships none.

## Acceptance Criteria

- **AC-N31-001**: Given the standalone repo and `IPOD_ROOT` pointing at the artifacts directory, when `make TARGET=n31` runs, then `out/n31/tinygb` exists, `check-built` passes all three assertions, and a copy lands in `$(IPOD_ROOT)/tinygb`.
- **AC-N31-002**: Given that binary on the device, when `tools/grab-screen.sh` captures `/dev/fb0` after 60 frames of dmg-acid2, then the 240 x 216 region is byte-identical to `tg_headless -S` output for the same frame and palette.
- **AC-N31-003**: Given a ROM with battery RAM, when a save is made in-game and the power button is held within 10 s, then on the next boot the save is present.
- **AC-N31-004**: Given the DRM path, when Home opens the pause menu and Resume is chosen, then the menu is visible while open and the game repaints fully on return.
- **AC-N31-005**: Given `grep -rn NanoApps n31/ Makefile`, when run in the standalone repo, then the only hits are provenance comments and the `NANOAPPS ?=` variable used by the shim sync script.

## Test Coverage Notes

Automated on the host: everything below the front end (see `TEST-PLAN-automated.md`). On the device: manual (`TEST-PLAN-manual.md` MAN-N31-*), plus the byte-exact screen capture, which is the one device check that needs no eyes.

## Open Questions

OQ-002 (vendor vs. reference the launcher shims), OQ-003 (where the N31 install script should look once the binary comes from a new repo).
