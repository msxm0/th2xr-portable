#!/usr/bin/env python3
"""Extract CuNNy NVL weights from its Magpie shaders into the port's files.

    tools/cunny_convert.py CUNNY_CHECKOUT OUT_DIR [MODEL...]

CuNNy (https://github.com/funnyplanter/CuNNy, LGPL-3.0; the generated
shaders say GPL-3.0-or-later) is a small CNN upscaler; its NVL models are
trained on visual novel screenshots and CG.  The weights come from the
Magpie effects in magpie/normal/CuNNy-<model>.hlsl, read as text:

- No pickle.  pretrained/*.pickle would need an unpickler, and a pickle can
  run code while it loads.
- For 4x16 and 4x32 the shaders hold the same weights as pretrained/ (checked
  to the shaders' four significant digits, 2026-10-13, CuNNy 906031b).  For
  the luma models - fast and below - pretrained/ holds a different training
  run; the shader is the network CuNNy distributes, so that is the one used.

Each HLSL pass is a 3x3 convolution.  Taps are loaded into variables,
    s<v>_<a>_<b> = L<i>(x, y);                      input block i, tap (x, y)
and the variables are reused - a pass reading three input blocks declares
s0 and s1 and loads s0 again for the third - so the statements are read in
order, each one resolved through the load that last set its variable:
    r<o> += mul(s.., M4(16 numbers));     rows = 4 inputs, cols = 4 outputs
    r<o> += mul(s.., M3x4(12 numbers));   the rgb input layer
    r<o> += V4(4 numbers) * s..;          the luma input layer
    r<o> += V4(4 numbers);                bias

Output, one file per model, little-endian:
    "CNNY" u32 version=1 u32 rgb u32 nconv
    per conv: u32 out u32 in
              f32[out*in*3*3] weight, PyTorch order (out, in, ky, kx)
              f32[out] bias (zero where the shader adds none)
Luma models take Rec.601 Y and end in 4 outputs, the 2x2 pixel shuffle of
it; rgb models take R, G, B and end in 12, channel c*4 + (dy*2 + dx).
"""
import re
import struct
import sys
from pathlib import Path

import numpy as np

DEFAULT_MODELS = ["fast-NVL", "4x16-NVL", "4x32-NVL"]

NUM = r"(-?\d\.\d+e[-+]\d+)"
LIST = r"((?:-?\d\.\d+e[-+]\d+(?:, )?)+)"
PASS = re.compile(r"^//!PASS (\d+)\n//!DESC (\S+) \((\d+)x(\d+)\)$", re.M)
STATEMENT = re.compile(
    r"(?P<load>(?P<lv>s\d+_\d_\d) = L(?P<li>\d+)\((?P<lx>-?\d\.0), (?P<ly>-?\d\.0)\))"
    r"|(?P<mul>r(?P<mo>\d+) \+= mul\((?P<mv>s\d+_\d_\d), M(?P<mk>4|3x4)\((?P<mw>" + LIST[1:-1] + r")\)\))"
    r"|(?P<luma>r(?P<lo>\d+) \+= V4\((?P<lw>" + LIST[1:-1] + r")\) \* (?P<lsv>s\d+_\d_\d))"
    r"|(?P<bias>r(?P<bo>\d+) \+= V4\((?P<bw>" + LIST[1:-1] + r")\);)")


def numbers(text):
    return [float(v) for v in text.split(", ")]


def parse_pass(body, cin, cout):
    weight = np.zeros((cout, cin, 3, 3), np.float64)
    bias = np.zeros(cout, np.float64)
    filled = np.zeros(weight.shape, int)
    taps = {}
    for m in STATEMENT.finditer(body):
        if m.group("load"):
            taps[m.group("lv")] = (int(m.group("li")),
                                   int(float(m.group("ly"))) + 1,
                                   int(float(m.group("lx"))) + 1)
        elif m.group("mul"):
            o = int(m.group("mo"))
            i, ky, kx = taps[m.group("mv")]
            rows = 4 if m.group("mk") == "4" else 3
            block = np.array(numbers(m.group("mw"))).reshape(rows, 4)
            if rows == 3 and i != 0:
                raise SystemExit("M3x4 outside the rgb input layer")
            weight[o*4:o*4+4, i*4:i*4+rows, ky, kx] = block.T
            filled[o*4:o*4+4, i*4:i*4+rows, ky, kx] += 1
        elif m.group("luma"):
            o = int(m.group("lo"))
            i, ky, kx = taps[m.group("lsv")]
            if i != 0 or cin != 1:
                raise SystemExit("scalar tap outside the luma input layer")
            weight[o*4:o*4+4, 0, ky, kx] = numbers(m.group("lw"))
            filled[o*4:o*4+4, 0, ky, kx] += 1
        else:
            o = int(m.group("bo"))
            bias[o*4:o*4+4] = numbers(m.group("bw"))
    if (filled != 1).any():
        raise SystemExit(f"{np.count_nonzero(filled == 0)} weights unset, "
                         f"{np.count_nonzero(filled > 1)} set twice")
    return weight, bias


def parse(text):
    if "//!MAGPIE EFFECT" not in text:
        raise SystemExit("not a Magpie effect")
    marks = list(PASS.finditer(text))
    if not marks:
        raise SystemExit("no passes")
    rgb = "V3(O(INPUT, x, y).rgb)" in text
    luma = "dot(float3(0.299, 0.587, 0.114), O(INPUT, x, y).rgb)" in text
    if rgb == luma:
        raise SystemExit("cannot tell an rgb model from a luma one")
    convs = []
    for n, mark in enumerate(marks):
        end = marks[n + 1].start() if n + 1 < len(marks) else len(text)
        name, cin, cout = mark.group(2), int(mark.group(3)), int(mark.group(4))
        if n == 0 and (name != "in" or cin != (3 if rgb else 1)):
            raise SystemExit(f"first pass is {name} ({cin}x{cout})")
        if n > 0 and cin != convs[-1][0].shape[0]:
            raise SystemExit(f"pass {name} takes {cin}, previous gives "
                             f"{convs[-1][0].shape[0]}")
        last = n + 1 == len(marks)
        if last and (name != "out-shuffle" or cout != (12 if rgb else 4)):
            raise SystemExit(f"last pass is {name} ({cin}x{cout})")
        convs.append(parse_pass(text[mark.end():end], cin, cout))
    return rgb, convs


def write(rgb, convs, out):
    buf = bytearray(b"CNNY")
    buf += struct.pack("<III", 1, 1 if rgb else 0, len(convs))
    for weight, bias in convs:
        buf += struct.pack("<II", weight.shape[0], weight.shape[1])
        buf += weight.astype("<f4").tobytes()
        buf += bias.astype("<f4").tobytes()
    out.write_bytes(bytes(buf))


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    checkout = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    for name in sys.argv[3:] or DEFAULT_MODELS:
        if not name.endswith("-NVL"):
            raise SystemExit(f"{name}: only the NVL (visual novel) models")
        source = checkout / "magpie" / "normal" / f"CuNNy-{name}.hlsl"
        rgb, convs = parse(source.read_text())
        out = out_dir / f"cunny-{name.lower()}.bin"
        write(rgb, convs, out)
        shape = [w.shape[0] for w, _ in convs]
        print(f"{out}  {'rgb' if rgb else 'luma'}  {shape}")


if __name__ == "__main__":
    main()
