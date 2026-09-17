# RetailOS Integration

How the RetailOS (NanoApps) build of TinyGB is produced, and how NanoApps carries it so that anyone who clones NanoApps can build and install it with the tooling NanoApps already has. Nothing in NanoApps' `start`, `build_apps.py`, `mkrelocapp.py`, or `hb_app.mk` needs to change for this to work.

## The model: this repository is the app directory

NanoApps discovers an app as `apps/<name>/` containing a `Makefile` (so `start` treats it as a target), `Info.plist` (identity, icon), and after a build `build/<name>.hbapp`. `sdk/hb_app.mk` finds the SDK from its own path, so it can be included from anywhere.

So the root of this repository is a NanoApps app directory: `Makefile`, `Info.plist`, `tinygb.c`, and the source tree beside them. NanoApps carries the whole repository as `apps/tinygb` with `git subtree`:

```sh
# in the NanoApps fork, once
git subtree add --prefix=apps/tinygb https://github.com/andrew867/tinygb main --squash

# after TinyGB moves on
git subtree pull --prefix=apps/tinygb https://github.com/andrew867/tinygb main --squash

# a fix made inside NanoApps/apps/tinygb, sent back here
git subtree push --prefix=apps/tinygb https://github.com/andrew867/tinygb main
```

`--squash` keeps NanoApps' history to one commit per pull rather than replaying TinyGB's. This repository stays the canonical source; NanoApps is a consumer. When TinyGB is submitted upstream to `nfzerox/NanoApps` the same subtree goes into the pull request, and upstream users get a buildable `apps/tinygb` with no second clone.

With that in place, from a NanoApps checkout:

```sh
./start build tinygb         # builds apps/tinygb/build/tinygb.hbapp
./start install tinygb       # builds, packs, installs, icon on the Home Screen
./start install              # everything, TinyGB included
```

The extra files a subtree brings along (`docs/`, `host/`, `linux/`, `vendor/`) are inert inside NanoApps; `apps/Makefile` only cares that `apps/tinygb/Makefile` exists and builds.

## The root `Makefile`

```make
NANOAPPS    ?= ../..                  # right when this is apps/tinygb; a standalone clone sets it
APP_NAME    := tinygb
SRCS        := tinygb.c ...           # core, portable platform, RetailOS platform, UI (Phase 3+)
RAW_SURFACE := 1
EXTRA_CFLAGS := -O2 -mcpu=cortex-a5 -mfpu=vfpv4 -DAUDIO_SAMPLE_RATE=22050 \
                -DMINIGB_APU_AUDIO_FORMAT_S16SYS=1 <vendor warning suppressions>
include $(NANOAPPS)/sdk/hb_app.mk     # guarded: a clone with no NanoApps gets a message, not a parse error
.DEFAULT_GOAL := all
```

Plus `test`, `gate`, `n31`, `check-seam`, `check-vendor`, `check-size`, `distclean`. `EXTRA_CFLAGS` must be complete before the include because `hb_app.mk` folds it into `CFLAGS` with `:=`.

Two facts about `hb_app.mk` worth knowing:

- It compiles and links every source in one `gcc` invocation, so there are no per-object flags; the vendor warning suppressions apply to everything.
- Its link flags restate `-mcpu=cortex-a8 -mthumb -mfpu=neon` after `EXTRA_CFLAGS`, so the arch override is inert today (OQ-008). `-O2` does apply.

Standalone, with NanoApps checked out beside this repository:

```sh
make NANOAPPS=../NanoApps            # build/tinygb.hbapp
make check-size NANOAPPS=../NanoApps
```

CI does exactly the subtree shape: it checks out `andrew867/NanoApps` and this repository into `NanoApps/apps/tinygb`, then runs `make all check-size` there.

## Data on the device

`/Apps/Data/TinyGB/roms/` holds cartridges. The app creates the folder on first launch and says so on screen when it is empty. TinyGB ships no cartridges; the owner copies theirs in disk mode. A `data/TinyGB/roms/README.txt` in NanoApps (one line) can make the folder appear through `./start install --demo-data`.

## Debugging

- `./start trace` prints the DRAM trace ring: TinyGB logs `INIT`, heap headroom, cartridge start, sink start/stop, and every refused file.
- Home triple-click takes a screenshot through the resident; `./start pull media` fetches it. This is how AC-ROS-001 is checked.
- A device that hangs on launch is almost always a `.hbapp` over the ceiling or an arena that did not fit: check `make check-size` and the last trace entries.

## Verification

`./start build tinygb` from a NanoApps checkout with the subtree produces `apps/tinygb/build/tinygb.hbapp`; `./start install tinygb` puts the icon on the Home Screen; MAN-SMOKE-001. The CI `retailos` job proves the build shape on every push.
