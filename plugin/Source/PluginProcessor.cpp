// SPDX-License-Identifier: GPL-3.0-only
#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr int stateMagic = 0x424C504F;             // "OPLB"
    constexpr int stateVersion = 3;                    // 2: the engine after MASTER; 3: then the tempo master
}

OPLBendProcessor::OPLBendProcessor()
    : juce::AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    addParameter (master = new juce::AudioParameterFloat (juce::ParameterID { "master", 1 }, "Master", 0.0f, 1.0f, 0.5f));
    addParameter (engine = new juce::AudioParameterChoice (juce::ParameterID { "engine", 1 }, "Engine",
                                                           juce::StringArray { "FM-1", "Nuked" }, 0));
    addParameter (tempoMaster = new juce::AudioParameterChoice (juce::ParameterID { "tempo", 1 }, "Tempo master",
                                                                juce::StringArray { "Live", "Tracker" }, 0));
}

void OPLBendProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    machine.prepare (sampleRate, samplesPerBlock);
    setLatencySamples (machine.getLatencySamples());
}

bool OPLBendProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void OPLBendProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    machine.setMaster (master->get());
    machine.setEngine ((Machine::Engine) engine->getIndex());
    machine.setTempoMaster ((Machine::TempoMaster) tempoMaster->getIndex());

    Machine::Transport transport;
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
        {
            transport.playing = position->getIsPlaying();
            transport.beat = position->getPpqPosition().orFallback (0.0);
            transport.bpm = position->getBpm().orFallback (120.0);
        }
    machine.pull (buffer.getWritePointer (0), buffer.getWritePointer (1), buffer.getNumSamples(), isNonRealtime(), transport);
}

juce::AudioProcessorEditor* OPLBendProcessor::createEditor()
{
    return new OPLBendEditor (*this);
}

//==============================================================================
void OPLBendProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto files = machine.saveFiles();

    juce::MemoryOutputStream out (destData, false);
    out.writeInt (stateMagic);
    out.writeInt (stateVersion);
    out.writeFloat (master->get());
    out.writeInt (engine->getIndex());
    out.writeInt (tempoMaster->getIndex());
    out.writeInt ((int) files.getSize());
    juce::GZIPCompressorOutputStream gzip (out);
    gzip.write (files.getData(), files.getSize());
}

void OPLBendProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::MemoryInputStream in (data, (size_t) sizeInBytes, false);
    if (in.readInt() != stateMagic)
        return;
    const auto version = in.readInt();
    if (version < 1 || version > stateVersion)
        return;

    master->setValueNotifyingHost (master->convertTo0to1 (in.readFloat()));
    const auto chosen = version >= 2 ? in.readInt() : 0;
    engine->setValueNotifyingHost (engine->convertTo0to1 (juce::jlimit (0, 1, chosen)));
    machine.setEngine ((Machine::Engine) engine->getIndex());
    const auto leader = version >= 3 ? in.readInt() : 0;
    tempoMaster->setValueNotifyingHost (tempoMaster->convertTo0to1 (juce::jlimit (0, 1, leader)));

    const auto size = in.readInt();
    if (size <= 0)
        return;

    juce::MemoryBlock files;
    juce::GZIPDecompressorInputStream gzip (in);
    gzip.readIntoMemoryBlock (files);
    if ((int) files.getSize() == size)
        machine.restoreFiles (files);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new OPLBendProcessor();
}
