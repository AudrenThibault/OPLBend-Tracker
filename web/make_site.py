#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Adapted from Felucca's web/make_site.py (Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments).
"""Make the OPLBend Tracker site (GitHub Pages, published from docs/):

  index.html            the home page (web/index.html)
  installer/index.html  web/installer.html, self-contained (fm1pkg.js, fm1ota.js and the metadata inlined)
  transfer/index.html   web/transfer.html: send songs to the FM-1 and copy them back
  firmware/             the package oplbend-tracker-VER.fwsc, an index page, LICENSE and LICENSES/
                        (the package holds JieLi SDK files under Apache-2.0)

  web/make_site.py build/tracker.fwsc VERSION docs [KOFI_URL]

Only a package made by tools/fm1pkg_make.py (Felucca's loader, no vendor files) is published.
"""
import json
import re
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
BLOCKS, BLK, KEEP = 20, 0x30, 0x2F


def strip_module(src):
    src = re.sub(r"^export\s+", "", src, flags=re.M)
    return re.sub(r"^import .*?;\n", "", src, flags=re.M)


def product_of(raw):
    """the package identity: one marker byte after each of the first 20 blocks (fm1pkg.js productOf)"""
    return "".join(chr((m - i - 1) & 0xFF) for i in range(BLOCKS) if (m := raw[i * BLK + KEEP]) != 0x7D)


def main(pkg, version, out, kofi="https://ko-fi.com/audren"):
    pkg, out = Path(pkg), Path(out)
    raw = pkg.read_bytes()
    product = product_of(raw)
    if not re.fullmatch(r"FM-1_\d\d\d", product):
        raise SystemExit(f"{pkg}: identity {product!r} is not an FM-1 package")
    if b"FELUCCA-LOADER-1" not in raw:              # never publish a package patched from an official one
        raise SystemExit(f"{pkg}: no Felucca loader in it (a package made from an official one carries vendor files)")
    name = f"oplbend-tracker-{re.sub(r'[^A-Za-z0-9.-]', '-', version)}.fwsc"
    lib = strip_module((HERE / "fm1pkg.js").read_text(encoding="utf-8")) + "\n" + \
        strip_module((HERE / "fm1ota.js").read_text(encoding="utf-8"))
    meta = json.dumps({"version": version, "product": product, "pkg": "../firmware/" + name})
    html = (HERE / "installer.html").read_text(encoding="utf-8")
    for mark in ("/*LIB*/", "/*META*/"):
        if html.count(mark) != 1:
            raise SystemExit(f"installer.html must contain {mark} once")
    html = html.replace("/*LIB*/", lib).replace("/*META*/", meta)
    inst, fw = out / "installer", out / "firmware"
    for d in (inst, fw, fw / "LICENSES"):
        d.mkdir(parents=True, exist_ok=True)
    for old in fw.glob("oplbend-tracker-*.fwsc"):   # one package: the current one
        old.unlink()
    (inst / "index.html").write_text(html, encoding="utf-8")
    (out / "transfer").mkdir(exist_ok=True)
    shutil.copy(HERE / "transfer.html", out / "transfer" / "index.html")
    (out / "index.html").write_text((HERE / "index.html").read_text(encoding="utf-8").replace("KOFI_URL", kofi),
                                    encoding="utf-8")
    (out / ".nojekyll").write_text("", encoding="utf-8")
    site = "https://audrenthibault.github.io/OPLBend-Tracker/"   # (le referencement : robots, plan du site)
    (out / "robots.txt").write_text(f"User-agent: *\nAllow: /\nSitemap: {site}sitemap.xml\n", encoding="utf-8")
    (out / "sitemap.xml").write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n'
        + "".join(f"  <url><loc>{site}{p}</loc></url>\n" for p in ("", "installer/", "transfer/", "firmware/"))
        + "</urlset>\n", encoding="utf-8")
    shutil.copy(pkg, fw / name)
    names = sorted(f.name for f in (ROOT / "LICENSES").glob("*.txt"))
    for n in names:
        shutil.copy(ROOT / "LICENSES" / n, fw / "LICENSES" / n)
    shutil.copy(ROOT / "LICENSE", fw / "LICENSE")
    (fw / "LICENSES" / "index.html").write_text(
        '<!doctype html><meta charset="utf-8"><title>OPLBend Tracker licences</title><h1>Licence texts</h1><ul>'
        + "".join(f'<li><a href="{n}">{n}</a></li>' for n in names)
        + '</ul><p><a href="../LICENSE">LICENSE (GPL-3.0)</a></p>\n', encoding="utf-8")
    (fw / "index.html").write_text(
        f'<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">'
        f'<title>OPLBend Tracker {version}</title><h1>OPLBend Tracker {version}</h1>'
        f'<p><a href="{name}" download>{name}</a> ({len(raw)} bytes, {product})</p>'
        f'<p>Install it with the <a href="../installer/">web installer</a> (Chrome or Edge), '
        f'or <code>tools/fm1_install.py</code>. We recommend running the official V15 on your FM-1 first.</p>'
        f'<p><a href="LICENSE">GPL-3.0</a> · <a href="LICENSES/">licences</a> · <a href="../">home</a></p>\n',
        encoding="utf-8")
    print(f"site: {out}: installer ({len(html)} B), firmware/{name} ({len(raw)} B, {product})")


if __name__ == "__main__":
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    main(*sys.argv[1:])
