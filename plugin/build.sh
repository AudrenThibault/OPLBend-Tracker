#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the OPLBend VST3 from the firmware's sources and install it in ~/Library/Audio/Plug-Ins/VST3.
# Needs CMake, Xcode's command line tools, Python 3 with Pillow, JUCE (default: ~/JUCE; set JUCE_DIR otherwise) and
# Ableton Link 3.1.3 with its submodules (default: ~/link; set LINK_DIR otherwise).
set -e
cd "$(dirname "$0")"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release ${JUCE_DIR:+-DJUCE_DIR="$JUCE_DIR"} ${LINK_DIR:+-DLINK_DIR="$LINK_DIR"}
# A running DAW may have the old one mapped: replace the file, never write into it.
rm -rf "$HOME/Library/Audio/Plug-Ins/VST3/OPLBend.vst3"
cmake --build build --target OPLBend_VST3 --parallel
