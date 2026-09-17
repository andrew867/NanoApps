# Third-party code under vendor/

Both files are vendored unmodified. Every adaptation TinyGB needs lives outside this directory: `core/tg_peanut.c` supplies the callbacks and the save-state serialiser, and the build sets `AUDIO_SAMPLE_RATE`, `MINIGB_APU_AUDIO_FORMAT_S16SYS`, `ENABLE_SOUND` and `ENABLE_LCD` from the makefiles. Warnings in these files are suppressed per object (`VENDOR_CFLAGS`) rather than fixed here.

Neither upstream file carries a version number. The identity of the vendored copies is their content hash; `make check-vendor` recomputes it.

| File | Licence | Copyright | Upstream | SHA-256 |
|---|---|---|---|---|
| `peanut_gb.h` | MIT | 2018-2023 Mahyar Koshkouei; parts from SameBoy, MIT, 2015-2019 Lior Halphon | https://github.com/deltabeard/Peanut-GB | `44d737081fd00d2be912ae193700c5501b40d25e789fa636ec9d841b403aa2c8` |
| `minigb_apu.c` | MIT | 2019 Mahyar Koshkouei; 2017 Alex Baines (MiniGBS) | https://github.com/deltabeard/minigb_apu | `cd173eb5979f1210de6eff6b75e76445db09643494f1cf471df43c5f223ec1f2` |
| `minigb_apu.h` | MIT (the header refers to a LICENSE file that is not vendored; the `.c` states MIT in full) | as above | as above | `60d3c838f55653804d66f51d27a33cf14518498a68d1e0506625c67ca061fad1` |

Hashes are of the files as committed (LF line endings; `.gitattributes` normalises them), which is also what a Linux checkout sees. A Windows checkout made before `.gitattributes` existed hashed differently, which is how the first recorded values were wrong.

The MIT text for both is the same as this repository's `LICENSE`, with the copyright lines above.

Upstream limits that TinyGB inherits (from the comments at the end of `peanut_gb.h`): MMM01 and MBC6 untested; MBC7, Pocket Camera, Bandai TAMA5, HuC3 and HuC1 unsupported; no Game Boy Color.
