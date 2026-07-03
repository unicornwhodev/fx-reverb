#include "PluginProcessor.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <string>

namespace
{
struct Runner
{
    int checks = 0;
    int failures = 0;

    void expect(bool condition, const std::string& name)
    {
        ++checks;
        std::cout << (condition ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!condition)
            ++failures;
    }
};

void setParameter(MusiqueReverbProcessor& processor, const juce::String& id, float value)
{
    auto* parameter = processor.getAPVTS().getParameter(id);
    if (parameter == nullptr)
    {
        std::cerr << "Missing parameter: " << id << '\n';
        std::exit(2);
    }
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

float getParameter(MusiqueReverbProcessor& processor, const juce::String& id)
{
    if (auto* value = processor.getAPVTS().getRawParameterValue(id))
        return value->load();
    std::cerr << "Missing parameter: " << id << '\n';
    std::exit(2);
}

std::unique_ptr<MusiqueReverbProcessor> makeProcessor(int channels = 2, int blockSize = 512)
{
    auto processor = std::make_unique<MusiqueReverbProcessor>();
    processor->setPlayConfigDetails(channels, channels, 48000.0, blockSize);
    processor->prepareToPlay(48000.0, blockSize);
    return processor;
}

juce::AudioBuffer<float> makeSignal(int channels, int samples, float frequency = 220.0f)
{
    juce::AudioBuffer<float> buffer(channels, samples);
    for (int sample = 0; sample < samples; ++sample)
    {
        const float t = (float) sample / 48000.0f;
        const float left = 0.18f * std::sin(2.0f * juce::MathConstants<float>::pi * frequency * t)
            + 0.04f * std::sin(2.0f * juce::MathConstants<float>::pi * frequency * 1.91f * t);
        buffer.setSample(0, sample, left);
        if (channels > 1)
            buffer.setSample(1, sample, 0.15f * std::sin(2.0f * juce::MathConstants<float>::pi * frequency * 1.37f * t + 0.6f));
    }
    return buffer;
}

juce::AudioBuffer<float> makeImpulse(int channels, int samples)
{
    juce::AudioBuffer<float> buffer(channels, samples);
    buffer.clear();
    buffer.setSample(0, 0, 0.8f);
    if (channels > 1)
        buffer.setSample(1, 0, 0.6f);
    return buffer;
}

void process(MusiqueReverbProcessor& processor, juce::AudioBuffer<float>& buffer)
{
    juce::MidiBuffer midi;
    processor.processBlock(buffer, midi);
}

bool isFinite(const juce::AudioBuffer<float>& buffer)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            if (!std::isfinite(buffer.getSample(channel, sample)))
                return false;
    return true;
}

float maxAbs(const juce::AudioBuffer<float>& buffer)
{
    float peak = 0.0f;
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        peak = juce::jmax(peak, buffer.getMagnitude(channel, 0, buffer.getNumSamples()));
    return peak;
}

float diffEnergy(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    float sum = 0.0f;
    const int channels = juce::jmin(a.getNumChannels(), b.getNumChannels());
    const int samples = juce::jmin(a.getNumSamples(), b.getNumSamples());
    for (int channel = 0; channel < channels; ++channel)
        for (int sample = 0; sample < samples; ++sample)
            sum += std::abs(a.getSample(channel, sample) - b.getSample(channel, sample));
    return sum / (float) juce::jmax(1, channels * samples);
}

float channelDiffEnergy(const juce::AudioBuffer<float>& buffer)
{
    if (buffer.getNumChannels() < 2)
        return 0.0f;

    float sum = 0.0f;
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        sum += std::abs(buffer.getSample(0, sample) - buffer.getSample(1, sample));
    return sum / (float) juce::jmax(1, buffer.getNumSamples());
}

juce::ValueTree copyState(MusiqueReverbProcessor& processor)
{
    juce::MemoryBlock data;
    processor.getStateInformation(data);
    auto xml = juce::AudioProcessor::getXmlFromBinary(data.getData(), (int) data.getSize());
    if (xml == nullptr)
        std::exit(2);
    return juce::ValueTree::fromXml(*xml);
}

void loadState(MusiqueReverbProcessor& processor, const juce::ValueTree& state)
{
    auto xml = state.createXml();
    juce::MemoryBlock data;
    juce::AudioProcessor::copyXmlToBinary(*xml, data);
    processor.setStateInformation(data.getData(), (int) data.getSize());
}

void removeParam(juce::ValueTree& state, const juce::String& id)
{
    for (int index = state.getNumChildren() - 1; index >= 0; --index)
    {
        auto child = state.getChild(index);
        if (child.hasType("PARAM") && child.getProperty("id").toString() == id)
            state.removeChild(index, nullptr);
    }
}

juce::File findFactoryBank()
{
    auto dir = juce::File::getCurrentWorkingDirectory();
    for (int depth = 0; depth < 8; ++depth)
    {
        const std::array<juce::File, 3> candidates {
            dir.getChildFile("Presets").getChildFile("factory_bank.json"),
            dir.getChildFile("fx-reverb").getChildFile("Presets").getChildFile("factory_bank.json"),
            dir.getChildFile("FX").getChildFile("fx-reverb").getChildFile("Presets").getChildFile("factory_bank.json")
        };
        for (const auto& candidate : candidates)
            if (candidate.existsAsFile())
                return candidate;

        const auto parent = dir.getParentDirectory();
        if (parent == dir)
            break;
        dir = parent;
    }
    return {};
}

juce::Array<juce::var> loadFactoryPresets(juce::var* rootOut = nullptr)
{
    const auto file = findFactoryBank();
    if (!file.existsAsFile())
    {
        std::cerr << "factory_bank.json not found\n";
        std::exit(2);
    }

    auto json = juce::JSON::parse(file.loadFileAsString());
    if (rootOut != nullptr)
        *rootOut = json;
    if (auto* object = json.getDynamicObject())
        if (auto* presets = object->getProperty("presets").getArray())
            return *presets;
    return {};
}

void testBypassDryStrict(Runner& runner)
{
    auto processor = makeProcessor();
    auto buffer = makeSignal(2, 512);
    const auto dry = buffer;
    setParameter(*processor, "bypass", 1.0f);
    setParameter(*processor, "mono", 1.0f);
    setParameter(*processor, "output", -12.0f);
    setParameter(*processor, "mix", 100.0f);
    setParameter(*processor, "freeze", 1.0f);
    process(*processor, buffer);
    runner.expect(diffEnergy(buffer, dry) < 1.0e-7f, "bypass is dry-identical without mono/output/mix/freeze");
}

void testLayouts(Runner& runner)
{
    juce::AudioProcessor::BusesLayout monoLayout;
    monoLayout.inputBuses.add(juce::AudioChannelSet::mono());
    monoLayout.outputBuses.add(juce::AudioChannelSet::mono());
    juce::AudioProcessor::BusesLayout stereoLayout;
    stereoLayout.inputBuses.add(juce::AudioChannelSet::stereo());
    stereoLayout.outputBuses.add(juce::AudioChannelSet::stereo());

    auto processor = makeProcessor();
    runner.expect(processor->isBusesLayoutSupported(monoLayout), "mono->mono layout is supported");
    runner.expect(processor->isBusesLayoutSupported(stereoLayout), "stereo->stereo layout is supported");

    auto mono = makeProcessor(1);
    auto monoBuffer = makeSignal(1, 512);
    process(*mono, monoBuffer);
    runner.expect(isFinite(monoBuffer), "mono processing remains finite");

    auto stereo = makeProcessor(2);
    auto stereoBuffer = makeSignal(2, 512);
    process(*stereo, stereoBuffer);
    runner.expect(isFinite(stereoBuffer), "stereo processing remains finite");
}

void testStateRoundTrip(Runner& runner)
{
    auto processor = makeProcessor();
    setParameter(*processor, "algorithm", 4.0f);
    setParameter(*processor, "quality", 2.0f);
    setParameter(*processor, "size", 0.83f);
    setParameter(*processor, "decay", 0.77f);
    setParameter(*processor, "predelay", 63.0f);
    setParameter(*processor, "damping", 0.61f);
    setParameter(*processor, "width", 1.08f);
    setParameter(*processor, "mix", 67.0f);
    setParameter(*processor, "early_level", -11.0f);
    setParameter(*processor, "tail_level", 1.2f);
    setParameter(*processor, "diffusion", 0.91f);
    setParameter(*processor, "low_cut", 220.0f);
    setParameter(*processor, "high_cut", 9600.0f);
    setParameter(*processor, "mod_depth", 0.47f);
    setParameter(*processor, "mod_rate", 0.31f);
    setParameter(*processor, "ducking", 0.35f);
    setParameter(*processor, "ducking_release", 420.0f);
    setParameter(*processor, "output", -3.0f);
    setParameter(*processor, "freeze", 1.0f);
    setParameter(*processor, "mono", 1.0f);

    const auto state = copyState(*processor);
    auto restored = makeProcessor();
    loadState(*restored, state);

    runner.expect(std::abs(getParameter(*restored, "algorithm") - 4.0f) < 0.001f, "state restores algorithm");
    runner.expect(std::abs(getParameter(*restored, "quality") - 2.0f) < 0.001f, "state restores quality");
    runner.expect(std::abs(getParameter(*restored, "size") - 0.83f) < 0.001f, "state restores size");
    runner.expect(std::abs(getParameter(*restored, "decay") - 0.77f) < 0.001f, "state restores decay");
    runner.expect(std::abs(getParameter(*restored, "predelay") - 63.0f) < 0.01f, "state restores predelay");
    runner.expect(std::abs(getParameter(*restored, "mix") - 67.0f) < 0.001f, "state restores mix");
    runner.expect(std::abs(getParameter(*restored, "tail_level") - 1.2f) < 0.001f, "state restores tail level");
    runner.expect(std::abs(getParameter(*restored, "high_cut") - 9600.0f) < 0.01f, "state restores high cut");
    runner.expect(std::abs(getParameter(*restored, "output") + 3.0f) < 0.001f, "state restores output");
    runner.expect(getParameter(*restored, "freeze") > 0.5f, "state restores freeze");
    runner.expect(getParameter(*restored, "mono") > 0.5f, "state restores mono");
}

void testLegacyStateMigration(Runner& runner)
{
    auto processor = makeProcessor();
    auto state = copyState(*processor);
    removeParam(state, "algorithm");
    removeParam(state, "quality");
    removeParam(state, "ducking_release");
    removeParam(state, "bypass");
    removeParam(state, "freeze");
    removeParam(state, "mono");

    auto restored = makeProcessor();
    loadState(*restored, state);
    runner.expect((int) std::round(getParameter(*restored, "algorithm")) == 2, "legacy state injects hall algorithm");
    runner.expect((int) std::round(getParameter(*restored, "quality")) == 1, "legacy state injects studio quality");
    runner.expect(std::abs(getParameter(*restored, "ducking_release") - 260.0f) < 0.01f, "legacy state injects ducking release");
    runner.expect(getParameter(*restored, "bypass") < 0.5f, "legacy state injects bypass false");
    runner.expect(getParameter(*restored, "freeze") < 0.5f, "legacy state injects freeze false");
    runner.expect(getParameter(*restored, "mono") < 0.5f, "legacy state injects mono false");
}

void testLegacyPresetCompat(Runner& runner)
{
    auto processor = makeProcessor();
    juce::DynamicObject::Ptr object = new juce::DynamicObject();
    object->setProperty("name", "LegacyReverb");
    object->setProperty("algorithm", 99.0);
    object->setProperty("quality", -4.0);
    object->setProperty("mix", 140.0);
    object->setProperty("high_cut", 50000.0);
    object->setProperty("predelay", -10.0);
    juce::var preset(object.get());
    processor->applyPresetCompat(preset);

    runner.expect((int) std::round(getParameter(*processor, "algorithm")) == 4, "legacy preset clamps algorithm");
    runner.expect((int) std::round(getParameter(*processor, "quality")) == 0, "legacy preset clamps quality");
    runner.expect(std::abs(getParameter(*processor, "mix") - 100.0f) < 0.001f, "legacy preset clamps mix");
    runner.expect(std::abs(getParameter(*processor, "high_cut") - 20000.0f) < 0.01f, "legacy preset clamps high cut");
    runner.expect(std::abs(getParameter(*processor, "predelay")) < 0.001f, "legacy preset clamps predelay");
    runner.expect(getParameter(*processor, "bypass") < 0.5f, "legacy preset injects bypass false");
    runner.expect(getParameter(*processor, "freeze") < 0.5f, "legacy preset injects freeze false");
    runner.expect(getParameter(*processor, "mono") < 0.5f, "legacy preset injects mono false");
}

void testFactoryPresets(Runner& runner)
{
    juce::var root;
    auto presets = loadFactoryPresets(&root);
    runner.expect(presets.size() == 18, "factory bank contains 18 presets");

    bool bankVersionOk = false;
    if (auto* object = root.getDynamicObject())
        bankVersionOk = object->hasProperty("bank_version") && (int) object->getProperty("bank_version") >= 2;
    runner.expect(bankVersionOk, "factory bank declares bank_version");

    std::set<int> algorithms;
    bool allCategorised = true;
    bool allFinite = true;
    auto processor = makeProcessor();
    for (auto& preset : presets)
    {
        MusiqueReverbProcessor::normalisePresetObject(preset);
        if (auto* object = preset.getDynamicObject())
        {
            algorithms.insert((int) object->getProperty("algorithm"));
            allCategorised = allCategorised && object->hasProperty("category") && object->getProperty("category").toString().isNotEmpty();
        }

        processor->applyPresetCompat(preset);
        auto buffer = makeSignal(2, 512);
        process(*processor, buffer);
        allFinite = allFinite && isFinite(buffer);
    }

    runner.expect(algorithms.size() == 5, "factory bank covers all algorithms");
    runner.expect(allCategorised, "factory bank presets have categories");
    runner.expect(allFinite, "all factory presets process finite audio");
}

void testReverbAudibleTail(Runner& runner)
{
    auto processor = makeProcessor();
    setParameter(*processor, "mix", 100.0f);
    setParameter(*processor, "predelay", 0.0f);
    setParameter(*processor, "early_level", 0.0f);
    setParameter(*processor, "tail_level", 0.0f);
    setParameter(*processor, "decay", 0.78f);
    processor->prepareToPlay(48000.0, 512);

    bool audible = false;
    for (int block = 0; block < 80; ++block)
    {
        auto buffer = block == 0 ? makeImpulse(2, 512) : makeSignal(2, 512, 180.0f + (float) block);
        const auto dry = buffer;
        process(*processor, buffer);
        audible = audible || diffEnergy(buffer, dry) > 1.0e-4f;
    }

    runner.expect(audible, "wet reverb produces measurable output difference");
}

void testFreezeFinite(Runner& runner)
{
    auto processor = makeProcessor();
    setParameter(*processor, "mix", 100.0f);
    setParameter(*processor, "decay", 0.86f);
    setParameter(*processor, "size", 0.92f);
    processor->prepareToPlay(48000.0, 512);

    for (int block = 0; block < 16; ++block)
    {
        auto buffer = makeSignal(2, 512, 160.0f + (float) block * 5.0f);
        process(*processor, buffer);
    }

    setParameter(*processor, "freeze", 1.0f);
    bool allFinite = true;
    float largestPeak = 0.0f;
    for (int block = 0; block < 80; ++block)
    {
        juce::AudioBuffer<float> buffer(2, 512);
        buffer.clear();
        process(*processor, buffer);
        allFinite = allFinite && isFinite(buffer);
        largestPeak = juce::jmax(largestPeak, maxAbs(buffer));
    }

    runner.expect(allFinite, "freeze remains finite across long tail");
    runner.expect(largestPeak < 12.0f, "freeze remains bounded");
}

void testMonoProcessing(Runner& runner)
{
    auto stereo = makeProcessor();
    setParameter(*stereo, "mono", 1.0f);
    setParameter(*stereo, "width", 0.0f);
    setParameter(*stereo, "mix", 100.0f);
    stereo->prepareToPlay(48000.0, 512);
    auto stereoBuffer = makeSignal(2, 512);
    process(*stereo, stereoBuffer);
    runner.expect(isFinite(stereoBuffer), "stereo mono mode remains finite");
    runner.expect(channelDiffEnergy(stereoBuffer) < 1.0e-4f, "mono mode produces coherent stereo wet output");

    auto mono = makeProcessor(1);
    setParameter(*mono, "mono", 1.0f);
    auto monoBuffer = makeSignal(1, 512);
    process(*mono, monoBuffer);
    runner.expect(isFinite(monoBuffer), "host mono buffer remains finite");
}

void testMixZeroDry(Runner& runner)
{
    auto processor = makeProcessor();
    setParameter(*processor, "mix", 0.0f);
    setParameter(*processor, "output", -12.0f);
    setParameter(*processor, "freeze", 1.0f);
    auto buffer = makeSignal(2, 512);
    const auto dry = buffer;
    process(*processor, buffer);
    runner.expect(diffEnergy(buffer, dry) < 1.0e-7f, "mix=0 is dry-identical without output trim");
}

void testRapidAutomationFinite(Runner& runner)
{
    auto processor = makeProcessor();
    bool allFinite = true;
    float largestPeak = 0.0f;

    for (int block = 0; block < 120; ++block)
    {
        setParameter(*processor, "algorithm", (float) (block % 5));
        setParameter(*processor, "quality", (float) (block % 3));
        setParameter(*processor, "size", (float) ((block * 7) % 100) / 100.0f);
        setParameter(*processor, "decay", (float) ((block * 11) % 100) / 100.0f);
        setParameter(*processor, "predelay", (float) ((block * 13) % 250));
        setParameter(*processor, "damping", (float) ((block * 17) % 100) / 100.0f);
        setParameter(*processor, "width", (float) ((block * 19) % 120) / 100.0f);
        setParameter(*processor, "early_level", -36.0f + (float) ((block * 23) % 42));
        setParameter(*processor, "tail_level", -36.0f + (float) ((block * 29) % 42));
        setParameter(*processor, "low_cut", 20.0f + (float) ((block * 31) % 1180));
        setParameter(*processor, "high_cut", 2000.0f + (float) ((block * 271) % 18000));
        setParameter(*processor, "mod_depth", (float) ((block * 37) % 100) / 100.0f);
        setParameter(*processor, "mod_rate", 0.02f + (float) ((block * 41) % 190) / 100.0f);
        setParameter(*processor, "ducking", (float) ((block * 43) % 100) / 100.0f);
        setParameter(*processor, "freeze", (float) (block % 2));
        setParameter(*processor, "mix", 5.0f + (float) ((block * 47) % 95));
        setParameter(*processor, "output", -12.0f + (float) (block % 24));

        auto buffer = makeSignal(2, 512, 130.0f + (float) block);
        process(*processor, buffer);
        allFinite = allFinite && isFinite(buffer);
        largestPeak = juce::jmax(largestPeak, maxAbs(buffer));
    }

    runner.expect(allFinite, "rapid automation remains finite");
    runner.expect(largestPeak < 24.0f, "rapid automation remains bounded");
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    Runner runner;
    testBypassDryStrict(runner);
    testLayouts(runner);
    testStateRoundTrip(runner);
    testLegacyStateMigration(runner);
    testLegacyPresetCompat(runner);
    testFactoryPresets(runner);
    testReverbAudibleTail(runner);
    testFreezeFinite(runner);
    testMonoProcessing(runner);
    testMixZeroDry(runner);
    testRapidAutomationFinite(runner);

    std::cout << "Checks: " << runner.checks << ", Failures: " << runner.failures << '\n';
    return runner.failures == 0 ? 0 : 1;
}
