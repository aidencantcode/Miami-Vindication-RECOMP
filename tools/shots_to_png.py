"""Convert the newest screenshot run (build/shots/run_*/frame_*.bmp) to PNG.
Usage: python -I tools/shots_to_png.py [run_dir]
"""
import glob
import os
import sys

from PIL import Image

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
runs = sorted(glob.glob(os.path.join(root, "build", "shots", "run_*")), key=os.path.getmtime)
run = sys.argv[1] if len(sys.argv) > 1 else (runs[-1] if runs else None)
if not run:
    sys.exit("no screenshot runs found")
files = sorted(glob.glob(os.path.join(run, "frame_*.bmp")))
for f in files:
    Image.open(f).save(os.path.splitext(f)[0] + ".png")
print(run)
print(" ".join(os.path.basename(os.path.splitext(f)[0]) + ".png" for f in files))
