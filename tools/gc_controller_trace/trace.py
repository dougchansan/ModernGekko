"""Traces a front photo of a purple GameCube controller into polygons.
  python trace.py <photo> <out-dir>
Writes trace.json (coordinates relative to the body's centre, in photo pixels)
and preview.png (the polygons re-rendered) for checking. The seed points below
are positions read off the photo used for src/runtime/gc_controller_shape.inc
(834x900, controller roughly centred); another photo needs its own."""
import json
import os
import sys
import cv2
import numpy as np
from PIL import Image

PHOTO, OUT_DIR = sys.argv[1], sys.argv[2]
im = cv2.cvtColor(np.array(Image.open(PHOTO).convert('RGB')), cv2.COLOR_RGB2BGR)
H, W = im.shape[:2]
hsv = cv2.cvtColor(im, cv2.COLOR_BGR2HSV)
h, s, v = [hsv[..., i].astype(int) for i in range(3)]


def largest(mask, near=None):
    n, lab, stats, cent = cv2.connectedComponentsWithStats(mask.astype(np.uint8), 8)
    best, area = 0, 0
    for i in range(1, n):
        if near is not None:
            x, y, w, hh = stats[i, :4]
            if not (x <= near[0] <= x + w and y <= near[1] <= y + hh):
                continue
        if stats[i, 4] > area:
            best, area = i, stats[i, 4]
    return lab == best


def contour(mask, eps=1.2):
    cs, _ = cv2.findContours(mask.astype(np.uint8), cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    c = max(cs, key=cv2.contourArea)
    c = cv2.approxPolyDP(c, eps, True)[:, 0, :]
    return c.astype(float)


def fill_holes(mask):
    m = mask.astype(np.uint8) * 255
    flood = m.copy()
    ff = np.zeros((H + 2, W + 2), np.uint8)
    cv2.floodFill(flood, ff, (0, 0), 255)
    return (m | cv2.bitwise_not(flood)) > 0


purple = (h >= 118) & (h <= 162) & (s > 50) & (v > 25)
# The pack's thin purple stripe touches the controller's top edge; opening
# removes it, closing seals the shadowed seams.
k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (15, 15))
body = cv2.morphologyEx(purple.astype(np.uint8), cv2.MORPH_OPEN, k) > 0
body = largest(body)
body = cv2.morphologyEx(body.astype(np.uint8), cv2.MORPH_CLOSE,
                        cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (21, 21))) > 0
body = fill_holes(body)
body = cv2.GaussianBlur(body.astype(np.float32), (9, 9), 0) > 0.5

grey = (s < 45) & (v > 150)
green = (h >= 65) & (h <= 100) & (s > 100) & (v > 50)
red = ((h <= 10) | (h >= 170)) & (s > 110) & (v > 70)
yellow = (h >= 15) & (h <= 38) & (s > 110) & (v > 110)

ys, xs = np.nonzero(body)
bx0, bx1, by0, by1 = xs.min(), xs.max(), ys.min(), ys.max()
cx, cy = (bx0 + bx1) / 2, (by0 + by1) / 2
print('body bbox', bx0, by0, bx1, by1)


def part(mask, seed, eps=0.8, close=5):
    m = largest(mask & body, seed)
    if close:
        m = cv2.morphologyEx(m.astype(np.uint8), cv2.MORPH_CLOSE,
                            cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (close, close))) > 0
    m = fill_holes(m)
    return contour(m, eps)


parts = {}
parts['body'] = contour(body, 1.5)
# Seeds: rough positions read off the photo.
parts['stick'] = part(grey, (180, 410))
parts['dpad'] = part(grey, (290, 590), 0.6, 3)
parts['start'] = part(grey, (422, 420))
parts['a'] = part(green, (660, 410))
parts['b'] = part(red, (572, 460))
parts['c'] = part(yellow, (552, 585))
parts['y'] = part(grey, (640, 325))
parts['x'] = part(grey, (745, 395))


def gate(seed, inner_mask, grow):
    """The recessed, darker well around a stick or the d-pad: the non-purple
    hole in the body around the part, grown slightly to its rim."""
    hole = body & ~((h >= 118) & (h <= 162) & (s > 50) & (v > 70))
    m = largest(hole | inner_mask, seed)
    m = cv2.morphologyEx(m.astype(np.uint8), cv2.MORPH_CLOSE,
                         cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (9, 9))) > 0
    m = fill_holes(m)
    if grow:
        m = cv2.dilate(m.astype(np.uint8), cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (grow, grow))) > 0
    return contour(m, 1.0)


parts['stick_gate'] = gate((180, 410), largest(grey & body, (180, 410)), 3)
parts['c_gate'] = gate((552, 585), largest(yellow & body, (552, 585)), 3)
parts['dpad_gate'] = gate((290, 590), largest(grey & body, (290, 590)), 3)

out = {name: [[round(float(x - cx), 1), round(float(y - cy), 1)] for x, y in poly]
       for name, poly in parts.items()}
out['_size'] = [float(bx1 - bx0), float(by1 - by0)]
json.dump(out, open(os.path.join(OUT_DIR, 'trace.json'), 'w'))
for k2, p in parts.items():
    print(k2, len(p))

# Preview: polygons filled in flat colours over a dark background.
prev = np.full((H, W, 3), 30, np.uint8)
cols = {'body': (150, 60, 120), 'stick_gate': (90, 30, 70), 'c_gate': (90, 30, 70),
        'dpad_gate': (90, 30, 70), 'stick': (200, 200, 200), 'dpad': (200, 200, 200),
        'start': (200, 200, 200), 'a': (110, 180, 40), 'b': (40, 40, 210), 'c': (40, 210, 230),
        'x': (210, 210, 210), 'y': (210, 210, 210)}
for name in ['body', 'stick_gate', 'c_gate', 'dpad_gate', 'stick', 'dpad', 'start', 'a', 'b', 'c', 'x', 'y']:
    cv2.fillPoly(prev, [parts[name].astype(np.int32)], cols[name])
cv2.imwrite(os.path.join(OUT_DIR, 'preview.png'), prev[230:800, 30:830])
