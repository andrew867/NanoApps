#!/usr/bin/env bash
# Refresh linux/shim/ from a NanoApps checkout, or say how far it has drifted.
#
# The N31 build borrows a few files from the launcher and the NanoApps tree -
# the DRM surface, the console handoff, the touch-panel search, the build
# stamp, the LVGL mirror rule, and the LVGL config the N31 config is based on.
# They are vendored so this repository builds on its own, and they are the
# launcher's to change: fbDOOM and TinyGB both draw through drmfb.c, and a fix
# there wants to reach both. So the copies are refreshed from NanoApps, never
# edited here, and PROVENANCE says which commit they came from.
#
#   tools/sync-n31-shims.sh            diff the copies against $NANOAPPS
#   tools/sync-n31-shims.sh --apply    copy, and rewrite PROVENANCE
#
# NANOAPPS defaults to ../NanoApps beside this repository, or ../.. when this
# repository is the apps/tinygb subtree inside one.
set -u

here="$(cd "$(dirname "$0")/.." && pwd)"
shim="$here/linux/shim"

if [[ -z "${NANOAPPS:-}" ]]; then
  if [[ -f "$here/../../sdk/hb_app.mk" ]]; then NANOAPPS="$here/../.."
  elif [[ -f "$here/../NanoApps/sdk/hb_app.mk" ]]; then NANOAPPS="$here/../NanoApps"
  else echo "no NanoApps tree found; set NANOAPPS" >&2; exit 2; fi
fi
NANOAPPS="$(cd "$NANOAPPS" && pwd)"

# copy-in-shim : source-relative-to-NanoApps
pairs=(
  "drmfb.c:apps/n31launcher/drmfb.c"
  "drmfb.h:apps/n31launcher/drmfb.h"
  "fbcon.c:apps/n31launcher/fbcon.c"
  "fbcon.h:apps/n31launcher/fbcon.h"
  "touch.c:apps/n31launcher/touch.c"
  "touch.h:apps/n31launcher/touch.h"
  "fbrefresh.h:apps/n31launcher/fbrefresh.h"
  "build_stamp.c:apps/build_stamp.c"
  "build_stamp.h:apps/build_stamp.h"
  "lvgl-mirror.mk:apps/lvgl-mirror.mk"
  "lv_conf_base.h:sdk/lv_conf.h"
)

apply=0
[[ "${1:-}" == "--apply" ]] && apply=1

drift=0
for p in "${pairs[@]}"; do
  dst="$shim/${p%%:*}"
  src="$NANOAPPS/${p#*:}"
  if [[ ! -f "$src" ]]; then echo "MISSING in NanoApps: ${p#*:}" >&2; drift=1; continue; fi
  # Line endings are not drift: this repository pins LF, and a NanoApps
  # checkout made on Windows can carry CRLF on the same bytes.
  if cmp -s <(tr -d '\r' < "$src") <(tr -d '\r' < "$dst"); then
    echo "  same     ${p%%:*}"
  else
    drift=1
    if (( apply )); then tr -d '\r' < "$src" > "$dst"; echo "  updated  ${p%%:*}  <- ${p#*:}"
    else echo "  DIFFERS  ${p%%:*}  <- ${p#*:}"; diff -u --strip-trailing-cr "$dst" "$src" | sed 's/^/           /' | head -n 40; fi
  fi
done

if (( apply )); then
  commit="$(git -C "$NANOAPPS" rev-parse --short=7 HEAD 2>/dev/null || echo unknown)"
  {
    echo "Vendored from NanoApps at commit $commit on $(date -u +%Y-%m-%d)."
    echo "Refresh with tools/sync-n31-shims.sh --apply; do not edit here."
    echo
    for p in "${pairs[@]}"; do printf '%-16s <- %s\n' "${p%%:*}" "${p#*:}"; done
  } > "$shim/PROVENANCE"
  echo "PROVENANCE rewritten ($commit)"
  exit 0
fi

(( drift )) && { echo "shims differ from $NANOAPPS; run with --apply to take theirs" >&2; exit 1; }
echo "shims match $NANOAPPS"
