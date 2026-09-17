# Milestones

## Milestone 1: Standalone repository (Phase 0 + 1)

**Outcome**: `andrew867/tinygb` exists with TinyGB's history, this documentation, and green CI that fetches the test ROMs and runs the unit tests and the Blargg/acid2 gate.

**Included**: extraction, top-level Makefile, CI, licences, `tg_palette` test, seam check.
**Excluded**: any change to emulator behaviour.
**Acceptance**: AC-CORE-001..005 in CI; the NanoApps fork untouched.
**Risks**: none technical; the GitHub repo creation needs the owner's go-ahead.

## Milestone 2: N31 built from here (Phase 2)

**Outcome**: the N31 binary in `artifacts/linux-n31/tinygb` comes from this repository, and the ipod tree's scripts know it.

**Included**: vendored shims, path rewrite, the two comment and return-code fixes, script re-pointing.
**Excluded**: any N31 feature work.
**Acceptance**: AC-N31-001..005; byte-identical acid2 capture.
**Risks**: shim drift from NanoApps (mitigated by the sync script and provenance file).

## Milestone 3: First picture on RetailOS (Phase 3)

**Outcome**: a cartridge from `/Apps/Data/TinyGB/roms` runs on stock firmware with the pad and volume buttons, silently, at full speed.

**Included**: freestanding pass, raw front end, input, library listing, forwarder in NanoApps, device measurements that settle OQ-004 and OQ-008.
**Excluded**: sound, menu pages, saves.
**Acceptance**: AC-ROS-001, -003, -006.
**Risks**: frame time over budget on the OS draw task; arena size at launch. Both are measured first.

## Milestone 4: Sound on RetailOS (Phase 4)

**Outcome**: the game has sound with no gaps or clicks, and pause/resume is clean.

**Included**: slot queue sink, pacing from queue depth, tone test, tuning.
**Excluded**: volume UI.
**Acceptance**: AC-AUD-001..004, AC-ROS-002.
**Risks**: the descriptor chain behaving differently at 66 ms slots than at Entrain's 1.28 s blocks; latency too high to feel right. Constants are isolated so tuning is cheap.

## Milestone 5: Playable release candidate (Phase 5)

**Outcome**: menu, saves, states, settings, wake lock; a player never needs a terminal.

**Acceptance**: every AC-ROS and AC-DATA; MAN-ROS-* and MAN-DATA-* all Pass.
**Risks**: `.hbapp` size creeping toward the ceiling with the font and menu (gated by the build).

## Milestone 6: v0.1.0 (Phase 6)

**Outcome**: tagged release; README with screenshots; NanoApps fork reduced to the forwarder; N31 release zip unchanged in shape.

**Acceptance**: `QA-CHECKLIST.md` fully ticked; ten-launch and one-hour soaks on both targets.
**Risks**: none new.
