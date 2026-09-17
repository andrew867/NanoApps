# SPEC: Persistence (Library, Saves, States, Settings)

## Purpose

Define what TinyGB writes to disk, where, in what format, and when, on both device targets, so that a player's save is never lost to a power-button hold, a file from another emulator loads or is refused predictably, and the two targets can share a cartridge folder on the same iPod.

## Scope

- The cartridge library directory and its listing rules.
- Battery RAM (`.sav`), save states (`.st0`), the settings file.
- The write cadence and the flush points on each target.
- The two implementations: `platform/tg_save.c` + `tg_roms.c` (POSIX, N31 and host) and `platform/tg_fs_hb.c` (RetailOS).

## Non-Goals

- Cloud sync, save transfer between devices, RTC persistence for MBC3 (no RTC games in scope).
- Multiple state slots (the `.st0` name is numbered so a second slot is a filename later).

## User / Actor Stories

- As the player, I copy `Tetris.gb` into the roms folder and it appears in the list, sorted, on the next launch.
- As the player, my `Pokemon.sav` from another emulator works here, and TinyGB's works there.
- As the player, I never see "save corrupted": a file that does not fit is left alone and I am told.
- As the maintainer, I can point either target at the same `roms/` folder on the iPod's volume and both see the same saves.

## Functional Requirements

### Library

- **REQ-DATA-001**: The library directory is, in order: `$TINYGB_ROMS` (N31/host only), else `roms/` beside the executable (N31: via `/proc/self/exe`), else `/Apps/Data/TinyGB/roms` (RetailOS). It shall be created if missing where the platform can.
- **REQ-DATA-002**: Files ending `.gb` or `.gbc` in any case are cartridges. Names beginning with `.` are ignored (macOS resource forks). The list is sorted case-insensitively.
- **REQ-DATA-003**: The RetailOS listing shall be bounded: at most 64 entries of 96 characters, stored in `.bss`; more files are counted and the count shown ("and 12 more"). N31 grows a heap list without limit.
- **REQ-DATA-004**: The last cartridge played shall be remembered in settings and preselected in the library.

### Battery RAM

- **REQ-DATA-010**: The save file is the cartridge's path with the extension replaced by `.sav` (only when the last dot is in the filename, not in a directory).
- **REQ-DATA-011**: The save is loaded before `open`, because a cartridge reads it at boot. A file whose size differs from the cartridge's declared SRAM is refused and left untouched, and the refusal is remembered for the menu's About page.
- **REQ-DATA-012**: During play the SRAM is CRC32-checked every `TG_SAVE_CHECK_MS` (N31: 5000; RetailOS: 2000) and written only when the checksum differs from what is on disk. The write is whole-file (`hb_fs_write` syncs before returning; POSIX `fwrite` + `fclose` checked).
- **REQ-DATA-013**: The save is flushed unconditionally on: pause menu open, save state, cartridge change, and (N31 only) process exit and SIGTERM.

### Save states

- **REQ-DATA-020**: The state file is the cartridge path with `.st0`. It contains exactly the bytes `core->state_save` produced; no header of TinyGB's own, because the core already refuses foreign blobs (`TG_ERR_STATE`).
- **REQ-DATA-021**: Saving a state shall flush the SRAM first, so a restored state never sits beside a stale `.sav`.
- **REQ-DATA-022**: A state write that fails (short write, `fclose` error, `hb_fs_write` false) shall be reported and shall not leave a partial file where a whole one was; the POSIX path writes to `.st0.tmp` and renames, the RetailOS path writes once (the SDK truncates and syncs) and reports failure.
- **REQ-DATA-023**: On RetailOS, opening the pause menu shall also write a state (the "resume where I left it" behaviour), and the library shall offer Resume for the last cartridge when its `.st0` exists. Loading it is explicit, never automatic.

### Settings

- **REQ-DATA-030**: Settings are one text file, `key=value` per line, keys `palette` (name), `smooth` (0/1), `tilt` (0/1), `last_rom` (filename), `volume` (RetailOS, 0..100). Location: N31 `$XDG_CONFIG_HOME/tinygb/settings` else `~/.config/tinygb/settings`; RetailOS `/Apps/Data/TinyGB/settings.txt`.
- **REQ-DATA-031**: Unknown keys are ignored; an unknown palette name selects palette 0; a missing file means defaults (palette 0, smooth 1, tilt: N31 1, RetailOS 0). The parser is portable and unit-tested.
- **REQ-DATA-032**: Settings are written on every change, whole-file, and never more than once per tick.

## State and Data

```
<library>/<Game>.gb        cartridge (read-only)
<library>/<Game>.sav       battery RAM, raw bytes, size == cartridge SRAM
<library>/<Game>.st0       save state, raw core blob
settings.txt               key=value lines
```

## Main Flows

Start cartridge: resolve path -> read ROM -> probe -> load `.sav` (or refuse) -> `open`.
Play: save tick every N ms.
Menu open: flush `.sav`; RetailOS also writes `.st0`.
Save state: flush `.sav`, `state_save`, write.
Load state: read, `state_load`; on `TG_ERR_STATE` show the reason and keep playing.

## Edge Cases

- Two cartridges differing only in case on a case-insensitive FAT volume: both listed as the filesystem reports them; their `.sav` names collide, which is the filesystem's rule, not ours.
- Cartridge removed while its `.sav` remains: the `.sav` is never deleted by TinyGB.
- `.st0` from a previous build of TinyGB: refused by the core with `TG_ERR_STATE`; the file is kept.
- Volume full: writes fail and are reported; the game keeps running; the next check retries.

## Failure Handling

Every write path returns a bool and a reason string that the menu can show. No write failure is fatal to play.

## Security / Privacy Notes

TinyGB writes only beside the cartridges it was given and to its own settings file. It never deletes a file.

## Acceptance Criteria

- **AC-DATA-001**: Given a `.sav` produced by SameBoy for the same cartridge, when TinyGB starts it, then the game shows the same save slots.
- **AC-DATA-002**: Given a `.sav` of the wrong size, when TinyGB starts the cartridge, then the game starts fresh, the file's bytes are unchanged, and the About page names the refusal.
- **AC-DATA-003**: Given in-game progress and the power button held 3 s later on RetailOS (6 s on N31), then the progress is present on the next launch.
- **AC-DATA-004**: Given a state saved and 60 more frames run, when the state is loaded, then the next frame is byte-identical to the frame that followed the save (host test).
- **AC-DATA-005**: Given a settings file with an unknown key and a misspelled palette, when the app starts, then it runs with palette 0 and the other settings intact.

## Test Coverage Notes

Host: `tg_save` path derivation and size refusal, settings parser round-trip, `tg_fs_hb` over a fake `hb_fs` (AUTO-DATA-*). Device: MAN-DATA-*.

## Open Questions

OQ-012 (whether RetailOS should auto-load the pause state on relaunch after an unexpected exit).
