#!/usr/bin/env python3
"""Les PRESETS du FM-1 (le bouton SEL) : ceux d'Amber OPL3 Tracker d'abord, puis tous les instruments livres
avec AdLib Tracker II (tracker/presets/at2 : SBI, SGI, CIF, FIN, INS, IBK, BNK), lus comme AT2 les lit
(meme lecture que le tracker DOS, import/ins_import.cpp). Le resultat, build/gen/fm1_presets.h : le nom
de chacun (16 lettres, en majuscules : la police n'a pas de minuscules) et ses onze octets au format
d'AT2 (celui du moteur, MD_OPL_*). La banque .A2W d'AT2 est compressee (aPLib) : elle n'est pas lue.
  presets_at2.py DOSSIER_AT2 HEADER"""
import sys
from pathlib import Path

# Les presets d'Amber (XMEngineManager.applyPreset) : { operateur (0, 1 ; 2 = la voie), reglage, valeur },
# poses sur l'instrument par defaut d'Amber (deux operateurs a fond, attaque F, relache 5, sinus, FM).
AMBER_DEFAUT = [0x01, 0x01, 0x00, 0x00, 0xF0, 0xF0, 0x05, 0x05, 0x00, 0x00, 0x00]
AMBER = [
    ("KICK", [(2,0,0),(2,1,6),(0,0,0),(0,1,0),(0,4,0),(0,6,0),(0,7,15),(0,8,5),(0,9,0),(0,10,8),(0,11,0),
              (1,0,0),(1,1,0),(1,4,0),(1,6,0),(1,7,15),(1,8,5),(1,9,0),(1,10,5),(1,11,0)]),
    ("SNARE", [(2,0,0),(2,1,6),(0,0,0),(0,1,0),(0,2,0),(0,3,0),(0,4,0),(0,6,0),(0,7,15),(0,8,2),(0,9,2),(0,10,5),(0,11,0),
               (1,0,0),(1,1,0),(1,2,0),(1,3,0),(1,4,0),(1,6,0),(1,7,15),(1,8,6),(1,9,15),(1,10,5),(1,11,0)]),
    ("BASS", [(2,0,0),(2,1,6),(0,0,0),(0,1,0),(0,4,0),(0,6,17),(0,7,12),(0,8,5),(0,9,15),(0,10,8),(0,11,1),
              (1,0,0),(1,1,0),(1,4,0),(1,6,0),(1,7,15),(1,8,4),(1,9,15),(1,10,8),(1,11,0)]),
    ("HH-C", [(2,0,0),(2,1,7),(0,0,15),(0,1,0),(0,4,0),(0,6,0),(0,7,11),(0,8,0),(0,9,15),(0,10,15),(0,11,0),
              (1,0,15),(1,1,0),(1,4,0),(1,6,0),(1,7,11),(1,8,8),(1,9,15),(1,10,15),(1,11,3)]),
    ("HH-O", [(2,0,0),(2,1,7),(0,0,15),(0,1,0),(0,2,1),(0,3,1),(0,4,0),(0,6,0),(0,7,15),(0,8,0),(0,9,8),(0,10,4),(0,11,0),
              (1,0,0),(1,1,0),(1,2,1),(1,3,1),(1,4,1),(1,6,0),(1,7,9),(1,8,7),(1,9,15),(1,10,4),(1,11,3)]),
]
# reglage d'Amber -> (octet, decalage, masque) ; les octets 0..9 vont par paires (modulateur, porteuse)
AMBER_CHAMP = {0: (0, 0, 15), 1: (0, 4, 1), 2: (0, 7, 1), 3: (0, 6, 1), 4: (0, 5, 1), 5: (2, 6, 3),
               6: (2, 0, 63), 7: (4, 4, 15), 8: (4, 0, 15), 9: (6, 4, 15), 10: (6, 0, 15), 11: (8, 0, 7)}


def amber(pas):
    fm = list(AMBER_DEFAUT)
    for op, prop, val in pas:
        if op == 2:
            o, d, m = (10, 0, 1) if prop == 0 else (10, 1, 7)
        else:
            o, d, m = AMBER_CHAMP[prop]
            o += op
        fm[o] = (fm[o] & ~(m << d) & 0xFF) | ((val & m) << d)
    return fm


def nom_propre(b):
    s = ""
    for c in b:
        if c == 0:
            break
        s += chr(c) if 32 <= c < 127 else " "
    s = s.strip().upper()
    return "".join(ch if 0x20 <= ord(ch) < 0x60 else "?" for ch in s)


def standard(s, onde):
    return list(s[0:8]) + [s[8] & onde, s[9] & onde, s[10] & 0x0F]


def hsc(s):
    fm = [0] * 11
    fm[1], fm[0], fm[3], fm[2] = s[0], s[1], s[2], s[3]
    fm[5], fm[4], fm[7], fm[6] = s[4], s[5], s[6], s[7]
    fm[10], fm[9], fm[8] = s[8] & 0x0F, s[9] & 3, s[10] & 3
    return fm


def sat(s):
    fm = [0] * 11
    fm[10] = s[0] & 0x0F
    fm[0], fm[1], fm[4], fm[5], fm[6], fm[7] = s[1], s[2], s[3], s[4], s[5], s[6]
    fm[8], fm[9], fm[2], fm[3] = s[7] & 3, s[8] & 3, s[9], s[10]
    return fm


def sgi(s):
    fm = [0] * 11
    for o in range(2):
        p = s[o * 12:o * 12 + 12]
        fm[4 + o] = ((p[0] & 15) << 4) | (p[1] & 15)
        fm[6 + o] = ((p[2] & 15) << 4) | (p[3] & 15)
        fm[8 + o] = p[4] & 3
        fm[2 + o] = ((p[6] & 3) << 6) | (p[7] & 0x3F)
        fm[0 + o] = (p[5] & 15) | ((p[8] & 1) << 4) | ((p[11] & 1) << 5) | ((p[10] & 1) << 6) | ((p[9] & 1) << 7)
    fm[10] = (s[25] & 1) | ((s[24] & 7) << 1)
    return fm


def fiche_bnk(r):
    fm = [0] * 11
    for o in range(2):
        p = r[2 + o * 13:2 + o * 13 + 13]   # ksl mult fb ar sl eg dr rr tl am vib ksr fm
        fm[0 + o] = (p[1] & 15) | ((p[11] & 1) << 4) | ((p[5] & 1) << 5) | ((p[10] & 1) << 6) | ((p[9] & 1) << 7)
        fm[2 + o] = (p[8] & 0x3F) | ((p[0] & 3) << 6)
        fm[4 + o] = (p[6] & 15) | ((p[3] & 15) << 4)
        fm[6 + o] = (p[7] & 15) | ((p[4] & 15) << 4)
    fm[8], fm[9] = r[28] & 7, r[29] & 7
    m = r[2:15]
    fm[10] = ((m[12] ^ 1) & 1) | ((m[2] & 7) << 1)
    return fm


def lit(f):
    """[(nom, onze octets)] d'un fichier d'instrument ou d'une banque"""
    d, e, stem = f.read_bytes(), f.suffix.upper(), nom_propre(f.stem.encode())
    if e == ".SBI" and len(d) >= 47 and d[:4] == b"SBI\x1a":
        return [(nom_propre(d[4:36]) or stem, standard(d[36:47], 3))]
    if e == ".CIF" and len(d) >= 32 and d[:20] == b"<CUD-FM-Instrument>\x1a":
        return [((nom_propre(d[32:52]) if len(d) >= 52 else "") or stem, hsc(d[20:31]))]
    if e == ".FIN" and len(d) >= 50:
        return [(nom_propre(d[12:39]) or stem, standard(d[39:50], 7))]
    if e == ".INS" and 12 <= len(d) <= 32:
        if len(d) == 12:
            fm = standard(d, 3) if d[8] <= 3 and d[9] <= 3 and d[10] <= 15 else hsc(d)
        else:
            fm = sat(d)
        return [(stem, fm)]
    if e == ".SGI" and len(d) >= 26:
        return [(stem, sgi(d))]
    if e == ".IBK" and len(d) >= 4 + 128 * 16 and d[:4] == b"IBK\x1a":
        r = []
        for i in range(128):
            pn = 4 + 128 * 16 + i * 9
            r.append((nom_propre(d[pn:pn + 9]) or f"{stem} {i:03d}", standard(d[4 + i * 16:4 + i * 16 + 11], 3)))
        return r
    if e == ".BNK" and len(d) >= 28 and d[0] == 1 and d[1] == 0 and d[2:8] == b"ADLIB-":
        n = d[8] | (d[9] << 8)
        noms = int.from_bytes(d[12:16], "little")
        fiches = int.from_bytes(d[16:20], "little")
        r = []
        for i in range(n):
            pn = noms + i * 12
            if pn + 12 > len(d):
                break
            pf = fiches + (d[pn] | (d[pn + 1] << 8)) * 30
            if pf + 30 > len(d):
                continue
            r.append((nom_propre(d[pn + 3:pn + 12]) or f"{stem} {i:04d}", fiche_bnk(d[pf:pf + 30])))
        return r
    return []


def main():
    dossier, sortie = Path(sys.argv[1]), Path(sys.argv[2])
    liste = [(n, amber(p)) for n, p in AMBER]
    vus = set()
    seuls, banques = [], []
    for f in sorted(dossier.iterdir(), key=lambda p: p.name.upper()) if dossier.is_dir() else []:
        r = lit(f)
        (banques if len(r) > 1 else seuls).extend(r)
    for n, fm in seuls + banques:
        cle = (n[:16], tuple(fm))
        if cle in vus:
            continue
        vus.add(cle)
        liste.append((n[:16], fm))
    l = [f"/* genere par tools/presets_at2.py : {len(liste)} presets (Amber, puis AdLib Tracker II) */",
         f"#define FM1_NB_PRESETS {len(liste)}",
         "static const char FM1_PRESET_NOMS[FM1_NB_PRESETS][17] = {"]
    for n, _ in liste:
        l.append('  "' + n.replace("\\", "/").replace('"', "'") + '",')
    l += ["};", "static const uint8_t FM1_PRESET_FM[FM1_NB_PRESETS][11] = {"]
    for _, fm in liste:
        l.append("  {" + ", ".join(f"0x{b:02X}" for b in fm) + "},")
    l.append("};")
    sortie.write_text("\n".join(l) + "\n")
    print(f"presets: {len(liste)} (Amber {len(AMBER)}, AdLib Tracker II {len(liste) - len(AMBER)})")


if __name__ == "__main__":
    main()
