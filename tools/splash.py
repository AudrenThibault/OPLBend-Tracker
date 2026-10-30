#!/usr/bin/env python3
"""L'ecran d'accueil du FM-1 : une puce OPL3 (YMF262) rafistolee, du scotch dessus, des fils soudes
(le circuit bending), le nom du tracker et la version. Dessine ici, 240 x 240, 256 couleurs.
  splash.py SORTIE.h [VERSION]   l'image pour le firmware (palette RGB565 + indices)
  splash.py --png FICHIER.png    l'image seule, pour la voir
Besoin de Pillow."""
import math
import random
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ICI = Path(__file__).resolve().parent.parent
POLICE = ICI / "assets/fonts/IBMPlexMono-Medium.ttf"
L = 240
X = 4                                              # (dessine 4 fois plus grand, puis reduit : des bords lisses)


def police(t):
    return ImageFont.truetype(str(POLICE), t * X)


def scotch(img, cx, cy, lg, ht, angle, graine):
    """une bande de scotch beige, translucide, bords dechires, a l'angle donne"""
    r = random.Random(graine)
    w, h = lg * X, ht * X
    b = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(b)
    haut = [(0, 0)]
    for x in range(0, w + 1, 3 * X):             # le haut et le bas, un peu froisses
        haut.append((x, r.randint(0, 2 * X)))
    bas = [(x, h - r.randint(0, 2 * X)) for x in range(w, -1, -3 * X)]
    gauche = [(r.randint(0, 3 * X), y) for y in range(h, 0, -2 * X)]   # le bout dechire
    droite = [(w - r.randint(0, 3 * X), y) for y in range(0, h, 2 * X)]
    d.polygon(haut + droite + bas + gauche, fill=(222, 196, 140, 175))
    for k in range(6):                            # les plis : des traits plus clairs
        x = r.randint(0, w)
        d.line([(x, 0), (x + r.randint(-6, 6) * X, h)], fill=(250, 235, 200, 70), width=X)
    for k in range(40):                           # la poussiere collee
        x, y = r.randint(0, w), r.randint(0, h)
        d.point((x, y), fill=(120, 100, 70, 120))
    b = b.rotate(angle, expand=True, resample=Image.BICUBIC)
    img.alpha_composite(b, (int(cx * X - b.width / 2), int(cy * X - b.height / 2)))


def fil(d, pts, couleur, ep=2):
    """un fil souple (une courbe par les points), avec sa gaine plus sombre"""
    ombre = tuple(max(0, c - 70) for c in couleur)
    tr = []
    for i in range(len(pts) - 1):
        (x0, y0), (x1, y1) = pts[i], pts[i + 1]
        for k in range(12):
            t = k / 12
            tr.append(((x0 + (x1 - x0) * t) * X, (y0 + (y1 - y0) * t + math.sin(t * math.pi) * 4) * X))
    tr.append((pts[-1][0] * X, pts[-1][1] * X))
    d.line(tr, fill=ombre, width=(ep + 1) * X, joint="curve")
    d.line(tr, fill=couleur, width=ep * X, joint="curve")
    d.line([(x - X // 2, y - X // 2) for x, y in tr], fill=tuple(min(255, c + 70) for c in couleur), width=X // 2)


def soudure(d, x, y, r=3):
    d.ellipse([(x - r) * X, (y - r) * X, (x + r) * X, (y + r) * X], fill=(170, 170, 165))
    d.ellipse([(x - r + 1) * X, (y - r + 1) * X, (x) * X, (y) * X], fill=(225, 225, 220))


# Les couleurs : le circuit, ses pistes, les fils du circuit bending, le bandeau du nom.
ACTUEL = dict(pcb=(10, 34, 22), piste=(24, 70, 44), pastille=(150, 120, 60),
              fils=[(210, 40, 40), (240, 200, 40), (50, 120, 230), (60, 190, 90), (210, 40, 40)],
              bandeau=(8, 10, 14), filet=(222, 196, 140), nom=(240, 236, 228), sous=(232, 72, 82), version=(150, 150, 170))
# le theme aubergine (les couleurs du FM-1) : les fils aux couleurs des traits du tracker, le bandeau prune
FILS_THEME = [(255, 111, 184), (255, 160, 112), (76, 157, 255), (53, 224, 122), (176, 140, 255)]
BANDEAU_THEME = dict(bandeau=(8, 0, 16), filet=(176, 84, 156), nom=(255, 244, 251), sous=(255, 48, 72), version=(201, 163, 255))
LEGER = dict(ACTUEL, fils=FILS_THEME, **BANDEAU_THEME)                       # le circuit vert garde
VIOLET = dict(LEGER, pcb=(30, 12, 40), piste=(66, 34, 86), pastille=(196, 160, 90))   # un circuit violet (OSH Park)
PRUNE = dict(LEGER, pcb=(16, 6, 22), piste=(46, 22, 58), pastille=(170, 136, 80))     # plus sombre, presque le fond
THEME = PRUNE                                     # (choisi : le circuit prune, aux couleurs du theme)


def dessine(version="", t=None):
    t = t or THEME
    img = Image.new("RGBA", (L * X, L * X), (0, 0, 0, 255))
    d = ImageDraw.Draw(img)
    r = random.Random(262)
    # le circuit : vert sombre, des pistes de cuivre
    d.rectangle([0, 0, L * X, L * X], fill=t["pcb"])
    for k in range(26):
        y = r.randint(0, 165) * X
        x0 = r.randint(-20, 200) * X
        x1 = x0 + r.randint(30, 120) * X
        d.line([(x0, y), (x1, y), (x1 + 12 * X, y + 12 * X)], fill=t["piste"], width=2 * X)
        d.ellipse([x1 + 10 * X, y + 10 * X, x1 + 16 * X, y + 16 * X], outline=t["pastille"], width=X)
    img = img.filter(ImageFilter.GaussianBlur(X))
    d = ImageDraw.Draw(img)

    # la puce : un boitier DIP noir, 2 x 12 pattes, un peu de travers
    puce = Image.new("RGBA", (150 * X, 96 * X), (0, 0, 0, 0))
    p = ImageDraw.Draw(puce)
    for i in range(12):                           # les pattes, en haut et en bas
        x = (13 + i * 10.5) * X
        for y0, y1 in ((0, 16), (80, 96)):
            p.rectangle([x, y0 * X, x + 5 * X, y1 * X], fill=(190, 192, 196))
            p.rectangle([x, y0 * X, x + 2 * X, y1 * X], fill=(235, 236, 240))
    p.rounded_rectangle([4 * X, 12 * X, 146 * X, 84 * X], radius=4 * X, fill=(26, 26, 30))
    p.rounded_rectangle([6 * X, 14 * X, 144 * X, 82 * X], radius=3 * X, fill=(40, 40, 46))
    p.ellipse([10 * X, 44 * X, 18 * X, 52 * X], fill=(22, 22, 26))   # l'encoche
    p.ellipse([20 * X, 66 * X, 25 * X, 71 * X], fill=(30, 30, 34))   # le point de la patte 1
    p.text((32 * X, 24 * X), "YMF262-M", font=police(17), fill=(170, 170, 176))
    p.text((32 * X, 46 * X), "OPL3  FM", font=police(11), fill=(130, 130, 138))
    p.text((32 * X, 62 * X), "9412 JAPAN", font=police(8), fill=(100, 100, 108))
    p.line([(96 * X, 16 * X), (118 * X, 52 * X), (110 * X, 82 * X)], fill=(70, 70, 78), width=X)   # une felure
    puce = puce.rotate(-4, expand=True, resample=Image.BICUBIC)
    ombre = Image.new("RGBA", puce.size, (0, 0, 0, 0))
    ombre.paste((0, 0, 0, 150), mask=puce.split()[3])
    ombre = ombre.filter(ImageFilter.GaussianBlur(4 * X))
    ox, oy = int((L * X - puce.width) / 2), 26 * X
    img.alpha_composite(ombre, (ox + 5 * X, oy + 7 * X))
    img.alpha_composite(puce, (ox, oy))
    d = ImageDraw.Draw(img)

    # les fils du circuit bending : soudes sur des pattes, ils partent hors de l'image
    f1, f2, f3, f4, f5 = t["fils"]
    fil(d, [(58, 33), (40, 18), (8, 10), (-10, 14)], f1)
    soudure(d, 58, 33)
    fil(d, [(173, 125), (200, 140), (232, 128), (250, 140)], f2)
    soudure(d, 173, 125)
    fil(d, [(110, 28), (128, 6), (170, 4), (200, -6)], f3)
    soudure(d, 110, 28)
    fil(d, [(78, 129), (60, 150), (20, 150), (-6, 160)], f4)
    soudure(d, 78, 129)
    fil(d, [(140, 129), (150, 142), (120, 152)], f5, 1)   # un pont entre deux pattes
    soudure(d, 140, 129, 2)
    soudure(d, 120, 152, 2)

    # le scotch, par-dessus
    scotch(img, 72, 58, 90, 22, 28, 1)
    scotch(img, 168, 96, 100, 20, -18, 2)
    scotch(img, 205, 40, 52, 16, 62, 3)
    d = ImageDraw.Draw(img)

    # le nom, et la version
    def centre(y, s, t, coul, ombre=True):
        f = police(t)
        w = d.textlength(s, font=f)
        x = (L * X - w) / 2
        if ombre:
            d.text((x + 2 * X, y * X + 2 * X), s, font=f, fill=(0, 0, 0))
        d.text((x, y * X), s, font=f, fill=coul)
    d.rectangle([0, 166 * X, L * X, L * X], fill=t["bandeau"])
    d.line([(0, 166 * X), (L * X, 166 * X)], fill=t["filet"], width=X)
    centre(170, "OPLBEND", 30, t["nom"])
    centre(206, "TRACKER", 13, t["sous"])
    if version:
        centre(224, version, 9, t["version"], False)
    return img.resize((L, L), Image.LANCZOS).convert("RGB")


def en_tete(img, sortie, version):
    q = img.quantize(colors=256, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG)
    pal = q.getpalette()[: 256 * 3]
    pal += [0] * (768 - len(pal))
    idx = q.tobytes()
    lignes = ["/* l'ecran d'accueil (tools/splash.py) : 240 x 240, une palette de 256 couleurs RGB565 */",
              "static const uint16_t SPLASH_PAL[256] = {"]
    vals = []
    for i in range(256):
        rr, gg, bb = pal[3 * i: 3 * i + 3]
        vals.append("0x%04X" % (((rr >> 3) << 11) | ((gg >> 2) << 5) | (bb >> 3)))
    for i in range(0, 256, 12):
        lignes.append("    " + ", ".join(vals[i:i + 12]) + ",")
    lignes.append("};")
    lignes.append("static const uint8_t SPLASH_IDX[240 * 240] = {")
    for i in range(0, len(idx), 24):
        lignes.append("    " + ",".join(str(b) for b in idx[i:i + 24]) + ",")
    lignes.append("};")
    Path(sortie).parent.mkdir(parents=True, exist_ok=True)
    Path(sortie).write_text("\n".join(lignes) + "\n")


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "--png":
        dessine(sys.argv[3] if len(sys.argv) > 3 else "V0.2").save(sys.argv[2])
        return
    version = sys.argv[2] if len(sys.argv) > 2 else ""
    en_tete(dessine(version), sys.argv[1], version)


if __name__ == "__main__":
    main()
