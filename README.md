# TinyGB

A Game Boy emulator for the iPod nano 7th generation, on both operating systems the device can run: Apple's stock firmware (RetailOS, through NanoApps) and the N31 Linux port.

## Plain-English summary

TinyGB puts a Game Boy on the nano's 240 x 432 screen at exactly one-and-a-half times its native size, with the other half of the screen as a touch pad, sound through whichever audio path the OS offers, and your battery saves kept beside your cartridges in the format every other emulator uses. The emulator itself (Peanut-GB behind a small interface of TinyGB's own) is plain C that runs and is tested on a desktop; each operating system gets a thin front end.

## Current status (2026-09-16)

| Part | State |
|---|---|
| Core, scaler, audio clock, tilt, save logic | done; unit tests and the Blargg / dmg-acid2 gate pass |
| N31 Linux front end | done and playable: DRM or fbdev picture, alsa-lib sound, keys + tilt + touch, LVGL menu, saves, states |
| RetailOS (NanoApps) front end | library, picture, multitouch pad, volume buttons, battery saves, and sound through the OS mixer's descriptor chain: built and unit-tested, not yet run on a device. The menu pages (Phase 5) to come |
| This repository | extracted from the NanoApps fork with history; documented; host tests, the gate, vendor and seam checks, and the RetailOS build run in CI (Phase 1 done) |

## Who this is for

People with a nano 7G who run NanoApps or N31, and the maintainer. It ships no cartridges.

## What it does

- Plays original Game Boy cartridges (`.gb`, and `.gbc` titles that also run on a DMG).
- Scales 160 x 144 to 240 x 216 with a symmetric 2:3 filter (or nearest neighbour), skipping rows that did not change.
- Keeps sound and picture locked: exact rational audio clocking, and on each OS the right clock is the master.
- Saves battery RAM to `<rom>.sav` on change, and one save state to `<rom>.st0`.
- Five palettes, tilt-to-steer as an option, an on-screen multitouch pad.

## Major components

`core/` the emulator behind `tg_core.h`. `vendor/` Peanut-GB and minigb_apu, unmodified. `platform/` portable helpers and the per-OS platform files (`_linux`, `_hb` suffixes). `ui/` the menus. `linux/` the N31 front end. `retailos/` the NanoApps front end. `host/` tests and the headless runner. `tools/` the gate and device scripts. See `docs/ARCHITECTURE.md`.

## Build

This repository is itself a NanoApps app directory. NanoApps carries it as `apps/tinygb` (a git subtree), so from a NanoApps checkout `./start build tinygb` and `./start install tinygb` just work. Standalone:

```sh
make test                      # host unit tests (seconds)
make gate                      # fetches Blargg + dmg-acid2, runs the core gate
make check-seam check-vendor   # no Peanut-GB symbol outside the core; vendor/ unmodified
make NANOAPPS=../NanoApps      # the RetailOS .hbapp, then `make check-size`
make n31                       # static ARM binary, needs IPOD_ROOT (see docs/N31-INTEGRATION.md)
```

Linux or WSL. `make test`/`gate` need gcc and python3 with Pillow; the RetailOS build needs `arm-none-eabi-gcc`, python3 `pyelftools`, and a NanoApps tree; the N31 build needs the toolchain and libraries described in `docs/N31-INTEGRATION.md`.

## Document map

| Document | What it is |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | modules, data flow, why it is shaped this way, failure modes |
| [docs/SPEC-core.md](docs/SPEC-core.md) | the emulator interface, numbered requirements, acceptance criteria |
| [docs/SPEC-n31-frontend.md](docs/SPEC-n31-frontend.md) | the existing Linux front end as requirements, so the move can be checked |
| [docs/SPEC-retailos-frontend.md](docs/SPEC-retailos-frontend.md) | the NanoApps front end: surface, pacing, input, menu, lifecycle, memory, build |
| [docs/SPEC-retailos-audio.md](docs/SPEC-retailos-audio.md) | streaming through chained SoundEffect descriptors |
| [docs/SPEC-persistence.md](docs/SPEC-persistence.md) | library, saves, states, settings on both targets |
| [docs/IMPLEMENTATION-PLAN.md](docs/IMPLEMENTATION-PLAN.md) | phases 0 to 6, repo layout, decisions, deferred work |
| [docs/MILESTONES.md](docs/MILESTONES.md) | six milestones with acceptance and risks |
| [docs/TEST-PLAN-automated.md](docs/TEST-PLAN-automated.md) | host tests, gate, build checks, requirement map |
| [docs/TEST-PLAN-manual.md](docs/TEST-PLAN-manual.md) | device tests a person runs, in tables |
| [docs/QA-CHECKLIST.md](docs/QA-CHECKLIST.md) | ship / no-ship for a tag |
| [docs/RISKS-AND-ASSUMPTIONS.md](docs/RISKS-AND-ASSUMPTIONS.md) | what could go wrong, how it is caught |
| [docs/OPEN-QUESTIONS.md](docs/OPEN-QUESTIONS.md) | decisions still open, with defaults |
| [docs/N31-INTEGRATION.md](docs/N31-INTEGRATION.md) | what the ipod tree's scripts need after the move |
| [docs/RETAILOS-INTEGRATION.md](docs/RETAILOS-INTEGRATION.md) | the forwarder in the NanoApps fork and the RetailOS Makefile |
| [CHANGELOG.md](CHANGELOG.md) | what changed |

## Current build priorities

1. Phases 3 to 5: the RetailOS front end. On-screen touch controls and a menu that behaves like the nano's own screens are the bar; picture first, then sound, then menu, saves, and settings.
2. Device session: RetailOS first, then the N31 build from this repository (Phase 2's deferred checks).
3. Upstream: submit `apps/tinygb` to nfzerox/NanoApps once v0.1 is playable.

## Known gaps

- RetailOS: everything (see status).
- No Game Boy Color: Peanut-GB has no colour code; that is a second core, deferred.
- N31: the touch panel does not register on current kernels, so the on-screen pad is not usable there yet; keys and tilt are.
- N31 built from this repository has not yet been run on the device (the byte-exact screen check and a play session are queued behind the RetailOS work).

## Licence

MIT. Peanut-GB and minigb_apu are MIT (Mahyar Koshkouei; minigb_apu also Alex Baines); see `vendor/`.
