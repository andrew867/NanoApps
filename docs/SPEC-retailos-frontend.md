# SPEC: RetailOS Front End

## Purpose

Define the Game Boy front end that runs as a NanoApps raw-surface app on the iPod nano 7G's stock firmware (RetailOS): the picture, the controls, the pacing, the menu, the lifecycle, and the memory and build constraints the platform imposes. This is Phase 04 of the original plan and the bulk of the new work in this repository.

Everything below is derived from the SDK as it is in the NanoApps fork on 2026-09-16 (`sdk/hb_raw_surface.*`, `hb_touch.c`, `hb_button.c`, `hb_accel.c`, `hb_fs.c`, `hb_heap.*`, `hb_app.mk`, `tools/mkrelocapp.py`, and the resident). Where the SDK's behaviour is surprising it is stated as a requirement so it cannot be forgotten.

## Scope

- `tinygb.c` (repository root, because the root is the NanoApps app directory): `hb_raw_init`, `hb_raw_frame`, the mode machine (library, playing, paused menu, settings).
- `platform/tg_input_hb.c`: buttons, multitouch pad, accelerometer into one held byte.
- `platform/tg_fs_hb.c`: `tg_save.h` and `tg_roms.h` over `hb_fs`.
- `platform/tg_sys_hb.c`: wake-lock poke, `abort` stub, the `time.h` shim the vendored core needs.
- `platform/tg_text.c`, `ui/tg_menu_raw.c`: text and menus without LVGL.
- `Makefile`, `Info.plist` (repository root).
- Audio is its own spec: `SPEC-retailos-audio.md`.

## Non-Goals

- LVGL on RetailOS. The raw surface has no LVGL; the menu is drawn by hand.
- Landscape or rotation. The raw buffer is portrait and unrotated; the layout is the same as N31.
- Game Boy Color, Super Game Boy borders, link cable.
- Making Home do anything but leave. RetailOS owns it.
- Screen recording or screenshots from inside the app. The resident's Home triple-click already does that.

## User / Actor Stories

- As the player, I tap TinyGB on the Home Screen, see my cartridges, tap one, and it is playing with sound in under two seconds.
- As the player, I hold the iPod in two hands and press a direction and A at the same time on the screen, and Mario jumps while running.
- As the player, I press Home in the middle of a level; when I come back the game resumes from where I left it, because a state was written when I paused and my battery save was written as I played.
- As the player, I open the in-game menu from a pill under the pad, change the palette, and go back without the game resetting.
- As the maintainer, I build the RetailOS app from this repository with `make retailos` and install it with NanoApps' `./start install tinygb`, and the packer tells me the file size against the ceiling every time.

## Functional Requirements

### Surface and picture

- **REQ-ROS-001**: The app shall be built with `RAW_SURFACE := 1` and shall define exactly `hb_raw_init(int w, int h)` and `hb_raw_frame(const hb_spoint_t *touch)`. It shall not use `HB_APP_ENTRY` (that emits a second `payload_entry`) and shall not define `hb_app_main` (no such entry exists; the stub before Phase 1 did this and could not link).
- **REQ-ROS-002**: The surface is 240 x 432 XRGB8888 with stride equal to width. Every pixel the app writes shall carry `0xFF` in the top byte. The scaler's palette tables shall be built with that byte set so its averaging preserves it.
- **REQ-ROS-003**: The picture shall be 240 x 216 at y = 0 (the same `tg_scale_15` output and the same layout as N31); the pad shall occupy y = 216..431 with the geometry in `tg_pad.c`, plus one additional "menu" pill.
- **REQ-ROS-004**: The framebuffer persists between ticks and between apps. `hb_raw_init` shall paint the whole surface before the first tick returns, and the scaler's row skipping shall stay enabled because the picture region is written by nothing else while playing.
- **REQ-ROS-005**: The app shall assume no vsync, no damage tracking, and no fixed frame rate: the OS re-arms a 16 ms one-shot timer after each tick, so the tick period is 16 ms plus the app's own frame time plus scheduling. The app shall budget under 8 ms per tick for one emulated frame plus scaling on this CPU, measured in Phase 3.

### Pacing

- **REQ-ROS-010**: With sound on, the audio queue shall be the clock. Per tick the app shall run 0, 1, or 2 emulated frames: run while the queued audio is below the target lead, never more than 2, and none when it is above. Each emulated frame pulls `tg_audio_clock_next()` frames (369 or 370 at 22050) into the slot being filled.
- **REQ-ROS-011**: With sound off (mute, or the sink refused to start), the app shall run exactly one emulated frame per tick.
- **REQ-ROS-012**: The picture shall be scaled once per tick from the most recent emulated frame, whether that tick ran 0, 1, or 2 frames.
- **REQ-ROS-013**: If the emulator cannot keep the queue fed (a tick that ran 2 frames and the lead is still falling for more than one second), the app shall show the fact on the optional overlay and shall not skip audio; video frames are already the thing being dropped.

### Input

- **REQ-ROS-020**: Every tick the app shall call `hb_touch_drain_all()` then `hb_touch_poll_multi()`, hit-test every active finger through `tg_pad_hit`, and OR the results. The single `touch` argument to `hb_raw_frame` shall be used only for the menu, never for the pad, because it carries one point.
- **REQ-ROS-021**: Vol Up shall be A and Vol Down shall be B, read as levels each tick through `hb_button_pressed` (they are GPIO, active-low, and work simultaneously). The OS volume HUD may appear; that is accepted.
- **REQ-ROS-022**: Play/Pause shall not be mapped to Start by default. RetailOS handles that key before the app sees it and starts the Music player underneath (Entrain measured this). Start and Select exist on the touch pad. Mapping Play/Pause is an option guarded by `hb_media_state()` re-pausing, deferred (see `OPEN-QUESTIONS.md` OQ-005).
- **REQ-ROS-023**: Home shall not be bound to anything. The app may see it held for a tick or two before teardown.
- **REQ-ROS-024**: Tilt shall use `hb_accel_read_milli_g` through the portable `tg_tilt` with range +/- 1000, calibrated over the first 250 ms after a cartridge starts, and shall be off by default on RetailOS (the touch pad works here; tilt is a setting).
- **REQ-ROS-025**: Edge detection is the app's job: `hb_button_pressed` and `poll_multi` are levels with no debounce. Menu navigation shall derive presses from level changes with a 380 ms / 90 ms repeat, the same numbers as the LVGL menu.
- **REQ-ROS-026**: The app shall never call `hb_ui_init()`; it disables the OS touch dispatch that feeds the touch mailbox.

### Menu and text

- **REQ-ROS-030**: The raw surface has no text rendering. The app shall carry its own fixed 6 x 8 ASCII (32..126) bitmap font in `platform/tg_text.c`, drawing into any XRGB surface with a stride, at 1x or 2x, portable and unit-tested on the host.
- **REQ-ROS-031**: `ui/tg_menu_raw.c` shall implement the same `tg_menu.h` interface and the same pages and rows as the LVGL menu (Library; Pause: Resume, Save state, Load state, Restart cartridge, Settings, Choose another game; Settings: Palette, Scaling, Tilt d-pad; Palette list), plus an About row showing the build stamp. "Quit TinyGB" is omitted: Home is the way out on RetailOS.
- **REQ-ROS-032**: The menu shall look and behave like the nano's own settings screens and the NanoApps LVGL apps, not like a game menu: a 44 px title bar with a back chevron on the left and the page title centred; full-width 48 px rows with 1 px separators, a right-hand chevron on rows that open a page, a check mark on the chosen entry of a list (palette), and an on/off pill on toggles (scaling, tilt); colours from the NanoApps theme (`hb_color_bg/surface/text/text_dim/primary`, `hb_tint_color`), which follow the device colour and the light/dark setting and are available on the raw path.
- **REQ-ROS-035**: Menu input shall be touch first and shall match the platform's gestures: tap a row to choose it (highlight on press, act on release inside the same row, cancel if the finger leaves it); drag vertically to scroll, content following the finger with no dead zone larger than 8 px; swipe right starting within 24 px of the left edge, or tap the back chevron, to go back. Vol Up / Vol Down shall also move a visible selection so the menu is usable without touch; there is no keyboard "select" on RetailOS other than a tap.
- **REQ-ROS-033**: The in-game menu shall open from a "menu" pill in the pad area (there is no interceptable Home). Opening it shall flush the battery save and shall pause audio (see the audio spec); closing it shall invalidate the scaler and pad so both repaint fully.
- **REQ-ROS-034**: The menu shall run inside `hb_raw_frame` as a mode, never as a loop that does not return: the OS draws only when the tick returns.

### Lifecycle

- **REQ-ROS-040**: `.bss` is zeroed on every launch, including cached relaunches, and there is no exit callback. All state the player cares about shall be on disk within 2 s of changing (SRAM: checksum every 2 s on RetailOS, not 5; settings: on change; state: on request).
- **REQ-ROS-041**: The app shall keep the panel awake while a cartridge is running by sending the OS idle-reset event every 10 s (the raw runtime has no `hb_wake_lock`; the addresses are in `sdk/hb_lv_surface.c`), and shall stop doing so in the library and menus so the normal backlight timer applies.
- **REQ-ROS-042**: If RetailOS reports the Music player active while a cartridge is running (`hb_media_state`, polled at 4 Hz), the app shall pause it once, as Entrain does, so a stray Play/Pause does not leave music playing under the game.

### Memory

- **REQ-ROS-050**: The build shall be freestanding: no `malloc`, no `printf` family, no `qsort`, no `<math.h>`. `platform/tg_tilt.c` shall lose its `snprintf`; `tg_save.c` and `tg_roms.c` are replaced on this target by `tg_fs_hb.c`; the vendored core's `#include <time.h>` shall be satisfied by `platform/retailos/include/time.h` declaring `struct tm`; `abort` is not referenced because `__builtin_unreachable` exists, and a stub shall be provided anyway.
- **REQ-ROS-051**: The ROM shall live in a static `.bss` window of 1 MiB (`TG_ROS_ROM_MAX`). A cartridge larger than the window shall be refused in the library with its size shown, not truncated. `.bss` is excluded from the `.hbapp` file, so the window costs nothing on disk.
- **REQ-ROS-052**: Before Phase 3 fixes the window size, the app shall log `hb_os_heap_largest()` and `hb_os_heap_free()` to the trace ring at init on a real device, because the arena that holds `.bss` is allocated with the OS allocator that reboots the device on failure. If a 1.2 MB arena proves unsafe the fallback is `hb_os_alloc` with the block recorded in a fixed DRAM mailbox so a relaunch can reuse or free it (OQ-004).
- **REQ-ROS-053**: The app shall make no `hb_os_alloc` call whose block it cannot free before the next launch, other than the audio slots defined in the audio spec, because those blocks are not part of the arena and are not freed at teardown.
- **REQ-ROS-054**: Stack use inside `hb_raw_frame` shall stay small (under 4 KiB): it runs on the OS draw task. The scaler's 1920-byte row buffers are the largest frame.

### Filesystem

- **REQ-ROS-060**: The library directory is `/Apps/Data/TinyGB/roms/`, created with `hb_fs_mkdir` at init. `.gb` and `.gbc` in any case are listed with `hb_fs_dir_open/next`; names beginning with `.` are skipped; the list is sorted case-insensitively with an insertion sort (no `qsort`).
- **REQ-ROS-061**: There is no seek and no partial read. A ROM shall be read in one `hb_fs_read` into the window after `hb_fs_size` confirms it fits. `.sav` and `.st0` likewise are whole-file reads and writes.
- **REQ-ROS-062**: File operations shall never be interleaved with an open write stream, and no file operation shall run from anywhere but the tick (the SDK's scratch cache is shared and not reentrant).

### Build and packaging

- **REQ-ROS-070**: The repository root shall itself be a valid NanoApps app directory: `Makefile` (sets `APP_NAME := tinygb`, lists the core, portable, RetailOS platform and UI sources by relative path, sets `RAW_SURFACE := 1`, sets `EXTRA_CFLAGS` before the include because a `:=` inside `hb_app.mk` consumes it, and includes `$(NANOAPPS)/sdk/hb_app.mk` with `NANOAPPS ?= ../..`), `Info.plist`, and `tinygb.c`. This is what lets NanoApps carry the repository as `apps/tinygb` (a git subtree) and build it with no other checkout.
- **REQ-ROS-071**: `EXTRA_CFLAGS` shall carry `-O2`, `-DAUDIO_SAMPLE_RATE=22050 -DMINIGB_APU_AUDIO_FORMAT_S16SYS=1`, the vendor warning suppressions, and `-I` for the `time.h` shim. `-O2` takes effect (a later `-O` wins). The arch flags `-mcpu=cortex-a5 -mfpu=vfpv4` (OQ-008, decided) are stated there too but are inert until `hb_app.mk` stops restating `-mcpu=cortex-a8 -mfpu=neon` in its link flags after `EXTRA_CFLAGS`; the plan carries a small `hb_app.mk` change (an `HB_ARCH_FLAGS` variable) for the NanoApps fork and upstream.
- **REQ-ROS-072**: `make check-size` shall print the `.hbapp` size and fail if it exceeds 512 KiB, leaving 64 KiB of headroom under the resident's 576 KiB staging slot, which truncates silently. (Phase 1: the stub is 676 bytes.)
- **REQ-ROS-073**: `Info.plist` shall declare `CFBundleIdentifier org.nanoapps.tinygb`, `CFBundleName TinyGB`, `HBAppKind 1`, a Game Boy-ish glyph (`gamepad`) and the green accent `#7bab3a` used by the N31 menu, replacing the scaffolder's paintbrush and purple.
- **REQ-ROS-074**: NanoApps (the fork first, upstream when submitted) shall carry this repository as `apps/tinygb` via `git subtree` (`git subtree add --prefix=apps/tinygb https://github.com/andrew867/tinygb main --squash`, refreshed with `git subtree pull`), so `./start build tinygb`, `./start install tinygb`, and `make -C apps` work unchanged for anyone who clones NanoApps. Changes flow from this repository to NanoApps; a fix made inside NanoApps is pushed back with `git subtree push`. See `RETAILOS-INTEGRATION.md`.

## State and Data

```
mode: LIBRARY | PLAYING | MENU
static: rom_window[1 MiB], sram[128 KiB max], ctx[core->ctx_size], tg_scaler, tg_menu_state,
        tg_tilt, tg_save, tg_rom_list (names in a fixed 64 x 96-byte table),
        settings {palette, smooth, tilt, last_rom}
per tick: held byte, prev_held for edges, tick counter, ms since last save check
```

## Main Flows

**Launch**: `hb_raw_init` paints the surface, makes the data directory, loads settings, scans the library, sets `mode = LIBRARY`, and logs heap headroom to the trace ring. Nothing is emulated yet.

**Start a cartridge**: size check -> read into window -> `tg_rom_probe` -> `tg_core_for_rom` -> load `.sav` -> `open` -> start the audio sink -> init scaler and pad -> calibrate tilt if enabled -> `mode = PLAYING`.

**Tick while playing**: read inputs -> decide frames from queue depth -> run them, pulling audio -> scale once -> draw pad if the held mask changed -> feed the sink -> save tick -> wake-lock poke every 10 s -> media re-pause check every 250 ms.

**Menu**: the pill opens `mode = MENU`; audio pauses; every tick draws the current page and handles taps/buttons; Resume returns to PLAYING with a full repaint; Choose another game closes the core and returns to LIBRARY.

## Edge Cases

- Empty library: the screen says where to put cartridges (`/Apps/Data/TinyGB/roms`) and how (the NanoApps `start` data copy, or disk mode).
- A ROM over 1 MiB: listed greyed with its size and a "too big for this build" note; tapping it does nothing.
- A `.sav` of the wrong size: refused, the game starts fresh, the file is left alone, and the menu's About page shows the last refusal.
- Two fingers on the d-pad plus one on A: three slots, three hits, one mask.
- The OS shows the volume HUD when Vol Up is pressed: accepted; the game still receives A.
- The player presses Home during a save write: `hb_fs_write` syncs before it returns; a tick is not interrupted mid-call.

## Failure Handling

- Sink refuses to start (heap too fragmented for the slots): play silent, show a one-line note on the overlay, keep running one frame per tick.
- Core refuses the cartridge: return to the library with `tg_strerror` shown under the entry.
- Heap headroom below the audio slots' requirement at launch: library still works; starting a cartridge tries again, since the OS may have freed memory.

## Security / Privacy Notes

The app writes only under `/Apps/Data/TinyGB`. It reads untrusted ROM bytes through the bounds-checked core. It calls three fixed firmware addresses (idle reset, media state, the SFX player) that NanoApps already uses.

## Acceptance Criteria

- **AC-ROS-001**: Given the fetched dmg-acid2 ROM in the library, when it is started and 60 frames have run, then a screenshot taken with the resident's triple-click shows the reference face in the top 240 x 216 and the 216-row region is byte-identical to `tg_headless -S` for the same palette (compared after pulling media with `./start pull media`).
- **AC-ROS-002**: Given Tetris (World), when it runs for five minutes with sound, then no audible gap or click occurs and the overlay's underrun counter is 0.
- **AC-ROS-003**: Given Super Mario Land, when Right and A are held on the pad at once, then Mario runs and jumps; when Vol Up is pressed with Right held on the pad, the same.
- **AC-ROS-004**: Given a cartridge with battery RAM, when a save is made in-game and Home is pressed within 3 s, then the save is present on the next launch.
- **AC-ROS-005**: Given the menu, when the palette is changed and Resume chosen, then the picture repaints fully in the new palette within one tick and the game has not reset.
- **AC-ROS-006**: Given `make retailos`, when the build finishes, then it prints the `.hbapp` size and the size is under 512 KiB.
- **AC-ROS-007**: Given ten consecutive launches from the Home Screen, when `hb_os_heap_free()` is logged at each init, then it does not fall by more than the audio slots' size per launch (no unbounded leak).
- **AC-ROS-008**: Given a cartridge running with tilt off and no touch for 60 s, then the panel has not dimmed or slept.

## Test Coverage Notes

Host-testable: `tg_text`, the refactored `tg_pad` (draw into a stride buffer, hit-test), the frame-count decision function (queue depth in, frames out), `tg_fs_hb` over a fake `hb_fs`, and the settings file parser. Device-only: everything with an `hb_` call, covered by `TEST-PLAN-manual.md` MAN-ROS-* and the byte-exact screenshot in AC-ROS-001.

## Open Questions

OQ-004 (ROM window vs. `hb_os_alloc`), OQ-005 (Play/Pause as Start), OQ-008 (`-mfpu` and jump tables), OQ-009 (whether to add an FPS/queue overlay toggle to Settings).
