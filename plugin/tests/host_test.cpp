// SPDX-License-Identifier: GPL-3.0-only
// Loads the installed OPLBend.vst3 the way a DAW does: scan, instantiate, process in real time,
// open the editor, save state, restore it on a second instance.
#include <juce_audio_utils/juce_audio_utils.h>

namespace
{
    int failures = 0;

    void check (bool ok, const juce::String& what)
    {
        std::cout << (ok ? "ok    " : "FAIL  ") << what << std::endl;
        if (! ok)
            ++failures;
    }

    void process (juce::AudioProcessor& plugin, double seconds)
    {
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        const auto blockMs = 512.0 * 1000.0 / 44100.0;
        const auto start = juce::Time::getMillisecondCounterHiRes();
        const auto blocks = (int) (seconds * 1000.0 / blockMs);
        for (int i = 0; i < blocks; ++i)
        {
            plugin.processBlock (buffer, midi);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
            juce::Time::waitForMillisecondCounter ((juce::uint32) (start + (i + 1) * blockMs));
        }
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const auto path = argc > 1 ? juce::String (argv[1])
                               : juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                     .getChildFile ("Library/Audio/Plug-Ins/VST3/OPLBend.vst3").getFullPathName();

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, path);
    check (found.size() == 1, "scan finds one plugin");
    if (found.isEmpty())
        return 1;

    const auto& description = *found[0];
    std::cout << "      " << description.name << " by " << description.manufacturerName
              << ", category " << description.category
              << ", instrument " << (int) description.isInstrument << std::endl;
    check (description.isInstrument, "listed as an instrument");

    juce::String error;
    auto plugin = format.createInstanceFromDescription (description, 44100.0, 512, error);
    check (plugin != nullptr, "instantiates " + error);
    if (plugin == nullptr)
        return 1;

    check (plugin->getTotalNumOutputChannels() == 2, "stereo output");
    check (plugin->acceptsMidi(), "event input (Ableton Live requires one on an instrument)");
    plugin->prepareToPlay (44100.0, 512);
    process (*plugin, 8.0);

    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (plugin->createEditorIfNeeded());
        check (editor != nullptr && editor->getWidth() > 0, "editor opens");
        process (*plugin, 1.0);
    }

    juce::MemoryBlock state;
    plugin->getStateInformation (state);
    check (state.getSize() > 16, "state saved, " + juce::String ((int) state.getSize()) + " bytes");

    auto second = format.createInstanceFromDescription (description, 48000.0, 256, error);
    check (second != nullptr, "second instance alongside the first");
    if (second != nullptr)
    {
        second->setStateInformation (state.getData(), (int) state.getSize());
        second->prepareToPlay (48000.0, 256);
        process (*second, 1.0);
        second->releaseResources();
    }

    plugin->releaseResources();
    second.reset();
    plugin.reset();
    std::cout << (failures == 0 ? "PASS" : "FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
