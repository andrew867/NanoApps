# N31 Launcher

A home screen for the iPod nano 7G running Linux. Three buttons down the side of
the device, no touchscreen, so the whole design falls out of that: **PLAY goes
in, HOME comes back, VOL moves.**

```
HOME screen          fixed, three tiles, one per button — no scrolling
  VOL +              Radio+
  PLAY               Extra apps  ──▶
  VOL −              TinyPod

EXTRA APPS           whatever is staged on the internal volume
  VOL ± / PLAY / HOME    scroll / open / back

while an app runs
  HOME               SIGTERM, so it saves; SIGKILL only after 4 s
```

The home screen deliberately does not scroll. A fixed screen can be driven by
feel without looking at it, which is the point of a launcher you reach for in a
pocket. The list lives one screen deeper, where scrolling costs nothing because
you are already looking at it.

## Linux only, for now

This is a normal Linux ELF: fbdev through LVGL, evdev for the keys, `fork`/`exec`
for the apps. It is not an `.hbapp` and does not build against the NanoApps SDK.

It lives here anyway because it is the same UI work as the rest of the tree and
shares the palette with Radio+ — and because the shape of it (a table of apps,
a screen generated from that table) is what a RetailOS home-screen replacement
would want too. The app table and the screens are separated with that in mind;
what is Linux-specific is confined to `launcher.c` and `fbcon.c`.

## What the apps are

Radio+ and TinyPod are compiled in as a floor, so the launcher has something to
show with no storage at all. Everything else is discovered at runtime under
`/mnt/*/n31os/apps/<app>/`, rescanned every couple of seconds — the internal
volume is not always mounted when the launcher starts, so an app that appears
later simply appears.

Each app may carry an `app.json` beside its binary:

```json
{ "name": "Radio+", "exec": "radioplus", "tagline": "FM, RDS, recording",
  "glyph": "FM", "color": "#22d3ee", "screen": "framebuffer" }
```

Every key is optional — an app with no manifest still lists, named after its
folder. `screen` is `framebuffer` (default) or `console`: a terminal program
needs the framebuffer console handed back and the screen cleared, or it draws
into a terminal nobody is displaying. Manifests are authored in the ipod tree
(`tools/linux-n31/n31os-apps/`) and staged onto the volume from there.

These are **not** the NanoApps `Info.plist` files, which describe `.hbapp`
payloads for RetailOS. One sitting in the same folder is ignored rather than
half-read.

Nothing here starts an app directly: that is `/bin/n31-autostart`, which
resolves a name against the same locations and adds the per-app setup the table
cannot know about — the wad path for fbdoom, a writable home for Radio+, since
the volume is mounted read-only.

## The console

The framebuffer console and the launcher draw to the same pixels. Turning
console logging off would be wrong — it is how a bad boot gets diagnosed — so it
is only *detached*, and only once the launcher can draw. Boot logs to it as
before, and exiting the launcher reattaches it, so HOME out and you have a
readable console again.

## Building

```bash
cd apps/n31launcher/host
make -f Makefile.preview shots   # render every screen to PNG, no device needed
make -f Makefile.n31             # the ARM binary
make -f Makefile.n31 push        # to /tmp on a device at 192.168.7.2
```

The ARM build reuses the LVGL static library from the Entrain app rather than
building its own copy of the same configuration:

```bash
make -f Makefile.n31 LVLIB=/path/to/liblvgl_n31.a
```

`Makefile.preview` renders all four screens headlessly. It is worth using: every
layout bug in this app so far — labels wrapping onto the row below, a scroll
indicator painted over by the rows, a modal clipping its own last line — was
found in a picture and was invisible in the source.

## Boot

`mk-initramfs.sh` installs it to `/bin/n31launcher` and defaults
`N31_AUTOSTART` to it. To go back:

```bash
N31_AUTOSTART=none tools/linux-n31/mk-initramfs.sh   # bake it off
```

or `n31.autostart=none` in bootargs for a single boot, or
`touch /tmp/n31-no-autostart` from a shell before it runs.
