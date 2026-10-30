#!/usr/bin/env python3
"""Les morceaux livres AVEC le firmware : tracker/songs/*.A2M (tels quels : LOAD SONG les importe sur
le FM-1) et *.XT deviennent build/gen/fm1_rom.h, lu par compat/fm1_fs.c comme des fichiers du dossier des morceaux (OPLTRACKER/Songs,
compat/fm1_plateforme.c s_arg0) en
lecture seule. Meme format qu'un fichier de la flash : blocs de 4 Ko compresses en PackBits, puis la
table des blocs (le debut de chacun, puis la fin). Un fichier du meme nom enregistre en flash passe
devant. C'est la seule facon d'apporter un morceau au FM-1 (et a l'emulateur en ligne) : rien ne
s'ecrit dans la zone des sauvegardes, qui restent intactes.
  rom_songs.py SONGS_DIR OUT_DIR HEADER"""
import subprocess
import sys
from pathlib import Path

BLOC = 4096


def packbits(s):
    """le meme codage que pb_comprime (compat/fm1_fs.c)"""
    d, i, n = bytearray(), 0, len(s)
    while i < n:
        r = 1
        while i + r < n and r < 129 and s[i + r] == s[i]:
            r += 1
        if r >= 2:
            d += bytes((126 + r, s[i]))
            i += r
            continue
        t = len(d)
        d.append(0)
        l = 0
        while i < n and l < 128 and not (i + 1 < n and s[i + 1] == s[i]):
            d.append(s[i])
            i += 1
            l += 1
        if not l:
            d.append(s[i])
            i += 1
            l = 1
        d[t] = l - 1
    return bytes(d)


def unpackbits(s, maxi):
    d, i = bytearray(), 0
    while i < len(s) and len(d) < maxi:
        h = s[i]
        i += 1
        if h < 128:
            d += s[i:i + h + 1]
            i += h + 1
        else:
            d += bytes((s[i],)) * (h - 126)
            i += 1
    return bytes(d[:maxi])


def fichier(raw):
    """(l'etendue : blocs puis table, l'index de la table)"""
    out, table = bytearray(), []
    for k in range(0, len(raw), BLOC):
        b = raw[k:k + BLOC]
        c = packbits(b)
        assert unpackbits(c, BLOC) == b
        table.append(len(out))
        out += c
    index = len(out)
    table.append(len(out))
    for t in table:
        out += t.to_bytes(4, "little")
    return bytes(out), index


def main():
    songs, out_dir, header = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    out_dir.mkdir(parents=True, exist_ok=True)
    tools = Path(__file__).resolve().parent
    xts = {}
    for f in sorted(songs.glob("*")) if songs.is_dir() else []:
        ext = f.suffix.upper()
        nom = f.stem.upper()[:40] + ".XT"
        if ext == ".XT":
            xts[nom] = f.read_bytes()
        elif ext == ".A2M":                        # tel quel : le FM-1 l'importe lui-meme (compat/fm1_a2m.c)
            xts[f.stem.upper()[:40] + ".A2M"] = f.read_bytes()
    data, ents = bytearray(), []
    for nom, raw in xts.items():
        etendue, index = fichier(raw)
        while len(data) % 4:
            data.append(0)
        ents.append((f"OPLTRACKER/Songs/{nom}", len(data), len(raw), index))
        data += etendue
    with open(header, "w") as h:
        h.write("/* genere par tools/rom_songs.py depuis tracker/songs : ne pas modifier */\n")
        h.write(f"#define FM1_ROM_NB {len(ents)}u\n")
        h.write("static const uint8_t fm1_rom_donnees[] __attribute__((aligned(4))) = {")
        for i, b in enumerate(data or b"\0"):
            h.write(("\n" if i % 24 == 0 else "") + f"{b},")
        h.write("\n};\n")
        h.write("static const struct { const char *nom; uint32_t debut, taille, index; } fm1_rom[] = {\n")
        for e in ents or [("", 0, 0, 0)]:
            h.write(f'    {{"{e[0]}", {e[1]}u, {e[2]}u, {e[3]}u}},\n')
        h.write("};\n")
    print(f"songs: {len(ents)} in the firmware, {len(data)} bytes" + "".join(f"\n  {e[0]}" for e in ents))


if __name__ == "__main__":
    main()
