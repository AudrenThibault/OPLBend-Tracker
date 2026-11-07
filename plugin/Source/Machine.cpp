// SPDX-License-Identifier: GPL-3.0-only
#include "Machine.h"
#include "Firmware.h"
#include <ableton/Link.hpp>
#include <dlfcn.h>

namespace
{
    constexpr double guestRate = FM1N_RATE;
    constexpr int minBufferFrames = 2048;              // 46 ms ahead of the host, to begin with
    constexpr int maxBufferFrames = 16384;             // 370 ms: what a busy Mac may need (a dropout doubles it)
    constexpr int latencyFrames = minBufferFrames;     // told to the host, which makes up for it
    constexpr double linkQuantum = 4.0;                // (a bar of 4 beats)
    constexpr double startAhead = 0.1;                 // the song is launched this long before Live starts (s)
    constexpr double linkStartAhead = 0.15;            // START with Link: Live starts this long after (s), on a bar
    constexpr double linkWaitMax = 0.5;                // ... and the song waits for it at most this long past that
    constexpr juce::uint32 hostGoneMs = 250;           // no pull for this long: follow the wall clock
    constexpr int screenFrames = FM1N_RATE / 30;

    // The firmware's flash file system: FS_BASE and FS_FIN in firmware/compat/fm1_fs.c.
    constexpr juce::uint32 filesBase = 0x97000, filesEnd = 0xE0000;

    float hermite (float a, float b, float c, float d, float t) noexcept
    {
        const auto c1 = 0.5f * (c - a);
        const auto c2 = a - 2.5f * b + 2.0f * c - 0.5f * d;
        const auto c3 = 0.5f * (d - a) + 1.5f * (b - c);
        return ((c3 * t + c2) * t + c1) * t + b;
    }
}

//==============================================================================
// A private copy of the embedded firmware library: written to a temporary file, loaded, the file removed.
// Loading the same path twice would share one set of globals between instances.
struct Machine::Library
{
    void* handle = nullptr;
    juce::String problem;

#define FM1N_FUNCTIONS(X) X (flash_write) X (flash_read) X (start) X (stop) X (render) X (key) X (dpad) X (encoder) \
                          X (master) X (engine) X (screen) X (leds) X (error) X (version) X (offline) \
                          X (load_wanted) X (load) X (saved) X (transport) X (link) X (play) X (tracker)
#define FM1N_POINTER(name) decltype (&fm1n_##name) name = nullptr;
    FM1N_FUNCTIONS (FM1N_POINTER)

    Library()
    {
        // (A unique name: a library still loaded under the same path would be handed back as it is.)
        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("OPLBend-" + juce::Uuid().toString() + ".dylib");
        if (! file.replaceWithData (firmware::libfm1_native_dylib, (size_t) firmware::libfm1_native_dylibSize))
        {
            problem = "cannot write " + file.getFullPathName();
            return;
        }
        handle = dlopen (file.getFullPathName().toRawUTF8(), RTLD_NOW | RTLD_LOCAL);
        file.deleteFile();
        if (handle == nullptr)
        {
            problem = dlerror();
            return;
        }
#define FM1N_LOOKUP(name) name = reinterpret_cast<decltype (name)> (dlsym (handle, "fm1n_" #name)); \
                          if (name == nullptr) problem = "missing fm1n_" #name;
        FM1N_FUNCTIONS (FM1N_LOOKUP)
        if (problem.isEmpty() && version() != FM1N_VERSION)
            problem = "library version " + juce::String (version());
    }

    ~Library()
    {
        if (stop != nullptr)
            stop();
        if (handle != nullptr)
            dlclose (handle);
    }

    bool ok() const { return problem.isEmpty(); }
};

//==============================================================================
Machine::Machine() : juce::Thread ("OPLBend FM-1")
{
    audioData.allocate ((size_t) fifoFrames * 2, true);
    screen.allocate ((size_t) lcdSize * lcdSize * 4, true);
    lcd.allocate ((size_t) lcdSize * lcdSize, true);
    link = std::make_unique<ableton::Link> (120.0);
    link->enableStartStopSync (true);
    link->enable (true);
    // A realtime thread: it computes little (the firmware is fast), but must wake on time; as a normal one,
    // macOS kept it waiting tens of milliseconds now and then.
    if (! startRealtimeThread (juce::Thread::RealtimeOptions().withPriority (8).withPeriodMs (FM1N_BLOCK * 1000.0 / guestRate)))
        startThread (juce::Thread::Priority::highest);
}

Machine::~Machine()
{
    stopThread (4000);
    library.reset();
    link.reset();
}

//==============================================================================
void Machine::key (int id, bool down)
{
    const auto scope = commandFifo.write (1);
    if (scope.blockSize1 > 0)
        commands[(size_t) scope.startIndex1] = { CommandType::key, id, down ? 1 : 0 };
}

void Machine::dpad (int function, bool down)
{
    const auto scope = commandFifo.write (1);
    if (scope.blockSize1 > 0)
        commands[(size_t) scope.startIndex1] = { CommandType::dpad, function, down ? 1 : 0 };
}

void Machine::encoder (int index, int steps)
{
    const auto scope = commandFifo.write (1);
    if (scope.blockSize1 > 0)
        commands[(size_t) scope.startIndex1] = { CommandType::encoder, index, steps };
}

int Machine::getLatencySamples() const noexcept
{
    return juce::roundToInt (latencyFrames / ratio);
}

int Machine::getLinkPeers() const
{
    return (int) link->numPeers();
}

juce::String Machine::getError() const
{
    const juce::ScopedLock lock (requestLock);
    return error;
}

bool Machine::copyScreen (juce::uint8* rgba, juce::uint8* leds, juce::uint32& seenVersion)
{
    const juce::SpinLock::ScopedLockType lock (screenLock);
    if (seenVersion == screenVersion)
        return false;
    seenVersion = screenVersion;
    std::memcpy (rgba, screen.get(), (size_t) lcdSize * lcdSize * 4);
    std::memcpy (leds, ledLevels.data(), ledLevels.size());
    return true;
}

juce::MemoryBlock Machine::saveFiles()
{
    saveDone.reset();
    saveWanted = true;
    saveDone.wait (2000);
    const juce::ScopedLock lock (requestLock);
    return lastSaved;
}

void Machine::restoreFiles (const juce::MemoryBlock& files)
{
    const juce::ScopedLock lock (requestLock);
    pendingRestore = files;
    lastSaved = files;
    restoreWanted = true;
}

void Machine::loadSong (const juce::String& name, const juce::MemoryBlock& contents)
{
    const juce::ScopedLock lock (requestLock);
    pendingSongName = name;
    pendingSong = contents;
    songWanted = true;
}

bool Machine::takeSavedSong (juce::String& name, juce::MemoryBlock& contents)
{
    if (! songSaved.exchange (false))
        return false;
    const juce::ScopedLock lock (requestLock);
    name = savedSongName;
    contents = savedSong;
    return true;
}

//==============================================================================
void Machine::boot()
{
    juce::MemoryBlock files;
    {
        const juce::ScopedLock lock (requestLock);
        files = pendingRestore;
        restoreWanted = false;
    }

    library.reset();                                    // (the old FM-1 stops first)
    library = std::make_unique<Library>();
    if (! library->ok())
    {
        fail ("Firmware: " + library->problem);
        return;
    }
    failed = false;
    appliedMaster = -1.0f;
    appliedEngine = -1;
    appliedOffline = -1;
    lcdVersion = 0;
    serviceRequests();                                  // (MASTER and the engine before the first sound)

    if (files.getSize() == filesEnd - filesBase)
        library->flash_write (filesBase, static_cast<const juce::uint8*> (files.getData()), (juce::uint32) files.getSize());
    if (library->start() != 0)
        fail ("Firmware: no thread");
}

void Machine::fail (const juce::String& why)
{
    const juce::ScopedLock lock (requestLock);
    error = why;
    failed = true;
}

void Machine::serviceRequests()
{
    if (restoreWanted.load())
        boot();
    if (library == nullptr || ! library->ok())
        return;

    if (saveWanted.exchange (false))
    {
        juce::MemoryBlock files (filesEnd - filesBase);
        library->flash_read (filesBase, static_cast<juce::uint8*> (files.getData()), (juce::uint32) files.getSize());
        const juce::ScopedLock lock (requestLock);
        lastSaved = std::move (files);
        saveDone.signal();
    }

    if (library->load_wanted() != 0)
        loadRequested = true;
    char savedName[40];
    if (const auto n = library->saved (savedName, (int) sizeof (savedName), savedScratch.get(), (juce::uint32) FM1N_SONG_MAX); n > 0)
    {
        const juce::ScopedLock lock (requestLock);
        savedSongName = savedName;
        savedSong.replaceAll (savedScratch.get(), (size_t) n);
        songSaved = true;
    }
    if (songWanted.exchange (false))
    {
        const juce::ScopedLock lock (requestLock);
        library->load (pendingSongName.isEmpty() ? nullptr : pendingSongName.toRawUTF8(),
                       static_cast<const juce::uint8*> (pendingSong.getData()), (juce::uint32) pendingSong.getSize());
    }

    const auto scope = commandFifo.read (commandFifo.getNumReady());
    const auto apply = [this] (const Command& c)
    {
        if (c.type == CommandType::key)
            library->key (c.a, c.b);
        else if (c.type == CommandType::dpad)
            library->dpad (c.a, c.b);
        else
            library->encoder (c.a, c.b);
    };
    for (int i = 0; i < scope.blockSize1; ++i)
        apply (commands[(size_t) (scope.startIndex1 + i)]);
    for (int i = 0; i < scope.blockSize2; ++i)
        apply (commands[(size_t) (scope.startIndex2 + i)]);

    if (const auto wanted = master.load(); wanted != appliedMaster)
    {
        appliedMaster = wanted;
        library->master (juce::roundToInt (juce::jlimit (0.0f, 1.0f, wanted) * 1023.0f));
    }
    if (const auto wanted = engine.load(); wanted != appliedEngine)
    {
        appliedEngine = wanted;
        library->engine (wanted);
    }
    if (const auto wanted = offlineRender.load() ? 1 : 0; wanted != appliedOffline)
    {
        appliedOffline = wanted;
        library->offline (wanted);                      // (an export: the tracker's frames, all of them)
    }
}

void Machine::publishScreen()
{
    const auto version = library->screen (lcd.get());
    if (version == lcdVersion)
        return;
    lcdVersion = version;

    std::array<juce::uint8, numControls> leds;
    library->leds (leds.data());
    const juce::SpinLock::ScopedLockType lock (screenLock);
    auto* out = screen.get();
    for (int i = 0; i < lcdSize * lcdSize; ++i, out += 4)
    {
        const auto c = lcd[i];
        out[0] = (juce::uint8) (((c >> 11) & 31) * 255 / 31);
        out[1] = (juce::uint8) (((c >> 5) & 63) * 255 / 63);
        out[2] = (juce::uint8) ((c & 31) * 255 / 31);
        out[3] = 255;
    }
    ledLevels = leds;
    ++screenVersion;
}

void Machine::runChunk (bool keepAudio)
{
    followHost (keepAudio);
    if (library->render (rendered.data(), FM1N_BLOCK) != FM1N_BLOCK)
        return;
    leadHost();
    char why[64];
    if (library->error (why, sizeof (why)))
    {
        fail ("Stopped: " + juce::String (why));
        return;
    }
    if (keepAudio)
    {
        constexpr float scale = 1.0f / 8388608.0f;      // 24-bit
        const auto scope = audioFifo.write (juce::jmin (FM1N_BLOCK, audioFifo.getFreeSpace()));
        for (int i = 0; i < scope.blockSize1 * 2; ++i)
            audioData[scope.startIndex1 * 2 + i] = (float) rendered[(size_t) i] * scale;
        for (int i = 0; i < scope.blockSize2 * 2; ++i)
            audioData[scope.startIndex2 * 2 + i] = (float) rendered[(size_t) (scope.blockSize1 * 2 + i)] * scale;
        renderedFrames += scope.blockSize1 + scope.blockSize2;
    }
    if ((sinceScreen += FM1N_BLOCK) >= screenFrames)
    {
        sinceScreen = 0;
        publishScreen();
    }
}

// The host's position when the frame about to be rendered is heard, less the latency the host makes up for:
// the song follows it (native/fm1_host.c fm1n_transport). Live starting or stopping starts or stops the song.
void Machine::followHost (bool hostThere)
{
    Snapshot s;
    {
        const juce::SpinLock::ScopedLockType lock (transportLock);
        s = snapshot;
    }
    const auto at = [this] (juce::int64 consumed)     // (2: the resampler's delay)
    {
        return (double) (renderedFrames - consumed + 2 - latencyFrames) / guestRate;
    };
    const auto sinceEdge = at (s.edge);
    const auto playing = hostThere && (sinceEdge >= 0.0 ? s.transport.playing : ! s.transport.playing);
    const auto starting = hostThere && s.transport.playing && sinceEdge < 0.0 && sinceEdge > -startAhead;
    const auto beat = s.transport.beat + at (s.consumed) * s.transport.bpm / 60.0;
    const auto leads = tempoMaster.load() == (int) TempoMaster::tracker && ! offlineRender.load() && link->numPeers() > 0;
    // (START with Link: the song holds on its first line until Live starts, then starts with it)
    if (waitingForLive && (playing || ! trackerPlaying || juce::Time::getMillisecondCounterHiRes() > waitForLiveUntil))
        waitingForLive = false;
    library->transport (leads ? 2 : 1, playing ? 1 : starting || waitingForLive ? 2 : 0, beat, s.transport.bpm);
    library->link (! offlineRender.load() && link->numPeers() > 0 ? 1 : 0);

    // (the song is launched a little ahead: it waits, then starts with Live, its first row on time)
    if ((playing || starting) != hostPlaying)
    {
        hostPlaying = playing || starting;
        if (! hostPlaying)
            library->play (0);
        else if (! leads || ! trackerPlaying)
            library->play (1);
    }
}

// The tracker leading: Live takes the song's tempo through Link; with Link on, START starts and stops Live too.
void Machine::leadHost()
{
    double bpm = 0.0, late = 0.0;
    int start = 0;
    trackerPlaying = library->tracker (&bpm, &start, &late) != 0;
    lateMs = late;
    songBpm = bpm;
    const auto leads = tempoMaster.load() == (int) TempoMaster::tracker;
    if (! leads)
        publishedBpm = 0.0;
    if (offlineRender.load())
        return;

    // (the session's tempo, looked at now and then: a peer joining may bring its own)
    const auto look = leads && (std::abs (bpm - publishedBpm) > 0.0005 || ++sinceLinkLook >= 32);
    if (! look && (start == 0 || link->numPeers() == 0))
        return;
    sinceLinkLook = 0;
    auto state = link->captureAppSessionState();
    const auto now = link->clock().micros();
    // (against Link's own playing state, not Live's: Live may not have followed the last one)
    const auto newStart = start != 0 && link->numPeers() > 0 && (start > 0) != state.isPlaying();
    const auto newTempo = look && std::abs (state.tempo() - bpm) > 0.0005;
    if (newTempo)
        state.setTempo (bpm, now);
    publishedBpm = bpm;
    if (! newTempo && ! newStart)
        return;
    if (newStart && start > 0)
    {
        // Live would wait for the session's next bar (it blinks): a bar starts just ahead instead, and Live with it
        const auto at = now + std::chrono::microseconds ((juce::int64) (linkStartAhead * 1.0e6));
        state.forceBeatAtTime (0.0, at, linkQuantum);
        state.setIsPlaying (true, at);
        waitingForLive = true;
        waitForLiveUntil = juce::Time::getMillisecondCounterHiRes() + (linkStartAhead + linkWaitMax) * 1000.0;
    }
    else if (newStart)
    {
        state.setIsPlaying (false, now);
    }
    link->commitAppSessionState (state);
}

void Machine::run()
{
    boot();

    while (! threadShouldExit())
    {
        serviceRequests();

        if (failed.load())
        {
            wait (20);
            continue;
        }

        const auto now = juce::Time::getMillisecondCounter();
        const auto hostThere = now - lastPullMs.load() < hostGoneMs;

        if (hostThere)
        {
            // Stay as far ahead of the host as the buffer allows, then wait for it.
            if (audioFifo.getNumReady() >= targetFrames.load())
            {
                wait (1);
                continue;
            }
        }
        else if (now - lastTick < (juce::uint32) (FM1N_BLOCK * 1000.0 / guestRate) + 1)
        {
            // No audio callbacks (engine off, device disabled): the screen and keys still follow the clock.
            wait (1);
            continue;
        }

        lastTick = now;
        runChunk (hostThere);
    }
}

//==============================================================================
void Machine::prepare (double hostSampleRate, int maxBlock)
{
    ratio = guestRate / hostSampleRate;
    position = 0.0;
    primed = false;
    juce::zeromem (history, sizeof (history));

    const auto blockFrames = (int) std::ceil (maxBlock * ratio) + 2;
    targetFrames = juce::jlimit (minBufferFrames, maxBufferFrames, blockFrames * 2);
}

bool Machine::nextFrame (bool offline) noexcept
{
    if (scratchRead == scratchCount)
    {
        // Rendering: the firmware sets the pace, whatever it takes, unless it has stopped.
        while (offline && audioFifo.getNumReady() == 0 && ! failed.load())
        {
            lastPullMs = juce::Time::getMillisecondCounter();
            juce::Thread::yield();
        }

        const auto scope = audioFifo.read (juce::jmin (scratchSize, audioFifo.getNumReady()));
        if (scope.blockSize1 > 0)
            std::memcpy (scratch, audioData + scope.startIndex1 * 2, (size_t) scope.blockSize1 * 2 * sizeof (float));
        if (scope.blockSize2 > 0)
            std::memcpy (scratch + scope.blockSize1 * 2, audioData + scope.startIndex2 * 2, (size_t) scope.blockSize2 * 2 * sizeof (float));
        scratchRead = 0;
        scratchCount = scope.blockSize1 + scope.blockSize2;
        if (scratchCount == 0)
            return false;
    }

    std::memmove (history[0], history[1], 3 * sizeof (history[0]));
    history[3][0] = scratch[scratchRead * 2];
    history[3][1] = scratch[scratchRead * 2 + 1];
    ++scratchRead;
    ++consumedFrames;
    return true;
}

void Machine::pull (float* left, float* right, int numSamples, bool offline, const Transport& transport) noexcept
{
    lastPullMs = juce::Time::getMillisecondCounter();
    offlineRender.store (offline);
    {
        const juce::SpinLock::ScopedTryLockType lock (transportLock);
        if (lock.isLocked())
        {
            if (transport.playing != snapshot.transport.playing)
                snapshot.edge = consumedFrames;
            snapshot.transport = transport;
            snapshot.consumed = consumedFrames;
        }
    }

    if (offline)
        primed = true;
    else if (! primed)
        primed = audioFifo.getNumReady() >= targetFrames.load() / 2;

    auto i = 0;
    for (; primed && i < numSamples; ++i)
    {
        auto ran = false;
        while (position >= 1.0)
        {
            if (! nextFrame (offline))
            {
                ran = true;
                break;
            }
            position -= 1.0;
        }
        if (ran)
            break;

        const auto t = (float) position;
        left[i]  = hermite (history[0][0], history[1][0], history[2][0], history[3][0], t);
        right[i] = hermite (history[0][1], history[1][1], history[2][1], history[3][1], t);
        position += ratio;
    }

    if (i < numSamples)
    {
        juce::FloatVectorOperations::clear (left + i, numSamples - i);
        juce::FloatVectorOperations::clear (right + i, numSamples - i);
        if (primed && ! offline)
        {
            underruns.fetch_add (1);
            primed = false;
            targetFrames = juce::jmin (maxBufferFrames, targetFrames.load() * 2);   // (more ahead from now on)
        }
    }
}
