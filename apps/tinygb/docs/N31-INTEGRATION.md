# N31 Integration

How the N31 Linux build of TinyGB is produced from this repository and picked up by the ipod tree's install and release scripts. Before the split, all of this pointed at `NanoApps/apps/tinygb/host`.

## What the ipod tree expects

| Script (in `C:\src\ipod\tools\linux-n31`) | Today | After Phase 2 |
|---|---|---|
| `build-n31-apps.sh` | `build_tinygb() { make -C "$NANOAPPS/apps/tinygb/host" -f Makefile.n31; }` | `TINYGB="${TINYGB:-$(dirname "$ROOT")/tinygb}"`; `build_tinygb() { make -C "$TINYGB/linux" -f Makefile.n31; }`; keep the "tinygb goes on the volume" note |
| `install-n31os-disk.ps1` | copies `$NanoApps\apps\tinygb\host\build-n31\tinygb` | copies `$TinyGB\linux\build-n31\tinygb` (or, simpler, `$Repo\artifacts\linux-n31\tinygb`, which the build stages anyway); cartridges still from `docs-internal\TinyGB` |
| `mk-release-zip.sh` | `want "$ART/tinygb" "$A/tinygb/tinygb"` | unchanged: the build stages into `artifacts/linux-n31/tinygb` |
| `n31os-apps/tinygb/app.json` | manifest (name, exec, tagline, glyph, color, screen=framebuffer) | unchanged; a copy lives in `linux/app.json` here for reference and the ipod copy stays authoritative |
| `build-n31-fbdoom.sh` | comment says the DRM surface is "shared with TinyGB rather than copied, it lives in NanoApps" | still true for fbDOOM; TinyGB's copy is a vendored shim |

## Build inputs

`linux/Makefile.n31` keeps every variable it has today:

- `IPOD_ROOT ?= /mnt/c/src/ipod/artifacts/linux-n31` (toolchain, alsa-lib, libdrm, tinyalsa, ssh key)
- `LVGL ?= ../../lvgl` today; after the move `LVGL ?= $(IPOD_ROOT)/../../../NanoApps/lvgl` is wrong in spirit, so it becomes `LVGL ?= $(HOME)/src/lvgl` overridable, with the README saying to point it at the pinned checkout NanoApps' `start` already makes (`C:\src\NanoApps\lvgl`, `829c7a22`). The mirror in `lvgl-mirror.mk` makes the location irrelevant for speed.
- `TG_AUDIO ?= alsalib`, `TG_AUDIO_RATE ?= 48000`, `FLOAT_ABI ?= softfp`, `TG_OPT ?= -O2`.

Sources that used to be `../../n31launcher/*.c` and `../../build_stamp.c` become `shim/*.c`, and `include ../../lvgl-mirror.mk` becomes `include shim/lvgl-mirror.mk`. `-I../../n31launcher` becomes `-Ishim`.

## The shims

`linux/shim/` holds copies of: `drmfb.c`, `drmfb.h`, `fbcon.c`, `fbcon.h`, `touch.c`, `touch.h`, `fbrefresh.h` (from `NanoApps/apps/n31launcher`), `build_stamp.c`, `build_stamp.h`, `lvgl-mirror.mk` (from `NanoApps/apps`). `PROVENANCE` records the source path and the NanoApps commit they were copied at (`08eed61` on 2026-09-16).

```sh
tools/sync-n31-shims.sh            # NANOAPPS=../NanoApps by default; prints a diff, copies with --apply
```

The script never runs from the build. Drift is caught by running it before an N31 tag (`QA-CHECKLIST.md`).

## Running on the device

Unchanged: `make -C linux -f Makefile.n31 push` copies to `/tmp/tinygb`; `run ROM=tetris` runs it; the volume install puts it at `/mnt/disk/n31os/apps/tinygb/tinygb` with `roms/` beside it; `TINYGB_ROMS` and `TINYGB_ALSA_DEVICE` override the library and the ALSA device (the latter is how `tinybt`'s `snd-aloop` route is used).

## Verification of the move

AC-N31-001 (build + `check-built` + staging), AC-N31-002 (byte-identical acid2 capture via `tools/grab-screen.sh`), and a play session (MAN-SMOKE-003, MAN-N31-010, MAN-DATA-001).
