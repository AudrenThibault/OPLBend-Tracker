#!/usr/bin/env python3
"""Les animations de la page CIRCUIT BENDING, dessinees ici (pixel art, 80 x 60, peu de couleurs) : elles sont
au projet, et se publient avec lui. Un clin d'oeil a trois GIF connus, sans rien en reprendre :
  chat   un chat en gros plan qui hoche la tete, trop pres de l'objectif ;
  tortue une tortue qui fait des tours a la barre fixe, devant la ville ;
  danse  le gars en rouge qui ondule n'importe comment, la camera zoome jusqu'a le perdre ;
  nunchaku un gars se prend son nunchaku dans la figure et tombe sur le cote.
  anims.py DOSSIER      ecrit DOSSIER/chat.gif, tortue.gif, danse.gif (et une planche, apercu.png)
Besoin de Pillow."""
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw

L, H = 80, 60


def chat(t, haut=0):
    """t de 0 a 1 : la tete monte, descend et penche ; les yeux clignent une fois. (haut : de la place en plus
    au-dessus, pour que ses oreilles tiennent en entier : l'ecran d'accueil, tools/splash.py)"""
    im = Image.new("RGB", (L, H + haut), (24, 22, 30))
    d = ImageDraw.Draw(im)
    a = math.sin(t * 2 * math.pi)
    cx, cy = 40 + int(4 * math.sin(t * 4 * math.pi)), 40 + haut + int(7 * a)
    r = 34 + int(3 * math.cos(t * 2 * math.pi))      # (trop pres : il deborde, s'approche et recule)
    creme, rose, gris = (236, 226, 210), (232, 160, 170), (70, 64, 70)
    for sx in (-1, 1):                                # les oreilles
        d.polygon([(cx + sx * (r - 4), cy - r + 10), (cx + sx * (r - 2), cy - r - 10),
                   (cx + sx * (r - 18), cy - r + 2)], fill=creme)
        d.polygon([(cx + sx * (r - 7), cy - r + 7), (cx + sx * (r - 5), cy - r - 4),
                   (cx + sx * (r - 14), cy - r + 3)], fill=rose)
    d.ellipse([cx - r, cy - r + 4, cx + r, cy + r], fill=creme)
    yeux = 1 if 0.45 < t < 0.55 else 6                # (un clignement)
    for sx in (-1, 1):
        ex = cx + sx * 10
        d.ellipse([ex - 5, cy - 6 - yeux // 2, ex + 5, cy - 6 + yeux // 2 + 1], fill=gris)
        if yeux > 2:
            d.point((ex - 2, cy - 7), fill=(250, 250, 250))
    d.polygon([(cx - 3, cy + 3), (cx + 3, cy + 3), (cx, cy + 6)], fill=rose)
    d.line([(cx, cy + 6), (cx - 4, cy + 10)], fill=gris)
    d.line([(cx, cy + 6), (cx + 4, cy + 10)], fill=gris)
    for sx in (-1, 1):                                # les moustaches
        for k in (-1, 1):
            d.line([(cx + sx * 8, cy + 6 + k), (cx + sx * 22, cy + 4 + 3 * k)], fill=(200, 190, 180))
    return im


def tortue(t):
    """t de 0 a 1 : un tour complet a la barre fixe, les mains sur la barre, devant la ville"""
    im = Image.new("RGB", (L, H), (120, 170, 220))
    d = ImageDraw.Draw(im)
    hauteurs = [18, 26, 14, 30, 22, 16, 28, 20]
    for i in range(9):                                # la ville, au fond
        x, h = i * 10 - 2, hauteurs[i % len(hauteurs)]
        d.rectangle([x, H - h, x + 8, H], fill=(70, 80, 100))
        for y in range(H - h + 3, H - 2, 5):
            d.point((x + 2, y), fill=(240, 220, 120))
            d.point((x + 5, y), fill=(240, 220, 120))
    bx, by = 40, 24                                   # la barre et ses montants
    d.line([(14, by), (66, by)], fill=(210, 210, 215), width=2)
    for x in (14, 66):
        d.line([(x, by), (x, H)], fill=(160, 160, 170), width=2)
    # la tortue, dessinee la tete vers la barre, puis tournee autour d'elle
    spr = Image.new("RGBA", (24, 30), (0, 0, 0, 0))
    s2 = ImageDraw.Draw(spr)
    vert, carap = (90, 170, 80), (130, 90, 50)
    s2.ellipse([8, 4, 15, 11], fill=vert)            # la tete
    s2.point((10, 7), fill=(0, 0, 0)); s2.point((13, 7), fill=(0, 0, 0))
    s2.ellipse([3, 9, 20, 26], fill=carap)           # la carapace
    for k in (8, 12, 16):
        s2.line([(6, k + 4), (17, k + 4)], fill=(90, 60, 30))
    s2.line([(6, 25), (4, 29)], fill=vert, width=2)  # les pattes arriere
    s2.line([(17, 25), (19, 29)], fill=vert, width=2)
    ang = 360.0 * t
    rot = spr.rotate(-ang, expand=True, resample=Image.NEAREST)
    r = 13                                            # la distance des mains au milieu de la tortue
    cx = bx + r * math.sin(math.radians(ang))
    cy = by + r * math.cos(math.radians(ang))
    for sx in (-3, 3):                                # les bras, de la barre aux epaules
        ex = cx - (r - 6) * math.sin(math.radians(ang)) + sx * math.cos(math.radians(ang))
        ey = cy - (r - 6) * math.cos(math.radians(ang)) - sx * math.sin(math.radians(ang))
        d.line([(bx + sx, by), (ex, ey)], fill=vert, width=2)
    im.paste(rot, (int(cx - rot.width / 2), int(cy - rot.height / 2)), rot)
    return im


def danseur(d, cx, w, chemise, nerd):
    """un danseur, les pieds en y = 51 ; nerd : il ondule de tout le corps (la vague part des genoux et monte),
    bras en nouilles, tete qui balance ; sinon il se dandine a peine"""
    peau, pantalon = (235, 200, 170), (45, 45, 60)
    amp = 5 if nerd else 1.5
    def dx(y):
        return amp * math.sin(w - (52 - y) * 0.18)
    for sx in (-1, 1):                               # les jambes, genoux qui plient a contretemps
        g = (3 if nerd else 1) * math.sin(w + sx)
        d.line([(cx + sx * 3 + dx(34), 34), (cx + sx * 4 + dx(43) + g, 43), (cx + sx * 5, 51)], fill=pantalon, width=3)
    for y in range(20, 35):                          # le tronc, tranche par tranche
        x = cx + dx(y)
        d.line([(x - 5, y), (x + 5, y)], fill=chemise if y < 32 else pantalon)
    for sx in (-1, 1):                               # les bras
        pts = []
        for k in range(9):
            u = k / 8
            if nerd:                                 # des nouilles, une sinusoide qui court
                px = cx + dx(22) + sx * (5 + 12 * u)
                py = 22 - 10 * u * (1 if sx < 0 else 0.4) + 4 * math.sin(w * 1.5 + u * 5 + sx)
            else:                                    # le long du corps, un peu leves
                px = cx + dx(22) + sx * (5 + 3 * u)
                py = 22 + 10 * u - 2 * math.sin(w) * u
            pts.append((px, py))
        d.line(pts, fill=peau, width=2)
    hx = cx + dx(12) + (3 * math.sin(w * 2) if nerd else 0)
    d.ellipse([hx - 6, 6, hx + 6, 19], fill=peau)
    d.chord([hx - 6, 4, hx + 7, 12], 180, 360, fill=(60, 40, 30) if nerd else (150, 110, 60))
    if nerd:
        d.rectangle([hx - 5, 11, hx - 1, 13], outline=(20, 20, 20))   # les lunettes
        d.rectangle([hx + 1, 11, hx + 5, 13], outline=(20, 20, 20))
        d.line([(hx - 1, 12), (hx + 1, 12)], fill=(20, 20, 20))
    d.line([(hx - 2, 16), (hx + 2, 16 + int(math.sin(w * 2) > 0))], fill=(150, 80, 70))


def danse(t):
    """t de 0 a 1 : le gars en rouge, seul, ondule n'importe comment ; la camera (qui tremble) zoome sur lui
    de plus en plus, en glissant, jusqu'a le perdre hors du cadre"""
    monde = Image.new("RGB", (L, H), (150, 128, 112))
    d = ImageDraw.Draw(monde)
    d.rectangle([0, 51, L, H], fill=(118, 96, 84))
    for x in range(0, L, 12):                        # le mur, quelques traces : on voit la camera bouger
        d.line([(x + 4, 6), (x + 8, 18)], fill=(138, 116, 102))
    w = t * 2 * math.pi * 2                          # deux vagues par boucle
    danseur(d, 40, w, (205, 50, 45), True)
    z = 1.3 + 2.6 * t * t                            # le zoom, de plus en plus serre
    cx = 40 + 22 * t * t + 1.5 * math.sin(t * 2 * math.pi * 6)   # et il glisse, en tremblant : le gars sort
    cy = 30 - 8 * t + 1.0 * math.sin(t * 2 * math.pi * 7)
    lw, lh = L / z, H / z
    x0, y0 = max(0, min(L - lw, cx - lw / 2)), max(0, min(H - lh, cy - lh / 2))
    return monde.crop((int(x0), int(y0), int(x0 + lw), int(y0 + lh))).resize((L, H), Image.NEAREST)


def nunchaku(t):
    """t de 0 a 1 : un gars fait tournoyer son nunchaku, se le prend en pleine figure, et tombe raide sur le
    cote"""
    im = Image.new("RGB", (L, H), (110, 150, 120))
    d = ImageDraw.Draw(im)
    d.rectangle([0, 57, L, H], fill=(80, 110, 70))   # l'herbe
    peau, kimono, ceinture = (235, 200, 170), (240, 240, 235), (30, 30, 30)
    # le gars, dessine debout autour de ses pieds (40, 50), puis tourne quand il tombe
    spr = Image.new("RGBA", (40, 46), (0, 0, 0, 0))
    g = ImageDraw.Draw(spr)
    g.line([(16, 30), (14, 45)], fill=kimono, width=3)          # les jambes
    g.line([(24, 30), (26, 45)], fill=kimono, width=3)
    g.rectangle([14, 15, 26, 31], fill=kimono)                   # le kimono
    g.line([(14, 26), (26, 26)], fill=ceinture, width=2)
    g.ellipse([15, 3, 25, 14], fill=peau)                         # la tete
    g.line([(15, 6), (25, 6)], fill=(200, 30, 30), width=2)       # le bandeau
    coup = 0.45 <= t < 0.62
    tombe = max(0.0, (t - 0.55) / 0.3)                            # 0 debout .. 1 a terre
    if coup or tombe > 0:                                         # le visage : les yeux en croix
        for ex in (18, 22):
            g.line([(ex - 1, 8), (ex + 1, 10)], fill=(0, 0, 0)); g.line([(ex - 1, 10), (ex + 1, 8)], fill=(0, 0, 0))
    else:
        g.point((18, 9), fill=(0, 0, 0)); g.point((22, 9), fill=(0, 0, 0))
    # le bras et le nunchaku : il tournoie, puis revient dans la figure
    if t < 0.45:
        a = t / 0.45 * 2 * math.pi * 3                            # trois tours
        main = (31, 18)
        b1 = (main[0] + 7 * math.cos(a), main[1] + 7 * math.sin(a))
        b2 = (b1[0] + 8 * math.cos(a + 0.6), b1[1] + 8 * math.sin(a + 0.6))
    else:
        main = (29, 16)
        b1 = (main[0] - 2, main[1] - 7)
        b2 = (21, 9)                                              # ... dans la figure
    g.line([(25, 17), main], fill=peau, width=2)                 # le bras
    g.line([main, b1], fill=(60, 40, 20), width=2)                # le premier baton
    g.line([b1, b2], fill=(60, 40, 20), width=2)                  # le second
    ang = 90 * min(1.0, tombe) ** 2                               # il tombe de plus en plus vite
    # une grosse afro noire, derriere la tete : la figure, posee 8 points plus bas sur une feuille plus haute
    tout = Image.new("RGBA", (40, 54), (0, 0, 0, 0))
    ImageDraw.Draw(tout).ellipse([7, 0, 33, 22], fill=(20, 15, 15))
    tout.paste(spr, (0, 8), spr)
    grand = tout.resize((42, 57), Image.NEAREST)                  # (un peu plus grand)
    toile = Image.new("RGBA", (L, H), (0, 0, 0, 0))               # tourne autour de ses pieds (16, 57) : allonge, il tient dans le cadre
    toile.paste(grand, (16 - 21, 57 - 56), grand)
    rot = toile.rotate(-ang, center=(16, 57), resample=Image.NEAREST)
    im.paste(rot, (0, 0), rot)
    return im


ANIMS = {"chat": (chat, 10, 100), "tortue": (tortue, 12, 100), "danse": (danse, 16, 80), "nunchaku": (nunchaku, 20, 110)}   # images, ms


def main():
    dossier = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    dossier.mkdir(parents=True, exist_ok=True)
    planche = Image.new("RGB", (L * 20, H * len(ANIMS)), (255, 255, 255))
    for j, (nom, (f, n, ms)) in enumerate(ANIMS.items()):
        imgs = [f(i / n) for i in range(n)]
        imgs[0].save(dossier / (nom + ".gif"), save_all=True, append_images=imgs[1:], duration=ms, loop=0)
        for i, im in enumerate(imgs):
            planche.paste(im, (i * L, j * H))
    planche.resize((L * 20 * 2, H * len(ANIMS) * 2), Image.NEAREST).save(dossier / "apercu.png")


if __name__ == "__main__":
    main()
