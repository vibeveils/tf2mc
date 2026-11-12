#!/usr/bin/env python3
"""Software-rasterises tests/build/preview_quads.txt from a TF2-style camera (Source coords, z up).
Back faces are culled assuming Source/D3D convention (front = clockwise on screen), so a correct
image also validates the mesher's winding. Usage: render_preview.py quads.txt out.png"""
import math, sys
from PIL import Image, ImageDraw

W, H = 1280, 720
import os
SKY = tuple(int(x) for x in os.environ.get("SKY", "150,190,240").split(","))
quads = []
for line in open(sys.argv[1]):
    v = line.split()
    p = int(v[0]); pts = []
    for k in range(4):
        b = 1 + k * 7
        pts.append(([float(x) for x in v[b:b + 3]], [int(x) for x in v[b + 3:b + 7]]))
    quads.append((p, pts))

def render(cam, yaw, pitch, out):
    cy, sy, cp, sp = math.cos(math.radians(yaw)), math.sin(math.radians(yaw)), math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    f = (cp * cy, cp * sy, -sp)                              # Source: +pitch looks down
    r = (f[1] * 0 - f[2] * 0, f[2] * 0 - f[0] * 1, 0) if False else (sy, -cy, 0.0)   # forward x up
    u = (r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0])
    fpx = W / 2 / math.tan(math.radians(45))
    img = Image.new("RGB", (W, H), SKY)
    d = ImageDraw.Draw(img, "RGBA")
    items, culled = [], 0
    for p, pts in quads:
        scr, depth = [], 0
        for (x, y, z), col in pts:
            dx, dy, dz = x - cam[0], y - cam[1], z - cam[2]
            zc = dx * f[0] + dy * f[1] + dz * f[2]
            if zc < 1: break
            scr.append((W / 2 + (dx * r[0] + dy * r[1] + dz * r[2]) / zc * fpx, H / 2 - (dx * u[0] + dy * u[1] + dz * u[2]) / zc * fpx))
            depth += zc
        else:
            area = sum(scr[i][0] * scr[(i + 1) % 4][1] - scr[(i + 1) % 4][0] * scr[i][1] for i in range(4))
            if area <= 0: culled += 1; continue          # back face (counter-clockwise on screen)
            col = tuple(sum(c[1][k] for c in pts) // 4 for k in range(4))
            items.append((depth, p, scr, col))
    items.sort(key=lambda t: (-t[0]))
    for depth, p, scr, col in items:
        d.polygon(scr, fill=col if p else col[:3] + (255,), outline=None)
    img.save(out)
    return len(items), culled

cam = (float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])) if len(sys.argv) > 5 else (-1500, 1700, 900)
yaw = float(sys.argv[6]) if len(sys.argv) > 6 else -50
pitch = float(sys.argv[7]) if len(sys.argv) > 7 else 22
n, c = render(cam, yaw, pitch, sys.argv[2])
print(f"drew {n} quads, culled {c} back faces -> {sys.argv[2]}")
