#pragma once

#include <JuceHeader.h>
#include "FXAudioVisualState.h"
#include "AdvancedReverbEngine.h"

class MusiqueReverbProcessor : public juce::AudioProcessor
{
public:
    MusiqueReverbProcessor();
    ~MusiqueReverbProcessor() override = default;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    static juce::StringArray getAllParameterIds();
    static void normalisePresetObject(juce::var& preset);

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorParameter* getBypassParameter() const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override
    {
       #if JucePlugin_Build_Standalone || JucePlugin_Build_VST3
        return JucePlugin_Name;
       #else
        return "Musique Reverb";
       #endif
    }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 8.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState& getAPVTS() noexcept { return parameters; }
    const fx::AudioVisualState& getVisualState() const noexcept { return visualState; }
    float getCurrentWetTrimDb() const noexcept { return currentWetTrimDb.load(std::memory_order_relaxed); }
    void applyPresetCompat(const juce::var& preset);

private:
    void normaliseStateTree(juce::ValueTree& state);

    juce::AudioProcessorValueTreeState parameters;
    fx::AudioVisualState visualState;
    AdvancedReverbEngine reverb;
    juce::AudioBuffer<float> reverbInputBuffer;
    juce::AudioBuffer<float> wetBuffer;
    juce::SmoothedValue<float> sizeSmoothed;
    juce::SmoothedValue<float> decaySmoothed;
    juce::SmoothedValue<float> predelaySmoothed;
    juce::SmoothedValue<float> dampingSmoothed;
    juce::SmoothedValue<float> widthSmoothed;
    juce::SmoothedValue<float> mixSmoothed;
    juce::SmoothedValue<float> earlySmoothed;
    juce::SmoothedValue<float> tailSmoothed;
    juce::SmoothedValue<float> diffusionSmoothed;
    juce::SmoothedValue<float> lowCutSmoothed;
    juce::SmoothedValue<float> highCutSmoothed;
    juce::SmoothedValue<float> modDepthSmoothed;
    juce::SmoothedValue<float> modRateSmoothed;
    juce::SmoothedValue<float> duckingSmoothed;
    std::atomic<float> currentWetTrimDb { 0.0f };
    float duckGain = 1.0f;
    double preparedSampleRate = 44100.0;
    int preparedBlockCapacity = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MusiqueReverbProcessor)
};
