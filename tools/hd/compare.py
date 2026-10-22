# Upscales the sample textures with each candidate model and lays them out side by side.
import os, sys, time
import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(__file__))
import upscale

torch_threads = int(os.environ.get('HD_THREADS', '0'))
if torch_threads:
    import torch
    torch.set_num_threads(torch_threads)

ROOT = os.path.join(os.path.dirname(__file__), '..', '..')
SRC = os.path.join(ROOT, 'build', 'hd_samples', 'src')
OUT = os.path.join(ROOT, 'build', 'hd_samples', 'out')
M = os.path.join(os.path.dirname(__file__), 'models')
SAMPLES = [  # (file prefix, tiles?)
    ('landtx13_000', True), ('landtx13_002', False), ('landtx13_020', True), ('landtx13_024', True),
    ('landtx13_125', False), ('landtx13_005', True), ('objtex_common_002', False), ('objtex_common_001', False),
    ('objtex_common_027', True), ('objtex_common_055', True), ('alphaTitle_e_002', False), ('alphaTitle_e_000', False),
]
MODELS = [  # (label, model, alpha model)
    ('HDCube 4 Compact 4x', f'{M}/Models/HDcube/4xHDCube4Compact.pth', f'{M}/Models/HDcube/4xHDcube-Alpha.pth'),
    ('HDCube Compact 2x', f'{M}/2xHDCube_Compact.pth', None),
    ('HDCube 3 4x', f'{M}/4xHDcube3_500k.pth', f'{M}/Models/HDcube/4xHDcube-Alpha.pth'),
    ('UltraSharp 4x', f'{M}/4x-UltraSharp.pth', None),
    ('Real-ESRGAN anime 4x', f'{M}/RealESRGAN_x4plus_anime_6B.pth', None),
]
os.makedirs(OUT, exist_ok=True)
files = sorted(os.listdir(SRC))
cell = 256
labels = ['Original (pixels)', 'Bicubic 4x'] + [m[0] for m in MODELS]
sheet = Image.new('RGB', (len(labels) * (cell + 4), len(SAMPLES) * (cell + 4) + 24), (24, 24, 30))
d = ImageDraw.Draw(sheet)
for c, l in enumerate(labels):
    d.text((c * (cell + 4) + 4, 6), l, fill=(255, 230, 120))


def put(img, r, c):
    im = (Image.fromarray(img) if isinstance(img, np.ndarray) else img).convert('RGBA')
    w, h = im.size
    sc = cell / max(w, h)
    im = im.resize((max(1, int(w * sc)), max(1, int(h * sc))), Image.LANCZOS if sc < 1 else Image.NEAREST)
    bg = Image.new('RGB', im.size, (110, 110, 120))
    bg.paste(im, mask=im.split()[3])
    sheet.paste(bg, (c * (cell + 4), 24 + r * (cell + 4)))


for r, (prefix, tiles) in enumerate(SAMPLES):
    fn = next(f for f in files if f.startswith(prefix + '_'))
    src = np.asarray(Image.open(os.path.join(SRC, fn)).convert('RGBA'))
    h, w = src.shape[:2]
    put(src, r, 0)
    bic = Image.fromarray(src, 'RGBA').resize((w * 4, h * 4), Image.BICUBIC)
    put(bic, r, 1)
    for c, (label, mp, ap) in enumerate(MODELS):
        t0 = time.time()
        up = upscale.upscale(src, mp, ap, wrap=tiles)
        Image.fromarray(up).save(os.path.join(OUT, f'{prefix}__{label.replace(" ", "_")}.png'))
        # shown at 4x of the original either way (a 2x result is enlarged to match)
        img = Image.fromarray(up)
        if img.size[0] != w * 4:
            img = img.resize((w * 4, h * 4), Image.BICUBIC)
        put(img, r, c + 2)
        print(f'{fn} {label}: {time.time() - t0:.1f}s', flush=True)
sheet.save(os.path.join(ROOT, 'build', 'hd_samples', 'compare.png'))
print('done')
