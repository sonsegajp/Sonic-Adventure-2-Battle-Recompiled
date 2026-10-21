# Summarises a tools/stagetest.ps1 (stages) or tools/eventtest.ps1 (--events) run: per stage/event,
# whether its module linked, how far it ran (and for events whether they ended), crashes and fatal
# errors, missing files and renderer complaints, audio level; plus a contact sheet of every one.
# Usage: stagereport.py <dir> [--events]
import glob, os, re, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw

out = sys.argv[1]
events = '--events' in sys.argv
rows, thumbs = [], []
for d in sorted(glob.glob(os.path.join(out, 'ev*' if events else 'stg*'))):
    name = open(os.path.join(d, 'name.txt')).read().strip() if os.path.exists(os.path.join(d, 'name.txt')) else '?'
    num = int(re.sub(r'\D', '', os.path.basename(d)))
    mfile = os.path.join(d, 'module.txt')
    module = 80 if events else int(open(mfile).read()) if os.path.exists(mfile) else num
    log = open(os.path.join(d, 'log.txt'), encoding='latin1').read() if os.path.exists(os.path.join(d, 'log.txt')) else ''
    linked = re.search(r'\[rel\] linked module %d \(' % module, log) is not None
    ended = re.search(r'\[rel\] unlinked module %d \(' % module, log) is not None
    problems = []
    for pat in [r'ABORT.*', r'CRASH.*', r'FATAL.*', r'\[gx\] unknown.*', r'\[aurora (error|fatal)\].*', r'\[ax\] unknown.*',
                r'overflowed.*', r'cannot grow.*']:
        for m in re.finditer(pat, log):
            problems.append(m.group(0)[:110])
    missing = sorted(set(re.findall(r"file '([^']+)' was not found", log)))
    frames = re.findall(r'\[frontend\] (\d+) frames, ([\d.]+) fps', log)
    last = frames[-1] if frames else ('0', '0')
    audio = os.path.join(d, 'audio.raw')
    rms = 0
    if os.path.exists(audio) and os.path.getsize(audio) > 128000:
        a = np.fromfile(audio, dtype='<i2')
        a = a[-32000 * 2 * 20:] if not events else a[len(a) // 3:]  # stage: the last 20 s; event: after boot
        rms = int(np.sqrt((a.astype(np.float64) ** 2).mean()))
    if glob.glob(os.path.join(d, '*.rgba')) and not glob.glob(os.path.join(d, 'frame_*.png')):
        subprocess.run([sys.executable, 'tools/rgba2png.py', d], capture_output=True)
    pngs = sorted(glob.glob(os.path.join(d, 'frame_*.png')))
    if not linked: status = 'NOT LOADED'
    elif problems: status = 'PROBLEMS'
    elif not pngs: status = 'NO FRAMES'
    elif events and not ended: status = 'NO END'
    else: status = 'OK'
    rows.append((num, name, status, len(pngs), last, rms, missing, problems))
    picks = [pngs[int(i * (len(pngs) - 1) / 3)] for i in range(4)] if len(pngs) >= 4 else pngs[:4]
    thumbs.append((num, name, status, picks if events else (pngs[1:2] or pngs[:1]) + pngs[-1:]))

print('%-4s %-26s %-10s %-6s %-14s %-7s %s' % ('id', 'name', 'status', 'frames', 'ran (frames,fps)', 'audio', 'missing files / problems'))
for num, name, status, n, last, rms, missing, problems in rows:
    print('%-4d %-26s %-10s %-6d %-14s %-7d %s' % (num, name[:26], status, n, '%s,%s' % last, rms, '; '.join(missing[:4] + problems[:3])))

if thumbs:
    per = 4 if events else 2
    w, h, cols = 320, 240, 8
    groups = cols // per
    sheet = Image.new('RGB', (w * cols, h * ((len(thumbs) + groups - 1) // groups)))
    for i, (num, name, status, files) in enumerate(thumbs):
        for j in range(per):
            f = files[j] if j < len(files) else None
            x, y = ((i % groups) * per + j) * w, (i // groups) * h
            im = Image.open(f).convert('RGB').resize((w, h)) if f else Image.new('RGB', (w, h), (60, 0, 0))
            dr = ImageDraw.Draw(im)
            if j == 0:
                dr.rectangle((0, 0, w, 14), fill=(0, 0, 0))
                dr.text((3, 2), '%d %s %s' % (num, name[:24], '' if status == 'OK' else status),
                        fill=(255, 255, 0) if status == 'OK' else (255, 80, 80))
            sheet.paste(im, (x, y))
    path = os.path.join(out, 'events.png' if events else 'stages.png')
    sheet.save(path)
    print('sheet:', path)
