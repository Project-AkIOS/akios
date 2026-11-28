#!/usr/bin/env python3
"""The Pixel 8a's snapshot ring, over the USB cable while AkIOS runs (usbmux=1, boot-arg snap=N): the phone's port 10,
where the kernel (xnu bsd/dev/hakios.akita_display.c) keeps, once a second, what the display's DMA reads, in RAM only.
Each frame comes with its time and the DQE's colour matrix then; the DQE sits after the DMA, so the matrix is applied
here (the panel's colours) unless --raw.

usage: usbmux-snap.py OUTDIR [--raw] [--last N] [--sheet]
       OUTDIR/snap-<seconds since boot>.png per frame (only the newest N with --last); --sheet adds sheet.png, every
       frame small with its time, for a look at the whole run
"""
import os
import re
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util

spec = importlib.util.spec_from_file_location('shot', os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                                   'usbmux-shot.py'))
args = [a for a in sys.argv[1:] if not a.startswith('--')]
if not args:
    sys.exit(__doc__)
# (usbmux-shot.py runs its main part at import: only its connect function is wanted, so it is read, not imported)
src = open(spec.origin).read().split('\nif len(sys.argv) < 2:')[0]
shot = {}
exec(compile(src, spec.origin, 'exec'), shot)

out = args[0]
raw = '--raw' in sys.argv
last = int(sys.argv[sys.argv.index('--last') + 1]) if '--last' in sys.argv else 0
os.makedirs(out, exist_ok=True)
s = shot['mux_connect'](10)
data = bytearray()
while chunk := s.recv(1 << 20):
    data += chunk
if not data.startswith(b'P6'):
    sys.exit('usbmux-snap: ' + data.decode('latin1', 'replace').strip())

frames, at = [], 0
while at < len(data) and data.startswith(b'P6', at):
    lines = []
    for _ in range(4):                      # P6, the comment, width height, 255
        end = data.index(b'\n', at)
        lines.append(data[at:end].decode())
        at = end + 1
    w, h = map(int, lines[2].split())
    info = dict(kv.split('=', 1) for kv in lines[1].lstrip('# ').split()[1:])
    frames.append((info, Image.frombytes('RGB', (w, h), bytes(data[at:at + w * h * 3]))))
    at += w * h * 3
if last:
    frames = frames[-last:]

saved = []
for info, im in frames:
    if not raw and info.get('dqe') == '1':
        # the DQE's matrix, its own order: rows and columns B, G, R, 1.0 = 1024 (offsets not applied: scale unknown)
        m = [int(v) / 1024 for v in info['m'].split(',')]
        rgb_matrix = [m[8], m[7], m[6], 0, m[5], m[4], m[3], 0, m[2], m[1], m[0], 0]
        im = im.convert('RGB', rgb_matrix)
    t = int(info['t_ns']) / 1e9
    path = os.path.join(out, f'snap-{t:08.1f}.png')
    im.save(path)
    saved.append((t, path, im))
print(f'usbmux-snap: {len(saved)} frames into {out}' + (f' ({saved[0][0]:.1f} s to {saved[-1][0]:.1f} s since boot)'
                                                         if saved else ''))
if '--sheet' in sys.argv and saved:
    tw = 180
    th = int(tw * saved[0][2].height / saved[0][2].width)
    cols = min(10, len(saved))
    sheet = Image.new('RGB', (tw * cols, th * ((len(saved) + cols - 1) // cols)))
    for i, (t, _, im) in enumerate(saved):
        small = im.resize((tw, th))
        ImageDraw.Draw(small).text((4, 4), f'{t:.0f}s', fill=(255, 255, 0))
        sheet.paste(small, ((i % cols) * tw, (i // cols) * th))
    sheet.save(os.path.join(out, 'sheet.png'))
    print(f'usbmux-snap: {os.path.join(out, "sheet.png")}')
