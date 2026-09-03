#!/usr/bin/env bash
# Fetch the Game Boy test ROMs the Phase 00 gate runs against.
#
# None of these are committed. They are freely redistributable homebrew written
# to test emulators, but they are still someone else's work with their own
# licences, and a repository that ships ROMs is a repository people are wary of.
# So: fetched on demand, into a gitignored directory.
#
#   dmg-acid2      Matt Currie's PPU test. One frame, either pixel-correct or
#                  not, and it fails loudly and visually when it is not.
#   cpu_instrs     Blargg's instruction suite. Reports over the link port, so
#                  it can be checked without looking at anything.
#   instr_timing   Blargg again, for cycle counts rather than results.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
dst="$here/../testroms"
mkdir -p "$dst"

fetch() {
  local name="$1" url="$2"
  if [[ -s "$dst/$name" ]]; then
    echo "  have  $name"
    return 0
  fi
  echo "  get   $name"
  if ! curl -fsSL --retry 2 -o "$dst/$name.part" "$url"; then
    echo "  FAIL  $name  ($url)" >&2
    rm -f "$dst/$name.part"
    return 1
  fi
  mv "$dst/$name.part" "$dst/$name"
}

echo "test ROMs -> $dst"

missing=0

# dmg-acid2, from the author's release page.
fetch dmg-acid2.gb \
  "https://github.com/mattcurrie/dmg-acid2/releases/download/v1.0/dmg-acid2.gb" \
  || missing=$((missing + 1))

# The reference frame acid2 is judged against. Not a ROM, but it belongs with
# them: without it the PPU check is someone squinting at a smiley face.
fetch reference-dmg.png \
  "https://raw.githubusercontent.com/mattcurrie/dmg-acid2/master/img/reference-dmg.png" \
  || missing=$((missing + 1))

# Blargg's suites, from the widely mirrored gb-test-roms collection.
blargg="https://raw.githubusercontent.com/retrio/gb-test-roms/master"
fetch cpu_instrs.gb   "$blargg/cpu_instrs/cpu_instrs.gb"   || missing=$((missing + 1))
fetch instr_timing.gb "$blargg/instr_timing/instr_timing.gb" || missing=$((missing + 1))

if [[ "$missing" -gt 0 ]]; then
  echo
  echo "$missing ROM(s) could not be fetched. The gate will skip what is absent;" >&2
  echo "drop the files into $dst by hand to run those checks." >&2
fi

ls -la "$dst" 2>/dev/null | tail -n +2 || true
