#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "FXTokens.h"
#include "FXLookAndFeel.h"
#include "FXComponents.h"

class MusiqueReverbEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit MusiqueReverbEditor(MusiqueReverbProcessor&);
    ~MusiqueReverbEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    using APVTS = juce::AudioProcessorValueTreeState;
    using SliderAttach = APVTS::SliderAttachment;
    using ButtonAttach = APVTS::ButtonAttachment;
    using ComboAttach = APVTS::ComboBoxAttachment;

    void timerCallback() override;
    void paintVisualization(juce::Graphics&, juce::Rectangle<int> area);
    void setupSlider(juce::Slider&, juce::Label&, const juce::String& label);
    void refreshPresetBox();
    void storeCurrentABSlot();
    void recallABSlot(bool slotA);

    MusiqueReverbProcessor& proc;
    fx::FXLookAndFeel lnf { fx::accent::reverb };

    juce::Label titleLabel;
    juce::Image pluginIcon, logoImg;
    juce::TextButton bypassBtn{"Bypass"}, freezeBtn{"Freeze"}, monoBtn{"STEREO IN"}, trimBtn{"WET SAFE"};

    juce::TextButton prevBtn{"<"}, nextBtn{">"}, saveBtn{"Save"}, abBtn{"A/B"};
    juce::ComboBox presetBox;
    juce::ComboBox algorithmBox, qualityBox;

    static constexpr int numKnobs = 13;
    juce::Slider knobs[numKnobs];
    juce::Label knobLabels[numKnobs];
    juce::Label groupLabels[5];

    // Footer
    fx::MeterComponent inMeter, outMeter;
    juce::Slider outputSlider;
    fx::LEDComponent freezeLED;
    juce::Label versionLabel;

    float animPhase = 0.0f;
    std::array<float, 64> particles{};
    juce::ValueTree abStateA, abStateB;
    bool showingA = true;

    std::unique_ptr<SliderAttach> sizeAtt, decayAtt, preAtt, dampAtt, widthAtt, mixAtt, outAtt;
    std::unique_ptr<SliderAttach> earlyAtt, tailAtt, diffusionAtt, lowCutAtt, highCutAtt;
    std::unique_ptr<SliderAttach> modDepthAtt, modRateAtt, duckingAtt;
    std::unique_ptr<ButtonAttach> bypassAtt, freezeAtt, monoAtt;
    std::unique_ptr<ComboAttach> algorithmAtt, qualityAtt;
    std::shared_ptr<juce::Array<juce::var>> presets;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MusiqueReverbEditor)
};
