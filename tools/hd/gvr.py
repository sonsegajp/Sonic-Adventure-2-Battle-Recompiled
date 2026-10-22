# GameCube Ninja textures (GVR, inside GVM archives and PRS-compressed files) to RGBA images.
# A GVR is 'GVRT', a little-endian chunk size, then at +0x0B the data format, +0x0C width and +0x0E
# height (big-endian) and the texel data from +0x10, in the GameCube's tiled layouts:
#   CMPR (0x0E)   8x8 tiles of four 4x4 DXT1-like blocks (two RGB565 colours, 2-bit indices)
#   RGB5A3 (0x05) 4x4 tiles of 16-bit texels: RGB555 when the top bit is set, else ARGB3444
#   I8 (0x01), IA8 (0x03), RGB565 (0x04), RGBA8 (0x06) also decoded; paletted ones are skipped.
import struct

import numpy as np

FORMATS = {0: 'I4', 1: 'I8', 2: 'IA4', 3: 'IA8', 4: 'RGB565', 5: 'RGB5A3', 6: 'RGBA8', 8: 'C4', 9: 'C8', 14: 'CMPR'}


def _rgb565(v):
    v = np.asarray(v).astype(np.int32)
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.int32)


def _untile(tiles, th, tw, bh, bw, h, w):
    """(th*tw, bh, bw, C) blocks in tile order -> (h, w, C) image (cropped)."""
    c = tiles.shape[-1]
    img = tiles.reshape(th, tw, bh, bw, c).transpose(0, 2, 1, 3, 4).reshape(th * bh, tw * bw, c)
    return np.ascontiguousarray(img[:h, :w])


def decode_cmpr(data, w, h):
    th, tw = (h + 7) // 8, (w + 7) // 8
    n = th * tw * 4
    raw = np.frombuffer(data, np.uint8, n * 8).reshape(n, 8)
    c0 = (raw[:, 0].astype(np.int32) << 8) | raw[:, 1]
    c1 = (raw[:, 2].astype(np.int32) << 8) | raw[:, 3]
    idx = (raw[:, 4].astype(np.uint32) << 24) | (raw[:, 5].astype(np.uint32) << 16) | (raw[:, 6].astype(np.uint32) << 8) | raw[:, 7]
    p0, p1 = _rgb565(c0), _rgb565(c1)
    four = (c0 > c1)[:, None]
    pal = np.zeros((n, 4, 4), np.int32)
    pal[:, 0, :3], pal[:, 1, :3] = p0, p1
    pal[:, 2, :3] = np.where(four, (2 * p0 + p1) // 3, (p0 + p1) // 2)
    pal[:, 3, :3] = np.where(four, (p0 + 2 * p1) // 3, 0)
    pal[:, :3, 3] = 255
    pal[:, 3, 3] = np.where(four[:, 0], 255, 0)
    shifts = 30 - 2 * np.arange(16, dtype=np.uint32)
    k = ((idx[:, None] >> shifts) & 3).astype(np.int64)  # (n, 16)
    px = np.take_along_axis(pal, k[..., None].repeat(4, -1), axis=1).reshape(n, 4, 4, 4)
    # a tile's four blocks are 2x2: (sub & 1) across, (sub >> 1) down
    tiles = px.reshape(th * tw, 2, 2, 4, 4, 4).transpose(0, 1, 3, 2, 4, 5).reshape(th * tw, 8, 8, 4)
    return _untile(tiles, th, tw, 8, 8, h, w).astype(np.uint8)


def decode_rgb5a3(data, w, h):
    th, tw = (h + 3) // 4, (w + 3) // 4
    v = np.frombuffer(data, '>u2', th * tw * 16).astype(np.int32).reshape(th * tw, 4, 4)
    opaque = (v & 0x8000) != 0
    r5, g5, b5 = (v >> 10) & 31, (v >> 5) & 31, v & 31
    a3, r4, g4, b4 = (v >> 12) & 7, (v >> 8) & 15, (v >> 4) & 15, v & 15
    out = np.stack([np.where(opaque, (r5 << 3) | (r5 >> 2), r4 * 17), np.where(opaque, (g5 << 3) | (g5 >> 2), g4 * 17),
                    np.where(opaque, (b5 << 3) | (b5 >> 2), b4 * 17), np.where(opaque, 255, (a3 << 5) | (a3 << 2) | (a3 >> 1))], -1)
    return _untile(out, th, tw, 4, 4, h, w).astype(np.uint8)


def decode_simple(fmt, data, w, h):
    bw, bh = (8, 4) if fmt == 1 else (4, 4)
    th, tw = (h + bh - 1) // bh, (w + bw - 1) // bw
    n = th * tw * bw * bh
    if fmt == 1:
        i = np.frombuffer(data, np.uint8, n).astype(np.int32)
        px = np.stack([i, i, i, np.full_like(i, 255)], -1)
    elif fmt == 3:
        ai = np.frombuffer(data, np.uint8, n * 2).astype(np.int32).reshape(n, 2)
        px = np.stack([ai[:, 1], ai[:, 1], ai[:, 1], ai[:, 0]], -1)
    else:
        px = np.concatenate([_rgb565(np.frombuffer(data, '>u2', n)), np.full((n, 1), 255, np.int32)], -1)
    return _untile(px.reshape(th * tw, bh, bw, 4), th, tw, bh, bw, h, w).astype(np.uint8)


def decode_rgba8(data, w, h):
    th, tw = (h + 3) // 4, (w + 3) // 4
    raw = np.frombuffer(data, np.uint8, th * tw * 64).reshape(th * tw, 2, 16, 2)
    ar, gb = raw[:, 0], raw[:, 1]
    px = np.stack([ar[..., 1], gb[..., 0], gb[..., 1], ar[..., 0]], -1).reshape(th * tw, 4, 4, 4)
    return _untile(px, th, tw, 4, 4, h, w)


def textures(blob):
    """Yields (offset, format name, width, height, RGBA array or None) for every GVR in a blob."""
    i = blob.find(b'GVRT')
    while i >= 0 and i + 16 <= len(blob):
        fmt = blob[i + 11]
        w, h = struct.unpack('>HH', blob[i + 12:i + 16])
        if fmt in FORMATS and 0 < w <= 1024 and 0 < h <= 1024:
            data = blob[i + 16:]
            img = None
            try:
                if fmt == 14: img = decode_cmpr(data, w, h)
                elif fmt == 5: img = decode_rgb5a3(data, w, h)
                elif fmt == 6: img = decode_rgba8(data, w, h)
                elif fmt in (1, 3, 4): img = decode_simple(fmt, data, w, h)
            except (struct.error, IndexError, ValueError):
                img = None
            yield i, FORMATS[fmt], w, h, img
        i = blob.find(b'GVRT', i + 4)
