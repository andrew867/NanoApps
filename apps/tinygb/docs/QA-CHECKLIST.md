# QA Checklist (ship / no-ship for a tag)

## Build

- [ ] Fresh clone on Linux: `make test` and `make gate` pass with no warnings in project code
- [ ] `make n31`: `check-built` passes (FP arch, soft-float ABI, static); binary staged to `artifacts/linux-n31/tinygb`; build stamp moved
- [ ] `make retailos`: `.hbapp` size printed and under 512 KiB; no undefined symbols reported by `mkrelocapp.py`
- [ ] `make check-seam` passes (no Peanut symbol outside `tg_peanut.o`)
- [ ] `grep -rn NanoApps` finds only provenance and `NANOAPPS ?=`
- [ ] CI green on the tag commit

## Static

- [ ] `-Wall -Wextra` clean outside `vendor/`
- [ ] No `snprintf`, `malloc`, `qsort`, `<math.h>` in files the RetailOS target compiles
- [ ] `vendor/` unchanged from upstream (diff against the recorded hashes in `vendor/LICENSES.md`)

## Automated

- [ ] AUTO-CORE-001..009 pass
- [ ] AUTO-UI-*, AUTO-ROS-*, AUTO-AUD-*, AUTO-DATA-* pass
- [ ] AUTO-GATE-001..003 pass
- [ ] AUTO-INT-001 CRC unchanged, or changed deliberately with a changelog line

## Manual (regression set)

- [ ] MAN-SMOKE-001..004
- [ ] MAN-ROS-001 and MAN-N31-001 byte-identical
- [ ] MAN-ROS-010 chord
- [ ] MAN-AUD-002, MAN-AUD-003
- [ ] MAN-DATA-001, MAN-DATA-004
- [ ] MAN-ROS-022 leak, MAN-ROS-023 and MAN-N31-030 soaks

## Data loss review

- [ ] Every path that writes a `.sav` or `.st0` checks the result and never truncates a good file on failure
- [ ] Save cadence on RetailOS is 2 s and flushes on menu open
- [ ] A wrong-size `.sav` is refused, never padded or overwritten

## Device safety

- [ ] `.hbapp` under the ceiling with headroom (a truncated blob reboots the device)
- [ ] Init logs heap headroom; the ROM window decision in `OPEN-QUESTIONS.md` OQ-004 reflects the last measurement
- [ ] No `hb_os_alloc` other than the audio slots; no fixed DRAM address claimed that is not listed in `SPEC-retailos-frontend.md`

## Diagnostics

- [ ] About row shows the build stamp and commit on both targets
- [ ] Overlay counters (fps, queue depth, underruns, restarts) work behind the Settings toggle
- [ ] N31 exit report prints the per-frame breakdown

## Documentation

- [ ] `README.md` status and document map current
- [ ] `CHANGELOG.md` has the tag's section
- [ ] `OPEN-QUESTIONS.md` has no question marked Open that blocks the tag
- [ ] `N31-INTEGRATION.md` and `RETAILOS-INTEGRATION.md` match the scripts in the ipod tree and the forwarder in the NanoApps fork

## Known issues triage

- [ ] Every known defect is either fixed, in `RISKS-AND-ASSUMPTIONS.md` with a mitigation, or in `OPEN-QUESTIONS.md`
