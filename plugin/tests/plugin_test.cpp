// SPDX-License-Identifier: GPL-3.0-only
// The plugin's processor and editor without a host. Works on any firmware; on one with
// a song embedded (a build with -DFM1_GEN_DIR= holding its fm1_rom.h), pass "song" to measure
// dropouts while it plays.
//   OPLBendTest OUTDIR [engines | song [INDEX [nuked]] | load SONGFILE | sync SONGFILE | link SONGFILE]
#include "PluginEditor.h"
#include <ableton/Link.hpp>

namespace
{
    enum { glo = 7, home = 8, save = 9, seq = 11, play = 12, up = 15, down = 16, select = 21, a = 39 };

    int failures = 0;

    void check (bool ok, const juce::String& what)
    {
        std::cout << (ok ? "ok    " : "FAIL  ") << what << std::endl;
        if (! ok)
            ++failures;
    }

    // A host's transport: playing moves its beat on, at its tempo (bpmAt: an automation, by beat).
    struct Head : juce::AudioPlayHead
    {
        bool playing = false;
        double beat = 0.0, bpm = 120.0;
        std::function<double (double)> bpmAt;

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setIsPlaying (playing);
            info.setPpqPosition (beat);
            info.setBpm (bpm);
            return info;
        }

        void advance (int samples, double rate)
        {
            if (bpmAt)
                bpm = bpmAt (beat);
            if (playing)
                beat += samples / rate * bpm / 60.0;
        }
    };

    struct Rig
    {
        OPLBendProcessor processor;
        juce::AudioBuffer<float> buffer { 2, 512 };
        juce::MidiBuffer midi;
        double peak = 0.0;
        Head head;
        std::vector<float>* capture = nullptr;         // the left channel, when set

        explicit Rig (double rate = 44100.0)
        {
            processor.setPlayHead (&head);
            processor.setRateAndBufferSizeDetails (rate, 512);
            processor.prepareToPlay (rate, 512);
            processor.setNonRealtime (true);
        }

        // Guest time follows the host: an offline block waits for the machine.
        void run (double seconds)
        {
            auto blocks = (int) (seconds * processor.getSampleRate() / 512.0);
            while (blocks-- > 0)
            {
                processor.processBlock (buffer, midi);
                peak = juce::jmax (peak, (double) buffer.getMagnitude (0, 512));
                if (capture != nullptr)
                    capture->insert (capture->end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + 512);
                head.advance (512, processor.getSampleRate());
            }
        }

        void tap (int id)
        {
            processor.machine.key (id, true);
            run (0.15);
            processor.machine.key (id, false);
            run (0.2);
        }

        // Key test: GLO to D-PAD, the help goes on the next press, SEL+UP is the project page.
        void openProject()
        {
            run (7.0);
            tap (glo);
            tap (down);
            processor.machine.key (select, true);
            run (0.15);
            tap (up);
            processor.machine.key (select, false);
            run (0.5);
        }

        // PROJECT > LOAD: the tracker asks for the Mac's files.
        bool askLoad()
        {
            openProject();
            for (int i = 0; i < 3; ++i)
                tap (down);
            tap (a);
            run (0.3);
            return processor.machine.takeLoadRequest();
        }

        // None chosen on the Mac: the list of the songs in the project (and the firmware's own).
        void loadSong (int index)
        {
            askLoad();
            processor.machine.loadSong ({}, {});
            run (0.8);
            for (int i = 0; i < index; ++i)
                tap (down);
            tap (a);
            run (1.5);
            tap (a);
            run (1.5);
        }

        juce::Image screenshot (const juce::File& file)
        {
            OPLBendEditor editor (processor);
            pump (200);
            const auto image = editor.createComponentSnapshot (editor.getLocalBounds());
            file.deleteFile();
            juce::FileOutputStream out (file);
            juce::PNGImageFormat().writeImageToStream (image, out);
            return image;
        }

        // The screen comes from guest time, so advance it while the editor's timer runs.
        void pump (int ms)
        {
            const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
            while (juce::Time::getMillisecondCounter() < end)
            {
                run (0.02);
                juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
            }
        }
    };

    void testAudioRates()
    {
        // 48 kHz: guest frames used must match host time, none lost or doubled.
        Rig rig (48000.0);
        rig.run (8.0);
        rig.tap (glo);                                  // still responsive after resampling for a while
        check (rig.processor.machine.getError().isEmpty(), "48 kHz host: the firmware keeps running");
    }

    // The LEDs must hold still on a page where nothing happens, for as long as it takes to see otherwise
    // (but `except`, which the firmware blinks there on purpose).
    int countLedChanges (Rig& rig, double seconds, std::array<juce::uint8, Machine::numControls>& leds, int except = -1)
    {
        juce::HeapBlock<juce::uint8> frame ((size_t) Machine::lcdSize * Machine::lcdSize * 4);
        std::array<juce::uint8, Machine::numControls> previous {};
        juce::uint32 version = 0;
        auto changes = 0, frames = 0;
        for (int block = 0; block < (int) (seconds * 44100.0 / 512.0); ++block)
        {
            rig.run (512.0 / 44100.0);
            if (! rig.processor.machine.copyScreen (frame, leds.data(), version))
                continue;
            if (frames++ > 0)
                for (size_t id = 0; id < leds.size(); ++id)
                    if (leds[id] != previous[id] && (int) id != except)
                    {
                        ++changes;
                        std::cout << "      LED " << id << ": " << (int) previous[id] << " -> " << (int) leds[id]
                                  << " at " << (block * 512.0 / 44100.0) << " s" << std::endl;
                    }
            previous = leds;
        }
        return changes;
    }

    void testLedsAreSteady()
    {
        enum { octDown = 0, lfo = 5, home = 8 };
        Rig rig;
        rig.run (9.0);

        std::array<juce::uint8, Machine::numControls> leds {};
        const auto keyboardMode = countLedChanges (rig, 40.0, leds, glo);   // (GLO blinks faint: the way to D-PAD)
        check (keyboardMode == 0, "LEDs steady in KEYBOARD mode, changes: " + juce::String (keyboardMode));
        check (leds[home] == 255, "HOME strong");
        check (leds[octDown] > 0 && leds[octDown] < 255, "OCT- faint");
        check (leds[lfo] == 0, "LFO off");

        rig.tap (glo);
        rig.tap (down);
        rig.run (2.0);
        const auto dpadMode = countLedChanges (rig, 40.0, leds);
        check (dpadMode == 0, "LEDs steady in D-PAD mode, changes: " + juce::String (dpadMode));

        // The firmware's own blinking must survive: a knob changed a sound, so EDIT blinks at 2 Hz.
        enum { edit = 6, knob1 = 3 };
        rig.processor.machine.encoder (knob1, 2);
        rig.run (0.5);
        juce::HeapBlock<juce::uint8> frame ((size_t) Machine::lcdSize * Machine::lcdSize * 4);
        juce::uint32 version = 0;
        auto rises = 0;
        juce::uint8 before = 0;
        for (int block = 0; block < (int) (4.0 * 44100.0 / 512.0); ++block)
        {
            rig.run (512.0 / 44100.0);
            if (rig.processor.machine.copyScreen (frame, leds.data(), version))
            {
                rises += leds[edit] == 255 && before != 255 ? 1 : 0;
                before = leds[edit];
            }
        }
        check (rises >= 6 && rises <= 9, "EDIT blinks: " + juce::String (rises) + " flashes in 4 s");
    }

    void testFilesSurviveReload (const juce::File& outDir)
    {
        juce::MemoryBlock state;
        {
            Rig rig;
            rig.openProject();
            rig.tap (down);                             // SAVE
            rig.tap (a);
            rig.run (0.5);
            rig.screenshot (outDir.getChildFile ("save_prompt.png"));
            rig.tap (a);                                // keeps the proposed name
            rig.run (2.0);
            juce::String name;
            juce::MemoryBlock song;
            check (rig.processor.machine.takeSavedSong (name, song) && name.endsWithIgnoreCase (".OBT") && song.getSize() > 100,
                   "SAVE offers the song to the Mac: " + name + ", " + juce::String ((int) song.getSize()) + " bytes");
            check (! rig.processor.machine.takeSavedSong (name, song), "... once");
            outDir.getChildFile (name).replaceWithData (song.getData(), song.getSize());   // (LOAD opens it again: load mode)
            rig.screenshot (outDir.getChildFile ("saved.png"));
            rig.processor.getStateInformation (state);
        }
        check (state.getSize() > 100 && state.getSize() < 200000, "state size " + juce::String ((int) state.getSize()) + " bytes");

        Rig reloaded;
        reloaded.processor.setStateInformation (state.getData(), (int) state.getSize());
        reloaded.openProject();
        for (int i = 0; i < 3; ++i)
            reloaded.tap (down);                        // LOAD
        reloaded.tap (a);
        reloaded.run (0.8);
        reloaded.screenshot (outDir.getChildFile ("reloaded_list.png"));

        juce::MemoryBlock again;
        reloaded.processor.getStateInformation (again);
        check (reloaded.processor.machine.getError().isEmpty(), "restored flash boots");
        check (again.getSize() > 100, "state saved again after restore");
    }

    // The Mac's keyboard works as the D-PAD in KEYBOARD mode too: SELECT+UP opens PROJECT, GLO stays as it was.
    // The points of the LCD that differ between two screenshots of the editor.
    int screenChange (const juce::Image& before, const juce::Image& after)
    {
        const auto scale = (float) before.getWidth() / 904.0f;  // (the editor's design width)
        auto changed = 0;
        for (int y = juce::roundToInt (20 * scale); y < juce::roundToInt (490 * scale); y += 3)
            for (int x = juce::roundToInt (252 * scale); x < juce::roundToInt (722 * scale); x += 3)
                changed += before.getPixelAt (x, y) != after.getPixelAt (x, y) ? 1 : 0;
        return changed;
    }

    void testMacDpad (const juce::File& outDir)
    {
        Rig rig;
        rig.run (9.0);
        auto& m = rig.processor.machine;
        const auto before = rig.screenshot (outDir.getChildFile ("dpad_before.png"));
        m.dpad (FM1N_DPAD_SELECT, true);
        rig.run (0.15);
        m.dpad (FM1N_DPAD_UP, true);
        rig.run (0.15);
        m.dpad (FM1N_DPAD_UP, false);
        rig.run (0.2);
        m.dpad (FM1N_DPAD_SELECT, false);
        rig.run (0.5);
        m.dpad (FM1N_DPAD_DOWN, true);                  // TEMPO -> SAVE
        rig.run (0.15);
        m.dpad (FM1N_DPAD_DOWN, false);
        rig.run (0.3);
        const auto after = rig.screenshot (outDir.getChildFile ("dpad_project.png"));
        std::array<juce::uint8, Machine::numControls> leds {};
        juce::HeapBlock<juce::uint8> frame ((size_t) Machine::lcdSize * Machine::lcdSize * 4);
        juce::uint32 version = 0;
        rig.run (0.1);
        m.copyScreen (frame, leds.data(), version);
        const auto changed = screenChange (before, after);
        check (changed > 300, "Mac keys in KEYBOARD mode: the page changed (" + juce::String (changed) + " points)");
        check (leds[glo] != 255, "GLO still in KEYBOARD mode");
    }

    // PROJECT > LOAD asks for the Mac's files; none chosen leaves the tracker as it was.
    // SEQ: each mode its page; back in TRACKER, the tracker's page as it was (not the LOOPER's left on screen).
    void testSeqPages (const juce::File& outDir)
    {
        Rig rig;
        rig.run (7.0);
        const auto song = rig.screenshot (outDir.getChildFile ("seq_song.png"));
        rig.tap (seq);
        rig.run (1.5);
        check (screenChange (song, rig.screenshot (outDir.getChildFile ("seq_drum.png"))) > 300, "SEQ: the DRUM MACHINE's page");
        rig.tap (seq);
        rig.run (1.5);
        const auto looper = rig.screenshot (outDir.getChildFile ("seq_looper.png"));
        check (screenChange (song, looper) > 300, "SEQ: the LOOPER's page");
        rig.tap (seq);
        rig.run (1.5);
        check (screenChange (song, rig.screenshot (outDir.getChildFile ("seq_tracker.png"))) < 300, "SEQ: back on the tracker's page");
    }

    // SAVE pressed by mistake: HOME closes its window, nothing written, and goes back to the tracker.
    void testHomeLeavesSave (const juce::File& outDir)
    {
        Rig rig;
        rig.run (7.0);
        const auto song = rig.screenshot (outDir.getChildFile ("home_song.png"));
        rig.tap (save);
        rig.run (0.5);
        const auto window = rig.screenshot (outDir.getChildFile ("home_save.png"));
        check (screenChange (song, window) > 300, "SAVE opens its window");
        rig.tap (home);
        rig.run (0.5);
        const auto back = rig.screenshot (outDir.getChildFile ("home_back.png"));
        check (screenChange (song, back) < 300, "HOME closes it, back on the tracker");
        juce::String name;
        juce::MemoryBlock contents;
        check (! rig.processor.machine.takeSavedSong (name, contents), "... with nothing saved");
    }

    void testLoadAsksTheMac (const juce::File& outDir)
    {
        Rig rig;
        check (rig.askLoad(), "LOAD asks for the Mac's files");
        rig.processor.machine.loadSong ({}, {});
        rig.run (0.5);
        rig.tap (down);
        rig.screenshot (outDir.getChildFile ("load_none.png"));
        check (rig.processor.machine.getError().isEmpty(), "none chosen: the tracker goes on");
    }

    // A song from the Mac opens and plays (OPLBendTest OUTDIR load FILE).
    void testLoadFile (const juce::File& outDir, const juce::File& song)
    {
        Rig rig;
        check (rig.askLoad(), "LOAD asks for the Mac's files");
        const auto before = rig.screenshot (outDir.getChildFile ("load_before.png"));
        juce::MemoryBlock contents;
        song.loadFileAsData (contents);
        rig.processor.machine.loadSong (song.getFileName().toUpperCase().replaceCharacter (' ', '_'), contents);
        rig.run (2.0);
        const auto after = rig.screenshot (outDir.getChildFile ("load_after.png"));
        check (screenChange (before, after) > 300, "the song opened (the screen changed)");
        rig.peak = 0.0;
        rig.tap (play);
        rig.run (4.0);
        std::cout << "      peak " << rig.peak << std::endl;
        check (rig.peak > 0.01, "the song plays");
    }

    // Both OPL3s sound, the switch takes at once, and the choice is saved with the project.
    void testEngines (const juce::File& outDir)
    {
        enum { noteC4 = 14 + 7 };
        Rig rig;
        rig.run (9.0);
        const auto notePeak = [&rig]
        {
            rig.processor.machine.key (noteC4, true);
            rig.peak = 0.0;
            rig.run (0.5);
            rig.processor.machine.key (noteC4, false);
            const auto p = rig.peak;
            rig.run (1.0);
            return p;
        };
        const auto fm1 = notePeak();
        rig.processor.engine->setValueNotifyingHost (1.0f);
        rig.run (0.1);
        const auto nuked = notePeak();
        std::cout << "      key peak: FM-1 " << fm1 << ", Nuked " << nuked << std::endl;
        check (fm1 > 0.01, "the FM-1's OPL3 sounds");
        check (nuked > 0.01, "Nuked OPL3 sounds");
        check (nuked > fm1 / 2.0 && nuked < fm1 * 2.0, "both at about the same level");
        rig.screenshot (outDir.getChildFile ("nuked.png"));

        rig.processor.tempoMaster->setValueNotifyingHost (1.0f);
        juce::MemoryBlock state;
        rig.processor.getStateInformation (state);
        Rig reloaded;
        reloaded.processor.setStateInformation (state.getData(), (int) state.getSize());
        check (reloaded.processor.engine->getIndex() == 1, "the engine is saved with the project");
        check (reloaded.processor.tempoMaster->getIndex() == 1, "the tempo master is saved with the project");
    }

    // Following Live (OPLBendTest OUTDIR sync SONGFILE): the song at Live's tempo, from Live's position,
    // through a tempo automation, with no drift.
    struct Onsets
    {
        static constexpr int hop = 64;
        std::vector<float> strength;

        explicit Onsets (const std::vector<float>& sound)
        {
            float before = 0.0f;
            for (size_t i = 0; i + hop <= sound.size(); i += hop)
            {
                auto e = 1.0e-6f;
                for (int k = 0; k < hop; ++k)
                    e += sound[i + (size_t) k] * sound[i + (size_t) k];
                const auto level = std::log (e);
                strength.push_back (juce::jmax (0.0f, level - before));
                before = level;
            }
        }
    };

    // Where `b` best matches `a` over the hops [from, to), within `range` hops of `around`: the lag of b, in hops.
    int lagHops (const Onsets& a, const Onsets& b, int from, int to, int around, int range)
    {
        auto bestLag = around;
        auto best = -1.0;
        for (int lag = around - range; lag <= around + range; ++lag)
        {
            auto sum = 0.0;
            for (int h = from; h < to; ++h)
                if (h + lag >= 0 && h + lag < (int) b.strength.size() && h < (int) a.strength.size())
                    sum += a.strength[(size_t) h] * b.strength[(size_t) (h + lag)];
            if (sum > best)
            {
                best = sum;
                bestLag = lag;
            }
        }
        return bestLag;
    }

    void testFollowLive (const juce::File& song)
    {
        juce::MemoryBlock contents;
        song.loadFileAsData (contents);
        auto songBpm = 0.0;
        const auto loaded = [&] (Rig& rig)
        {
            rig.askLoad();
            rig.processor.machine.loadSong (song.getFileName().toUpperCase().replaceCharacter (' ', '_'), contents);
            rig.run (2.0);
            songBpm = rig.processor.machine.getSongBpm();
        };
        // Live plays from `startBeat`; the song's lateness on it, the worst after the first half second.
        const auto follow = [&] (double startBeat, double seconds, std::function<double (double)> bpmAt, std::vector<float>* sound)
        {
            Rig rig;
            loaded (rig);
            rig.head.bpmAt = std::move (bpmAt);
            rig.head.bpm = rig.head.bpmAt (startBeat);
            rig.head.beat = startBeat;
            rig.head.playing = true;
            rig.capture = sound;
            rig.run (0.5);
            auto worst = 0.0;
            for (double t = 0.5; t < seconds; t += 0.05)
            {
                rig.run (0.05);
                worst = juce::jmax (worst, std::abs (rig.processor.machine.getLateMs()));
            }
            return worst;
        };
        const auto constant = [] (double bpm) { return [bpm] (double) { return bpm; }; };
        const auto report = [&] (const juce::String& what, double worst, const juce::String& claim)
        {
            std::cout << "      " << what << ": " << juce::String (worst, 2) << " ms at worst" << std::endl;
            check (worst <= 1.5, claim);
        };
        report ("120 BPM, 30 s", follow (0.0, 30.0, constant (120.0), nullptr), "the song keeps to Live's tempo, with no drift");
        report ("150 BPM, 30 s", follow (0.0, 30.0, constant (150.0), nullptr), "the song follows another tempo");
        report ("from bar 9", follow (32.0, 10.0, constant (120.0), nullptr), "the song starts where Live starts");
        report ("from bar 200", follow (796.0, 2.0, constant (120.0), nullptr), "the song starts far in, where Live starts");
        report ("120 -> 160 BPM over 8 bars", follow (0.0, 24.0, [] (double b) { return b < 32.0 ? 120.0 + 40.0 * b / 32.0 : 160.0; }, nullptr),
                "the song follows a tempo automation");

        // The sound itself: following Live at the song's own tempo, it plays as the tracker does on its own.
        std::vector<float> alone, following;
        {
            Rig rig;
            loaded (rig);
            rig.capture = &alone;
            rig.tap (play);
            rig.run (20.0);
        }
        follow (0.0, 20.0, constant (songBpm), &following);
        const Onsets a (alone), b (following);
        const auto hopsPerSecond = 44100.0 / Onsets::hop;
        // (the start differs by a few ms: START is read at the tracker's next frame; then, within 10 ms of it,
        // since the music repeats a beat or two away)
        const auto offset = lagHops (a, b, (int) (2 * hopsPerSecond), (int) (18 * hopsPerSecond), 0, 400);
        auto drift = 0;
        for (double t = 2.0; t + 4.0 <= 18.0; t += 4.0)
            drift = juce::jmax (drift, std::abs (lagHops (a, b, (int) (t * hopsPerSecond), (int) ((t + 4.0) * hopsPerSecond), offset, 7) - offset));
        std::cout << "      at the song's " << songBpm << " BPM, against the tracker alone: drift " << drift * Onsets::hop * 1000.0 / 44100.0
                  << " ms" << std::endl;
        check (drift <= 1, "following Live, the song sounds as on its own");
    }

    // Link, with another peer standing for Live: TEMPO TRACKER gives it the song's tempo; START starts and
    // stops it (OPLBendTest OUTDIR link SONGFILE).
    void testLink (const juce::File& song)
    {
        ableton::Link live (100.0);
        live.enableStartStopSync (true);
        live.enable (true);
        Rig rig;
        rig.processor.tempoMaster->setValueNotifyingHost (1.0f);
        juce::MemoryBlock contents;
        song.loadFileAsData (contents);
        rig.askLoad();
        rig.processor.machine.loadSong (song.getFileName().toUpperCase().replaceCharacter (' ', '_'), contents);
        rig.run (2.0);
        rig.processor.setNonRealtime (false);           // (Link's part, in real time)
        const auto wait = [&rig] (double seconds)       // (at the pace of real time: Link's clock)
        {
            for (auto end = juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0; juce::Time::getMillisecondCounterHiRes() < end;)
            {
                rig.run (512.0 / 44100.0);
                juce::Thread::sleep (10);
            }
        };
        wait (3.0);
        std::cout << "      Link peers seen by the plugin: " << rig.processor.machine.getLinkPeers() << std::endl;
        check (rig.processor.machine.getLinkPeers() > 0, "the plugin finds Live on Link");
        const auto tempo = live.captureAppSessionState().tempo();
        std::cout << "      Live's tempo: " << tempo << " (the song's " << rig.processor.machine.getSongBpm() << ")" << std::endl;
        check (std::abs (tempo - rig.processor.machine.getSongBpm()) < 0.01, "Live takes the song's tempo");
        const auto press = [&] { rig.processor.machine.key (play, true); wait (0.15); rig.processor.machine.key (play, false); };
        for (const auto* mode : { "TRACKER", "LIVE" })
        {
            rig.processor.tempoMaster->setValueNotifyingHost (juce::String (mode) == "TRACKER" ? 1.0f : 0.0f);
            wait (0.3);
            press();
            wait (0.5);
            check (live.captureAppSessionState().isPlaying(), juce::String ("START starts Live (TEMPO ") + mode + ")");
            rig.head.playing = true;                    // (Live plays)
            wait (1.0);
            press();
            wait (0.5);
            check (! live.captureAppSessionState().isPlaying(), juce::String ("START again stops Live (TEMPO ") + mode + ")");
            rig.head.playing = false;
            wait (0.5);
        }
    }

    void testRealtime (const juce::File& outDir, int songIndex, bool nuked)
    {
        Rig rig;
        rig.processor.engine->setValueNotifyingHost (nuked ? 1.0f : 0.0f);
        rig.loadSong (songIndex);
        rig.tap (play);
        rig.screenshot (outDir.getChildFile ("playing.png"));

        // Now as a host does: one block every 512 samples of wall time, for 60 s of song, from a realtime
        // thread (as a DAW's audio thread; a normal one is held up now and then, then catches up in bursts).
        rig.processor.setNonRealtime (false);
        struct Host : juce::Thread
        {
            explicit Host (Rig& r) : juce::Thread ("host audio"), rig (r) {}
            void run() override
            {
                const auto blockMs = 512.0 * 1000.0 / 44100.0;
                const auto start = juce::Time::getMillisecondCounterHiRes();
                auto seen = 0;
                for (int i = 0; i < (int) (60.0 * 1000.0 / blockMs) && ! threadShouldExit(); ++i)
                {
                    rig.processor.processBlock (rig.buffer, rig.midi);
                    peak = juce::jmax (peak, (double) rig.buffer.getMagnitude (0, 512));
                    if (rig.processor.machine.getUnderruns() != seen)
                    {
                        seen = rig.processor.machine.getUnderruns();
                        times.add (i * blockMs / 1000.0);
                    }
                    juce::Time::waitForMillisecondCounter ((juce::uint32) (start + (i + 1) * blockMs));
                }
            }
            Rig& rig;
            double peak = 0.0;
            juce::Array<double> times;
        } host (rig);
        host.startRealtimeThread (juce::Thread::RealtimeOptions().withPriority (10).withPeriodMs (512.0 * 1000.0 / 44100.0));
        while (host.isThreadRunning())
            juce::Thread::sleep (50);
        for (const auto t : host.times)
            std::cout << "      dropout at " << t << " s" << std::endl;
        const auto peak = host.peak;
        std::cout << "      realtime 60 s: dropouts " << rig.processor.machine.getUnderruns()
                  << ", peak " << peak << std::endl;
        check (peak > 0.001, "song is audible");
        check (rig.processor.machine.getUnderruns() == 0, "no dropouts in 60 s of song");
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const auto outDir = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "plugin_test");
    outDir.createDirectory();

    if (argc > 2 && juce::String (argv[2]) == "engines")
    {
        testEngines (outDir);
        testMacDpad (outDir);
    }
    else if (argc > 3 && juce::String (argv[2]) == "load")
    {
        testLoadFile (outDir, juce::File::getCurrentWorkingDirectory().getChildFile (argv[3]));
    }
    else if (argc > 3 && juce::String (argv[2]) == "sync")
    {
        testFollowLive (juce::File::getCurrentWorkingDirectory().getChildFile (argv[3]));
    }
    else if (argc > 3 && juce::String (argv[2]) == "link")
    {
        testLink (juce::File::getCurrentWorkingDirectory().getChildFile (argv[3]));
    }
    else if (argc > 2 && juce::String (argv[2]) == "seq")
    {
        Rig rig;                                        // (SEQ three times: TRACKER, DRUM, LOOPER, TRACKER)
        if (argc > 3)
        {
            juce::MemoryBlock contents;
            const auto song = juce::File::getCurrentWorkingDirectory().getChildFile (argv[3]);
            song.loadFileAsData (contents);
            rig.askLoad();
            rig.processor.machine.loadSong (song.getFileName().toUpperCase(), contents);
            rig.run (2.0);
        }
        else
            rig.run (7.0);
        if (argc > 4)                                   // (GLO in D-PAD, its help closed)
        {
            rig.tap (glo);
            rig.tap (down);
        }
        rig.processor.setNonRealtime (false);
        const auto wait = [&rig] (double seconds)
        {
            for (auto end = juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0; juce::Time::getMillisecondCounterHiRes() < end;)
            {
                rig.run (512.0 / 44100.0);
                juce::Thread::sleep (10);
            }
        };
        for (int i = 1; i <= 3; ++i)
        {
            rig.processor.machine.key (11, true);
            wait (0.12);
            rig.processor.machine.key (11, false);
            wait (1.0);
        }
        for (const auto t : { 0, 1, 3 })
        {
            wait (t);
            rig.screenshot (outDir.getChildFile ("seqrt" + juce::String (t) + ".png"));
        }
    }
    else if (argc > 2 && juce::String (argv[2]) == "song")
    {
        testRealtime (outDir, argc > 3 ? juce::String (argv[3]).getIntValue() : 0, argc > 4 && juce::String (argv[4]) == "nuked");
    }
    else
    {
        testAudioRates();
        testLedsAreSteady();
        testFilesSurviveReload (outDir);
        testEngines (outDir);
        testMacDpad (outDir);
        testLoadAsksTheMac (outDir);
        testHomeLeavesSave (outDir);
        testSeqPages (outDir);
    }

    std::cout << (failures == 0 ? "PASS" : "FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
