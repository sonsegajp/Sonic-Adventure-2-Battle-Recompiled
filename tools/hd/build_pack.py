# Builds the HD texture pack from the user's disc: every GVR texture on it, upscaled (4x HDCube 4
# Compact by default, HDCube's alpha model for transparency), written where and as the game's
# renderer (Aurora) looks replacements up: tex1_{w}x{h}_{XXH64 of the base level's texel bytes}_{GX
# format}.png - the bytes the game hands the GPU are the GVR's own, so the names are known without
# running the game. Paletted textures (their key needs the palette) are left out.
#
# Runs from the launcher (HD TEXTURES) or by hand; resumes where it stopped (finished textures are
# skipped); <out>/pause makes it wait; <out>/status.txt says how far it is:
#   "<running|paused|finished|failed> <done> <total> <seconds left> <note>"
#   python build_pack.py --iso <disc image> --out <pack folder> [--limit N] [--only name,name]
import argparse
import os
import struct
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..'))
import numpy as np
import xxhash
from PIL import Image

import gvr
import prs

MODELS = os.path.join(HERE, 'models', 'Models', 'HDcube')
SKIP_EXT = ('.adx', '.afs', '.sfd', '.m1v', '.dsp', '.str', '.thp', '.rel', '.dol', '.bin')
# texel blocks per GX format: (block width, block height, bits per texel)
BLOCKS = {0: (8, 8, 4), 1: (8, 4, 8), 2: (8, 4, 8), 3: (4, 4, 16), 4: (4, 4, 16), 5: (4, 4, 16), 6: (4, 4, 32), 14: (8, 8, 4)}


class Disc:
    """A GameCube disc image's files: (path, offset, size) from its FST."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        h = self.read(0, 0x440)
        if h[0x1C:0x20] != b'\xc2\x33\x9f\x3d':
            raise SystemExit('not a GameCube disc image')
        self.game = h[:6].decode('ascii', 'replace')
        fst_off, fst_size = struct.unpack('>II', h[0x424:0x42C])
        self.fst = self.read(fst_off, fst_size)

    def read(self, off, size):
        self.f.seek(off)
        return self.f.read(size)

    def files(self):
        fst = self.fst
        count = struct.unpack('>I', fst[8:12])[0]
        names = count * 12

        def name(i):
            off = struct.unpack('>I', fst[i * 12:i * 12 + 4])[0] & 0xFFFFFF
            end = fst.index(b'\0', names + off)
            return fst[names + off:end].decode('latin-1')

        out = []

        def walk(lo, hi, prefix):
            i = lo
            while i < hi:
                kind = fst[i * 12]
                a, b = struct.unpack('>II', fst[i * 12 + 4:i * 12 + 12])
                if kind:
                    walk(i + 1, b, prefix + name(i) + '/')
                    i = b
                else:
                    out.append((prefix + name(i), a, b))
                    i += 1

        walk(1, count, '')
        return out


def base_size(fmt, w, h):
    bw, bh, bpp = BLOCKS[fmt]
    return ((w + bw - 1) // bw * bw) * ((h + bh - 1) // bh * bh) * bpp // 8


def decode(fmt, data, w, h):
    if fmt == 14: return gvr.decode_cmpr(data, w, h)
    if fmt == 5: return gvr.decode_rgb5a3(data, w, h)
    if fmt == 6: return gvr.decode_rgba8(data, w, h)
    if fmt in (1, 3, 4): return gvr.decode_simple(fmt, data, w, h)
    return None


def scan(disc, only=None):
    """Every distinct texture: {name: (fmt, w, h, base bytes, where)}, in disc order."""
    found = {}
    for path, off, size in disc.files():
        low = path.lower()
        if low.endswith(SKIP_EXT) or size > 32 << 20:
            continue
        if only and not any(o in low for o in only):
            continue
        blob = disc.read(off, size)
        if low.endswith('.prs'):
            try:
                blob = prs.decompress(blob)
            except IndexError:
                continue
        i = blob.find(b'GVRT')
        while i >= 0 and i + 16 <= len(blob):
            fmt = blob[i + 11]
            w, h = struct.unpack('>HH', blob[i + 12:i + 16])
            if fmt in BLOCKS and 0 < w <= 1024 and 0 < h <= 1024:
                n = base_size(fmt, w, h)
                data = blob[i + 16:i + 16 + n]
                if len(data) == n:
                    key = f'tex1_{w}x{h}_{xxhash.xxh64_intdigest(data, 0):016x}_{fmt}'
                    found.setdefault(key, (fmt, w, h, data, path))
            i = blob.find(b'GVRT', i + 4)
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--iso', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--model', default=os.path.join(MODELS, '4xHDCube4Compact.pth'))
    ap.add_argument('--alpha-model', default=os.path.join(MODELS, '4xHDcube-Alpha.pth'))
    ap.add_argument('--limit', type=int, default=0, help='only the first N textures (tests)')
    ap.add_argument('--only', default='', help='only files whose path contains one of these (comma-separated)')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    status_path = os.path.join(a.out, 'status.txt')

    def status(state, done, total, eta, note=''):
        tmp = status_path + '.tmp'
        with open(tmp, 'w') as f:
            f.write(f'{state} {done} {total} {int(eta)} {note}\n')
        os.replace(tmp, status_path)

    status('running', 0, 0, 0, 'Reading the textures on your disc...')
    try:
        disc = Disc(a.iso)
        if disc.game != 'GSNE8P':
            raise SystemExit(f'this disc is {disc.game}, not Sonic Adventure 2: Battle (USA)')
        found = scan(disc, [o.strip().lower() for o in a.only.split(',') if o.strip()])
        keys = list(found)
        if a.limit:
            keys = keys[:a.limit]
        total = len(keys)
        pixels = sum(found[k][1] * found[k][2] for k in keys)
        print(f'{total} textures, {pixels / 1e6:.1f} megapixels', flush=True)
        import upscale  # (torch loads slowly: after the scan has said something)
        print(f'upscaling on {upscale.DEVICE}', flush=True)
        done = 0
        done_px = 0
        work_px = 0
        work_s = 0.0
        last_status = 0
        for k in keys:
            fmt, w, h, data, where = found[k]
            out = os.path.join(a.out, k + '.png')
            if not os.path.exists(out):
                while os.path.exists(os.path.join(a.out, 'pause')):
                    status('paused', done, total, 0)
                    time.sleep(1)
                t0 = time.time()
                rgba = decode(fmt, data, w, h)
                if rgba is not None:
                    up = upscale.upscale(rgba, a.model, a.alpha_model)
                    Image.fromarray(up).save(out + '.tmp.png', compress_level=4)
                    os.replace(out + '.tmp.png', out)
                work_s += time.time() - t0
                work_px += w * h
            done += 1
            done_px += w * h
            if time.time() - last_status > 1 or done == total:
                rate = work_px / work_s if work_s > 0 else 0
                eta = (pixels - done_px) / rate if rate > 0 else 0
                status('running', done, total, eta)
                last_status = time.time()
        with open(os.path.join(a.out, 'pack.txt'), 'w') as f:
            f.write(f'model {os.path.basename(a.model)}\nalpha {os.path.basename(a.alpha_model)}\ntextures {total}\n'
                    f'made {time.strftime("%Y-%m-%d %H:%M")}\n')
        status('finished', total, total, 0)
        print('finished', flush=True)
    except SystemExit as e:
        status('failed', 0, 0, 0, str(e))
        raise
    except Exception as e:
        traceback.print_exc()
        status('failed', 0, 0, 0, f'{type(e).__name__}: {e}')
        sys.exit(1)


if __name__ == '__main__':
    main()
