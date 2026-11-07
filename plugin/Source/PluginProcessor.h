// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Machine.h"

class OPLBendProcessor : public juce::AudioProcessor
{
public:
    OPLBendProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "OPLBend"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    Machine machine;
    juce::AudioParameterFloat* master = nullptr;
    juce::AudioParameterChoice* engine = nullptr;      // the OPL3: the FM-1's own, or Nuked OPL3
    juce::AudioParameterChoice* tempoMaster = nullptr; // who leads the tempo: Live, or the tracker (through Link)

    int editorWidth = 0;                               // (the editor's, kept while the plugin lives)

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OPLBendProcessor)
};
