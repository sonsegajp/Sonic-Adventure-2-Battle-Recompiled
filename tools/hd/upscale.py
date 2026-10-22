# Upscales GameCube textures with an ESRGAN-family model (any architecture spandrel loads).
#   - Colour goes through the model; alpha through a dedicated alpha model if given (HDCube ships
#     one), else through the colour model as greyscale.
#   - The colour of fully transparent texels is meaningless (CMPR stores black there) but the model
#     blends it into the visible edge: it is filled from the nearest visible texels first ("alpha
#     bleed"), or soft glows and cut-outs get dark fringes.
#   - Textures that tile are padded with their own opposite edges first (and cropped after), so the
#     model sees the wrap and no seam appears where copies meet; per axis (see tiles()).
#   - Cut-out alpha (CMPR: only fully transparent or opaque) is thresholded back to cut-out.
import numpy as np
import torch
from spandrel import ModelLoader

_cache = {}
DEVICE = 'cuda' if torch.cuda.is_available() else 'cpu'


def load(path):
    if path not in _cache:
        m = ModelLoader().load_from_file(path)
        m.model.eval()
        m.model.to(DEVICE)
        _cache[path] = m
    return _cache[path]


def _run(model, rgb):  # rgb: HxWx3 float 0..1
    t = torch.from_numpy(np.ascontiguousarray(rgb.transpose(2, 0, 1))).unsqueeze(0).float().to(DEVICE)
    with torch.no_grad():
        out = model(t)
    return out.squeeze(0).clamp(0, 1).cpu().numpy().transpose(1, 2, 0)


def bleed(rgba):
    """RGB of transparent texels filled from the visible ones around them (alpha untouched): a
    push-pull pyramid - averages of the visible texels at ever coarser scales, then each hole takes
    the finest average that covers it."""
    img = rgba.astype(np.float32)
    known = rgba[..., 3] > 0
    if known.all() or not known.any():
        return img
    w = known.astype(np.float32)
    c = img[..., :3] * w[..., None]
    levels = [(c, w)]
    while min(c.shape[:2]) > 1:  # push: 2x2 sums
        h, wd = c.shape[:2]
        c = np.pad(c, ((0, h & 1), (0, wd & 1), (0, 0)))
        w = np.pad(w, ((0, h & 1), (0, wd & 1)))
        c = c[0::2, 0::2] + c[1::2, 0::2] + c[0::2, 1::2] + c[1::2, 1::2]
        w = w[0::2, 0::2] + w[1::2, 0::2] + w[0::2, 1::2] + w[1::2, 1::2]
        levels.append((c, w))
    fillc = levels[-1][0] / np.maximum(levels[-1][1], 1e-6)[..., None]
    for c, w in reversed(levels[:-1]):  # pull: holes take the coarser average
        h, wd = c.shape[:2]
        up = np.repeat(np.repeat(fillc, 2, axis=0), 2, axis=1)[:h, :wd]
        avg = c / np.maximum(w, 1e-6)[..., None]
        fillc = np.where((w > 0)[..., None], avg, up)
    img[..., :3] = np.where(known[..., None], img[..., :3], fillc)
    return img


def tiles(rgba):
    """(wraps across x, wraps across y): opposite edges about as alike as neighbouring texels are."""
    a = rgba.astype(np.float32)
    if a.shape[0] < 8 or a.shape[1] < 8:
        return False, False
    step = (np.abs(a[:, 1:, :3] - a[:, :-1, :3]).mean() + np.abs(a[1:, :, :3] - a[:-1, :, :3]).mean()) / 2
    lr = np.abs(a[:, 0, :3] - a[:, -1, :3]).mean()
    tb = np.abs(a[0, :, :3] - a[-1, :, :3]).mean()
    return lr <= step * 1.6 + 2, tb <= step * 1.6 + 2


def _pad(img, p, wrap):
    img = np.pad(img, ((0, 0), (p, p), (0, 0)), mode='wrap' if wrap[0] else 'edge')
    return np.pad(img, ((p, p), (0, 0), (0, 0)), mode='wrap' if wrap[1] else 'edge')


def upscale(rgba, model_path, alpha_model_path=None, wrap=None, pad=8):
    """rgba: HxWx4 uint8. wrap: (x, y) or a bool for both, None to detect. Returns HxWx4 uint8, scaled."""
    m = load(model_path)
    s = m.scale
    if wrap is None:
        wrap = tiles(rgba)
    elif isinstance(wrap, bool):
        wrap = (wrap, wrap)
    has_alpha = (rgba[..., 3] < 255).any()
    img = (bleed(rgba) if has_alpha else rgba.astype(np.float32)) / 255.0
    h, w = img.shape[:2]
    p = min(pad, h, w)
    padded = _pad(img, p, wrap)
    rgb = _run(m, padded[..., :3])
    out_a = None
    if has_alpha:
        am = load(alpha_model_path) if alpha_model_path else m
        a = _run(am, np.repeat(padded[..., 3:4], 3, axis=2))
        if am.scale != s:  # (an alpha model of another scale: resize to match)
            from PIL import Image
            a = np.asarray(Image.fromarray((a * 255).astype(np.uint8)).resize((rgb.shape[1], rgb.shape[0]), Image.BICUBIC)) / 255.0
        out_a = a.mean(axis=2)
        if np.isin(rgba[..., 3], (0, 255)).all():  # cut-out stays cut-out
            out_a = (out_a >= 0.5).astype(np.float32)
    rgb = rgb[p * s:(p + h) * s, p * s:(p + w) * s]
    if out_a is None:
        return (rgb * 255 + 0.5).clip(0, 255).astype(np.uint8)  # opaque: RGB only
    out_a = out_a[p * s:(p + h) * s, p * s:(p + w) * s]
    return (np.dstack([rgb, out_a]) * 255 + 0.5).clip(0, 255).astype(np.uint8)
