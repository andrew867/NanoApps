# Risks and Assumptions

## Risk: The RetailOS tick cannot afford an emulated frame

**Category:** Technical
**Likelihood:** Medium
**Impact:** High
**Description:** `hb_raw_frame` runs on the OS draw task, after which the OS copies the whole 240 x 432 buffer. On N31 the same core takes a measured fraction of a 16.74 ms frame at `-O2`, but RetailOS compiles at `-Os` with `-fno-jump-tables` by default, and the OS is doing its own work on the same CPU. If one emulated frame plus scaling exceeds about 8 ms the audio queue starves and the game slows.
**Mitigation:** Phase 3 measures first with `hb_time_uptime_us` around the core and the scaler, at `-Os` and `-O2`, with and without jump tables and with `vfpv4` in place of `neon`. Peanut-GB's `frame_skip` is the fallback: skip PPU rendering on alternate frames when the queue is falling.
**Detection:** the overlay's per-tick ms and queue depth; MAN-ROS-023.

## Risk: The app arena does not fit at launch and the device reboots

**Category:** Technical
**Likelihood:** Medium
**Impact:** High (reboot, and the player blames the app)
**Description:** The resident allocates code + data + `.bss` in one block with the OS allocator that panics on failure. A 1 MiB ROM window plus 64 KiB of machine plus code is about 1.2 MB, above the ~1 MB the resident describes as typical, and free heap depends on what the OS was doing.
**Mitigation:** log `hb_os_heap_largest()` and `hb_os_heap_free()` at init on the first device build and read them back before the window size is fixed (REQ-ROS-052). Fallback is `hb_os_alloc` with the block recorded in a fixed mailbox word so a relaunch reuses it (OQ-004).
**Detection:** MAN-ROS-024; a reboot on launch after a NanoApps update.

## Risk: The audio chain behaves differently at 66 ms slots than at Entrain's 1.28 s blocks

**Category:** Technical
**Likelihood:** Medium
**Impact:** Medium (clicks or gaps)
**Description:** Entrain proved chaining, in-place silence, and the duration trap with long blocks. Short slots mean the completion handler and our reclaim run twenty times as often, and the wall-clock play cursor estimate has less slack relative to the 250 ms safety margin.
**Mitigation:** the tone test (AC-AUD-001) before the APU; constants isolated; the completion callback that harness T3b proved works is the escalation path (OQ-011).
**Detection:** underrun and zero-duration-join counters on the overlay; MAN-AUD-001..003.

## Risk: Audio latency is too high to feel right

**Category:** Product
**Likelihood:** Medium
**Impact:** Medium
**Description:** With two slots of lead plus the sounding remainder, latency is 130 to 200 ms. A jump sound that late is noticeable.
**Mitigation:** shorten slots after the chain is proven stable (the constraint is that a slot must outlast tick jitter); target one slot of lead if underruns stay at zero.
**Detection:** MAN-AUD-004 against the N31 build.

## Risk: `.hbapp` grows past the silent-truncation ceiling

**Category:** Technical
**Likelihood:** Low
**Impact:** High (hang or reboot that looks like a random bug)
**Description:** The resident reads the blob into a 576 KiB slot without checking. The N31 binary is 928 KB but includes LVGL, alsa-lib, and libdrm; the RetailOS blob is core + APU + scaler + menu + font + the SDK, estimated 150 to 250 KB at `-O2`.
**Mitigation:** the build prints the size and fails over 512 KiB (REQ-ROS-072).
**Detection:** AUTO-BUILD-003.

## Risk: Play/Pause cannot be a game button

**Category:** Product
**Likelihood:** High (already observed by Entrain)
**Impact:** Low
**Description:** RetailOS acts on the key first and starts the Music player under the app. The original plan mapped it to Start.
**Mitigation:** Start and Select live on the touch pad; Play/Pause is unmapped by default with a media re-pause poll. Revisit in OQ-005.
**Detection:** MAN-ROS-013.

## Risk: Saves lost because there is no exit callback on RetailOS

**Category:** Data
**Likelihood:** Medium
**Impact:** High for the player
**Description:** Home tears the app down without notice; a save made in the last check interval is lost.
**Mitigation:** 2 s check interval, flush on menu open, a state written on pause; the cost is a small CRC over up to 128 KiB every 2 s.
**Detection:** MAN-DATA-001 with a 3 s hold.

## Risk: The launcher shims drift from NanoApps

**Category:** Operational
**Likelihood:** Medium
**Impact:** Low to Medium
**Description:** `drmfb.c`, `fbcon.c`, `touch.c` are actively developed in the NanoApps fork for the launcher and fbDOOM; TinyGB's vendored copies can fall behind a DRM fix.
**Mitigation:** `tools/sync-n31-shims.sh` with a diff, a `PROVENANCE` file naming the commit, and a line in the QA checklist.
**Detection:** the sync script's diff before every N31 tag.

## Risk: Two front ends and two file layers diverge in behaviour

**Category:** Technical
**Likelihood:** Medium
**Impact:** Medium
**Description:** `tg_save.c`/`tg_roms.c` (POSIX) and `tg_fs_hb.c` implement the same headers; refusal rules or path derivation could differ.
**Mitigation:** shared portable pieces (path derivation, CRC, settings parser) in one file each; the fake-`hb_fs` host test runs the same cases against both.
**Detection:** AUTO-ROS-002 and AUTO-DATA-002 share a case table.

## Risk: Peanut-GB accuracy limits

**Category:** Product
**Likelihood:** Low for the listed cartridges
**Impact:** Low
**Description:** Some cartridges (MBC7, HuC1/3, TAMA5, camera) are unsupported upstream; some timing-sensitive titles glitch on any Peanut-GB build.
**Mitigation:** the probe refuses unsupported mappers with a message; the README says which cartridges were tested.
**Detection:** player reports; the About page shows the core name.

## Risk: The freestanding pass turns up a libc call the audit missed

**Category:** Technical
**Likelihood:** Low
**Impact:** Low (link fails loudly at `mkrelocapp.py`)
**Description:** GCC can emit `memmove` or `__aeabi_*` helpers on its own.
**Mitigation:** AUTO-BUILD-002 compiles the RetailOS objects in CI; the shim `string.h` provides `memmove` inline; libgcc is linked.
**Detection:** the build.

## Assumptions that may be wrong

- The Cortex-A5 finding from N31's `/proc/cpuinfo` applies to RetailOS builds too; if the SDK's NEON flag matters somewhere in the SDK's own code, compiling everything with `vfpv4` will show it (OQ-008).
- The touch panel keeps working under RetailOS with three simultaneous fingers (the SDK reserves 8 slots; Paint uses one).
- `hb_fs_read` of 1 MiB in one call is acceptable on the tick (it blocks; the library screen is the right place for it, not mid-game).
- `hb_media_state()` reliably reports Music starting under the app (Entrain relies on this).
- The mixer's output rate is 22050 or 44100; the duration exactness test handles both.
- Nobody needs a cartridge over 1 MiB on RetailOS in v0.1.
