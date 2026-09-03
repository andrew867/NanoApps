#!/usr/bin/env bash
# The Phase 00 gate: is the core actually emulating a Game Boy?
#
# Two questions, and they fail in different ways, which is why both are here.
# Blargg's suites exercise every opcode and every flag and report in words - a
# CPU bug shows up as a named failing test. dmg-acid2 draws one frame that
# depends on nearly every PPU behaviour at once, and a bug shows up as a face
# with the wrong parts missing. Passing one and failing the other is common and
# informative.
#
# Blargg's ROMs need a while: cpu_instrs runs eleven sub-suites in sequence and
# reports as each finishes, so it is thousands of frames rather than tens.
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
roms="$here/../testroms"
bin="$here/../host/build/tg_headless"
out="$here/../host/build"

[[ -x "$bin" ]] || { echo "build it first: make -f Makefile.host" >&2; exit 2; }

pass=0 fail=0 skip=0

run_serial() {
  local name="$1"
  local frames="$2"
  local rom="$roms/$name"

  if [[ ! -s "$rom" ]]; then
    echo "SKIP  $name  (not fetched)"
    skip=$((skip + 1))
    return
  fi

  local log
  log="$("$bin" "$rom" -f "$frames" -s -q 2>&1)"
  local rc=$?

  # The exit status is the verdict. It used to be status AND a grep for
  # "Passed" in the output - but the run is made with -q, which is exactly
  # the mode that does not print the link-port log, so the grep could never
  # match and every suite failed no matter what it did.
  if [[ $rc -eq 0 ]]; then
    echo "PASS  $name"
    pass=$((pass + 1))
  else
    echo "FAIL  $name"
    sed 's/^/        /' <<<"$log"
    fail=$((fail + 1))
  fi
}

run_frame() {
  local name="$1"
  local frames="$2"
  local rom="$roms/$name"

  if [[ ! -s "$rom" ]]; then
    echo "SKIP  $name  (not fetched)"
    skip=$((skip + 1))
    return
  fi

  local ppm="$out/${name%.gb}.ppm"
  local line
  line="$("$bin" "$rom" -f "$frames" -o "$ppm" -q 2>&1)"

  # There is no pass/fail signal in the picture itself - that is the point of
  # an acid test, it is read by eye. What can be automated is that it rendered
  # something, and that the hash is stable from run to run, so a change to the
  # core that alters the picture is visible in a diff.
  echo "DREW  $name  ->  ${ppm##*/}"
  sed 's/^/        /' <<<"$line"
  pass=$((pass + 1))
}

# acid2 is one frame, compared pixel for pixel against the reference. The
# picture is written out too, because when this fails the picture is what says
# which part of the PPU did it - a missing eye and a missing window are
# different bugs and the pixel count alone cannot tell them apart.
run_acid2() {
  local rom="$roms/dmg-acid2.gb"
  local ref="$roms/reference-dmg.png"

  if [[ ! -s "$rom" || ! -s "$ref" ]]; then
    echo "SKIP  dmg-acid2  (ROM or reference not fetched)"
    skip=$((skip + 1))
    return
  fi

  "$bin" "$rom" -f 60 -r "$out/acid2.raw" -o "$out/dmg-acid2.ppm" -q >/dev/null

  if python3 "$here/acid2check.py" "$out/acid2.raw" "$ref"; then
    echo "PASS  dmg-acid2.gb"
    pass=$((pass + 1))
  else
    echo "FAIL  dmg-acid2.gb   (frame written to build/dmg-acid2.ppm)"
    fail=$((fail + 1))
  fi
}

echo "=== Blargg, over the link port ==="
run_serial cpu_instrs.gb   8000
run_serial instr_timing.gb 2000

echo
echo "=== dmg-acid2, against the reference ==="
run_acid2

echo
echo "pass $pass  fail $fail  skip $skip"
[[ $fail -eq 0 ]]
