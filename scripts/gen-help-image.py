#!/usr/bin/env python3
"""
Generate the launcher's controller help screen (shown on a tap of PS).

    python gen-help-image.py <launcher.ttf> <DejaVuSans.ttf> <DejaVuSans-Bold.ttf> <out.png>

Writes a 1920x1080 PNG; the launcher scales it to the screen. The image is
committed (rootfs_overlay/usr/share/retroopi/help-controller.png), so this only
needs re-running when a binding changes. Needs Pillow (Windows' python has it;
WSL's does not).

EVERY binding below must match the code that implements it:
  launcher           launcher/launcher.c main loop
  PS + button        retroarch.cfg  input_*_btn (hotkey = input_enable_hotkey_btn 10)
  PS + Up/Down       tools/volumed.c (ALSA volume, works in the launcher too)
"""
import math
import sys

from PIL import Image, ImageDraw, ImageFont

W, H = 1920, 1080
S = 2                                   # supersampling factor

BG = (16, 24, 40)                       # Midnight theme
PANEL = (30, 41, 59)
TEXT = (226, 232, 240)
DIM = (148, 163, 184)
HOT = (251, 191, 36)                    # "hold PS +" colour
LINE = (100, 116, 139)
BODY = (51, 65, 85)
BODY_EDGE = (100, 116, 139)
KEY = (22, 30, 46)
KEY_EDGE = (71, 85, 105)
C_TRI, C_CIR, C_CRO, C_SQU = (64, 224, 160), (255, 107, 107), (122, 167, 255), (255, 138, 216)

# (launcher action or None, in-game "PS +" action or None)
CALLOUTS_LEFT = [
    ('l1',    'Page up',            'Load state'),
    ('up',    'Move up / down',     'Volume up / down'),
    ('left',  'Jump to letter',     'Save slot - / +'),
    ('lstick', 'Works as the D-pad', None),
]
# Order matters: it keeps the connector lines from crossing each other or a
# button. Square is last; its line leaves downward, between Cross and the stick.
CALLOUTS_RIGHT = [
    ('r1',    'Page down',          'Save state'),
    ('tri',   'Favorite',           'RetroArch menu'),
    ('cir',   'Back',               None),
    ('cro',   'Open / Play',        None),
    ('squ',   'Search',             None),
]


def s(v):
    return int(round(v * S))


def ellipse_poly(cx, cy, rx, ry, angle_deg, n=90):
    a = math.radians(angle_deg)
    pts = []
    for i in range(n):
        t = 2 * math.pi * i / n
        x, y = rx * math.cos(t), ry * math.sin(t)
        pts.append((s(cx + x * math.cos(a) - y * math.sin(a)),
                    s(cy + x * math.sin(a) + y * math.cos(a))))
    return pts


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    f_main, f_sym, f_bold, out = sys.argv[1:]
    font = lambda path, px: ImageFont.truetype(path, s(px))
    title_f = font(f_bold, 46)
    sub_f = font(f_main, 26)
    main_f = font(f_main, 29)
    hot_f = font(f_main, 26)
    badge_f = font(f_bold, 19)
    sym_f = font(f_bold, 34)
    small_f = font(f_main, 24)

    img = Image.new('RGB', (s(W), s(H)), BG)
    d = ImageDraw.Draw(img)

    # ---- controller --------------------------------------------------------
    cx = W / 2
    # shoulder buttons (L2/R2 behind, L1/R1 in front)
    for sx in (-1, 1):
        x0 = cx + sx * 175 - 95
        d.rounded_rectangle([s(x0 + sx * 12), s(292), s(x0 + 190 + sx * 12), s(336)], s(16),
                            fill=KEY, outline=KEY_EDGE, width=s(2))
        d.rounded_rectangle([s(x0), s(322), s(x0 + 190), s(368)], s(16),
                            fill=BODY, outline=BODY_EDGE, width=s(3))
    d.text((s(cx - 175), s(345)), 'L1', font=badge_f, fill=DIM, anchor='mm')
    d.text((s(cx + 175), s(345)), 'R1', font=badge_f, fill=DIM, anchor='mm')

    # body: grips + centre block, outline drawn by painting a grown copy first
    def body(grow, colour):
        for sx in (-1, 1):
            d.polygon(ellipse_poly(cx + sx * 250, 600, 118 + grow, 215 + grow, -sx * 18), fill=colour)
        d.rounded_rectangle([s(cx - 330 - grow), s(355 - grow), s(cx + 330 + grow), s(640 + grow)],
                            s(95), fill=colour)
    body(4, BODY_EDGE)
    body(0, BODY)

    # D-pad
    dx, dy = cx - 250, 470
    arm = 34
    for (ax, ay) in ((0, -1), (0, 1), (-1, 0), (1, 0)):
        x0 = dx + ax * 44 - arm / 2 - (0 if ax == 0 else 10)
        y0 = dy + ay * 44 - arm / 2 - (0 if ay == 0 else 10)
        w_, h_ = (arm + 20, arm) if ax else (arm, arm + 20)
        d.rounded_rectangle([s(x0), s(y0), s(x0 + w_), s(y0 + h_)], s(6),
                            fill=KEY, outline=KEY_EDGE, width=s(2))
    d.rectangle([s(dx - arm / 2), s(dy - arm / 2), s(dx + arm / 2), s(dy + arm / 2)], fill=KEY)
    for (ax, ay) in ((0, -1), (0, 1), (-1, 0), (1, 0)):   # arrow marks
        px, py = dx + ax * 48, dy + ay * 48
        tri = [(px + ax * 10, py + ay * 10),
               (px - ax * 4 + ay * 9, py - ay * 4 + ax * 9),
               (px - ax * 4 - ay * 9, py - ay * 4 - ax * 9)]
        d.polygon([(s(x), s(y)) for x, y in tri], fill=DIM)

    # face buttons
    fx, fy, off, r = cx + 250, 470, 58, 29
    face = {'tri': (fx, fy - off), 'cir': (fx + off, fy), 'cro': (fx, fy + off), 'squ': (fx - off, fy)}
    for k, (bx, by) in face.items():
        d.ellipse([s(bx - r), s(by - r), s(bx + r), s(by + r)], fill=KEY, outline=KEY_EDGE, width=s(2))
    bx, by = face['tri']
    d.polygon([(s(bx), s(by - 13)), (s(bx + 13), s(by + 10)), (s(bx - 13), s(by + 10))],
              outline=C_TRI, width=s(4))
    bx, by = face['cir']
    d.ellipse([s(bx - 13), s(by - 13), s(bx + 13), s(by + 13)], outline=C_CIR, width=s(4))
    bx, by = face['cro']
    d.line([(s(bx - 12), s(by - 12)), (s(bx + 12), s(by + 12))], fill=C_CRO, width=s(4))
    d.line([(s(bx - 12), s(by + 12)), (s(bx + 12), s(by - 12))], fill=C_CRO, width=s(4))
    bx, by = face['squ']
    d.rectangle([s(bx - 11), s(by - 11), s(bx + 11), s(by + 11)], outline=C_SQU, width=s(4))

    # select / start / PS
    sel = (cx - 72, 455)
    sta = (cx + 72, 455)
    d.rounded_rectangle([s(sel[0] - 22), s(sel[1] - 9), s(sel[0] + 22), s(sel[1] + 9)], s(9), fill=KEY)
    d.polygon([(s(sta[0] - 16), s(sta[1] - 12)), (s(sta[0] + 20), s(sta[1])), (s(sta[0] - 16), s(sta[1] + 12))], fill=KEY)
    d.text((s(sel[0]), s(sel[1] + 30)), 'SELECT', font=badge_f, fill=DIM, anchor='mm')
    d.text((s(sta[0]), s(sta[1] + 30)), 'START', font=badge_f, fill=DIM, anchor='mm')
    ps = (cx, 545)
    d.ellipse([s(ps[0] - 30), s(ps[1] - 30), s(ps[0] + 30), s(ps[1] + 30)], fill=KEY, outline=HOT, width=s(3))
    d.text((s(ps[0]), s(ps[1])), 'PS', font=badge_f, fill=HOT, anchor='mm')

    # sticks
    ls = (cx - 120, 590)
    rs = (cx + 120, 590)
    for (x, y) in (ls, rs):
        d.ellipse([s(x - 52), s(y - 52), s(x + 52), s(y + 52)], fill=KEY, outline=KEY_EDGE, width=s(2))
        d.ellipse([s(x - 34), s(y - 34), s(x + 34), s(y + 34)], fill=(37, 49, 69))

    anchors = {
        'l1': (cx - 175, 322), 'r1': (cx + 175, 322),
        'up': (dx, dy - 71), 'left': (dx - 71, dy),
        'lstick': (ls[0] - 40, ls[1] + 30),
        'tri': (face['tri'][0], face['tri'][1] - r), 'cir': (face['cir'][0] + r, face['cir'][1]),
        'cro': (face['cro'][0] + r, face['cro'][1]), 'squ': (face['squ'][0], face['squ'][1] + r),
        'ps': (ps[0], ps[1] + 30), 'start': (sta[0], sta[1] - 12),
    }

    # ---- callouts ----------------------------------------------------------
    BOX_W = 440

    def box(x, y, launcher, hot, w=BOX_W, badge=True):
        """Callout box at (x, y) top-left; returns its height."""
        lines = (1 if launcher else 0) + (1 if hot else 0)
        h = 22 + 40 * lines
        d.rounded_rectangle([s(x), s(y), s(x + w), s(y + h)], s(14), fill=PANEL)
        ty = y + 12
        if launcher:
            d.text((s(x + 20), s(ty)), launcher, font=main_f, fill=TEXT)
            ty += 40
        if hot and not badge:
            d.text((s(x + 20), s(ty + 2)), hot, font=hot_f, fill=HOT)
        elif hot:
            bw = 58
            d.rounded_rectangle([s(x + 20), s(ty + 3), s(x + 20 + bw), s(ty + 33)], s(8), fill=HOT)
            d.text((s(x + 20 + bw / 2), s(ty + 18)), 'PS +', font=badge_f, fill=BG, anchor='mm')
            d.text((s(x + 20 + bw + 12), s(ty + 2)), hot, font=hot_f, fill=HOT)
        return h

    def connect(p0, p1, via=None):
        pts = [p0] + ([via] if via else []) + [p1]
        d.line([(s(x), s(y)) for x, y in pts], fill=LINE, width=s(2), joint='curve')
        d.ellipse([s(p1[0] - 6), s(p1[1] - 6), s(p1[0] + 6), s(p1[1] + 6)], fill=TEXT)

    def column(items, x, y0, gap, side):
        y = y0
        for key, launcher, hot in items:
            h = box(x, y, launcher, hot)
            edge = (x + BOX_W, y + h / 2) if side == 'left' else (x, y + h / 2)
            # Square sits behind Triangle/Circle/Cross as seen from the right:
            # drop straight down past Cross, then across.
            via = (anchors[key][0], edge[1]) if key == 'squ' else None
            connect(edge, anchors[key], via)
            y += h + gap

    column(CALLOUTS_LEFT, 50, 200, 34, 'left')
    column(CALLOUTS_RIGHT, W - 50 - BOX_W, 170, 26, 'right')

    # PS: under the controller, its line rising between the sticks
    x, y = cx - BOX_W / 2, 872
    box(x, y, 'Tap: show this help', 'Hold + a button: game hotkeys', badge=False)
    connect((cx, y), anchors['ps'])
    # Start: above the controller, between the shoulder buttons
    w = 300
    x, y = cx - w / 2, 170
    h = box(x, y, None, 'Exit game', w)
    connect((sta[0], y + h), anchors['start'])

    # ---- title / footer ----------------------------------------------------
    d.text((s(cx), s(62)), 'CONTROLS', font=title_f, fill=TEXT, anchor='mm')
    d.text((s(cx), s(112)), 'White: in the launcher        Amber: in a game, hold PS and press',
           font=sub_f, fill=DIM, anchor='mm')
    d.text((s(cx), s(1045)), 'Press any button to close', font=small_f, fill=DIM, anchor='mm')

    img.resize((W, H), Image.LANCZOS).save(out, optimize=True)
    print(f'gen-help-image: {out}')


if __name__ == '__main__':
    main()
