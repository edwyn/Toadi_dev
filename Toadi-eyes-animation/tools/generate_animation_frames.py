#!/usr/bin/env python3
"""Generate spidey-eye and spider animation frames for the example plug-in.

Frames: 128x64, 1 bpp, MSB-first, visible panel orientation (same stored format
as hearts.h). The plug-in reverses each byte before EyesUILayer::drawImages.

Spidey eyes are extracted from the user-supplied stencil
(reference/spidey_ref/spidey_ref.jpg, first left/right pair) as white outlines
on black. The spider is drawn procedurally with the reference proportions.

Outputs:
  src/animation_plugin/src/spidey_frames.h
  src/animation_plugin/src/spider_frames.h
  animations_preview/{spidey_left,spidey_right,spider}_contact.png
"""
import math
import os

from PIL import Image, ImageDraw, ImageFilter

W, H = 128, 64
SS = 4
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EYE_PATHS = {
    'left': os.path.join(ROOT, 'reference', 'spidey_ref', 'left.png'),
    'right': os.path.join(ROOT, 'reference', 'spidey_ref', 'right.png'),
}
EYE_W, EYE_H = 86, 60             # filled lens fit on a 128x64 panel
EYE_SHIFT = 8                     # move lenses toward the robot centre
SPIDEY2_W, SPIDEY2_H = 112, 62    # larger centred contour variant
PUMPKIN_PATH = os.path.join(ROOT, 'reference', 'spidey_ref', 'pumping.png')
PUMPKIN_W, PUMPKIN_H = 84, 58     # pumpkin body fit
BADGE_PATH = os.path.join(
    ROOT, 'reference', 'spidey_ref', 'paw-patrol-badge-diy-1440x768.png')
BADGE_BOX = (721, 345, 1006, 668)  # classic blue badge, bottom centre
WALLE_PATHS = {
    'left': os.path.join(ROOT, 'reference', 'spidey_ref', 'walle_left.png'),
    'right': os.path.join(ROOT, 'reference', 'spidey_ref', 'walle_right.png'),
}


def pack(img):
    buf = bytearray(1024)
    px = img.load()
    for y in range(H):
        for x in range(W):
            if px[x, y]:
                idx = y * W + x
                buf[idx >> 3] |= 0x80 >> (idx & 7)
    return bytes(buf)


def eye_mask(path, fill):
    """Mask from a black-outline/white-interior PNG (filled or outline)."""
    im = Image.open(path).convert('L')
    bbox = im.point(lambda v: 255 if v < 100 else 0).getbbox()
    im = im.crop(bbox)
    w, h = im.size
    px = im.load()
    outline = [[px[x, y] < 100 for x in range(w)] for y in range(h)]
    if fill:
        outside = [[False] * w for _ in range(h)]
        stack = []
        for x in range(w):
            for y in (0, h - 1):
                if not outline[y][x] and not outside[y][x]:
                    outside[y][x] = True
                    stack.append((x, y))
        for y in range(h):
            for x in (0, w - 1):
                if not outline[y][x] and not outside[y][x]:
                    outside[y][x] = True
                    stack.append((x, y))
        while stack:
            x, y = stack.pop()
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if (0 <= nx < w and 0 <= ny < h and not outline[ny][nx]
                        and not outside[ny][nx]):
                    outside[ny][nx] = True
                    stack.append((nx, ny))
        filled = [[outline[y][x] or (not outside[y][x]) for x in range(w)]
                  for y in range(h)]
    else:
        filled = outline
    # Keep only the largest connected component; drop scan/JPEG specks.
    seen = [[False] * w for _ in range(h)]
    comps = []
    for y in range(h):
        for x in range(w):
            if not filled[y][x] or seen[y][x]:
                continue
            stack = [(x, y)]
            seen[y][x] = True
            comp = []
            while stack:
                cx, cy = stack.pop()
                comp.append((cx, cy))
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = cx + dx, cy + dy
                    if (0 <= nx < w and 0 <= ny < h and filled[ny][nx]
                            and not seen[ny][nx]):
                        seen[ny][nx] = True
                        stack.append((nx, ny))
            comps.append(comp)
    if comps:
        keep = max(comps, key=len)
        filled = [[False] * w for _ in range(h)]
        for x, y in keep:
            filled[y][x] = True
    return filled, w, h


def render_eye(mask, w, h, scale_y, shift_x, target_w=EYE_W, target_h=EYE_H):
    if scale_y <= 0.0:
        return Image.new('L', (W, H), 0)
    src = Image.new('L', (w, h), 0)
    p = src.load()
    for y in range(h):
        for x in range(w):
            p[x, y] = 255 if mask[y][x] else 0
    th = max(1, int(target_h * scale_y))
    src = src.resize((target_w, th), Image.LANCZOS).point(
        lambda v: 255 if v >= 110 else 0)
    pan = Image.new('L', (W, H), 0)
    pan.paste(src, ((W - target_w) // 2 + shift_x, (H - th) // 2))
    return pan


def draw_spider(t, crawl, bob):
    sup = Image.new('L', (W * SS, H * SS), 0)
    d = ImageDraw.Draw(sup)
    cx = (64 + crawl) * SS
    cy = (36 + bob) * SS
    for side in (-1, 1):
        for i in range(4):
            phase = t * 2 * math.pi + i * 0.8
            sw = math.sin(phase) * 3.0
            lift = math.cos(phase) * 2.5
            bx = cx + side * 6 * SS
            by = cy + (-14 + i * 6) * SS
            kx = cx + side * (19 + i * 3 + sw) * SS
            ky = cy + (-24 + i * 10 + lift) * SS
            fx = cx + side * (44 + i * 3 + sw * 0.6) * SS
            fy = cy + (-14 + i * 12 + lift * 1.2) * SS
            d.line([(bx, by), (kx, ky), (fx, fy)],
                   fill=255, width=3 * SS // 2)
            for x, y in ((bx, by), (kx, ky), (fx, fy)):
                r = SS * 0.8
                d.ellipse([x - r, y - r, x + r, y + r], fill=255)
    d.ellipse([cx - 7 * SS, cy - 22 * SS, cx + 7 * SS, cy - 6 * SS], fill=255)
    d.ellipse([cx - 10 * SS, cy - 8 * SS, cx + 10 * SS, cy + 16 * SS], fill=255)
    d.polygon([(cx - 4 * SS, cy - 4 * SS), (cx + 4 * SS, cy - 4 * SS),
               (cx, cy + 2 * SS), (cx + 4 * SS, cy + 8 * SS),
               (cx - 4 * SS, cy + 8 * SS), (cx, cy + 2 * SS)], fill=0)
    for dx in (-2.5, 2.5):
        d.ellipse([cx + (dx - 1) * SS, cy - 18 * SS,
                   cx + (dx + 1) * SS, cy - 16 * SS], fill=0)
    return sup.resize((W, H), Image.LANCZOS).point(
        lambda v: 255 if v >= 110 else 0)


def write_header(path, macro, arrays):
    """arrays: list of (suffix, [frames]); one C array per suffix."""
    with open(path, 'w') as f:
        f.write('/* Auto-generated by tools/generate_animation_frames.py.\n'
                '   128x64 1-bpp, MSB-first stored frames (visible panel orientation).\n'
                '   The plug-in reverses each byte before EyesUILayer::drawImages. */\n')
        for suffix, frames in arrays:
            name = f'{macro.lower()}_{suffix}_frames' if suffix else \
                   f'{macro.lower()}_frames'
            f.write(f'#define {name.upper()} {len(frames)}\n')
            f.write(f'static const unsigned char {name}[{len(frames)}][1024] = {{\n')
            for fr in frames:
                f.write('  {' + ','.join('0x%02x' % b for b in fr) + '},\n')
            f.write('};\n')


def contact(frames, path):
    cols, pad = 4, 6
    rows = (len(frames) + cols - 1) // cols
    sheet = Image.new('L', (cols * (W * 2 + pad), rows * (H * 2 + pad)), 70)
    for i, fr in enumerate(frames):
        im = Image.new('L', (W, H), 0)
        px = im.load()
        for y in range(H):
            for x in range(W):
                idx = y * W + x
                if fr[idx >> 3] & (0x80 >> (idx & 7)):
                    px[x, y] = 255
        r, c = divmod(i, cols)
        sheet.paste(im.resize((W * 2, H * 2), Image.NEAREST),
                    (c * (W * 2 + pad), r * (H * 2 + pad)))
    sheet.save(path)
    print('wrote', path)



def render_pumpkin(scale):
    """White pumpkin with the black lettering as cut-outs, gently pulsing."""
    im = Image.open(PUMPKIN_PATH).convert('RGB')
    w, h = im.size
    px = im.load()
    mask = [[max(px[x, y]) > 60 for x in range(w)] for y in range(h)]
    bbox = Image.frombytes('L', (w, h), bytes(
        255 if mask[y][x] else 0 for y in range(h) for x in range(w))).getbbox()
    src = Image.new('L', (w, h), 0)
    sp = src.load()
    for y in range(h):
        for x in range(w):
            sp[x, y] = 255 if mask[y][x] else 0
    src = src.crop(bbox)
    cw, ch = src.size
    th = max(1, int(PUMPKIN_H * scale))
    tw = max(1, int(PUMPKIN_W * scale))
    src = src.resize((tw, th), Image.LANCZOS).point(
        lambda v: 255 if v >= 110 else 0)
    pan = Image.new('L', (W, H), 0)
    ox, oy = (W - tw) // 2, (H - th) // 2
    pan.paste(src, (ox, oy))
    # Four subtle crease lines; the source shading is too soft at 1 bpp.
    d = ImageDraw.Draw(pan)
    for fx in (0.2, 0.4, 0.6, 0.8):
        x0 = ox + tw * fx
        pts = [(x0 + 2.0 * ((yy / max(1, th - 1)) - 0.5), oy + yy)
               for yy in range(th)]
        d.line(pts, fill=0, width=1)
    return pan


def render_badge(scale):
    """Blue Paw Patrol badge: white border+paw, black interior, pulsing."""
    im = Image.open(BADGE_PATH).convert('RGB')
    hsv = im.convert('HSV').crop(BADGE_BOX)
    w, h = hsv.size
    px = hsv.load()
    colored = set()
    for y in range(h):
        for x in range(w):
            hh, ss, vv = px[x, y]
            if ss > 100 and vv > 50:
                colored.add((x, y))
    seen = set()
    comps = []
    for p in colored:
        if p in seen:
            continue
        stack = [p]
        seen.add(p)
        comp = []
        while stack:
            x, y = stack.pop()
            comp.append((x, y))
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    n = (x + dx, y + dy)
                    if n in colored and n not in seen:
                        seen.add(n)
                        stack.append(n)
        comps.append(comp)
    comps.sort(key=len, reverse=True)
    keep = set()
    for comp in comps[:5]:  # border + pad + three toes
        keep.update(comp)
    xs = [x for x, y in keep]
    ys = [y for x, y in keep]
    x0, x1 = min(xs), max(xs) + 1
    y0, y1 = min(ys), max(ys) + 1
    src = Image.new('L', (x1 - x0, y1 - y0), 0)
    sp = src.load()
    for x, y in keep:
        if x0 <= x < x1 and y0 <= y < y1:
            sp[x - x0, y - y0] = 255
    th = max(1, int(58 * scale))
    tw = max(1, int(src.width * th / src.height))
    src = src.resize((tw, th), Image.LANCZOS).point(
        lambda v: 255 if v >= 110 else 0)
    pan = Image.new('L', (W, H), 0)
    pan.paste(src, ((W - tw) // 2, (H - th) // 2))
    return pan



def _component_bbox(comp):
    xs = [x for x, y in comp]
    ys = [y for x, y in comp]
    return min(xs), min(ys), max(xs) + 1, max(ys) + 1


def walle_parts(path):
    """Return (housing outline, lens disc) components.

    In the reference line art the lens is a solid black disc nested inside the
    thin housing outline, so the larger *area* component is the lens.  Pick the
    housing by bounding-box containment instead of by area.
    """
    im = Image.open(path).convert('L')
    mask = im.point(lambda v: 255 if v < 80 else 0)
    mask = mask.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.MinFilter(5))
    w, h = mask.size
    px = mask.load()
    black = set((x, y) for y in range(h) for x in range(w) if px[x, y] > 128)
    seen = set()
    comps = []
    for p in black:
        if p in seen:
            continue
        stack = [p]
        seen.add(p)
        comp = []
        while stack:
            x, y = stack.pop()
            comp.append((x, y))
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    q = (x + dx, y + dy)
                    if q in black and q not in seen:
                        seen.add(q)
                        stack.append(q)
        comps.append(comp)
    comps.sort(key=len, reverse=True)
    if len(comps) < 2:
        raise ValueError('Wall-E reference needs housing and lens: %s' % path)
    b0, b1 = _component_bbox(comps[0]), _component_bbox(comps[1])

    def inside(a, b):
        return (a[0] >= b[0] and a[1] >= b[1]
                and a[2] <= b[2] and a[3] <= b[3])

    if inside(b0, b1) and b0 != b1:
        return comps[1], comps[0]
    if inside(b1, b0) and b0 != b1:
        return comps[0], comps[1]
    # Fallback: the solid lens has the higher fill ratio.
    a0 = len(comps[0]) / ((b0[2] - b0[0]) * (b0[3] - b0[1]))
    a1 = len(comps[1]) / ((b1[2] - b1[0]) * (b1[3] - b1[1]))
    return (comps[1], comps[0]) if a0 > a1 else (comps[0], comps[1])


def render_walle(path, angle, lens_scale):
    housing, lens = walle_parts(path)
    xs = [x for x, y in housing]
    ys = [y for x, y in housing]
    hx0, hx1, hy0, hy1 = min(xs), max(xs) + 1, min(ys), max(ys) + 1
    hw, hh = hx1 - hx0, hy1 - hy0
    lxs = [x for x, y in lens]
    lys = [y for x, y in lens]
    lx0, lx1, ly0, ly1 = min(lxs), max(lxs) + 1, min(lys), max(lys) + 1
    lcx = (lx0 + lx1) / 2.0
    lcy = (ly0 + ly1) / 2.0
    lrx = max(1.0, (lx1 - lx0) / 2.0)
    lry = max(1.0, (ly1 - ly0) / 2.0)
    th = 54                    # leaves room for the eye to tilt
    tw = max(1, int(hw * th / hh))

    # housing mask at 4x
    hs = Image.new('L', (hw, hh), 0)
    hp = hs.load()
    for x, y in housing:
        hp[x - hx0, y - hy0] = 255
    HS = 4
    tws, ths = tw * HS, th * HS
    hs = hs.resize((tws, ths), Image.LANCZOS).point(
        lambda v: 255 if v >= 110 else 0)
    sup = Image.new('L', (W * HS, H * HS), 0)
    ox = (W - tw) // 2 * HS
    oy = (H - th) // 2 * HS
    sup.paste(hs, (ox, oy))

    cx = ox + (lcx - hx0) / hw * tws
    cy = oy + (lcy - hy0) / hh * ths
    rx = lrx / hw * tws * lens_scale
    ry = lry / hh * ths * lens_scale
    d = ImageDraw.Draw(sup)
    # Lens rim + glint; the dark glass stays as black negative space.
    d.ellipse([cx - rx, cy - ry, cx + rx, cy + ry],
              outline=255, width=max(2, int(2.2 * HS)))
    if lens_scale > 0.18:
        hr = max(2, int(2.2 * HS))
        hx = cx - rx * 0.38
        hy = cy - ry * 0.38
        d.ellipse([hx - hr, hy - hr, hx + hr, hy + hr], fill=255)
    # Tilt the eye unit around the lens: the circular lens stays put while the
    # angular housing pivots, instead of the whole graphic sliding sideways.
    if angle:
        sup = sup.rotate(angle, resample=Image.BICUBIC,
                         center=(cx, cy), fillcolor=0)
    return sup.resize((W, H), Image.LANCZOS).point(
        lambda v: 255 if v >= 110 else 0)


def main():
    outdir = os.path.join(ROOT, 'animations_preview')
    srcdir = os.path.join(ROOT, 'src', 'animation_plugin', 'src')
    os.makedirs(outdir, exist_ok=True)
    os.makedirs(srcdir, exist_ok=True)

    filled = {k: eye_mask(p, True) for k, p in EYE_PATHS.items()}
    outline_left, ow, oh = eye_mask(EYE_PATHS['left'], False)
    outline_right = [[outline_left[y][ow - 1 - x] for x in range(ow)]
                     for y in range(oh)]
    # open, squint, blink, reopen, second smaller blink and hold
    scales = [1.0, 1.0, 0.78, 0.48, 0.0, 0.48, 0.78, 1.0, 1.0, 0.9, 1.0, 1.0]

    left = [pack(render_eye(*filled['left'], s, EYE_SHIFT)) for s in scales]
    right = [pack(render_eye(*filled['right'], s, -EYE_SHIFT)) for s in scales]
    write_header(os.path.join(srcdir, 'spidey_frames.h'), 'SPIDEY',
                 [('left', left), ('right', right)])
    contact(left, os.path.join(outdir, 'spidey_left_contact.png'))
    contact(right, os.path.join(outdir, 'spidey_right_contact.png'))

    left_o = [pack(render_eye(outline_left, ow, oh, s, 0,
                              SPIDEY2_W, SPIDEY2_H)) for s in scales]
    right_o = [pack(render_eye(outline_right, ow, oh, s, 0,
                               SPIDEY2_W, SPIDEY2_H)) for s in scales]
    write_header(os.path.join(srcdir, 'spidey2_frames.h'), 'SPIDEY2',
                 [('left', left_o), ('right', right_o)])
    contact(left_o, os.path.join(outdir, 'spidey2_left_contact.png'))
    contact(right_o, os.path.join(outdir, 'spidey2_right_contact.png'))

    pumpkin = []
    for sc in (1.0, 1.02, 1.04, 1.06, 1.04, 1.02,
               1.0, 0.98, 0.96, 0.98, 1.0, 1.0):
        pumpkin.append(pack(render_pumpkin(sc)))
    write_header(os.path.join(srcdir, 'pumpkin_frames.h'), 'PUMPKIN',
                 [('', pumpkin)])
    contact(pumpkin, os.path.join(outdir, 'pumpkin_contact.png'))

    badge = []
    for sc in (1.0, 1.02, 1.04, 1.02, 1.0, 0.98, 1.0, 1.0):
        badge.append(pack(render_badge(sc)))
    write_header(os.path.join(srcdir, 'badge_frames.h'), 'BADGE',
                 [('', badge)])
    contact(badge, os.path.join(outdir, 'badge_contact.png'))

    walle_l, walle_r = [], []
    for angle, sc in ((0, 1), (-4, 1), (-8, 1), (-8, 1), (-4, 1), (0, 1),
                      (4, 1), (8, 1), (8, 1), (4, 1), (0, 1), (0, 0.6),
                      (0, 0.12), (0, 0.6), (0, 1), (0, 1)):
        walle_l.append(pack(render_walle(WALLE_PATHS['left'], angle, sc)))
        walle_r.append(pack(render_walle(WALLE_PATHS['right'], angle, sc)))
    write_header(os.path.join(srcdir, 'walle_frames.h'), 'WALLE',
                 [('left', walle_l), ('right', walle_r)])
    contact(walle_l, os.path.join(outdir, 'walle_left_contact.png'))
    contact(walle_r, os.path.join(outdir, 'walle_right_contact.png'))

    spider = []
    for i in range(16):
        t = i / 16.0
        pan = draw_spider(t, 8 * math.sin(2 * math.pi * t),
                          1.5 * math.sin(4 * math.pi * t))
        spider.append(pack(pan))
    write_header(os.path.join(srcdir, 'spider_frames.h'), 'SPIDER',
                 [('', spider)])
    contact(spider, os.path.join(outdir, 'spider_contact.png'))


if __name__ == '__main__':
    main()
