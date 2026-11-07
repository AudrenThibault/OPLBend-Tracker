# OPLBend VST3

OPLBend Tracker as a VST3 instrument. The plugin runs the tracker's own firmware (`../firmware`), compiled for the Mac. A small hardware layer (`native/`) stands in for the FM-1's screen, keys, LEDs, flash and clock, so the tracker behaves as it does on the device.

`./build.sh` builds it and installs it in `~/Library/Audio/Plug-Ins/VST3` (macOS, Apple silicon). It needs CMake, Xcode's command line tools, Python 3 with Pillow, JUCE (default `~/JUCE`) and Ableton Link 3.1.3 with its submodules (default `~/link`: `git clone --branch Link-3.1.3 --recurse-submodules https://github.com/Ableton/link.git ~/link`). The FM-1's toolchain is not needed. Rebuild it whenever the firmware changes.

- **OPL3:** under the screen, choose the FM-1's own OPL3 (`../firmware/src/opl3.c`) or Nuked OPL3 (`../firmware/nuked`, a cycle-accurate YMF262 by Nuke.YKT). The circuit bending works with both. The choice is saved with the project and can be automated.
- **Output:** stereo. The device's MASTER knob sets the level and can be automated.
- **Mac keyboard:** it plays the D-PAD in every GLO mode (KEYBOARD, D-PAD, DRUM KEYBOARD), by key position, whatever the layout: arrows = the cross, either Shift = SELECT, the two keys right of the left Shift = B then A (on an ISO keyboard the key next to Shift, then the next one; on a US keyboard Z, then X), Return or Space = START, Backspace = delete, G = GLO. The tracker's combinations work as on the device: SELECT+B selection, SELECT+A paste, SELECT+B+A deep clone, A+B delete, SELECT+arrows pages.
- **PROJECT > LOAD** opens a song from the Mac (.OBT, .XT, .A2M) in a file dialog. Cancel it to see the songs saved in the project instead.
- **PROJECT > SAVE** stores the song in the DAW project, in the device's own file format, then offers to save it on the Mac too (Cancel: in the project only). A song that was not saved is lost when the project closes.
- LOAD and SAVE each open in the Mac folder they last used, in every project.
- **Window:** opens at 3/4 of the panel's full size; drag the corner to resize it (its proportions stay).
- **Tempo:** under the screen, choose who leads.
  - **LIVE:** the song follows the DAW's tempo, its tempo automation, Play and Stop. It starts from the DAW's position, counted from the song's first line, and loops as on the device. The song's speed commands keep its groove, but the tempo is the DAW's.
  - **TRACKER:** the DAW follows the song's tempo and its tempo changes through Ableton Link. Turn LINK on in Live (Settings > Link, Tempo & MIDI > Show Link Toggle, and Start Stop Sync to share Play and Stop). The song still keeps to the DAW's position. Without Link, the song follows the DAW as in LIVE.
  - **START:** with Link on, it starts and stops the DAW too: the DAW starts on a bar 0.15 s later, and the song waits for it. Without Link, it plays the tracker alone.
  - A beat is the song's rows per beat (4 by default), doubled while that would make the tempo faster than 200 BPM: a song at tempo 102, speed 6 plays 17 rows a second, 255 BPM at 4 rows a beat, so 127.5 BPM at 8.
  - The plugin reports its latency (46 ms), so the DAW lines it up with the other tracks.
- **No MIDI:** the tracker does not read incoming notes.
- **Exports** are exact: when the DAW renders offline, the tracker gets all the time it needs, so the same project always gives the same sound. An export follows the DAW's tempo, even in TRACKER mode: tempo changes that came through Link are not recorded in the DAW.

Each instance loads its own copy of the firmware library (written to the temporary folder, then removed), because the firmware keeps its state in global variables.

Licences: GPL-3.0 (the firmware and the plugin), LGPL-2.1-or-later (Nuked OPL3), GPL-2.0-or-later (Ableton Link).
