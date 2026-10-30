#!/usr/bin/env python3
"""Les images du mode D-PAD (SEQ, le mode ou le clavier sert de croix et de boutons), aux couleurs du theme
aubergine :
  - l'ECRAN D-PAD (un cadre au milieu ; autour, la page SONG se voit), en passant dans le mode : le clavier du FM-1, ce que fait chaque touche,
    « Press a key » et « -> ENV for help », au milieu de l'ecran ;
  - le CLAVIER seul (240 x CLAV_H), en haut des pages HELP du D-PAD : les raccourcis s'y lisent touche en main.
Les touches sont basses, comme celles du vrai clavier. Dessinees en x4, reduites, 16 couleurs (une palette
fixe), chaque ligne en series de points de meme couleur (comme tools/gifs.py).
  dpad.py SORTIE.h [APERCU.png]
Besoin de Pillow."""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ICI = Path(__file__).resolve().parent.parent
X = 4
F = str(ICI / "assets/fonts/IBMPlexMono-Medium.ttf")
FOND = (8, 0, 16); BLANCHE = (122, 79, 200); NOIRE = (58, 31, 92); BORD = (20, 8, 26)
TXT = (245, 207, 230); PALE = (150, 118, 160)
BLEU = (76, 157, 255); PECHE = (255, 160, 112); VERT = (53, 224, 122); ROUGE = (255, 48, 72); TURQ = (111, 227, 224)
CLAV_H = 46                                       # (le clavier des pages HELP, en points)


def pol(t):
    return ImageFont.truetype(F, int(t * X))


def clavier(d, y0, H, BH, taille):
    """les deux bouts du clavier : a gauche fa .. do (n 0..7), a droite re .. sol (n 21..26), chaque touche
    qui sert de bouton a la couleur de son role"""
    W = 24

    def ttxt(cx, cy, s, c, t):
        f = pol(t)
        w = d.textlength(s, font=f)
        d.text((cx * X - w / 2, cy * X), s, font=f, fill=c)

    def morceau(x0, blanches, noires):
        for i, (s, c) in enumerate(blanches):
            x = x0 + i * W
            d.rounded_rectangle([x * X, y0 * X, (x + W - 2) * X, (y0 + H) * X], radius=3 * X, fill=c or BLANCHE,
                                outline=BORD, width=X)
            if s:
                t = taille if len(s) < 3 else taille * 0.66
                ttxt(x + (W - 2) / 2, y0 + H - t - 5, s, BORD, t)
        for i, (s, c) in noires.items():
            x = x0 + i * W + W - 8
            d.rounded_rectangle([x * X, y0 * X, (x + 14) * X, (y0 + BH) * X], radius=2 * X, fill=c or NOIRE,
                                outline=BORD, width=X)
            if s == "PLAY":
                m = y0 + BH - taille * 0.55 - 4
                d.polygon([((x + 3) * X, (m - 5) * X), ((x + 3) * X, (m + 5) * X), ((x + 12) * X, m * X)], fill=BORD)
            elif s:
                ttxt(x + 7, y0 + BH - taille - 5, s, BORD, taille * 0.95)

    morceau(6, [("←", BLEU), ("↓", BLEU), ("→", BLEU), ("SEL", PECHE), ("SEL", PECHE)],
            {0: ("↑", BLEU), 1: ("", None), 2: ("", None)})
    d.text((126 * X, (y0 + H / 2 - 8) * X), "..", font=pol(12), fill=TXT)
    morceau(138, [("SEL", PECHE), ("SEL", PECHE), ("B", ROUGE), ("", None)], {0: ("PLAY", TURQ), 2: ("A", VERT)})


CADRE = (2, 56, 237, 186)                         # (l'ecran D-PAD : un cadre, la page SONG visible autour)
TRANSP = (255, 0, 255)                            # (hors du cadre : transparent, la page se voit)


def ecran():
    img = Image.new("RGB", (240 * X, 240 * X), FOND)
    d = ImageDraw.Draw(img)

    def centre(y, t, taille, coul):
        f = pol(taille)
        w = d.textlength(t, font=f)
        d.text(((240 * X - w) / 2, y * X), t, font=f, fill=coul)
    # le clavier et « ENV for help », centres en hauteur (l'annonce « D-PAD » les couvre le temps de s'afficher,
    # comme sur les autres pages)
    clavier(d, 66, 54, 33, 13)
    centre(130, "Press a key", 15, PALE)
    centre(156, "→ ENV for help", 16, TXT)
    return img.resize((240, 240), Image.LANCZOS)


def cadre(q):
    """hors du cadre : l'indice transparent ; son bord : le rouge des annonces (comme KEYBOARD, DRUM MACHINE)"""
    t, rouge = FIXE.index(TRANSP), FIXE.index(ROUGE)
    x0, y0, x1, y1 = CADRE
    px = q.load()
    for y in range(240):
        for x in range(240):
            if not (x0 <= x <= x1 and y0 <= y <= y1):
                px[x, y] = t
            elif x in (x0, x1) or y in (y0, y1):
                px[x, y] = rouge
    return q


def bandeau():
    img = Image.new("RGB", (240 * X, CLAV_H * X), FOND)
    clavier(ImageDraw.Draw(img), 1, CLAV_H - 3, 26, 11)
    return img.resize((240, CLAV_H), Image.LANCZOS)


# (une palette fixe : les couleurs du dessin, et des demi-teintes pour le bord des lettres)
def mi(c): return tuple((a + b) // 2 for a, b in zip(c, FOND))
def entre(c, e): return tuple((a + b) // 2 for a, b in zip(c, e))
FIXE = [FOND, BLANCHE, NOIRE, BORD, TXT, PALE, BLEU, PECHE, VERT, ROUGE, TURQ, mi(TXT), mi(PECHE), TRANSP,
        entre(PECHE, BORD), entre(BLEU, BORD)]   # (les deux dernieres : les lettres sombres sur les touches)


def series(img, nom, retouche=None):
    """l'image en 16 couleurs, chaque ligne en series (longueur - 1) << 4 | couleur, et ou commence chaque ligne"""
    modele = Image.new("P", (1, 1))
    modele.putpalette([v for c in FIXE for v in c] + [0] * (768 - 48))
    q = img.quantize(palette=modele, dither=Image.NONE)
    if retouche:
        q = retouche(q)
    w, h = q.size
    idx = q.tobytes()
    octets, debuts = bytearray(), []
    for y in range(h):
        debuts.append(len(octets))
        ligne = idx[y * w:(y + 1) * w]
        x = 0
        while x < w:
            n = 1
            while x + n < w and n < 16 and ligne[x + n] == ligne[x]:
                n += 1
            octets.append((n - 1) << 4 | (ligne[x] & 15))
            x += n
    lignes = ["static const uint8_t %s[%d] = {" % (nom, len(octets))]
    for i in range(0, len(octets), 32):
        lignes.append("    " + ",".join(str(b) for b in octets[i:i + 32]) + ",")
    lignes.append("};")
    lignes.append("static const uint16_t %s_LIGNES[%d] = {" % (nom, h))
    for i in range(0, h, 24):
        lignes.append("    " + ",".join(str(b) for b in debuts[i:i + 24]) + ",")
    lignes.append("};")
    return lignes, q, len(octets)


def main():
    p565 = [((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3) for r, g, b in FIXE]
    lignes = ["/* les images du D-PAD (tools/dpad.py) : 16 couleurs RGB565, chaque ligne en series (longueur - 1) << 4 | couleur */",
              "static const uint16_t FM1_DPAD_PAL[16] = {" + ",".join("0x%04X" % c for c in p565) + "};",
              "#define FM1_CLAV_H %d" % CLAV_H,
              "#define FM1_DPAD_TRANSP %d   /* (l'indice transparent : la page se voit) */" % FIXE.index(TRANSP)]
    l1, q1, n1 = series(ecran(), "FM1_DPAD", cadre)
    l2, q2, n2 = series(bandeau(), "FM1_CLAV")
    Path(sys.argv[1]).parent.mkdir(parents=True, exist_ok=True)
    Path(sys.argv[1]).write_text("\n".join(lignes + l1 + l2) + "\n")
    if len(sys.argv) > 2:
        apercu = Image.new("RGB", (240, 240 + 8 + CLAV_H), (255, 255, 255))
        apercu.paste(q1.convert("RGB"), (0, 0))
        apercu.paste(q2.convert("RGB"), (0, 248))
        apercu.save(sys.argv[2])
    print("dpad : %d + %d octets" % (n1, n2))


if __name__ == "__main__":
    main()
