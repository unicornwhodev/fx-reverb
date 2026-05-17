#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
static float dbToGain(float dB) { return std::pow(10.0f, dB / 20.0f); }

static float raw(juce::AudioProcessorValueTreeState& apvts, const char* id, float fallback = 0.0f)
{
    if (auto* p = apvts.getRawParameterValue(id))
        return p->load();
    return fallback;
}
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

void MusiqueReverbProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    preparedSampleRate = sampleRate;
    reverb.reset();
    reverb.prepare(sampleRate, samplesPerBlock);
    wetBuffer.setSize(2, samplesPerBlock, false, false, true);
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
    bypassSmoothed.reset(sampleRate, 0.025);

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
    bypassSmoothed.setCurrentAndTargetValue(raw(parameters, "bypass", 0.0f) > 0.5f ? 0.0f : 1.0f);
}
void MusiqueReverbProcessor::releaseResources()
{
    reverb.reset();
    wetBuffer.setSize(0, 0);
    currentWetTrimDb.store(0.0f, std::memory_order_relaxed);
}

bool MusiqueReverbProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{ return l.getMainInputChannelSet()==juce::AudioChannelSet::stereo() && l.getMainOutputChannelSet()==juce::AudioChannelSet::stereo(); }

void MusiqueReverbProcessor::processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    visualState.captureInput(b);

    const int numSamples = b.getNumSamples();
    if (numSamples <= 0)
    {
        currentWetTrimDb.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const bool mono = parameters.getRawParameterValue("mono")->load() > 0.5f;
    const bool bypass = parameters.getRawParameterValue("bypass")->load() > 0.5f;
    const bool frozen = parameters.getRawParameterValue("freeze")->load() > 0.5f;
    const float output = parameters.getRawParameterValue("output")->load();

    if (mono)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            float m = 0.5f * (b.getSample(0, i) + b.getSample(1, i));
            b.setSample(0, i, m);
            b.setSample(1, i, m);
        }
    }

    sizeSmoothed.setTargetValue(raw(parameters, "size", 0.6f));
    decaySmoothed.setTargetValue(raw(parameters, "decay", 0.5f));
    predelaySmoothed.setTargetValue(raw(parameters, "predelay", 20.0f));
    dampingSmoothed.setTargetValue(raw(parameters, "damping", 0.4f));
    widthSmoothed.setTargetValue(raw(parameters, "width", 1.0f));
    mixSmoothed.setTargetValue(raw(parameters, "mix", 25.0f) / 100.0f);
    earlySmoothed.setTargetValue(raw(parameters, "early_level", -8.0f));
    tailSmoothed.setTargetValue(raw(parameters, "tail_level", 0.0f));
    diffusionSmoothed.setTargetValue(raw(parameters, "diffusion", 0.72f));
    lowCutSmoothed.setTargetValue(raw(parameters, "low_cut", 120.0f));
    highCutSmoothed.setTargetValue(raw(parameters, "high_cut", 12000.0f));
    modDepthSmoothed.setTargetValue(raw(parameters, "mod_depth", 0.28f));
    modRateSmoothed.setTargetValue(raw(parameters, "mod_rate", 0.22f));
    duckingSmoothed.setTargetValue(raw(parameters, "ducking", 0.0f));
    bypassSmoothed.setTargetValue(bypass ? 0.0f : 1.0f);

    wetBuffer.setSize(2, numSamples, false, false, true);
    wetBuffer.clear();

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
        const float mixValue = mixSmoothed.skip(chunkSize);
        const float earlyValue = earlySmoothed.skip(chunkSize);
        const float tailValue = tailSmoothed.skip(chunkSize);
        const float diffusionValue = diffusionSmoothed.skip(chunkSize);
        const float lowCutValue = lowCutSmoothed.skip(chunkSize);
        const float highCutValue = highCutSmoothed.skip(chunkSize);
        const float modDepthValue = modDepthSmoothed.skip(chunkSize);
        const float modRateValue = modRateSmoothed.skip(chunkSize);
        const float duckingValue = duckingSmoothed.skip(chunkSize);
        const float activeValue = bypassSmoothed.skip(chunkSize);

        const float trimDb = juce::jlimit(0.0f, 9.0f,
            juce::jmax(0.0f, sizeValue - 0.72f) * 6.0f
            + juce::jmax(0.0f, decayValue - 0.68f) * 7.5f
            + juce::jmax(0.0f, mixValue - 0.32f) * 5.0f
            + juce::jmax(0.0f, tailValue - 1.0f) * 2.0f
            + (frozen ? 3.0f : 0.0f));
        const float wetInputGain = dbToGain(-trimDb);
        currentWetTrimDb.store(trimDb, std::memory_order_relaxed);

        juce::AudioBuffer<float> dryChunk(b.getArrayOfWritePointers(), b.getNumChannels(), offset, chunkSize);
        juce::AudioBuffer<float> wetChunk(wetBuffer.getArrayOfWritePointers(), 2, offset, chunkSize);

        AdvancedReverbEngine::Parameters rp;
        rp.algorithm = (int) raw(parameters, "algorithm", (float) AdvancedReverbEngine::hall);
        rp.quality = (int) raw(parameters, "quality", (float) AdvancedReverbEngine::studio);
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
        reverb.process(dryChunk, wetChunk, rp);

        float dryRms = 0.0f;
        for (int ch = 0; ch < juce::jmin(2, b.getNumChannels()); ++ch)
            dryRms += b.getRMSLevel(ch, offset, chunkSize);
        dryRms *= 0.5f;
        const float duckTarget = juce::jlimit(0.25f, 1.0f, 1.0f - duckingValue * juce::jlimit(0.0f, 0.75f, dryRms * 1.8f));
        const float releaseMs = raw(parameters, "ducking_release", 260.0f);
        const float releaseCoeff = std::exp(-1000.0f / (juce::jmax(1.0f, releaseMs) * (float) preparedSampleRate));
        if (duckTarget < duckGain)
            duckGain = duckTarget;
        else
            duckGain = duckTarget + (duckGain - duckTarget) * releaseCoeff;

        for (int i = 0; i < chunkSize; ++i)
        {
            const int sampleIndex = offset + i;
            const float wetMix = mixValue * activeValue * duckGain;
            for (int ch = 0; ch < 2; ++ch)
            {
                const float dry = b.getSample(ch, sampleIndex);
                const float wet = wetBuffer.getSample(ch, sampleIndex) * wetInputGain;
                const float out = dry * (1.0f - wetMix) + wet * wetMix;
                b.setSample(ch, sampleIndex, std::isfinite(out) ? juce::jlimit(-1.8f, 1.8f, out) : 0.0f);
            }
        }

        offset += chunkSize;
    }

    if (numSamples == 0)
        currentWetTrimDb.store(0.0f, std::memory_order_relaxed);

    b.applyGain(dbToGain(output));
    visualState.captureOutput(b);
}

void MusiqueReverbProcessor::getStateInformation(juce::MemoryBlock& d)
{ auto s=parameters.copyState(); std::unique_ptr<juce::XmlElement> x(s.createXml()); copyXmlToBinary(*x,d); }
void MusiqueReverbProcessor::setStateInformation(const void* data, int size)
{ std::unique_ptr<juce::XmlElement> x(getXmlFromBinary(data,size)); if (x && x->hasTagName(parameters.state.getType())) parameters.replaceState(juce::ValueTree::fromXml(*x)); }

juce::AudioProcessorEditor* MusiqueReverbProcessor::createEditor(){ return new MusiqueReverbEditor(*this);} 
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter(){ return new MusiqueReverbProcessor(); }
