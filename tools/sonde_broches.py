#!/usr/bin/env python3
"""Diagnostic : l'etat des broches de la puce du FM-1 (commande SONDE de firmware/compat/fm1_transfert.c),
pour trouver celle qui change quand on branche un casque.
  sonde_broches.py NOM        enregistre l'etat dans build/sonde_NOM.json
  sonde_broches.py --diff A B les broches qui different entre deux enregistrements
Besoin de mido et python-rtmidi (comme fm1_install.py)."""
import json
import sys
import time
from pathlib import Path

ICI = Path(__file__).resolve().parent.parent
CHAMPS = ["DIR", "DIE", "PU", "PD", "IN", "IN entree", "IN tire haut", "IN tire bas"]
PORTS = "ABCDEFGH"


def pack7(b):
    out, acc, nb = [], 0, 0
    for o in b:
        acc |= o << nb
        nb += 8
        while nb >= 7:
            out.append(acc & 0x7F)
            acc >>= 7
            nb -= 7
    if nb:
        out.append(acc & 0x7F)
    return out


def unpack7(b):
    out, acc, nb = [], 0, 0
    for o in b:
        acc |= (o & 0x7F) << nb
        nb += 7
        if nb >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nb -= 8
    return out


def sonde():
    import mido
    sortie = next((n for n in mido.get_output_names() if "felucca" in n.lower() or "fm-1" in n.lower()), None)
    entree = next((n for n in mido.get_input_names() if "felucca" in n.lower() or "fm-1" in n.lower()), None)
    if not sortie or not entree:
        sys.exit("FM-1 introuvable (branche et allume, avec le firmware du tracker)")
    etat = {}
    with mido.open_output(sortie) as o, mido.open_input(entree) as i:
        for p in range(8):
            o.send(mido.Message("sysex", data=pack7([0x00, 0x54, 0x4B, 0x10, p])))
            t0 = time.time()
            while time.time() - t0 < 2:
                m = i.poll()
                if m is None:
                    time.sleep(0.005)
                    continue
                if m.type != "sysex":
                    continue
                d = unpack7(list(m.data))
                if d[:4] == [0x00, 0x54, 0x4B, 0x50] and len(d) >= 37:
                    etat[PORTS[p]] = [int.from_bytes(bytes(d[5 + 4 * k: 9 + 4 * k]), "little") for k in range(8)]
                    break
    return etat


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "--diff":
        a = json.loads((ICI / f"build/sonde_{sys.argv[2]}.json").read_text())
        b = json.loads((ICI / f"build/sonde_{sys.argv[3]}.json").read_text())
        trouve = False
        for port in a:
            for k, champ in enumerate(CHAMPS):
                x = a[port][k] ^ b[port][k]
                for bit in range(32):
                    if x >> bit & 1:
                        trouve = True
                        print(f"P{port}{bit:<2} {champ:13} {sys.argv[2]}={a[port][k] >> bit & 1}  {sys.argv[3]}={b[port][k] >> bit & 1}")
        if not trouve:
            print("aucune broche ne change")
        return
    nom = sys.argv[1] if len(sys.argv) > 1 else "etat"
    etat = sonde()
    (ICI / f"build/sonde_{nom}.json").write_text(json.dumps(etat))
    for port, v in etat.items():
        print(f"P{port} " + "  ".join(f"{c}={x:08X}" for c, x in zip(CHAMPS, v)))


if __name__ == "__main__":
    main()
