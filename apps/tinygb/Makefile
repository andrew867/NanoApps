# TinyGB — one Makefile, three targets.
#
# This file is, first, a NanoApps app Makefile: the RetailOS build, made the
# way NanoApps makes every app (APP_NAME, SRCS, RAW_SURFACE, then the SDK's
# hb_app.mk). NanoApps carries this repository as apps/tinygb - a git subtree -
# so a plain `make` here, or `./start install tinygb` there, builds the .hbapp
# with no other checkout involved. NANOAPPS points at the tree; ../.. is right
# when this is apps/tinygb inside it, and a standalone clone sets it.
#
# Second, it drives the other two builds, which have makefiles of their own:
#
#   make test            host unit tests            (host/Makefile.host)
#   make gate            Blargg + dmg-acid2 gate    (fetches the ROMs first)
#   make n31             the N31 Linux binary       (host/Makefile.n31)
#   make check-seam      no Peanut-GB symbol outside core/tg_peanut.o
#   make check-vendor    vendor/ still matches vendor/LICENSES.md
#   make check-size      the .hbapp is under the ceiling the resident truncates at
#
# `clean` belongs to hb_app.mk (the RetailOS build directory); `distclean`
# takes the host and N31 trees with it.

NANOAPPS ?= ../..

# ---- the RetailOS app -------------------------------------------------------

APP_NAME    := tinygb
SRCS        := tinygb.c
RAW_SURFACE := 1

# hb_app.mk folds EXTRA_CFLAGS into CFLAGS with := at include time, so it has
# to be complete here, not appended later.
#
# -O2 over the SDK's -Os: an emulator's inner loop is where the time goes and
# an in-order core pays for every branch -Os keeps. A later -O wins, so this
# one does apply to every source in the link, SDK included.
#
# The arch flags are the part's (Cortex-A5, VFPv4, no NEON - read off the
# device under N31; the SDK's neon is a claim the silicon does not back), and
# today they do NOT take effect: hb_app.mk compiles and links in one gcc
# invocation and restates -mcpu=cortex-a8 -mfpu=neon in RELOC_LDFLAGS after
# EXTRA_CFLAGS, and the last -m flag wins. They are here so the intent is in
# one place; they start working the day hb_app.mk reads its arch flags from a
# variable (docs/OPEN-QUESTIONS.md OQ-008). Until then the build is the SDK's
# cortex-a8/neon, which every other NanoApps app runs on.
EXTRA_CFLAGS := -O2 -mcpu=cortex-a5 -mfpu=vfpv4 \
                -DAUDIO_SAMPLE_RATE=22050 -DMINIGB_APU_AUDIO_FORMAT_S16SYS=1 \
                -Wno-sign-compare -Wno-implicit-fallthrough \
                -Wno-unused-but-set-variable -Wno-type-limits

HB_APP_MK := $(NANOAPPS)/sdk/hb_app.mk

ifneq ($(wildcard $(HB_APP_MK)),)
include $(HB_APP_MK)
else
.PHONY: all clean
all:
	@echo "no NanoApps tree at $(NANOAPPS) (expected $(HB_APP_MK))." >&2
	@echo "The RetailOS build needs one: clone https://github.com/nfzerox/NanoApps" >&2
	@echo "beside this repository, or pass NANOAPPS=/path/to/NanoApps." >&2
	@exit 1
clean:
	@rm -rf build
endif

# Stated, because the targets below would otherwise be candidates for the
# default and a bare `make` from NanoApps' apps/Makefile has to build the app.
.DEFAULT_GOAL := all

# The resident reads the .hbapp into a 576 KiB slot and truncates without a
# word past it; a truncated blob has garbage relocations and hangs or reboots
# the device in a way that looks like any other bug. 512 KiB leaves headroom.
HBAPP_LIMIT := 524288

.PHONY: check-size
check-size: build/$(APP_NAME).hbapp
	@sz=$$(wc -c < $<); \
	 echo "  $< $$sz bytes (limit $(HBAPP_LIMIT))"; \
	 if [ "$$sz" -gt "$(HBAPP_LIMIT)" ]; then echo "  TOO BIG for the resident's slot" >&2; exit 1; fi

# ---- host: tests and the gate ---------------------------------------------

.PHONY: host test gate n31 check-seam check-vendor distclean

host:
	@$(MAKE) -s -C host -f Makefile.host

test:
	@$(MAKE) -s -C host -f Makefile.host test

gate:
	@$(MAKE) -s -C host -f Makefile.host gate

# Every front end reaches the emulator through core/tg_core.h and nothing
# else. `nm -u` lists what an object needs from outside itself; a Peanut-GB
# or minigb_apu symbol wanted by anything but tg_peanut.o is a front end
# reaching past the seam.
check-seam: host
	@bad=$$(for o in host/build/*.o; do \
	          case "$$o" in */tg_peanut.o|*/minigb_apu.o) continue;; esac; \
	          nm -u "$$o" | grep -E ' (gb_|audio_read|audio_write|audio_init)' | sed "s|^|$$o: |"; \
	        done); \
	 if [ -n "$$bad" ]; then echo "$$bad" >&2; echo "  a front end reaches past tg_core.h" >&2; exit 1; \
	 else echo "  seam ok: only tg_peanut.o names Peanut-GB"; fi

# vendor/ is unmodified upstream, and the proof is the hash in LICENSES.md.
check-vendor:
	@fail=0; \
	 for f in vendor/peanut_gb.h vendor/minigb_apu.c vendor/minigb_apu.h; do \
	   want=$$(grep "\`$$(basename $$f)\`" vendor/LICENSES.md | grep -o '[0-9a-f]\{64\}'); \
	   have=$$(sha256sum "$$f" | cut -c1-64); \
	   if [ "$$want" = "$$have" ]; then echo "  $$f ok"; else echo "  $$f CHANGED ($$have)" >&2; fail=1; fi; \
	 done; exit $$fail

# ---- N31 ------------------------------------------------------------------

n31:
	@$(MAKE) -C host -f Makefile.n31

distclean:
	@rm -rf build host/build host/build-n31
