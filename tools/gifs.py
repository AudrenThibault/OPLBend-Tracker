#!/usr/bin/env python3
"""Les animations de la page CIRCUIT BENDING (dessinees par tools/anims.py, dans DOSSIER) pour le firmware : chacun reduit a 80 x 60 (affiche en
120 x 90, un quart de l'ecran), 16 couleurs, 20 images au plus, chaque ligne en series de points de meme couleur.
  gifs.py SORTIE.h DOSSIER
Besoin de Pillow."""
import sys
from pathlib import Path

from PIL import Image

ICI = Path(__file__).resolve().parent.parent
GIFS = ["chat.gif", "tortue.gif", "danse.gif", "nunchaku.gif"]   # (0 GLITCH, 1 DRONE, 2 NOISE, 3 CHAOS)
L, H, MAX = 80, 60, 20


def images(chemin):
    """des images QUI SE SUIVENT, au moins 10 par seconde (une sur deux si le GIF en a plus) : un extrait a
    sa vraie vitesse, qui boucle, plutot que tout le GIF etire ; 20 au plus. Rend aussi leur duree."""
    im = Image.open(chemin)
    n = getattr(im, "n_frames", 1)
    tout, durees = [], []
    for k in range(n):
        im.seek(k)
        durees.append(im.info.get("duration", 100) or 100)
        tout.append(im.convert("RGB"))
    moyenne = sum(durees) / n
    pas = max(1, int(100 // moyenne))              # (une image tous les 100 ms au plus)
    garde = list(range(0, n, pas))[:MAX]
    duree = sum(durees[i] for i in garde) * pas if pas > 1 else sum(durees[i] for i in garde)
    imgs = [tout[i] for i in garde]
    w, h = imgs[0].size
    s = min(L / w, H / h)
    tw, th = max(2, int(w * s)) & ~1, max(2, int(h * s))
    return [g.resize((tw, th), Image.LANCZOS) for g in imgs], duree


def main():
    global SOURCE
    SOURCE = Path(sys.argv[2])
    lignes = ["/* les GIF de la page CIRCUIT BENDING (tools/gifs.py) : 16 couleurs RGB565, 4 bits par point */",
              "typedef struct { uint8_t l, h, n; uint16_t ms; uint16_t pal[16]; const uint8_t *pts; const uint16_t *lignes; } fm1_gif_t;"]
    defs = []
    for k, nom in enumerate(GIFS):
        if not (SOURCE / nom).exists():   # (absent : pas de GIF, une image vide d'un point)
            lignes.append("static const uint8_t FM1_GIF%d[1] = {16};" % k)
            lignes.append("static const uint16_t FM1_GIF%d_LIGNES[1] = {0};" % k)
            defs.append("    {2, 1, 1, 1, {0}, FM1_GIF%d, FM1_GIF%d_LIGNES}," % (k, k))
            print("gif %s : absent" % nom)
            continue
        imgs, duree = images(SOURCE / nom)
        tw, th = imgs[0].size
        # une palette commune a toutes les images : la planche, quantifiee une fois
        planche = Image.new("RGB", (tw, th * len(imgs)))
        for i, g in enumerate(imgs):
            planche.paste(g, (0, i * th))
        q = planche.quantize(colors=16, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG)
        pal = (q.getpalette() or [])[:48]
        pal += [0] * (48 - len(pal))
        p565 = [((pal[3 * i] >> 3) << 11) | ((pal[3 * i + 1] >> 2) << 5) | (pal[3 * i + 2] >> 3) for i in range(16)]
        idx = q.tobytes()
        # Chaque ligne en series : un octet par serie, (longueur - 1) << 4 | couleur (16 points au plus) ;
        # et ou commence chaque ligne (2 octets), pour en dessiner une sans lire les autres.
        octets, debuts = bytearray(), []
        for y in range(th * len(imgs)):
            debuts.append(len(octets))
            ligne = idx[y * tw:(y + 1) * tw]
            x = 0
            while x < tw:
                n = 1
                while x + n < tw and n < 16 and ligne[x + n] == ligne[x]:
                    n += 1
                octets.append((n - 1) << 4 | (ligne[x] & 15))
                x += n
        assert len(octets) < 65536
        lignes.append("static const uint8_t FM1_GIF%d[%d] = {" % (k, len(octets)))
        for i in range(0, len(octets), 32):
            lignes.append("    " + ",".join(str(b) for b in octets[i:i + 32]) + ",")
        lignes.append("};")
        lignes.append("static const uint16_t FM1_GIF%d_LIGNES[%d] = {" % (k, len(debuts)))
        for i in range(0, len(debuts), 24):
            lignes.append("    " + ",".join(str(b) for b in debuts[i:i + 24]) + ",")
        lignes.append("};")
        defs.append("    {%d, %d, %d, %d, {%s}, FM1_GIF%d, FM1_GIF%d_LIGNES}," % (tw, th, len(imgs), min(65535, duree),
                                                                ",".join("0x%04X" % c for c in p565), k, k))
        print("gif %s : %d x %d, %d images, %d ms, %d octets" % (nom, tw, th, len(imgs), duree, len(octets)))
    lignes.append("static const fm1_gif_t FM1_GIFS[%d] = {" % len(GIFS))
    lignes += defs
    lignes.append("};")
    Path(sys.argv[1]).parent.mkdir(parents=True, exist_ok=True)
    Path(sys.argv[1]).write_text("\n".join(lignes) + "\n")


if __name__ == "__main__":
    main()
