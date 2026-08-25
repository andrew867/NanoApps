#!/usr/bin/env python3
"""Append one new surface app into an existing AllApps.pack without rebuilding
every icon. Copies Executables/Icons for the new app onto the device tree.

Usage:
  append_app_to_pack.py <AllApps.pack> <out.pack> \\
      --name Ftlprobe --sbid hb.ftlprobe --id 0x0dad9abc --kind 1
"""
from __future__ import annotations
import argparse, struct, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from mkapp import build_pack, make_label_db, BUNDLE_MAGIC, VERSION, HDR_FMT, HDR_SIZE, SEC_FMT, SEC_SIZE, SEC, _align4  # type: ignore

PACK_MAGIC = 0x4B504248  # HBPK
HBSCREEN = 229474306
HBLAYOUT = 229474305
HBAP = 0x48426170
HBSD = 0x48425344


def unpack_pack(data: bytes) -> list[bytes]:
    magic, ver, n = struct.unpack_from("<III", data, 0)
    if magic != PACK_MAGIC:
        raise SystemExit(f"bad pack magic {magic:#x}")
    offs = [struct.unpack_from("<I", data, 12 + 4 * i)[0] for i in range(n)]
    bundles = []
    for i, off in enumerate(offs):
        end = offs[i + 1] if i + 1 < n else len(data)
        # trim padding zeros at end of each slot by reading bundle_len from header
        blen = struct.unpack_from("<I", data, off + 44)[0]  # bundle_len field
        bundles.append(data[off:off + blen])
    return bundles


def make_bundle(app_id: int, kind: int, sbid: str, name: str, label_id: int) -> bytes:
    sections = []
    sections.append((SEC["label"], make_label_db(label_id, name)))
    # screen section omitted — resident uses screen_id/layout_id from header
    sbid_b = sbid.encode("ascii") + b"\0"
    name_b = name.encode("ascii") + b"\0"
    sec_table_off = HDR_SIZE
    sbid_off = sec_table_off + SEC_SIZE * len(sections)
    name_off = sbid_off + len(sbid_b)
    cur = _align4(name_off + len(name_b))
    descs, data = [], bytearray()
    for kind_s, payload in sections:
        descs.append((kind_s, cur, len(payload)))
        data += payload
        pad = _align4(len(payload)) - len(payload)
        data += b"\0" * pad
        cur += len(payload) + pad
    out = bytearray(cur)
    struct.pack_into(
        HDR_FMT, out, 0,
        BUNDLE_MAGIC, VERSION, kind, app_id, label_id,
        HBSCREEN, HBLAYOUT, 0,
        HBAP, HBSD, sbid_off, name_off, cur,
        len(sections),
    )
    for i, (ks, off, ln) in enumerate(descs):
        struct.pack_into(SEC_FMT, out, sec_table_off + i * SEC_SIZE, ks, off, ln)
    out[sbid_off:sbid_off + len(sbid_b)] = sbid_b
    out[name_off:name_off + len(name_b)] = name_b
    out[_align4(name_off + len(name_b)):] = data
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pack_in")
    ap.add_argument("pack_out")
    ap.add_argument("--name", required=True)
    ap.add_argument("--sbid", required=True)
    ap.add_argument("--id", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--kind", type=int, default=1)
    args = ap.parse_args()

    data = Path(args.pack_in).read_bytes()
    bundles = unpack_pack(data)
    # drop existing same name if re-running
    kept = []
    for b in bundles:
        # name_off at header+40
        name_off = struct.unpack_from("<I", b, 40)[0]
        nm = b[name_off:].split(b"\0", 1)[0].decode("ascii", "replace")
        if nm.lower() != args.name.lower():
            kept.append(b)
    kept.append(make_bundle(args.id, args.kind, args.sbid, args.name, args.id))
    # write via mkapp.build_pack helper — needs temp files
    import tempfile, os
    td = tempfile.mkdtemp(prefix="hbpack_")
    paths = []
    for i, b in enumerate(kept):
        p = os.path.join(td, f"{i}.app")
        open(p, "wb").write(b)
        paths.append(p)
    build_pack(args.pack_out, paths)
    print(f"apps in pack: {len(kept)}")


if __name__ == "__main__":
    main()
