#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The firmware's generated headers (tracker/build/gen: tables, presets, songs, pictures), made as
tools/build.py makes them, for the plugin's build of the firmware."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import build  # noqa: E402

build.generate()
