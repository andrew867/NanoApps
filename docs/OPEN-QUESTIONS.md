# Open Questions

Decisions taken by the owner on 2026-09-16 are marked Decided with the answer; the specs and plan have been updated to match.

| ID | Question | Area | Impact | Needed by | Status |
|---|---|---|---|---|---|
| OQ-001 | Repository name and visibility | Repo | Low | Milestone 1 | Decided: `andrew867/tinygb`, public, MIT. Created 2026-09-16. |
| OQ-002 | Vendor the launcher shims (`drmfb`, `fbcon`, `touch`, `build_stamp`, `lvgl-mirror.mk`) with a sync script, or read them from `$NANOAPPS` at build time? | N31 build | Medium | Phase 2 | Decided: vendor. The repo must build alone, and NanoApps integration is the first priority. |
| OQ-003 | Where does the ipod tree expect the standalone repo? | N31 integration | Low | Phase 2 | Decided: `$(dirname ROOT)/tinygb` like `NanoApps`, overridable with `TINYGB=`. |
| OQ-004 | ROM storage on RetailOS: static 1 MiB `.bss` window vs `hb_os_alloc` with a fixed DRAM mailbox | RetailOS memory | High | Phase 3 | Decided: the window, after logging `hb_os_heap_largest()` on the device first. |
| OQ-005 | Map Play/Pause to Start with the `hb_media_state` re-pause trick? | RetailOS input | Low | Phase 5 | Decided: unmapped. On-screen touch controls are the first priority. |
| OQ-006 | Which colour core? | Core | Low now | Deferred | Deferred. |
| OQ-007 | How to pin the vendored Peanut-GB and minigb_apu, which carry no version? | Vendor | Low | Phase 1 | Decided: SHA-256 of each file plus the upstream URL in `vendor/LICENSES.md`, checked by `make check-vendor`. Upstream commit unknown. |
| OQ-008 | RetailOS compiler flags | RetailOS build | Medium | Phase 3 | Decided: `-O2 -mcpu=cortex-a5 -mfpu=vfpv4`. Finding from Phase 1: `-O2` applies, but the arch flags are inert because `hb_app.mk` restates `-mcpu=cortex-a8 -mfpu=neon` in its link flags after `EXTRA_CFLAGS` and compiles and links in one invocation. Phase 3 adds an `HB_ARCH_FLAGS` variable to `hb_app.mk` in the NanoApps fork (default unchanged, so no other app moves) and offers it upstream. `-fjump-tables` is still to be measured. |
| OQ-009 | Ship the fps / queue-depth overlay? | RetailOS UI | Low | Phase 5 | Decided: behind a Settings toggle, off by default. |
| OQ-010 | Final audio constants (`TG_AUD_SLOT_FRAMES`, `TG_AUD_SLOTS`, `TG_AUD_TARGET_LEAD`), starting at 1470 / 6 / 2 | RetailOS audio | Medium | Phase 4 | Open: measured on the device in Phase 4. |
| OQ-011 | Use the voice completion callback instead of polling `+0x64`? | RetailOS audio | Low | Phase 4 | Open (default: poll first). |
| OQ-012 | Auto-load the pause state on relaunch, or ask? | Persistence | Low | Phase 5 | Decided: ask, via a Resume row. |
| OQ-013 | List `tinygb` in the NanoApps README? | Docs | Low | Phase 6 | Decided: list it with a link to this repository. |
| OQ-014 | Submit TinyGB to upstream `nfzerox/NanoApps`, or stay fork-only? | Community | Low | After v0.1 | Decided: submit it, so anyone who clones NanoApps can build it. Consequence: NanoApps carries this repository as `apps/tinygb` through `git subtree`, and this repository's root is a valid NanoApps app directory (`Makefile`, `Info.plist`, `tinygb.c` at the root). The forwarder idea is dropped. |
| OQ-015 | Menu on RetailOS: how far to go toward the nano's native list UI in a hand-drawn menu (momentum scrolling, edge-swipe back, pressed-row highlight)? | RetailOS UI | Medium | Phase 5 | Decided in principle: the menu must operate like the platform's own screens (REQ-ROS-032, REQ-ROS-035). Momentum scrolling is not required in v0.1; edge-swipe back, pressed highlight, chevrons, toggles, and theme colours are. |
