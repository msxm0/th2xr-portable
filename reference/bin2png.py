#!/usr/bin/env python3
"""Convert a TH2REF frame dump to PNG.  Format: one text header line
'TH2REF <w> <h> 24\n' then w*h BGR triples, top row first."""
import subprocess, sys, pathlib

def load(path):
    raw = pathlib.Path(path).read_bytes()
    nl = raw.index(b"\n")
    tag, w, h, bpp = raw[:nl].split()
    assert tag == b"TH2REF", tag
    w, h = int(w), int(h)
    return w, h, raw[nl+1:nl+1+w*h*3]

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
