#!/usr/bin/env python3
"""kex-icon.py: the Notepad++ KEX app icon, made from the KEX logo.

    python3 kex-icon.py LOGO.png OUT.ico [PREVIEW.png]

LOGO is the 256 px logo (patch/kex-logo.png). Every icon size is that drawing scaled down, with a
little sharpening below 64 px so the outlines stay clear. A size can have a drawing of its own:
LOGO-16.png next to LOGO (any size from SIZES) is used as it is for that size. OUT has 32-bit
images at 16-256 px: bitmaps up to 128 px, PNG at 256 px, as in Notepad++'s own file. Needs Pillow.
"""
import io
import os
import struct
import sys

from PIL import Image, ImageFilter

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def icon_image(logo, logo_path, size):
    """The logo at this size: its own drawing if there is one, else the big one scaled down."""
    own = "%s-%d.png" % (os.path.splitext(logo_path)[0], size)
    if os.path.exists(own):
        im = Image.open(own).convert("RGBA")
        if im.size != (size, size):
            sys.exit("%s is %dx%d, not %dx%d" % (own, im.size[0], im.size[1], size, size))
        return im
    if logo.size == (size, size):
        return logo
    # scaled with premultiplied alpha, so the transparent edge doesn't darken the outline
    im = logo.convert("RGBa").resize((size, size), Image.LANCZOS)
    if size < 64:
        im = im.filter(ImageFilter.UnsharpMask(radius=0.6, percent=60, threshold=0))
    return im.convert("RGBA")


def bmp_entry(im):
    """A 32-bit icon bitmap: BITMAPINFOHEADER, bottom-up BGRA rows, then the 1-bit AND mask."""
    w, h = im.size
    px = im.load()
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = bytearray()
    for y in range(h - 1, -1, -1):
        for x in range(w):
            r, g, b, a = px[x, y]
            xor += bytes((b, g, r, a))
    stride = ((w + 31) // 32) * 4
    mask = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray(stride)
        for x in range(w):
            if px[x, y][3] < 128:
                row[x // 8] |= 0x80 >> (x % 8)
        mask += row
    return header + bytes(xor) + bytes(mask)


def png_entry(im):
    buf = io.BytesIO()
    im.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def write_ico(images, path):
    entries = [(im.size[0], png_entry(im) if im.size[0] >= 256 else bmp_entry(im)) for im in images]
    out = bytearray(struct.pack("<HHH", 0, 1, len(entries)))
    offset = 6 + 16 * len(entries)
    for size, data in entries:
        dim = 0 if size >= 256 else size
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in entries:
        out += data
    with open(path, "wb") as f:
        f.write(out)


def preview(images, path):
    """Each size, actual size and enlarged 4x, on a light and a dark background."""
    pad = 12
    w = sum(s for s in SIZES) + pad * (len(SIZES) + 1)
    big = [im.resize((im.size[0] * 4, im.size[1] * 4), Image.NEAREST) for im in images if im.size[0] <= 32]
    w2 = sum(b.size[0] for b in big) + pad * (len(big) + 1)
    W = max(w, w2)
    H = (256 + pad * 2) + (128 + pad * 2)
    sheet = Image.new("RGBA", (W, H * 2), (0, 0, 0, 0))
    for row, bg in enumerate([(243, 243, 243, 255), (32, 32, 32, 255)]):
        y0 = row * H
        sheet.paste(Image.new("RGBA", (W, H), bg), (0, y0))
        x = pad
        for im in images:
            sheet.alpha_composite(im, (x, y0 + pad + 256 - im.size[1]))
            x += im.size[0] + pad
        x = pad
        for b in big:
            sheet.alpha_composite(b, (x, y0 + 256 + pad * 3))
            x += b.size[0] + pad
    sheet.save(path)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    logo = Image.open(sys.argv[1]).convert("RGBA")
    if logo.size[0] < 256 or logo.size[0] != logo.size[1]:
        sys.exit("the logo must be square and at least 256 px, not %dx%d" % logo.size)
    if logo.size[0] > 256:
        logo = logo.convert("RGBa").resize((256, 256), Image.LANCZOS).convert("RGBA")
    images = [icon_image(logo, sys.argv[1], size) for size in SIZES]
    write_ico(images, sys.argv[2])
    if len(sys.argv) > 3:
        preview(images, sys.argv[3])


if __name__ == "__main__":
    main()
