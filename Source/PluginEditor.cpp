#include "PluginEditor.h"
#include "BinaryData.h"

namespace
{
float paramValue(juce::AudioProcessorValueTreeState& apvts, const char* id, float fallback = 0.0f)
{
    if (auto* p = apvts.getRawParameterValue(id))
        return p->load();
    return fallback;
}

juce::String algorithmName(int index)
{
    static const juce::StringArray names { "ROOM", "PLATE", "HALL", "CHAMBER", "SPACE" };
    return names[juce::jlimit(0, names.size() - 1, index)];
}

juce::String qualityName(int index)
{
    static const juce::StringArray names { "ECO", "STUDIO", "HIGH" };
    return names[juce::jlimit(0, names.size() - 1, index)];
}
}

MusiqueReverbEditor::MusiqueReverbEditor(MusiqueReverbProcessor& p)
    : AudioProcessorEditor(&p), proc(p)
{
    setLookAndFeel(&lnf);
    setSize(fx::dim::appW, fx::dim::appH);

    juce::Random rng;
    for (auto& v : particles)
        v = rng.nextFloat();

    titleLabel.setText("REVERB V2", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions{}.withHeight(fx::font::header).withStyle("Bold")));
    titleLabel.setJustificationType(juce::Justification::centredLeft);
    titleLabel.setColour(juce::Label::textColourId, fx::col::textPrimary);
    addAndMakeVisible(titleLabel);

    pluginIcon = juce::ImageCache::getFromMemory(BinaryData::icon_small_png, BinaryData::icon_small_pngSize);
    logoImg = juce::ImageCache::getFromMemory(BinaryData::logo_png, BinaryData::logo_pngSize);

    auto setupBtn = [&](juce::TextButton& b, bool toggle = false) {
        b.setColour(juce::TextButton::buttonColourId, fx::col::surfSecondary);
        b.setColour(juce::TextButton::textColourOffId, fx::col::textPrimary);
        if (toggle)
            b.setClickingTogglesState(true);
        addAndMakeVisible(b);
    };
    setupBtn(bypassBtn, true);
    setupBtn(freezeBtn, true);
    setupBtn(monoBtn, true);
    setupBtn(trimBtn);
    trimBtn.setTooltip("Internal wet trim and ducking status");
    trimBtn.onClick = [] {};

    setupBtn(prevBtn);
    setupBtn(nextBtn);
    setupBtn(saveBtn);
    setupBtn(abBtn);
    addAndMakeVisible(presetBox);

    algorithmBox.addItemList(juce::StringArray { "Room", "Plate", "Hall", "Chamber", "Space" }, 1);
    qualityBox.addItemList(juce::StringArray { "Eco", "Studio", "High" }, 1);
    addAndMakeVisible(algorithmBox);
    addAndMakeVisible(qualityBox);

    presets = std::make_shared<juce::Array<juce::var>>(fx::preset::loadAllPresets("fx-reverb"));
    refreshPresetBox();
    if (!presets->isEmpty())
        fx::preset::applyToAPVTS(proc.getAPVTS(), presets->getReference(0));

    presetBox.onChange = [this] {
        int i = presetBox.getSelectedItemIndex();
        if (i >= 0 && i < presets->size())
        {
            storeCurrentABSlot();
            fx::preset::applyToAPVTS(proc.getAPVTS(), presets->getReference(i));
            abStateA = proc.getAPVTS().copyState();
            abStateB = abStateA.createCopy();
            showingA = true;
            abBtn.setButtonText("A/B");
        }
    };
    prevBtn.onClick = [this] { int i = presetBox.getSelectedItemIndex(); if (i > 0) presetBox.setSelectedItemIndex(i - 1); };
    nextBtn.onClick = [this] { int i = presetBox.getSelectedItemIndex(); if (i < presetBox.getNumItems() - 1) presetBox.setSelectedItemIndex(i + 1); };
    saveBtn.onClick = [this] {
        auto name = juce::String("User_") + juce::Time::getCurrentTime().formatted("%H%M%S");
        juce::StringArray ids {
            "algorithm","size","decay","predelay","damping","width","mix",
            "early_level","tail_level","diffusion","low_cut","high_cut",
            "mod_depth","mod_rate","ducking","ducking_release","quality",
            "output","bypass","freeze","mono"
        };
        if (fx::preset::saveUserPreset("fx-reverb", name, ids, proc.getAPVTS()))
        {
            *presets = fx::preset::loadAllPresets("fx-reverb");
            refreshPresetBox();
            presetBox.setSelectedItemIndex(presetBox.getNumItems() - 1);
        }
    };
    abBtn.onClick = [this] {
        storeCurrentABSlot();
        recallABSlot(!showingA);
    };

    const char* labels[numKnobs] = {
        "SIZE", "DIFFUSE", "PRE", "DECAY", "EARLY", "TAIL",
        "LOW CUT", "HIGH CUT", "DAMP", "MOD DEPTH", "MOD RATE", "MIX", "DUCK"
    };
    for (int i = 0; i < numKnobs; ++i)
        setupSlider(knobs[i], knobLabels[i], labels[i]);

    const char* groups[5] = { "SPACE", "TIME", "TONE", "MOTION", "MIX" };
    for (int i = 0; i < 5; ++i)
    {
        groupLabels[i].setText(groups[i], juce::dontSendNotification);
        groupLabels[i].setFont(juce::Font(juce::FontOptions{}.withHeight(10.0f).withStyle("Bold")));
        groupLabels[i].setColour(juce::Label::textColourId, fx::col::textMuted);
        groupLabels[i].setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(groupLabels[i]);
    }

    addAndMakeVisible(inMeter);
    addAndMakeVisible(outMeter);
    outputSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    outputSlider.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    addAndMakeVisible(outputSlider);

    freezeLED.setAccent(fx::accent::reverb);
    addAndMakeVisible(freezeLED);
    versionLabel.setText("Musique Reverb v2.0", juce::dontSendNotification);
    versionLabel.setFont(juce::Font(juce::FontOptions{}.withHeight(fx::font::footer)));
    versionLabel.setColour(juce::Label::textColourId, fx::col::textMuted);
    versionLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(versionLabel);

    algorithmAtt = std::make_unique<ComboAttach>(proc.getAPVTS(), "algorithm", algorithmBox);
    qualityAtt   = std::make_unique<ComboAttach>(proc.getAPVTS(), "quality",   qualityBox);
    sizeAtt      = std::make_unique<SliderAttach>(proc.getAPVTS(), "size",        knobs[0]);
    diffusionAtt = std::make_unique<SliderAttach>(proc.getAPVTS(), "diffusion",   knobs[1]);
    preAtt       = std::make_unique<SliderAttach>(proc.getAPVTS(), "predelay",    knobs[2]);
    decayAtt     = std::make_unique<SliderAttach>(proc.getAPVTS(), "decay",       knobs[3]);
    earlyAtt     = std::make_unique<SliderAttach>(proc.getAPVTS(), "early_level", knobs[4]);
    tailAtt      = std::make_unique<SliderAttach>(proc.getAPVTS(), "tail_level",  knobs[5]);
    lowCutAtt    = std::make_unique<SliderAttach>(proc.getAPVTS(), "low_cut",     knobs[6]);
    highCutAtt   = std::make_unique<SliderAttach>(proc.getAPVTS(), "high_cut",    knobs[7]);
    dampAtt      = std::make_unique<SliderAttach>(proc.getAPVTS(), "damping",     knobs[8]);
    modDepthAtt  = std::make_unique<SliderAttach>(proc.getAPVTS(), "mod_depth",   knobs[9]);
    modRateAtt   = std::make_unique<SliderAttach>(proc.getAPVTS(), "mod_rate",    knobs[10]);
    mixAtt       = std::make_unique<SliderAttach>(proc.getAPVTS(), "mix",         knobs[11]);
    duckingAtt   = std::make_unique<SliderAttach>(proc.getAPVTS(), "ducking",     knobs[12]);
    outAtt       = std::make_unique<SliderAttach>(proc.getAPVTS(), "output",      outputSlider);
    bypassAtt    = std::make_unique<ButtonAttach>(proc.getAPVTS(), "bypass",      bypassBtn);
    freezeAtt    = std::make_unique<ButtonAttach>(proc.getAPVTS(), "freeze",      freezeBtn);
    monoAtt      = std::make_unique<ButtonAttach>(proc.getAPVTS(), "mono",        monoBtn);

    abStateA = proc.getAPVTS().copyState();
    abStateB = abStateA.createCopy();
    startTimerHz(fx::anim::fftRefreshHz);
}

MusiqueReverbEditor::~MusiqueReverbEditor() { setLookAndFeel(nullptr); }

void MusiqueReverbEditor::setupSlider(juce::Slider& slider, juce::Label& label, const juce::String& text)
{
    slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 68, 16);
    addAndMakeVisible(slider);

    label.setText(text, juce::dontSendNotification);
    label.setFont(juce::Font(juce::FontOptions{}.withHeight(fx::font::label).withStyle("Bold")));
    label.setJustificationType(juce::Justification::centred);
    label.setColour(juce::Label::textColourId, fx::col::textMuted);
    addAndMakeVisible(label);
}

void MusiqueReverbEditor::refreshPresetBox()
{
    presetBox.clear(juce::dontSendNotification);
    if (presets->isEmpty())
    {
        presetBox.addItem("Init", 1);
        presetBox.setSelectedId(1, juce::dontSendNotification);
        return;
    }

    int id = 1;
    for (auto& pv : *presets)
        if (auto* o = pv.getDynamicObject())
            presetBox.addItem(o->getProperty("name").toString(), id++);
    presetBox.setSelectedItemIndex(0, juce::dontSendNotification);
}

void MusiqueReverbEditor::storeCurrentABSlot()
{
    if (showingA)
        abStateA = proc.getAPVTS().copyState();
    else
        abStateB = proc.getAPVTS().copyState();
}

void MusiqueReverbEditor::recallABSlot(bool slotA)
{
    auto state = slotA ? abStateA : abStateB;
    if (state.isValid())
    {
        proc.getAPVTS().replaceState(state.createCopy());
        showingA = slotA;
        abBtn.setButtonText(showingA ? "A" : "B");
    }
}

void MusiqueReverbEditor::timerCallback()
{
    const auto inputLevels = proc.getVisualState().getInputLevels();
    const auto outputLevels = proc.getVisualState().getOutputLevels();
    inMeter.setLevel(inputLevels.left, inputLevels.right);
    outMeter.setLevel(outputLevels.left, outputLevels.right);

    animPhase += 0.03f;
    if (animPhase > juce::MathConstants<float>::twoPi * 10.0f)
        animPhase -= juce::MathConstants<float>::twoPi * 10.0f;

    const bool frozen = paramValue(proc.getAPVTS(), "freeze") > 0.5f;
    const bool mono = paramValue(proc.getAPVTS(), "mono") > 0.5f;
    const float wetTrimDb = proc.getCurrentWetTrimDb();
    const float ducking = paramValue(proc.getAPVTS(), "ducking");
    freezeLED.setOn(frozen);
    monoBtn.setButtonText(mono ? "MONO IN" : "STEREO IN");
    qualityBox.setTooltip("Render quality: " + qualityName((int) paramValue(proc.getAPVTS(), "quality", 1.0f)));
    algorithmBox.setTooltip("Algorithm: " + algorithmName((int) paramValue(proc.getAPVTS(), "algorithm", 2.0f)));

    if (wetTrimDb > 0.25f)
        trimBtn.setButtonText("WET -" + juce::String(wetTrimDb, 1) + "dB");
    else if (ducking > 0.01f)
        trimBtn.setButtonText("DUCK " + juce::String(ducking * 100.0f, 0) + "%");
    else
        trimBtn.setButtonText("WET SAFE");

    trimBtn.setColour(juce::TextButton::buttonColourId,
        wetTrimDb > 0.25f ? fx::accent::reverb.withAlpha(0.18f) : fx::col::surfSecondary);
    trimBtn.setColour(juce::TextButton::textColourOffId,
        wetTrimDb > 0.25f ? fx::accent::reverb.brighter(0.25f) : fx::col::textPrimary);

    repaint(0, fx::dim::headerH + fx::dim::presetBarH, getWidth(), fx::dim::visualH);
}

void MusiqueReverbEditor::paintVisualization(juce::Graphics& g, juce::Rectangle<int> area)
{
    const float size = paramValue(proc.getAPVTS(), "size", 0.6f);
    const float decay = paramValue(proc.getAPVTS(), "decay", 0.5f);
    const float width = paramValue(proc.getAPVTS(), "width", 1.0f);
    const float predelay = paramValue(proc.getAPVTS(), "predelay", 20.0f);
    const float damping = paramValue(proc.getAPVTS(), "damping", 0.4f);
    const float diffusion = paramValue(proc.getAPVTS(), "diffusion", 0.72f);
    const int algorithm = (int) paramValue(proc.getAPVTS(), "algorithm", 2.0f);
    const int quality = (int) paramValue(proc.getAPVTS(), "quality", 1.0f);
    const bool frozen = paramValue(proc.getAPVTS(), "freeze") > 0.5f;
    const auto outputLevels = proc.getVisualState().getOutputLevels();
    const float energy = juce::jlimit(0.03f, 1.0f, 0.5f * (outputLevels.left + outputLevels.right));

    const float w = (float) area.getWidth();
    const float h = (float) area.getHeight();
    const float ax = (float) area.getX();
    const float ay = (float) area.getY();
    const float cx = ax + w * 0.5f;
    const float cy = ay + h * 0.52f;

    auto drawBadge = [&](juce::Rectangle<float> rect, const juce::String& text, juce::Colour colour)
    {
        g.setColour(colour.withAlpha(0.16f));
        g.fillRoundedRectangle(rect, 7.0f);
        g.setColour(colour.withAlpha(0.62f));
        g.drawRoundedRectangle(rect, 7.0f, 1.0f);
        g.setColour(fx::col::textPrimary);
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(10.0f).withStyle("Bold")));
        g.drawText(text, rect.toNearestInt(), juce::Justification::centred);
    };

    drawBadge({ ax + 22.0f, ay + 16.0f, 88.0f, 22.0f }, algorithmName(algorithm), fx::accent::reverb);
    drawBadge({ ax + 118.0f, ay + 16.0f, 82.0f, 22.0f }, qualityName(quality), fx::col::textSecondary);
    drawBadge({ ax + 208.0f, ay + 16.0f, 86.0f, 22.0f }, frozen ? "FREEZE" : "LIVE TAIL", frozen ? fx::accent::reverb : fx::col::textSecondary);
    drawBadge({ ax + w - 260.0f, ay + 16.0f, 88.0f, 22.0f }, "EARLY " + juce::String(paramValue(proc.getAPVTS(), "early_level", -8.0f), 0) + "dB", fx::col::textSecondary);
    drawBadge({ ax + w - 164.0f, ay + 16.0f, 88.0f, 22.0f }, "TAIL " + juce::String(paramValue(proc.getAPVTS(), "tail_level", 0.0f), 0) + "dB", fx::accent::reverb);

    const float roomW = w * juce::jmap(size, 0.0f, 1.0f, 0.22f, 0.78f);
    const float roomH = h * juce::jmap(size, 0.0f, 1.0f, 0.18f, 0.58f);
    juce::Rectangle<float> chamber(cx - roomW * 0.5f, cy - roomH * 0.5f, roomW, roomH);
    g.setColour(fx::accent::reverb.withAlpha(0.06f + energy * 0.05f));
    g.fillEllipse(chamber);
    g.setColour(fx::accent::reverb.withAlpha(0.30f));
    g.drawEllipse(chamber, 1.2f);

    const int particleCount = (int) juce::jmap(diffusion, 0.0f, 1.0f, 14.0f, (float) particles.size());
    for (int i = 0; i < particleCount; ++i)
    {
        const float seed = particles[(size_t) i];
        const float angle = seed * juce::MathConstants<float>::twoPi + animPhase * (0.22f + seed * 0.28f);
        const float radius = (0.18f + particles[(size_t) ((i + 11) % particles.size())] * 0.82f);
        const float px = cx + std::cos(angle) * roomW * 0.48f * radius * (0.55f + width * 0.45f);
        const float py = cy + std::sin(angle) * roomH * 0.48f * radius;
        const float alpha = (0.10f + decay * 0.42f * seed) * energy;
        const float dot = 2.0f + seed * 3.5f + (frozen ? 1.5f : 0.0f);
        g.setColour(fx::accent::reverb.withAlpha(alpha));
        g.fillEllipse(px - dot * 0.5f, py - dot * 0.5f, dot, dot);
    }

    const float timelineX = ax + 28.0f;
    const float timelineY = ay + h - 36.0f;
    const float timelineW = w * 0.32f;
    g.setColour(fx::col::gridMinor);
    g.fillRoundedRectangle(timelineX, timelineY, timelineW, 6.0f, 3.0f);
    g.setColour(fx::accent::reverb.withAlpha(0.38f));
    g.fillRoundedRectangle(timelineX, timelineY, timelineW * juce::jlimit(0.0f, 1.0f, predelay / 250.0f), 6.0f, 3.0f);

    g.setColour(fx::accent::reverb.withAlpha(0.55f));
    for (int i = 0; i < 6; ++i)
    {
        const float x = timelineX + 20.0f + (float) i * 28.0f + predelay * 0.09f;
        const float y = timelineY - 38.0f + std::sin(animPhase + (float) i) * 8.0f;
        g.fillEllipse(x, y, 5.0f, 5.0f);
    }

    juce::Path decayCurve;
    const float envStartX = ax + w * 0.64f;
    const float envEndX = ax + w - 22.0f;
    const float envTop = ay + 58.0f;
    const float envBot = ay + h - 48.0f;
    decayCurve.startNewSubPath(envStartX, envTop);
    for (int i = 0; i <= 48; ++i)
    {
        const float t = (float) i / 48.0f;
        float env = std::exp(-t * juce::jmap(decay, 0.0f, 1.0f, 5.5f, 0.55f));
        env *= 1.0f - damping * t * 0.38f;
        if (frozen)
            env = juce::jmax(env, 0.72f - t * 0.12f);
        decayCurve.lineTo(envStartX + t * (envEndX - envStartX), envBot - env * (envBot - envTop));
    }
    g.setColour(fx::accent::reverb.withAlpha(0.15f + energy * 0.12f));
    g.strokePath(decayCurve, juce::PathStrokeType(6.0f));
    g.setColour(fx::accent::reverb.withAlpha(frozen ? 0.95f : 0.68f));
    g.strokePath(decayCurve, juce::PathStrokeType(1.8f));

    g.setColour(fx::col::textMuted);
    g.setFont(juce::Font(juce::FontOptions{}.withHeight(10.0f)));
    g.drawText("PRE " + juce::String(predelay, 0) + " ms", (int) timelineX, (int) (timelineY - 18.0f), (int) timelineW, 14, juce::Justification::left);
    g.drawText("WIDTH " + juce::String(width * 100.0f, 0) + "%", (int) (cx - 58.0f), (int) (cy + roomH * 0.5f + 12.0f), 116, 14, juce::Justification::centred);
    g.drawText("DECAY", (int) envStartX, (int) (envBot + 8.0f), (int) (envEndX - envStartX), 14, juce::Justification::centred);
}

void MusiqueReverbEditor::paint(juce::Graphics& g)
{
    g.fillAll(fx::col::bg);
    fx::paint::header(g, getWidth(), fx::accent::reverb);
    if (pluginIcon.isValid())
        g.drawImage(pluginIcon, juce::Rectangle<float>(12, 10, 40, 40), juce::RectanglePlacement::centred);
    fx::paint::presetBar(g, getWidth());
    fx::paint::graphArea(g, getWidth());
    fx::paint::graphGrid(g, getWidth(), 48, 48);

    auto vis = juce::Rectangle<int>(0, fx::dim::headerH + fx::dim::presetBarH, getWidth(), fx::dim::visualH);
    paintVisualization(g, vis);

    fx::paint::controls(g, getWidth(), numKnobs);
    fx::paint::footer(g, getWidth());
    if (logoImg.isValid())
    {
        const int fy = fx::dim::appH - fx::dim::footerH;
        g.drawImage(logoImg, juce::Rectangle<float>((float) getWidth() - 52.0f, (float) fy + 4.0f, 32.0f, 32.0f), juce::RectanglePlacement::centred);
    }
    fx::paint::footerLabel(g, "OUT", 80, 180);
    fx::paint::outline(g, getLocalBounds());
}

void MusiqueReverbEditor::resized()
{
    titleLabel.setBounds(56, 10, 150, 40);
    bypassBtn.setBounds(getWidth() - 356, 16, 64, fx::dim::btnH);
    freezeBtn.setBounds(getWidth() - 286, 16, 60, fx::dim::btnH);
    monoBtn.setBounds(getWidth() - 220, 16, 96, fx::dim::btnH);
    trimBtn.setBounds(getWidth() - 118, 16, 96, fx::dim::btnH);

    const int py = fx::dim::headerH + 11;
    prevBtn.setBounds(210, py, 30, fx::dim::btnH);
    presetBox.setBounds(244, py, 218, fx::dim::btnH);
    nextBtn.setBounds(466, py, 30, fx::dim::btnH);
    saveBtn.setBounds(506, py, 56, fx::dim::btnH);
    abBtn.setBounds(568, py, 48, fx::dim::btnH);
    algorithmBox.setBounds(632, py, 118, fx::dim::btnH);
    qualityBox.setBounds(758, py, 96, fx::dim::btnH);

    const int ctrlTop = fx::dim::headerH + fx::dim::presetBarH + fx::dim::visualH;
    const int kW = getWidth() / numKnobs;
    const int kY = ctrlTop + 20;
    for (int i = 0; i < numKnobs; ++i)
    {
        const int x = i * kW;
        knobs[i].setBounds(x + (kW - 70) / 2, kY + 8, 70, 76);
        knobLabels[i].setBounds(x + (kW - 84) / 2, kY + 86, 84, 16);
    }

    groupLabels[0].setBounds(18, ctrlTop + 10, 160, 14);
    groupLabels[1].setBounds(188, ctrlTop + 10, 220, 14);
    groupLabels[2].setBounds(444, ctrlTop + 10, 240, 14);
    groupLabels[3].setBounds(770, ctrlTop + 10, 160, 14);
    groupLabels[4].setBounds(875, ctrlTop + 10, 130, 14);

    const int fy = fx::dim::appH - fx::dim::footerH;
    inMeter.setBounds(16, fy + 6, 20, fx::dim::footerH - 12);
    outMeter.setBounds(42, fy + 6, 20, fx::dim::footerH - 12);
    outputSlider.setBounds(80, fy + 8, 180, 24);
    freezeLED.setBounds(280, fy + 14, 12, 12);
    versionLabel.setBounds(getWidth() - 220, fy + 8, 160, 24);
}
