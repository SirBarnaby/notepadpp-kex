#!/usr/bin/env python3
"""kexrec2gif.py: turn a screen recording of the KEX demo (scene.kexrec) into a GIF for the README.

    python3 kexrec2gif.py IN.kexrec OUT.gif [--crop X,Y,W,H] [--hold-start MS] [--hold-end MS]
                                            [--trim-start MS] [--trim-end MS] [--width PX] [--colors N]

The recording is made by the self-test's demo mode (-pluginMessage="KexSuite_selftest=1;KexSuite_demo=all",
see addDemoSteps in SelfTest.cpp): the screen in a window's rectangle, a frame whenever something changed.
Here the frames are put on a 20 ms clock (the shortest delay browsers play as written), repeats are merged
into one longer frame, the first and last frames are held a little longer, and every frame gets the same
256-colour palette without dithering, like a screen recorder's GIF. gifsicle -O3 shrinks the result when it
is installed. Needs Pillow and numpy.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys

import numpy as np
from PIL import Image

TICK = 20  # ms


def recording_end(path):
    """The size and the time the recording ended (its last record)."""
    with open(path, "rb") as f:
        if f.read(8) != b"KEXREC1\n":
            sys.exit("%s is not a KEX recording" % path)
        w, h = struct.unpack("<ii", f.read(8))
        f.seek(-20, os.SEEK_END)
        ms, _, _, fw, fh = struct.unpack("<Iiiii", f.read(20))
        if fw or fh:
            sys.exit("%s ends before its last record (was the recording stopped?)" % path)
        return (w, h), ms


def records(path):
    """(ms, x, y, w, h, XOR pixels) for every change."""
    with open(path, "rb") as f:
        f.seek(16)
        while True:
            head = f.read(20)
            if len(head) < 20:
                return
            ms, x, y, fw, fh = struct.unpack("<Iiiii", head)
            if fw == 0 or fh == 0:
                return
            yield ms, x, y, fw, fh, np.frombuffer(f.read(fw * fh * 4), dtype=np.uint32).reshape(fh, fw)


def ticked(path, start, stop, crop):
    """The picture at every 20 ms tick from start to stop (cropped), as [array, ms shown] with repeats merged."""
    (w, h), _ = recording_end(path)
    x, y, cw, ch = crop
    cur = np.zeros((h, w), dtype=np.uint32)
    seq, pending = [], None
    it = records(path)
    for t in range(start, stop + 1, TICK):
        while True:
            if pending is None:
                pending = next(it, None)
            if pending is None or pending[0] > t:
                break
            _, rx, ry, rw, rh, data = pending
            cur[ry:ry + rh, rx:rx + rw] ^= data
            pending = None
        px = cur[y:y + ch, x:x + cw]
        if seq and np.array_equal(seq[-1][0], px):
            seq[-1][1] += TICK
        else:
            seq.append([px.copy(), TICK])
    return seq


def to_rgb(px):
    """0x00RRGGBB words to an RGB image array."""
    b = (px & 0xFF).astype(np.uint8)
    g = ((px >> 8) & 0xFF).astype(np.uint8)
    r = ((px >> 16) & 0xFF).astype(np.uint8)
    return np.dstack([r, g, b])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--crop", help="X,Y,W,H inside the recording")
    ap.add_argument("--hold-start", type=int, default=600, help="extra ms on the first frame")
    ap.add_argument("--hold-end", type=int, default=800, help="extra ms on the last frame")
    ap.add_argument("--trim-start", type=int, default=0, help="ms cut from the start")
    ap.add_argument("--trim-end", type=int, default=0, help="ms cut from the end")
    ap.add_argument("--width", type=int, default=0, help="scale down to this width")
    ap.add_argument("--colors", type=int, default=256)
    a = ap.parse_args()

    (w, h), end = recording_end(a.src)
    first = next(records(a.src), None)
    if first is None:
        sys.exit("no frames in %s" % a.src)
    crop = tuple(int(v) for v in a.crop.split(",")) if a.crop else (0, 0, w, h)
    seq = ticked(a.src, first[0] + a.trim_start, end - a.trim_end, crop)
    images = [Image.fromarray(to_rgb(px), "RGB") for px, _ in seq]
    delays = [d for _, d in seq]
    del seq
    delays[0] += a.hold_start
    delays[-1] += a.hold_end
    if a.width and a.width < cw:
        size = (a.width, round(ch * a.width / cw))
        images = [im.resize(size, Image.LANCZOS) for im in images]

    # one palette for every frame, from a sample of them side by side
    step = max(1, len(images) // 24)
    sample = images[::step] + [images[-1]]
    sw, sh = sample[0].size
    sheet = Image.new("RGB", (sw, sh * len(sample)))
    for i, im in enumerate(sample):
        sheet.paste(im, (0, i * sh))
    pal = sheet.quantize(colors=a.colors, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    out = [im.quantize(palette=pal, dither=Image.Dither.NONE) for im in images]

    tmp = a.out + ".tmp.gif"
    out[0].save(tmp, save_all=True, append_images=out[1:], duration=delays, loop=0, optimize=False, disposal=1)
    if shutil.which("gifsicle"):
        subprocess.run(["gifsicle", "-O3", "--no-comments", "--no-names", tmp, "-o", a.out], check=True)
        os.remove(tmp)
    else:
        os.replace(tmp, a.out)
    total = sum(delays)
    print("%s: %d frames, %.1f s, %dx%d, %d KB" % (a.out, len(out), total / 1000, out[0].size[0], out[0].size[1],
                                                os.path.getsize(a.out) // 1024))


if __name__ == "__main__":
    main()
