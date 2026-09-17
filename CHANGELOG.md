# Changelog

## Unreleased

### Added

- Standalone repository: history of `apps/tinygb` extracted from the NanoApps fork (18 commits, 2026-09-03 to 2026-09-08); public on GitHub as `andrew867/tinygb`.
- `docs/`: architecture, specifications for the core, both front ends, audio, and persistence; implementation plan; milestones; test plans; QA checklist; risks; open questions; integration notes for the ipod tree and NanoApps.
- Root `Makefile`: the NanoApps app Makefile plus `test`, `gate`, `n31`, `check-seam`, `check-vendor`, `check-size`, `distclean`.
- CI: host tests, vendor hashes, seam check, the Blargg/acid2 gate, and the RetailOS `.hbapp` built inside a NanoApps tree.
- Host tests for the palettes and for the scaler keeping the palette's top byte.
- `vendor/LICENSES.md` with copyright lines, upstream URLs, and SHA-256 of each vendored file.
- `.gitattributes`: LF line endings, so a Windows clone does not break the shell scripts.

- `linux/shim/`: the launcher's DRM surface, console handoff, and touch-panel search, plus the build stamp, the LVGL mirror rule, and the base LVGL config, vendored from NanoApps with a `PROVENANCE` file; `tools/sync-n31-shims.sh` diffs or refreshes them.
- The RetailOS front end (Phase 3): a library screen under `/Apps/Data/TinyGB/roms`, cartridges loaded into a static 1 MiB window, the picture scaled into the top of the surface, the multitouch pad with a menu pill, Vol Up/Down as A/B, optional tilt, battery saves every two seconds. Silent until Phase 4.
- `tg_text` with two bitmap fonts rendered from DejaVu Sans Mono by `tools/gen-font.py`; `tg_surface`; `tg_util`; `tg_sys` with POSIX and RetailOS backends; `tg_roms_hb`, `tg_input_hb`, `tg_sys_hb`.
- Host tests for the text renderer, the pad's hit-testing and drawing, and the string helpers.
- In the NanoApps fork, `sdk/hb_app.mk` reads its target from `HB_ARCH_FLAGS` (default unchanged); TinyGB builds for the Cortex-A5 with VFPv4 the part actually is.

### Changed

- `tg_scaler_init` keeps the palette's top byte instead of masking it to zero; RetailOS's compositor wants 0xFF there. N31 passes 0x00RRGGBB and is unchanged.
- `tinygb.c` is now the smallest raw-surface app that links (it paints the surface and idles), so NanoApps' build of every app no longer fails on TinyGB. The RetailOS front end proper is Phases 3 to 5.
- The N31 build (`make n31`, or `host/Makefile.n31`) no longer reaches into a NanoApps tree: it compiles the vendored shims and finds the pinned LVGL checkout whether this repository is NanoApps' `apps/tinygb` subtree or a sibling checkout.
- The Cortex-A8 comments in `host/lv_conf_n31.h` and `platform/tg_scale.h` now say what the part is: Cortex-A5, VFPv4, no NEON.
- `tg_pad` draws into a `tg_surface` instead of the Linux framebuffer struct, and gained the menu pill (`TG_PAD_MENU`, above the joypad bits). `tg_save` reads and writes through `tg_sys` instead of stdio, with the check interval a build setting. `tg_rom_list` gained `skipped`.
- `Info.plist`: a gamepad glyph and the green accent the N31 menu uses, in place of the scaffolder's paintbrush and purple.

### Fixed

- The N31 front end's `main` returned 0 even when a cartridge named on the command line would not start; it now returns 2, as `play_once` reports.

### Removed
