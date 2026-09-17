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

### Changed

- `tg_scaler_init` keeps the palette's top byte instead of masking it to zero; RetailOS's compositor wants 0xFF there. N31 passes 0x00RRGGBB and is unchanged.
- `tinygb.c` is now the smallest raw-surface app that links (it paints the surface and idles), so NanoApps' build of every app no longer fails on TinyGB. The RetailOS front end proper is Phases 3 to 5.
- The N31 build still expects the launcher sources at `../../n31launcher` until Phase 2 vendors them.

### Fixed

### Removed
