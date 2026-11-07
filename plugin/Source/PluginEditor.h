// SPDX-License-Identifier: GPL-3.0-only
// The FM-1's panel: screen, knobs, buttons and the 27-key keyboard; under the screen, the OPL3 to play with.
// It is laid out at one size and scaled to the window, which keeps its proportions when resized.
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "MacKeys.h"

class OPLBendEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit OPLBendEditor (OPLBendProcessor&);
    ~OPLBendEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void focusLost (FocusChangeType) override;

private:
    class Knob;
    class Button;
    class Keyboard;
    class ChoiceSwitch;
    class Screen;
    class Plate;

    void timerCallback() override;
    void layOut();
    void chooseSong();
    void saveSong (const juce::String& name, const juce::MemoryBlock& contents);

    OPLBendProcessor& processor;
    Machine& machine;

    juce::HeapBlock<juce::uint8> frame;
    std::array<juce::uint8, Machine::numControls> leds {};
    juce::uint32 seenVersion = 0;

    juce::Component content;                            // everything, at the design size, scaled to the window
    std::unique_ptr<Screen> screen;
    juce::Label statusLabel;
    std::unique_ptr<juce::LookAndFeel> dialLook;       // MASTER drawn as the other knobs
    juce::Slider masterKnob;
    juce::Label masterLabel;
    std::unique_ptr<juce::SliderParameterAttachment> masterAttachment;
    juce::OwnedArray<Knob> knobs;
    juce::OwnedArray<Button> buttons;
    std::unique_ptr<Plate> buttonPlate;
    std::unique_ptr<Keyboard> keyboard;
    std::unique_ptr<ChoiceSwitch> engineSwitch, tempoSwitch;

    std::unique_ptr<MacKeys> macKeys;
    std::unique_ptr<juce::FileChooser> chooser;        // PROJECT > LOAD: the Mac's files
    bool choosing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OPLBendEditor)
};
