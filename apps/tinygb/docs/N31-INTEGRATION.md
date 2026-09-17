# N31 Integration

How the N31 Linux build of TinyGB is produced and picked up by the ipod tree's install and release scripts. Since Phase 2 the build reaches into no other checkout: everything it borrowed from NanoApps is vendored under `linux/shim/`.

## Building

```sh
make n31                         # from this repository; or, inside NanoApps, make -C apps/tinygb/host -f Makefile.n31
```

`host/Makefile.n31` keeps every variable it had:

- `IPOD_ROOT ?= /mnt/c/src/ipod/artifacts/linux-n31` (toolchain, alsa-lib, libdrm, tinyalsa, ssh key). The stripped binary is copied to `$(IPOD_ROOT)/tinygb` (`-hard` suffix for a hard-float experiment) at the end of every build.
- `LVGL`: the pinned LVGL checkout that NanoApps' `start` makes. Found at `../../../lvgl` when this repository is `apps/tinygb` inside NanoApps, at `../../NanoApps/lvgl` when it is a checkout beside one; set `LVGL=` for anything else. The native mirror (`linux/shim/lvgl-mirror.mk`) makes the location irrelevant for speed.
- `TG_AUDIO ?= alsalib`, `TG_AUDIO_RATE ?= 48000`, `FLOAT_ABI ?= softfp`, `TG_OPT ?= -O2`, `LV_CACHE` keyed by the `lv_conf_n31.h` hash and the float ABI.

`check-built` still refuses a binary with the wrong FP arch, a hard-float ABI tag, or any `NEEDED` entry.

## The shims

`linux/shim/` holds copies of, from `NanoApps/apps/n31launcher`: `drmfb.c/h` (the DRM surface), `fbcon.c/h` (console handoff), `touch.c/h` (finding the touch panel), `fbrefresh.h`; from `NanoApps/apps`: `build_stamp.c/h`, `lvgl-mirror.mk`; and `lv_conf_base.h`, which is `NanoApps/sdk/lv_conf.h`, the config `host/lv_conf_n31.h` layers on. `PROVENANCE` names the source path of each and the NanoApps commit they were copied at.

They are the launcher's to change (fbDOOM draws through the same `drmfb.c`), so the copies are refreshed, never edited:

```sh
tools/sync-n31-shims.sh            # diff the copies against NanoApps (exit 1 on drift)
tools/sync-n31-shims.sh --apply    # take NanoApps' versions and rewrite PROVENANCE
```

`NANOAPPS` defaults to `../..` inside a NanoApps tree or `../NanoApps` beside one. The script never runs from the build; `QA-CHECKLIST.md` runs it before an N31 tag.

## What the ipod tree does (as of Phase 2)

| Script (in `C:\src\ipod\tools\linux-n31`) | Behaviour |
|---|---|
| `build-n31-apps.sh` | `TINYGB="${TINYGB:-$(dirname "$ROOT")/tinygb}"`, falling back to `$NANOAPPS/apps/tinygb` when there is no standalone checkout; `build_tinygb` runs `make -C "$TINYGB/host" -f Makefile.n31`. Prints the "tinygb goes on the volume" note as before. |
| `install-n31os-disk.ps1` | copies `artifacts\linux-n31\tinygb` (where either build stages it) into `n31os/apps/tinygb/`, cartridges from `docs-internal\TinyGB` into `roms\` beside it |
| `mk-release-zip.sh` | unchanged: reads `artifacts/linux-n31/tinygb` |
| `n31os-apps/tinygb/app.json` | unchanged (name, exec, tagline, glyph, color, screen=framebuffer) |
| `build-n31-fbdoom.sh` | unchanged; fbDOOM still compiles the launcher's `drmfb.c` from NanoApps, which is the original of TinyGB's copy |

Those two edits sit uncommitted in the ipod tree (it was on a feature branch with other work in progress when they were made).

## Running on the device

Unchanged: `make -C host -f Makefile.n31 push` copies to `/tmp/tinygb`; `run ROM=tetris` runs it; the volume install puts it at `/mnt/disk/n31os/apps/tinygb/tinygb` with `roms/` beside it; `TINYGB_ROMS` and `TINYGB_ALSA_DEVICE` override the library and the ALSA device (the latter is how `tinybt`'s `snd-aloop` route is used).

## Verification of the move

Done without a device (Phase 2): a clean `make n31` from the standalone checkout succeeds, `check-built` passes, no dependency file names a NanoApps path, and the sync script reports no drift. Still to do on the device, after the RetailOS work: AC-N31-002 (byte-identical acid2 capture via `tools/grab-screen.sh`) and a play session (MAN-SMOKE-003, MAN-N31-010, MAN-DATA-001).
