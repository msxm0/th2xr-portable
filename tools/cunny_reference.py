#!/usr/bin/env python3
"""A numpy CuNNy, computing what the shader computes.

    tools/cunny_reference.py MODEL.bin IN.png OUT.png [WIDTHxHEIGHT]

MODEL.bin is a file written by tools/cunny_convert.py.  Each layer stores
into an 8-bit texture, so every activation is rounded to n/255 after the
ReLU (the clamp to 1 comes with the format); taps outside the picture clamp
to its edge; the result is added to the bilinear 2x picture (luma models in
Rec.601 YCbCr, the matrices from the Magpie effect) and saturated.  With a
size, the 2x picture then goes through src/gl_lanczos.cpp's filter
(lanczos_axis below), 8-bit between its two passes as there.  Used to
check th2-cunny-check's output against.
"""
import struct
import sys
from pathlib import Path

import numpy as np

RY = np.array([[0.299, 0.587, 0.114], [-0.169, -0.331, 0.5], [0.5, -0.419, -0.081]])
YR = np.array([[1, -0.00093, 1.401687], [1, -0.3437, -0.71417], [1, 1.77216, 0.00099]])


def load(path):
    data = Path(path).read_bytes()
    if data[:4] != b"CNNY":
        raise SystemExit(f"{path}: not a CuNNy weight file")
    version, rgb, n = struct.unpack_from("<III", data, 4)
    if version != 1:
        raise SystemExit(f"{path}: version {version}")
    off, convs = 16, []
    for _ in range(n):
        out, inp = struct.unpack_from("<II", data, off)
        off += 8
        w = np.frombuffer(data, "<f4", out * inp * 9, off).reshape(out, inp, 3, 3)
        off += out * inp * 9 * 4
        b = np.frombuffer(data, "<f4", out, off)
        off += out * 4
        convs.append((w.astype(np.float64), b.astype(np.float64)))
    if off != len(data):
        raise SystemExit(f"{path}: {len(data) - off} trailing bytes")
    return bool(rgb), convs


def conv3x3(x, w, b):
    """x (C, H, W) -> (O, H, W), clamp-to-edge taps."""
    c, h, wd = x.shape
    p = np.pad(x, ((0, 0), (1, 1), (1, 1)), mode="edge")
    out = np.zeros((w.shape[0], h, wd))
    for ky in range(3):
        for kx in range(3):
            tap = p[:, ky:ky + h, kx:kx + wd].reshape(c, -1)
            out += (w[:, :, ky, kx] @ tap).reshape(-1, h, wd)
    return out + b[:, None, None]


def store8(x):
    return np.round(np.clip(x, 0.0, 1.0) * 255.0) / 255.0


def bilinear2x(img):
    """(H, W, 3) -> (2H, 2W, 3), sampled at output texel centres, clamped."""
    h, w, _ = img.shape
    def axis(n):
        pos = (np.arange(2 * n) + 0.5) / 2 - 0.5
        lo = np.floor(pos).astype(int)
        f = pos - lo
        return np.clip(lo, 0, n - 1), np.clip(lo + 1, 0, n - 1), f
    y0, y1, fy = axis(h)
    x0, x1, fx = axis(w)
    top = img[y0][:, x0] * (1 - fx)[None, :, None] + img[y0][:, x1] * fx[None, :, None]
    bot = img[y1][:, x0] * (1 - fx)[None, :, None] + img[y1][:, x1] * fx[None, :, None]
    return top * (1 - fy)[:, None, None] + bot * fy[:, None, None]


def upscale(rgb_model, convs, img):
    """img (H, W, 3) floats in 0..1 -> (2H, 2W, 3)."""
    if rgb_model:
        x = img.transpose(2, 0, 1)
    else:
        x = (img @ np.array([0.299, 0.587, 0.114]))[None]
    for w, b in convs[:-1]:
        x = store8(np.maximum(conv3x3(x, w, b), 0.0))
    r = conv3x3(x, *convs[-1])                     # (4*ch, H, W)
    ch = r.shape[0] // 4
    h, wd = img.shape[:2]
    res = r.reshape(ch, 2, 2, h, wd).transpose(3, 1, 4, 2, 0).reshape(2 * h, 2 * wd, ch)
    base = bilinear2x(img)
    if rgb_model:
        return np.clip(base + res, 0.0, 1.0)
    yuv = base @ RY.T
    yuv[..., 0] = np.clip(yuv[..., 0] + res[..., 0], 0.0, 1.0)
    return yuv @ YR.T


def lanczos2(x):
    x = np.abs(x)
    k = np.pi * x
    with np.errstate(divide="ignore", invalid="ignore"):
        w = np.sin(k) * np.sin(0.5 * k) / (x * x)
    w = np.where(x < 1e-5, 1.0, w)
    return np.where(x >= 2.0, 0.0, w)


def lanczos_axis(img, out, axis):
    """gl_lanczos.cpp's pass along `axis` (1 = x, 0 = y), (H, W, C) 0..1."""
    n = img.shape[axis]
    ratio = n / out
    stretch = max(1.0, ratio)
    taps = min(16, 2 * int(np.ceil(np.float32(2.0 * stretch))))
    centre = (np.arange(out) + 0.5) * ratio - 0.5
    first = np.floor(centre).astype(int) - taps // 2 + 1
    idx = first[:, None] + np.arange(taps)[None, :]
    w = lanczos2((idx - centre[:, None]) / stretch)
    src = np.moveaxis(img, axis, 0)                      # (n, ...)
    lin = src.copy()
    lin[..., :3] = lin[..., :3] ** 2
    g = lin[np.clip(idx, 0, n - 1)]                      # (out, taps, ...)
    wsh = w.reshape(w.shape + (1,) * (g.ndim - 2))
    v = (g * wsh).sum(axis=1) / w.sum(axis=1).reshape((-1,) + (1,) * (g.ndim - 2))
    lo = g[..., :3].min(axis=1)
    hi = g[..., :3].max(axis=1)
    v[..., :3] = np.sqrt(np.clip(v[..., :3], lo, hi))
    return np.moveaxis(v, 0, axis)


def resample(img, width, height):
    mid = lanczos_axis(img, width, 1)
    mid = np.round(np.clip(mid, 0, 1) * 255) / 255     # the 8-bit intermediate
    return lanczos_axis(mid, height, 0)


def main():
    from PIL import Image
    if len(sys.argv) not in (4, 5):
        raise SystemExit(__doc__)
    rgb, convs = load(sys.argv[1])
    img = np.asarray(Image.open(sys.argv[2]).convert("RGB"), np.float64) / 255.0
    out = upscale(rgb, convs, img)
    if len(sys.argv) == 5:
        width, height = (int(v) for v in sys.argv[4].split("x"))
        out = resample(np.round(np.clip(out, 0, 1) * 255) / 255, width, height)
    Image.fromarray(np.round(np.clip(out, 0, 1) * 255).astype(np.uint8)).save(sys.argv[3])


if __name__ == "__main__":
    main()
