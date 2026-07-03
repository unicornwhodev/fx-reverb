#include "PluginProcessor.h"

#ifndef MUSIQUE_REVERB_DSP_TESTS
#define MUSIQUE_REVERB_DSP_TESTS 0
#endif

#if ! MUSIQUE_REVERB_DSP_TESTS
#include "PluginEditor.h"
#endif

#include "FXComponents.h"
#include <cmath>

namespace
{
static float dbToGain(float dB) { return std::pow(10.0f, dB / 20.0f); }

static float raw(const juce::AudioProcessorValueTreeState& apvts, const char* id, float fallback = 0.0f)
{
    if (auto* p = apvts.getRawParameterValue(id))
        return p->load();
    return fallback;
}

static void setParam(juce::AudioProcessorValueTreeState& apvts, const juce::String& id, float value)
{
    if (auto* parameter = apvts.getParameter(id))
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

static bool stateHasParameter(const juce::ValueTree& state, const juce::String& id)
{
    for (int index = 0; index < state.getNumChildren(); ++index)
    {
        const auto child = state.getChild(index);
        if (child.hasType("PARAM") && child.getProperty("id").toString() == id)
            return true;
    }
    return false;
}

static void ensureStateParamValue(juce::AudioProcessorValueTreeState& apvts,
                                  const juce::ValueTree& state,
                                  const juce::String& id,
                                  float value)
{
    if (!stateHasParameter(state, id))
        setParam(apvts, id, value);
}

static void setPresetDefault(juce::DynamicObject& object, const juce::Identifier& id, const juce::var& value)
{
    if (!object.hasProperty(id))
        object.setProperty(id, value);
}

static float clampPresetFloat(juce::DynamicObject& object, const juce::Identifier& id, float minimum, float maximum)
{
    const auto clamped = juce::jlimit(minimum, maximum, (float) object.getProperty(id));
    object.setProperty(id, (double) clamped);
    return clamped;
}

static int clampPresetInt(juce::DynamicObject& object, const juce::Identifier& id, int minimum, int maximum)
{
    const auto clamped = juce::jlimit(minimum, maximum, (int) std::round((float) object.getProperty(id)));
    object.setProperty(id, clamped);
    return clamped;
}

static float clampFloat(float value, float minimum, float maximum)
{
    return juce::jlimit(minimum, maximum, value);
}

static float clampBool(float value)
{
    return value > 0.5f ? 1.0f : 0.0f;
}
}

juce::StringArray MusiqueReverbProcessor::getAllParameterIds()
{
    return {
        "algorithm","size","decay","predelay","damping","width","mix",
        "early_level","tail_level","diffusion","low_cut","high_cut",
        "mod_depth","mod_rate","ducking","ducking_release","quality",
        "output","bypass","freeze","mono"
    };
}

MusiqueReverbProcessor::MusiqueReverbProcessor()
: AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                  .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
  parameters(*this, nullptr, "MusiqueReverb", createParameterLayout()) {}

juce::AudioProcessorValueTreeState::ParameterLayout MusiqueReverbProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    auto percent = juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f);
    auto hertz = juce::NormalisableRange<float>(20.0f, 20000.0f, 0.1f, 0.32f);
    auto lowCut = juce::NormalisableRange<float>(20.0f, 1200.0f, 0.1f, 0.38f);
    p.push_back(std::make_unique<juce::AudioParameterChoice>("algorithm", "Algorithm", juce::StringArray { "Room", "Plate", "Hall", "Chamber", "Space" }, 2));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("size", "Size", percent, 0.6f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("decay", "Decay", percent, 0.5f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("predelay", "PreDelay", juce::NormalisableRange<float>(0.0f, 250.0f, 0.1f), 20.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("damping", "Damping", percent, 0.4f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("width", "Width", juce::NormalisableRange<float>(0.0f, 1.2f, 0.001f), 1.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("mix", "Mix", juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 25.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("early_level", "Early Level", juce::NormalisableRange<float>(-36.0f, 6.0f, 0.1f), -8.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("tail_level", "Tail Level", juce::NormalisableRange<float>(-36.0f, 6.0f, 0.1f), 0.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("diffusion", "Diffusion", percent, 0.72f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("low_cut", "Low Cut", lowCut, 120.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("high_cut", "High Cut", hertz, 12000.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("mod_depth", "Mod Depth", percent, 0.28f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("mod_rate", "Mod Rate", juce::NormalisableRange<float>(0.02f, 2.0f, 0.001f, 0.45f), 0.22f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("ducking", "Ducking", percent, 0.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("ducking_release", "Ducking Release", juce::NormalisableRange<float>(40.0f, 1200.0f, 1.0f, 0.5f), 260.0f));
    p.push_back(std::make_unique<juce::AudioParameterChoice>("quality", "Quality", juce::StringArray { "Eco", "Studio", "High" }, 1));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("output", "Output", juce::NormalisableRange<float>(-24.0f, 12.0f, 0.1f), 0.0f));
    p.push_back(std::make_unique<juce::AudioParameterBool>("bypass", "Bypass", false));
    p.push_back(std::make_unique<juce::AudioParameterBool>("freeze", "Freeze", false));
    p.push_back(std::make_unique<juce::AudioParameterBool>("mono", "Mono", false));
    return { p.begin(), p.end() };
}

void MusiqueReverbProcessor::normalisePresetObject(juce::var& preset)
{
    auto* object = preset.getDynamicObject();
    if (object == nullptr)
        return;

    setPresetDefault(*object, "algorithm", 2);
    setPresetDefault(*object, "quality", 1);
    setPresetDefault(*object, "size", 0.6);
    setPresetDefault(*object, "decay", 0.5);
    setPresetDefault(*object, "predelay", 20.0);
    setPresetDefault(*object, "damping", 0.4);
    setPresetDefault(*object, "width", 1.0);
    setPresetDefault(*object, "mix", 25.0);
    setPresetDefault(*object, "early_level", -8.0);
    setPresetDefault(*object, "tail_level", 0.0);
    setPresetDefault(*object, "diffusion", 0.72);
    setPresetDefault(*object, "low_cut", 120.0);
    setPresetDefault(*object, "high_cut", 12000.0);
    setPresetDefault(*object, "mod_depth", 0.28);
    setPresetDefault(*object, "mod_rate", 0.22);
    setPresetDefault(*object, "ducking", 0.0);
    setPresetDefault(*object, "ducking_release", 260.0);
    setPresetDefault(*object, "output", 0.0);
    setPresetDefault(*object, "bypass", false);
    setPresetDefault(*object, "freeze", false);
    setPresetDefault(*object, "mono", false);

    clampPresetInt(*object, "algorithm", 0, 4);
    clampPresetInt(*object, "quality", 0, 2);
    clampPresetFloat(*object, "size", 0.0f, 1.0f);
    clampPresetFloat(*object, "decay", 0.0f, 1.0f);
    clampPresetFloat(*object, "predelay", 0.0f, 250.0f);
    clampPresetFloat(*object, "damping", 0.0f, 1.0f);
    clampPresetFloat(*object, "width", 0.0f, 1.2f);
    clampPresetFloat(*object, "mix", 0.0f, 100.0f);
    clampPresetFloat(*object, "early_level", -36.0f, 6.0f);
    clampPresetFloat(*object, "tail_level", -36.0f, 6.0f);
    clampPresetFloat(*object, "diffusion", 0.0f, 1.0f);
    clampPresetFloat(*object, "low_cut", 20.0f, 1200.0f);
    clampPresetFloat(*object, "high_cut", 20.0f, 20000.0f);
    clampPresetFloat(*object, "mod_depth", 0.0f, 1.0f);
    clampPresetFloat(*object, "mod_rate", 0.02f, 2.0f);
    clampPresetFloat(*object, "ducking", 0.0f, 1.0f);
    clampPresetFloat(*object, "ducking_release", 40.0f, 1200.0f);
    clampPresetFloat(*object, "output", -24.0f, 12.0f);
}

void MusiqueReverbProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    preparedSampleRate = sampleRate;
    preparedBlockCapacity = juce::jmax(juce::jmax(1, samplesPerBlock), 8192);
    reverb.reset();
    reverb.prepare(sampleRate, preparedBlockCapacity);
    reverbInputBuffer.setSize(2, preparedBlockCapacity, false, false, false);
    wetBuffer.setSize(2, preparedBlockCapacity, false, false, false);
    currentWetTrimDb.store(0.0f, std::memory_order_relaxed);
    duckGain = 1.0f;

    sizeSmoothed.reset(sampleRate, 0.04);
    decaySmoothed.reset(sampleRate, 0.06);
    predelaySmoothed.reset(sampleRate, 0.05);
    dampingSmoothed.reset(sampleRate, 0.05);
    widthSmoothed.reset(sampleRate, 0.05);
    mixSmoothed.reset(sampleRate, 0.04);
    earlySmoothed.reset(sampleRate, 0.05);
    tailSmoothed.reset(sampleRate, 0.05);
    diffusionSmoothed.reset(sampleRate, 0.05);
    lowCutSmoothed.reset(sampleRate, 0.08);
    highCutSmoothed.reset(sampleRate, 0.08);
    modDepthSmoothed.reset(sampleRate, 0.08);
    modRateSmoothed.reset(sampleRate, 0.08);
    duckingSmoothed.reset(sampleRate, 0.06);

    sizeSmoothed.setCurrentAndTargetValue(raw(parameters, "size", 0.6f));
    decaySmoothed.setCurrentAndTargetValue(raw(parameters, "decay", 0.5f));
    predelaySmoothed.setCurrentAndTargetValue(raw(parameters, "predelay", 20.0f));
    dampingSmoothed.setCurrentAndTargetValue(raw(parameters, "damping", 0.4f));
    widthSmoothed.setCurrentAndTargetValue(raw(parameters, "width", 1.0f));
    mixSmoothed.setCurrentAndTargetValue(raw(parameters, "mix", 25.0f) / 100.0f);
    earlySmoothed.setCurrentAndTargetValue(raw(parameters, "early_level", -8.0f));
    tailSmoothed.setCurrentAndTargetValue(raw(parameters, "tail_level", 0.0f));
    diffusionSmoothed.setCurrentAndTargetValue(raw(parameters, "diffusion", 0.72f));
    lowCutSmoothed.setCurrentAndTargetValue(raw(parameters, "low_cut", 120.0f));
    highCutSmoothed.setCurrentAndTargetValue(raw(parameters, "high_cut", 12000.0f));
    modDepthSmoothed.setCurrentAndTargetValue(raw(parameters, "mod_depth", 0.28f));
    modRateSmoothed.setCurrentAndTargetValue(raw(parameters, "mod_rate", 0.22f));
    duckingSmoothed.setCurrentAndTargetValue(raw(parameters, "ducking", 0.0f));
}

void MusiqueReverbProcessor::releaseResources()
{
    reverb.reset();
    reverbInputBuffer.setSize(0, 0);
    wetBuffer.setSize(0, 0);
    preparedBlockCapacity = 0;
    currentWetTrimDb.store(0.0f, std::memory_order_relaxed);
}

bool MusiqueReverbProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    return in == out && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
}

juce::AudioProcessorParameter* MusiqueReverbProcessor::getBypassParameter() const
{
    return const_cast<juce::AudioProcessorValueTreeState&>(parameters).getParameter("bypass");
}

void MusiqueReverbProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    visualState.captureInput(buffer);
    currentWetTrimDb.store(0.0f, std::memory_order_relaxed);
    visualState.captureOutput(buffer);
}

void MusiqueReverbProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);
    juce::ScopedNoDenormals noDenormals;
    visualState.captureInput(buffer);

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0)
    {
        currentWetTrimDb.store(0.0f, std::memory_order_relaxed);
        return;
    }

    if (raw(parameters, "bypass") > 0.5f)
    {
        processBlockBypassed(buffer, midiMessages);
        return;
    }

    const float rawMix = juce::jlimit(0.0f, 100.0f, raw(parameters, "mix", 25.0f));
    if (rawMix <= 0.0001f)
    {
        currentWetTrimDb.store(0.0f, std::memory_order_relaxed);
        visualState.captureOutput(buffer);
        return;
    }

    if (numSamples > wetBuffer.getNumSamples() || numSamples > reverbInputBuffer.getNumSamples())
    {
        preparedBlockCapacity = numSamples;
        reverbInputBuffer.setSize(2, preparedBlockCapacity, false, false, false);
        wetBuffer.setSize(2, preparedBlockCapacity, false, false, false);
    }

    reverbInputBuffer.clear(0, 0, numSamples);
    reverbInputBuffer.clear(1, 0, numSamples);
    wetBuffer.clear(0, 0, numSamples);
    wetBuffer.clear(1, 0, numSamples);

    const bool hasStereo = numChannels > 1;
    const bool mono = raw(parameters, "mono") > 0.5f;
    const bool frozen = raw(parameters, "freeze") > 0.5f;

    for (int sample = 0; sample < numSamples; ++sample)
    {
        float left = buffer.getSample(0, sample);
        float right = hasStereo ? buffer.getSample(1, sample) : left;
        if (mono)
            left = right = 0.5f * (left + right);

        reverbInputBuffer.setSample(0, sample, left);
        reverbInputBuffer.setSample(1, sample, right);
    }

    sizeSmoothed.setTargetValue(raw(parameters, "size", 0.6f));
    decaySmoothed.setTargetValue(raw(parameters, "decay", 0.5f));
    predelaySmoothed.setTargetValue(raw(parameters, "predelay", 20.0f));
    dampingSmoothed.setTargetValue(raw(parameters, "damping", 0.4f));
    widthSmoothed.setTargetValue(raw(parameters, "width", 1.0f));
    mixSmoothed.setTargetValue(rawMix / 100.0f);
    earlySmoothed.setTargetValue(raw(parameters, "early_level", -8.0f));
    tailSmoothed.setTargetValue(raw(parameters, "tail_level", 0.0f));
    diffusionSmoothed.setTargetValue(raw(parameters, "diffusion", 0.72f));
    lowCutSmoothed.setTargetValue(raw(parameters, "low_cut", 120.0f));
    highCutSmoothed.setTargetValue(raw(parameters, "high_cut", 12000.0f));
    modDepthSmoothed.setTargetValue(raw(parameters, "mod_depth", 0.28f));
    modRateSmoothed.setTargetValue(raw(parameters, "mod_rate", 0.22f));
    duckingSmoothed.setTargetValue(raw(parameters, "ducking", 0.0f));

    constexpr int reverbChunkSize = 48;
    int offset = 0;
    while (offset < numSamples)
    {
        const int chunkSize = juce::jmin(reverbChunkSize, numSamples - offset);
        const float sizeValue = sizeSmoothed.skip(chunkSize);
        const float decayValue = decaySmoothed.skip(chunkSize);
        const float predelayValue = predelaySmoothed.skip(chunkSize);
        const float dampingValue = dampingSmoothed.skip(chunkSize);
        const float widthValue = widthSmoothed.skip(chunkSize);
        const float mixValue = juce::jlimit(0.0f, 1.0f, mixSmoothed.skip(chunkSize));
        const float earlyValue = earlySmoothed.skip(chunkSize);
        const float tailValue = tailSmoothed.skip(chunkSize);
        const float diffusionValue = diffusionSmoothed.skip(chunkSize);
        const float lowCutValue = lowCutSmoothed.skip(chunkSize);
        const float highCutValue = highCutSmoothed.skip(chunkSize);
        const float modDepthValue = modDepthSmoothed.skip(chunkSize);
        const float modRateValue = modRateSmoothed.skip(chunkSize);
        const float duckingValue = duckingSmoothed.skip(chunkSize);

        const float trimDb = juce::jlimit(0.0f, 9.0f,
            juce::jmax(0.0f, sizeValue - 0.72f) * 6.0f
            + juce::jmax(0.0f, decayValue - 0.68f) * 7.5f
            + juce::jmax(0.0f, mixValue - 0.32f) * 5.0f
            + juce::jmax(0.0f, tailValue - 1.0f) * 2.0f
            + (frozen ? 3.0f : 0.0f));
        const float wetInputGain = dbToGain(-trimDb);
        currentWetTrimDb.store(trimDb, std::memory_order_relaxed);

        juce::AudioBuffer<float> inputChunk(reverbInputBuffer.getArrayOfWritePointers(), 2, offset, chunkSize);
        juce::AudioBuffer<float> wetChunk(wetBuffer.getArrayOfWritePointers(), 2, offset, chunkSize);

        AdvancedReverbEngine::Parameters rp;
        rp.algorithm = juce::jlimit(0, 4, (int) std::round(raw(parameters, "algorithm", (float) AdvancedReverbEngine::hall)));
        rp.quality = juce::jlimit(0, 2, (int) std::round(raw(parameters, "quality", (float) AdvancedReverbEngine::studio)));
        rp.size = sizeValue;
        rp.decay = decayValue;
        rp.predelayMs = predelayValue;
        rp.damping = dampingValue;
        rp.width = widthValue;
        rp.earlyLevelDb = earlyValue;
        rp.tailLevelDb = tailValue;
        rp.diffusion = diffusionValue;
        rp.lowCutHz = lowCutValue;
        rp.highCutHz = highCutValue;
        rp.modDepth = modDepthValue;
        rp.modRate = modRateValue;
        rp.freeze = frozen;
        reverb.process(inputChunk, wetChunk, rp);

        float dryRms = 0.0f;
        for (int ch = 0; ch < juce::jmin(2, numChannels); ++ch)
            dryRms += buffer.getRMSLevel(ch, offset, chunkSize);
        dryRms /= (float) juce::jmax(1, juce::jmin(2, numChannels));

        const float duckTarget = juce::jlimit(0.25f, 1.0f, 1.0f - duckingValue * juce::jlimit(0.0f, 0.75f, dryRms * 1.8f));
        const float releaseMs = raw(parameters, "ducking_release", 260.0f);
        const float releaseCoeff = std::exp(-1000.0f / (juce::jmax(1.0f, releaseMs) * (float) preparedSampleRate));
        if (duckTarget < duckGain)
            duckGain = duckTarget;
        else
            duckGain = duckTarget + (duckGain - duckTarget) * releaseCoeff;

        const float outputGain = dbToGain(raw(parameters, "output"));
        for (int i = 0; i < chunkSize; ++i)
        {
            const int sampleIndex = offset + i;
            const float wetMix = mixValue * duckGain;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const int wetChannel = ch == 0 ? 0 : 1;
                const float dry = buffer.getSample(ch, sampleIndex);
                const float wet = wetBuffer.getSample(wetChannel, sampleIndex) * wetInputGain;
                const float out = (dry * (1.0f - wetMix) + wet * wetMix) * outputGain;
                buffer.setSample(ch, sampleIndex, std::isfinite(out) ? juce::jlimit(-1.8f, 1.8f, out) : 0.0f);
            }
        }

        offset += chunkSize;
    }

    visualState.captureOutput(buffer);
}

void MusiqueReverbProcessor::getStateInformation(juce::MemoryBlock& destination)
{
    auto state = parameters.copyState();
    normaliseStateTree(state);
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destination);
}

void MusiqueReverbProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml == nullptr || !xml->hasTagName(parameters.state.getType()))
        return;

    auto state = juce::ValueTree::fromXml(*xml);
    normaliseStateTree(state);
    parameters.replaceState(state);

    ensureStateParamValue(parameters, state, "algorithm", 2.0f);
    ensureStateParamValue(parameters, state, "quality", 1.0f);
    ensureStateParamValue(parameters, state, "bypass", 0.0f);
    ensureStateParamValue(parameters, state, "freeze", 0.0f);
    ensureStateParamValue(parameters, state, "mono", 0.0f);
}

void MusiqueReverbProcessor::normaliseStateTree(juce::ValueTree& state)
{
    auto findParamChild = [&state](const juce::String& id) -> juce::ValueTree
    {
        for (int index = 0; index < state.getNumChildren(); ++index)
        {
            auto child = state.getChild(index);
            if (child.hasType("PARAM") && child.getProperty("id").toString() == id)
                return child;
        }
        return {};
    };

    auto readValue = [&state, &findParamChild](const juce::String& id, float fallback) -> float
    {
        if (auto child = findParamChild(id); child.isValid())
            return (float) child.getProperty("value", fallback);
        if (state.hasProperty(id))
            return (float) state.getProperty(id, fallback);
        return fallback;
    };

    auto writeValue = [&state, &findParamChild](const juce::String& id, float value)
    {
        auto child = findParamChild(id);
        if (!child.isValid())
        {
            child = juce::ValueTree("PARAM");
            child.setProperty("id", id, nullptr);
            state.addChild(child, -1, nullptr);
        }
        child.setProperty("value", value, nullptr);
        if (state.hasProperty(id))
            state.removeProperty(id, nullptr);
    };

    writeValue("algorithm", (float) juce::jlimit(0, 4, (int) std::round(readValue("algorithm", 2.0f))));
    writeValue("quality", (float) juce::jlimit(0, 2, (int) std::round(readValue("quality", 1.0f))));
    writeValue("size", clampFloat(readValue("size", 0.6f), 0.0f, 1.0f));
    writeValue("decay", clampFloat(readValue("decay", 0.5f), 0.0f, 1.0f));
    writeValue("predelay", clampFloat(readValue("predelay", 20.0f), 0.0f, 250.0f));
    writeValue("damping", clampFloat(readValue("damping", 0.4f), 0.0f, 1.0f));
    writeValue("width", clampFloat(readValue("width", 1.0f), 0.0f, 1.2f));
    writeValue("mix", clampFloat(readValue("mix", 25.0f), 0.0f, 100.0f));
    writeValue("early_level", clampFloat(readValue("early_level", -8.0f), -36.0f, 6.0f));
    writeValue("tail_level", clampFloat(readValue("tail_level", 0.0f), -36.0f, 6.0f));
    writeValue("diffusion", clampFloat(readValue("diffusion", 0.72f), 0.0f, 1.0f));
    writeValue("low_cut", clampFloat(readValue("low_cut", 120.0f), 20.0f, 1200.0f));
    writeValue("high_cut", clampFloat(readValue("high_cut", 12000.0f), 20.0f, 20000.0f));
    writeValue("mod_depth", clampFloat(readValue("mod_depth", 0.28f), 0.0f, 1.0f));
    writeValue("mod_rate", clampFloat(readValue("mod_rate", 0.22f), 0.02f, 2.0f));
    writeValue("ducking", clampFloat(readValue("ducking", 0.0f), 0.0f, 1.0f));
    writeValue("ducking_release", clampFloat(readValue("ducking_release", 260.0f), 40.0f, 1200.0f));
    writeValue("output", clampFloat(readValue("output", 0.0f), -24.0f, 12.0f));
    writeValue("bypass", clampBool(readValue("bypass", 0.0f)));
    writeValue("freeze", clampBool(readValue("freeze", 0.0f)));
    writeValue("mono", clampBool(readValue("mono", 0.0f)));
}

void MusiqueReverbProcessor::applyPresetCompat(const juce::var& preset)
{
    auto normalised = preset;
    normalisePresetObject(normalised);
    fx::preset::applyToAPVTS(parameters, normalised);
}

juce::AudioProcessorEditor* MusiqueReverbProcessor::createEditor()
{
#if MUSIQUE_REVERB_DSP_TESTS
    return nullptr;
#else
    return new MusiqueReverbEditor(*this);
#endif
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MusiqueReverbProcessor();
}
