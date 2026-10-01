#!/usr/bin/env python3
"""Convert a trace frame dump to PNG, and the loader other scripts should use.

    bin2png.py FRAME OUT.png

Two formats, both written by the reference's th2ref_dump_frame and by the
port's write_trace_frame:

  f*.bin   'TH2REF <w> <h> 24\\n' then w*h BGR triples, top row first.
  f*.binz  'TH2REFZ <w> <h> 24 <key> <n>\\n' then zlib data.  key 1 is a whole
           frame; key 0 is a DELTA - each byte is (frame - previous) mod 256
           against the frame one tick earlier, which is only written when
           that tick was dumped too.  A keyframe comes after any gap and every
           TH2REF_KEY_PERIOD ticks.

Reading a delta as if it were a picture gives plausible-looking nonsense -
black where nothing changed, near-white where a little did - which once
passed for "dense dumps change what the engine draws".  load() follows the
chain back to its keyframe; use it rather than decoding by hand.
"""
import pathlib
import re
import subprocess
import sys
import zlib


def _read(path):
    raw = pathlib.Path(path).read_bytes()
    nl = raw.index(b"\n")
    fields = raw[:nl].split()
    tag = fields[0]
    w, h = int(fields[1]), int(fields[2])
    if tag == b"TH2REF":
        return w, h, True, raw[nl + 1:nl + 1 + w * h * 3]
    if tag == b"TH2REFZ":
        key = int(fields[4]) != 0
        return w, h, key, zlib.decompress(raw[nl + 1:])
    raise ValueError(f"{path}: unknown frame tag {tag!r}")


def load(path):
    """(w, h, bgr bytes) of the frame at `path`, deltas resolved."""
    path = pathlib.Path(path)
    w, h, key, data = _read(path)
    if key:
        return w, h, data
    m = re.fullmatch(r"f(\d+)\.binz", path.name)
    if not m:
        raise ValueError(f"{path}: delta frame with no tick in its name")
    tick = int(m.group(1))
    previous = path.with_name(f"f{tick - 1:0{len(m.group(1))}d}.binz")
    pw, ph, base = load(previous)
    if (pw, ph) != (w, h):
        raise ValueError(f"{path}: delta against a {pw}x{ph} frame")
    return w, h, bytes((a + b) & 0xFF for a, b in zip(base, data))


def to_png(src, dst):
    w, h, bgr = load(src)
    rgb = bytearray(len(bgr))
    rgb[0::3] = bgr[2::3]
    rgb[1::3] = bgr[1::3]
    rgb[2::3] = bgr[0::3]
    subprocess.run(["magick", "-size", f"{w}x{h}", "-depth", "8", "rgb:-", str(dst)],
                   input=bytes(rgb), check=True)
    return w, h


if __name__ == "__main__":
    w, h = to_png(sys.argv[1], sys.argv[2])
    print(f"{sys.argv[1]} -> {sys.argv[2]}  {w}x{h}")
