// SPDX-License-Identifier: GPL-3.0-only
// One FM-1: the tracker's firmware compiled for the Mac (native/fm1_host.c), embedded in the plugin as a library.
// The firmware keeps its state in globals, so each instance loads its own copy of the library.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include "../native/fm1_native.h"

namespace ableton { class Link; }

class Machine : private juce::Thread
{
public:
    static constexpr int lcdSize = FM1N_LCD;
    static constexpr int numControls = FM1N_CONTROLS;
    enum class Engine { fm1 = 0, nuked = 1 };
    enum class TempoMaster { live = 0, tracker = 1 };   // tracker: Live follows the song's tempo through Link

    // The host's playback at a block's start.
    struct Transport { bool playing = false; double beat = 0.0, bpm = 120.0; };

    Machine();
    ~Machine() override;

    // Message thread (one caller at a time).
    void key (int id, bool down);
    void dpad (int function, bool down);               // FM1N_DPAD_*: the D-PAD's, in every GLO mode
    void encoder (int index, int steps);

    // Any thread.
    void setMaster (float zeroToOne) noexcept { master.store (zeroToOne); }
    void setEngine (Engine e) noexcept { engine.store ((int) e); }
    void setTempoMaster (TempoMaster m) noexcept { tempoMaster.store ((int) m); }
    int getLatencySamples() const noexcept;            // the sound is computed this far ahead (host samples)
    int getLinkPeers() const;                          // the other Link apps (Live, with LINK on)
    double getLateMs() const noexcept { return lateMs.load(); }   // following Live: how late the song is on it
    double getSongBpm() const noexcept { return songBpm.load(); } // the song's own tempo
    int getUnderruns() const noexcept { return underruns.load(); }
    juce::String getError() const;

    // A new frame since `seenVersion`? Copies the RGBA screen and the LED levels.
    bool copyScreen (juce::uint8* rgba, juce::uint8* leds, juce::uint32& seenVersion);

    // The firmware's saved files (its flash file system), and restarting the device on saved ones.
    juce::MemoryBlock saveFiles();
    void restoreFiles (const juce::MemoryBlock& files);

    // PROJECT > LOAD asks for a song from the Mac (true once); the song chosen there opens in the tracker.
    // No name: none chosen (the tracker shows the songs saved in the project instead).
    bool takeLoadRequest() noexcept { return loadRequested.exchange (false); }
    void loadSong (const juce::String& name, const juce::MemoryBlock& contents);

    // PROJECT > SAVE: the song just saved (its file name and contents), once, for the Mac.
    bool takeSavedSong (juce::String& name, juce::MemoryBlock& contents);

    // Audio thread.
    void prepare (double hostSampleRate, int maxBlock);
    void pull (float* left, float* right, int numSamples, bool offline, const Transport& transport) noexcept;

private:
    struct Library;
    enum class CommandType { key, dpad, encoder };
    struct Command { CommandType type; int a, b; };

    void run() override;
    void boot();
    void serviceRequests();
    void runChunk (bool keepAudio);
    void followHost (bool hostThere);
    void leadHost();
    void publishScreen();
    void fail (const juce::String& why);
    bool nextFrame (bool offline) noexcept;

    std::unique_ptr<Library> library;

    juce::AbstractFifo commandFifo { 256 };
    std::array<Command, 256> commands;

    // Guest audio, interleaved, at the FM-1's rate.
    static constexpr int fifoFrames = 1 << 15;
    juce::AbstractFifo audioFifo { fifoFrames };
    juce::HeapBlock<float> audioData;
    std::atomic<int> targetFrames { 2048 };
    std::atomic<juce::uint32> lastPullMs { 0 };
    std::atomic<int> underruns { 0 };
    std::atomic<float> master { 0.5f };
    std::atomic<int> engine { 0 }, tempoMaster { 0 };
    std::atomic<bool> offlineRender { false };
    std::array<juce::int32, FM1N_BLOCK * 2> rendered {};

    // The host's playback, as the audio thread saw it at its last block's start: the guest frames consumed
    // then, and when it last started or stopped. The worker works out where the frame it renders falls.
    struct Snapshot { Transport transport; juce::int64 consumed = 0, edge = std::numeric_limits<juce::int64>::min() / 2; };
    juce::SpinLock transportLock;
    Snapshot snapshot;
    juce::int64 consumedFrames = 0;                    // (audio thread)
    juce::int64 renderedFrames = 0;                    // (worker)
    bool hostPlaying = false, trackerPlaying = false;  // (worker)
    bool waitingForLive = false;                       // (worker: START with Link, until Live starts)
    double waitForLiveUntil = 0.0;
    double publishedBpm = 0.0;
    int sinceLinkLook = 0;
    std::atomic<double> lateMs { 0.0 }, songBpm { 0.0 };
    std::unique_ptr<ableton::Link> link;

    // Resampler state (audio thread).
    static constexpr int scratchSize = 256;
    float scratch[scratchSize * 2] = {};
    int scratchRead = 0, scratchCount = 0;
    double ratio = 1.0, position = 0.0;
    float history[4][2] = {};
    bool primed = false;

    juce::SpinLock screenLock;
    juce::HeapBlock<juce::uint8> screen;
    juce::HeapBlock<juce::uint16> lcd;
    std::array<juce::uint8, numControls> ledLevels {};
    juce::uint32 screenVersion = 0, lcdVersion = 0;
    int sinceScreen = 0;

    // Save and restore requests, served between chunks.
    mutable juce::CriticalSection requestLock;
    juce::MemoryBlock pendingRestore, lastSaved, pendingSong, savedSong;
    juce::String error, pendingSongName, savedSongName;
    std::atomic<bool> restoreWanted { false }, saveWanted { false }, songWanted { false }, loadRequested { false }, failed { false };
    std::atomic<bool> songSaved { false };
    juce::HeapBlock<juce::uint8> savedScratch { (size_t) FM1N_SONG_MAX };   // (worker)
    juce::WaitableEvent saveDone;
    float appliedMaster = -1.0f;
    int appliedEngine = -1, appliedOffline = -1;
    juce::uint32 lastTick = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Machine)
};
