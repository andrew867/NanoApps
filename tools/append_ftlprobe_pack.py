#!/usr/bin/env python3
"""Append Ftlprobe into AllApps.pack; reuse screen section from an existing bundle."""
from __future__ import annotations
import struct, sys, tempfile, os
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from mkapp import (  # type: ignore
    build_pack, make_label_db, BUNDLE_MAGIC, VERSION, HDR_FMT, HDR_SIZE,
    SEC_FMT, SEC_SIZE, SEC, _align4,
)

PACK_MAGIC = 0x4B504248
HBSCREEN = 229474306
HBLAYOUT = 229474305
HBAP = 0x48426170
HBSD = 0x48425344


def unpack_pack(data: bytes):
    magic, ver, n = struct.unpack_from("<III", data, 0)
    assert magic == PACK_MAGIC, hex(magic)
    offs = [struct.unpack_from("<I", data, 12 + 4 * i)[0] for i in range(n)]
    bundles = []
    for i, off in enumerate(offs):
        blen = struct.unpack_from("<I", data, off + 44)[0]
        bundles.append(data[off:off + blen])
    return bundles


def bundle_name(b: bytes) -> str:
    name_off = struct.unpack_from("<I", b, 40)[0]
    return b[name_off:].split(b"\0", 1)[0].decode("ascii", "replace")


def extract_screen(b: bytes) -> bytes | None:
    # section_count at +48
    scount = struct.unpack_from("<I", b, 48)[0]
    for i in range(scount):
        kind, off, ln = struct.unpack_from(SEC_FMT, b, HDR_SIZE + i * SEC_SIZE)
        if kind == SEC["screen"]:
            return b[off:off + ln]
    return None


def make_bundle(app_id: int, kind: int, sbid: str, name: str, screen: bytes) -> bytes:
    sections = [
        (SEC["label"], make_label_db(app_id, name)),
        (SEC["screen"], screen),
    ]
    sbid_b = sbid.encode("ascii") + b"\0"
    name_b = name.encode("ascii") + b"\0"
    sec_table_off = HDR_SIZE
    sbid_off = sec_table_off + SEC_SIZE * len(sections)
    name_off = sbid_off + len(sbid_b)
    cur = _align4(name_off + len(name_b))
    descs, blob = [], bytearray()
    for ks, payload in sections:
        descs.append((ks, cur, len(payload)))
        blob += payload
        pad = _align4(len(payload)) - len(payload)
        blob += b"\0" * pad
        cur += len(payload) + pad
    out = bytearray(cur)
    struct.pack_into(
        HDR_FMT, out, 0,
        BUNDLE_MAGIC, VERSION, kind, app_id, app_id,
        HBSCREEN, HBLAYOUT, 0,
        HBAP, HBSD, sbid_off, name_off, cur,
        len(sections),
    )
    for i, (ks, off, ln) in enumerate(descs):
        struct.pack_into(SEC_FMT, out, sec_table_off + i * SEC_SIZE, ks, off, ln)
    out[sbid_off:sbid_off + len(sbid_b)] = sbid_b
    out[name_off:name_off + len(name_b)] = name_b
    out[_align4(name_off + len(name_b)):] = blob
    return bytes(out)


def main():
    pack_in = Path(sys.argv[1])
    pack_out = Path(sys.argv[2])
    name = "Ftlprobe"
    sbid = "hb.ftlprobe"
    app_id = 0x0DAD91A1  # unique-ish in homebrew id range

    bundles = unpack_pack(pack_in.read_bytes())
    screen = None
    for b in bundles:
        screen = extract_screen(b)
        if screen:
            break
    if not screen:
        sys.exit("no screen section found in existing pack")

    kept = [b for b in bundles if bundle_name(b).lower() != name.lower()]
    kept.append(make_bundle(app_id, 1, sbid, name, screen))

    td = tempfile.mkdtemp(prefix="hbpack_")
    paths = []
    for i, b in enumerate(kept):
        p = os.path.join(td, f"{i}.app")
        open(p, "wb").write(b)
        paths.append(p)
    build_pack(str(pack_out), paths)
    print(f"ok: {len(kept)} apps -> {pack_out}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: append_ftlprobe_pack.py AllApps.pack out.pack")
    main()
