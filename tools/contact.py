# Contact sheet of dumped PNG frames with their frame numbers. Usage: contact.py <dir> <out.png> [cols] [first] [step]
import glob, os, sys
from PIL import Image, ImageDraw
d, out = sys.argv[1], sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 6
first = int(sys.argv[4]) if len(sys.argv) > 4 else 0
step = int(sys.argv[5]) if len(sys.argv) > 5 else 1
files = [f for f in sorted(glob.glob(os.path.join(d, 'frame_*.png')))][first::step][:cols * cols]
w, h = 320, 240
rows = (len(files) + cols - 1) // cols
sheet = Image.new('RGB', (w * cols, h * rows))
for i, f in enumerate(files):
    im = Image.open(f).convert('RGB').resize((w, h))
    ImageDraw.Draw(im).text((4, 4), os.path.basename(f)[6:11], fill=(255, 255, 0))
    sheet.paste(im, ((i % cols) * w, (i // cols) * h))
sheet.save(out)
print(out, len(files), 'frames')
