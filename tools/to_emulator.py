#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Put the last build into the local emulator's firmware list (../emulateur/web/firmware), first in the list.

  to_emulator.py build/tracker.fwsc

The catalog entry's file carries the build time (?v=...) so the browser never plays a cached older build.
Nothing leaves this Mac: the emulator is served from localhost (Lancer l'émulateur FM-1.command).
"""
import json
import shutil
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EMU = ROOT.parent / "emulateur" / "web" / "firmware"


def main():
    src = Path(sys.argv[1])
    if not EMU.is_dir():
        print("to_emulator: no ../emulateur/web/firmware, skipped")
        return
    shutil.copy(src, EMU / "tracker.fwsc")
    cat_path = EMU / "catalog.json"
    cat = json.loads(cat_path.read_text()) if cat_path.exists() else []
    cat = [e for e in cat if e.get("id") != "tracker"]
    stamp = time.strftime("%Y-%m-%d %H:%M")
    cat.insert(0, {
        "id": "tracker",
        "name": "FM-1 TRACKER",
        "version": "dev " + stamp,
        "author": "local build",
        "file": f"firmware/tracker.fwsc?v={int(time.time())}",
        "license": "GPL-3.0 (private, not published)",
        "description": "Your tracker: LSDJ-style song / chain / phrase, 8 OPL3-style FM channels.",
        "date": stamp[:10],
    })
    cat_path.write_text(json.dumps(cat, ensure_ascii=False, indent=2))
    print(f"emulator {EMU / 'tracker.fwsc'} (first in the list)")


if __name__ == "__main__":
    main()
