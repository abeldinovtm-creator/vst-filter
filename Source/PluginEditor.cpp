#include "PluginEditor.h"

namespace
{
    juce::Colour bandColour (int)
    {
        return juce::Colour (FabColours::kGold);
    }
}

// ============================== FabLookAndFeel ==============================

FabLookAndFeel::FabLookAndFeel()
{
    setColour (juce::ToggleButton::tickColourId, juce::Colour (FabColours::kAccent));
}

void FabLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                       float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                                       juce::Slider& slider)
{
    auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (4.0f);
    auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    auto centre = bounds.getCentre();
    auto angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
    auto accent = juce::Colour (FabColours::kAccent);

    auto arcBounds = bounds.withSizeKeepingCentre (radius * 2.0f, radius * 2.0f);

    // Фоновый трек
    juce::Path track;
    track.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.strokePath (track, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Дуга значения: широкий полупрозрачный штрих + тонкий яркий поверх
    juce::Path valueArc;
    valueArc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, rotaryStartAngle, angle, true);
    g.setColour (accent.withAlpha (0.25f));
    g.strokePath (valueArc, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (accent);
    g.strokePath (valueArc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Тело ручки — градиент
    auto knobBounds = arcBounds.reduced (radius * 0.28f);
    juce::ColourGradient grad (juce::Colour (0xff3a2a2a), knobBounds.getTopLeft(),
                                juce::Colour (0xff181212), knobBounds.getBottomRight(), false);
    g.setGradientFill (grad);
    g.fillEllipse (knobBounds);
    g.setColour (juce::Colours::white.withAlpha (0.15f));
    g.drawEllipse (knobBounds, 1.0f);

    // Указатель
    juce::Path pointer;
    auto pointerLength = knobBounds.getHeight() * 0.5f * 0.85f;
    pointer.addRectangle (-1.2f, -pointerLength, 2.4f, pointerLength);
    pointer.applyTransform (juce::AffineTransform::rotation (angle).translated (centre));
    g.setColour (accent);
    g.fillPath (pointer);

    juce::ignoreUnused (slider);
}

void FabLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                       bool shouldDrawButtonAsHighlighted, bool)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (1.0f);
    bool on = button.getToggleState();
    auto accent = juce::Colour (FabColours::kAccent);

    g.setColour (on ? accent : juce::Colours::white.withAlpha (shouldDrawButtonAsHighlighted ? 0.16f : 0.08f));
    g.fillRoundedRectangle (bounds, 4.0f);
    g.setColour (juce::Colours::white.withAlpha (0.2f));
    g.drawRoundedRectangle (bounds, 4.0f, 1.0f);

    g.setColour (on ? juce::Colours::black.withAlpha (0.85f) : juce::Colours::white.withAlpha (0.8f));
    g.setFont (juce::Font (juce::jmin (14.0f, bounds.getHeight() * 0.55f)));
    g.drawText (button.getButtonText(), bounds, juce::Justification::centred);
}

// ============================== EQGraphComponent ==============================

EQGraphComponent::EQGraphComponent (EQAudioProcessor& p)
    : processor (p), spectrum (p)
{
    spectrum.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (spectrum);
    startTimerHz (60);
    setInterceptsMouseClicks (true, true);
}

void EQGraphComponent::resized()
{
    spectrum.setBounds (getLocalBounds());
}

float EQGraphComponent::freqToX (float freq) const
{
    auto b = getLocalBounds().toFloat();
    return b.getX() + b.getWidth() * (std::log (freq / minFreq) / std::log (maxFreq / minFreq));
}

float EQGraphComponent::xToFreq (float x) const
{
    auto b = getLocalBounds().toFloat();
    float t = juce::jlimit (0.0f, 1.0f, (x - b.getX()) / b.getWidth());
    return minFreq * std::pow (maxFreq / minFreq, t);
}

float EQGraphComponent::gainToY (float gain) const
{
    auto b = getLocalBounds().toFloat();
    return juce::jmap (gain, minDb, maxDb, b.getBottom(), b.getY());
}

float EQGraphComponent::yToGain (float y) const
{
    auto b = getLocalBounds().toFloat();
    return juce::jmap (y, b.getBottom(), b.getY(), minDb, maxDb);
}

void EQGraphComponent::drawGrid (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour (juce::Colours::white.withAlpha (0.08f));

    // Вертикальные линии на характерных частотах
    for (float f : { 20.f, 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f, 20000.f })
    {
        float x = freqToX (f);
        g.drawVerticalLine ((int) x, b.getY(), b.getBottom());
    }

    // Горизонтальные линии на dB-шагах
    for (float db = minDb; db <= maxDb; db += 6.0f)
    {
        float y = gainToY (db);
        g.setColour (db == 0.0f ? juce::Colours::white.withAlpha (0.25f) : juce::Colours::white.withAlpha (0.08f));
        g.drawHorizontalLine ((int) y, b.getX(), b.getRight());
    }
}

void EQGraphComponent::updateDisplayBands()
{
    double sr = processor.currentSampleRate > 0.0 ? processor.currentSampleRate : 44100.0;
    if (sr != displaySampleRate)
    {
        displaySampleRate = sr;
        juce::dsp::ProcessSpec spec { sr, 32, 1 };
        for (auto& band : displayBands)
            band.prepare (spec);
        for (auto& band : displayBandsStatic)
            band.prepare (spec);
    }

    for (int i = 0; i < numBands; ++i)
    {
        BandParamIDs id (i);
        float freq = processor.apvts.getRawParameterValue (id.freq)->load();
        float gain = processor.apvts.getRawParameterValue (id.gain)->load();
        float q    = processor.apvts.getRawParameterValue (id.q)->load();
        int type   = (int) processor.apvts.getRawParameterValue (id.type)->load();
        bool en    = processor.apvts.getRawParameterValue (id.enabled)->load() > 0.5f;
        float slopeDbPerOct = processor.apvts.getRawParameterValue (id.slope)->load();
        float order = juce::jlimit (1.0f, 8.0f, slopeDbPerOct / 6.0f);

        auto ft = filterTypeFromIndex (type);

        displayBandsStatic[(size_t) i].enabled = en;
        displayBandsStatic[(size_t) i].setParameters (ft, freq, gain, q, order);

        gain += processor.currentDynGainOffsetDb[(size_t) i].load();

        displayBands[(size_t) i].enabled = en;
        displayBands[(size_t) i].setParameters (ft, freq, gain, q, order);
    }
}

void EQGraphComponent::drawDynamicsOverlay (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    int numPx = juce::jmax (2, (int) b.getWidth());

    std::vector<float> xs ((size_t) numPx), liveYs ((size_t) numPx), staticYs ((size_t) numPx), diffs ((size_t) numPx);

    for (int i = 0; i < numPx; ++i)
    {
        float x = b.getX() + b.getWidth() * (float) i / (float) (numPx - 1);
        float freq = xToFreq (x);
        double liveMag = 1.0, staticMag = 1.0;

        for (int bd = 0; bd < numBands; ++bd)
        {
            if (displayBands[(size_t) bd].enabled)
                liveMag *= displayBands[(size_t) bd].getMagnitudeForFrequency (freq, displaySampleRate);
            if (displayBandsStatic[(size_t) bd].enabled)
                staticMag *= displayBandsStatic[(size_t) bd].getMagnitudeForFrequency (freq, displaySampleRate);
        }

        float liveDb   = juce::Decibels::gainToDecibels ((float) liveMag, -60.0f);
        float staticDb = juce::Decibels::gainToDecibels ((float) staticMag, -60.0f);

        xs[(size_t) i]      = x;
        liveYs[(size_t) i]  = gainToY (juce::jlimit (minDb, maxDb, liveDb));
        staticYs[(size_t) i] = gainToY (juce::jlimit (minDb, maxDb, staticDb));
        diffs[(size_t) i]   = liveDb - staticDb;
    }

    auto boost = juce::Colour (FabColours::kDynBoost);
    auto cut   = juce::Colour (FabColours::kDynCut);

    int i = 0;
    while (i < numPx)
    {
        if (std::abs (diffs[(size_t) i]) < 0.05f) { ++i; continue; }

        bool boosting = diffs[(size_t) i] > 0.0f;
        int start = i;
        float maxAbs = std::abs (diffs[(size_t) i]);

        while (i < numPx && std::abs (diffs[(size_t) i]) >= 0.05f && (diffs[(size_t) i] > 0.0f) == boosting)
        {
            maxAbs = juce::jmax (maxAbs, std::abs (diffs[(size_t) i]));
            ++i;
        }
        int end = i - 1;

        juce::Path ribbon;
        ribbon.startNewSubPath (xs[(size_t) start], staticYs[(size_t) start]);
        for (int k = start + 1; k <= end; ++k)
            ribbon.lineTo (xs[(size_t) k], staticYs[(size_t) k]);
        for (int k = end; k >= start; --k)
            ribbon.lineTo (xs[(size_t) k], liveYs[(size_t) k]);
        ribbon.closeSubPath();

        float alpha = juce::jlimit (0.12f, 0.55f, maxAbs / 6.0f);
        g.setColour ((boosting ? boost : cut).withAlpha (alpha));
        g.fillPath (ribbon);
    }
}

void EQGraphComponent::drawResponseCurve (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    juce::Path curve;

    // Оверсэмплинг: одной точки на пиксель не хватает для узких высоко-Q резонансов
    // (полоса в пикселях может быть меньше десятка) — кривая рисуется зубчатым "шипом"
    // вместо гладкого колокола. Берём в несколько раз больше точек, чем пикселей.
    const int numPoints = juce::jmax (2, (int) b.getWidth() * 4);
    for (int i = 0; i < numPoints; ++i)
    {
        float x = b.getX() + b.getWidth() * (float) i / (float) (numPoints - 1);
        float freq = xToFreq (x);
        double totalMagLin = 1.0;

        for (auto& band : displayBands)
            if (band.enabled)
                totalMagLin *= band.getMagnitudeForFrequency (freq, displaySampleRate);

        float db = juce::Decibels::gainToDecibels ((float) totalMagLin, -60.0f);
        float y = gainToY (juce::jlimit (minDb, maxDb, db));

        if (i == 0) curve.startNewSubPath (x, y);
        else        curve.lineTo (x, y);
    }

    auto accent = juce::Colour (FabColours::kAccent);

    auto fillPath = curve;
    fillPath.lineTo (b.getBottomRight());
    fillPath.lineTo (b.getBottomLeft());
    fillPath.closeSubPath();
    g.setColour (accent.withAlpha (0.12f));
    g.fillPath (fillPath);

    g.setColour (accent);
    g.strokePath (curve, juce::PathStrokeType (2.0f));
}

void EQGraphComponent::drawBandPoints (juce::Graphics& g)
{
    for (int i = 0; i < numBands; ++i)
    {
        BandParamIDs id (i);
        float freq = processor.apvts.getRawParameterValue (id.freq)->load();
        float gain = processor.apvts.getRawParameterValue (id.gain)->load();
        bool  en   = processor.apvts.getRawParameterValue (id.enabled)->load() > 0.5f;

        float x = freqToX (freq);
        float y = gainToY (gain);

        auto colour = bandColour (i).withAlpha (en ? 1.0f : 0.3f);
        float radius = (i == selectedBand) ? 8.0f : 6.0f;

        g.setColour (colour);
        g.drawEllipse (x - radius, y - radius, radius * 2, radius * 2, i == selectedBand ? 2.5f : 1.5f);
        if (i == selectedBand)
            g.fillEllipse (x - 3, y - 3, 6, 6);

        g.setFont (11.0f);
        g.drawText (juce::String (i + 1), (int) x - 10, (int) y - 22, 20, 16, juce::Justification::centred);
    }
}

void EQGraphComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    juce::ColourGradient bg (juce::Colour (0xff14131a), bounds.getTopLeft(),
                              juce::Colour (0xff1f1416), bounds.getBottomRight(), false);
    g.setGradientFill (bg);
    g.fillRect (bounds);

    updateDisplayBands();

    drawGrid (g);
    drawDynamicsOverlay (g);
    drawResponseCurve (g);
    drawBandPoints (g);
}

int EQGraphComponent::findBandNear (juce::Point<float> pos) const
{
    for (int i = 0; i < numBands; ++i)
    {
        BandParamIDs id (i);
        float freq = processor.apvts.getRawParameterValue (id.freq)->load();
        float gain = processor.apvts.getRawParameterValue (id.gain)->load();
        juce::Point<float> p (freqToX (freq), gainToY (gain));
        if (p.getDistanceFrom (pos) < 12.0f)
            return i;
    }
    return -1;
}

void EQGraphComponent::mouseDown (const juce::MouseEvent& e)
{
    int band = findBandNear (e.position);
    if (band >= 0)
    {
        selectedBand = band;
        if (onBandSelected)
            onBandSelected (band);
    }
    draggingBand = band;
    repaint();
}

void EQGraphComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingBand < 0)
        return;

    BandParamIDs id (draggingBand);
    float freq = juce::jlimit (minFreq, maxFreq, xToFreq (e.position.x));
    float gain = juce::jlimit (minDb, maxDb, yToGain (e.position.y));

    if (auto* p = processor.apvts.getParameter (id.freq))
        p->setValueNotifyingHost (p->convertTo0to1 (freq));
    if (auto* p = processor.apvts.getParameter (id.gain))
        p->setValueNotifyingHost (p->convertTo0to1 (gain));

    repaint();
}

void EQGraphComponent::mouseUp (const juce::MouseEvent&)
{
    draggingBand = -1;
}

void EQGraphComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    int band = findBandNear (e.position);
    if (band < 0)
        band = selectedBand;
    if (band < 0)
        return;

    BandParamIDs id (band);
    int type = (int) processor.apvts.getRawParameterValue (id.type)->load();
    bool isCut = (type == 3 /* LowCut */ || type == 4 /* HighCut */);

    if (isCut)
    {
        // Для Low/High Cut колесо мыши плавно меняет крутизну ската (6..48 dB/oct)
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter (id.slope)))
        {
            float slope = juce::jlimit (6.0f, 48.0f, p->get() + wheel.deltaY * 6.0f);
            p->setValueNotifyingHost (p->convertTo0to1 (slope));
        }
    }
    else if (auto* p = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter (id.q)))
    {
        float q = p->get() + wheel.deltaY * 2.0f;
        q = juce::jlimit (0.05f, 18.0f, q);
        p->setValueNotifyingHost (p->convertTo0to1 (q));
    }
    repaint();
}

void EQGraphComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    int band = findBandNear (e.position);
    if (band < 0)
        return;

    BandParamIDs id (band);
    if (auto* p = processor.apvts.getParameter (id.enabled))
    {
        bool cur = p->getValue() > 0.5f;
        p->setValueNotifyingHost (cur ? 0.0f : 1.0f);
    }
    repaint();
}

// ============================== EQAudioProcessorEditor ==============================

EQAudioProcessorEditor::EQAudioProcessorEditor (EQAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p), graph (p)
{
    setLookAndFeel (&lookAndFeel);

    addAndMakeVisible (graph);

    for (auto* s : { &freqSlider, &gainSlider, &qSlider, &slopeSlider })
    {
        s->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 60, 16);
        addAndMakeVisible (s);
    }
    freqSlider.setSkewFactorFromMidPoint (1000.0);

    typeBox.addItemList ({ "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch" }, 1);
    addAndMakeVisible (typeBox);
    typeBox.onChange = [this] { updateQVsSlopeVisibility(); };

    addAndMakeVisible (enabledToggle);
    addAndMakeVisible (bandLabel);
    bandLabel.setJustificationType (juce::Justification::centred);

    addAndMakeVisible (dynEnabledToggle);
    for (auto* s : { &dynThresholdSlider, &dynRangeSlider })
    {
        s->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 60, 16);
        addAndMakeVisible (s);
    }
    for (auto* l : { &dynThresholdLabel, &dynRangeLabel })
    {
        l->setJustificationType (juce::Justification::centred);
        l->setFont (11.0f);
        addAndMakeVisible (l);
    }

    phaseModeBox.addItemList ({ "Zero Latency", "Linear Phase" }, 1);
    qualityBox.addItemList ({ "Low", "Medium", "High", "Maximum" }, 1);
    addAndMakeVisible (phaseModeBox);
    addAndMakeVisible (qualityBox);
    addAndMakeVisible (phaseModeLabel);
    addAndMakeVisible (qualityLabel);
    addAndMakeVisible (latencyLabel);
    phaseModeLabel.setJustificationType (juce::Justification::centred);
    qualityLabel.setJustificationType (juce::Justification::centred);
    latencyLabel.setJustificationType (juce::Justification::centred);
    latencyLabel.setFont (11.0f);

    phaseModeAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        audioProcessor.apvts, "phase_mode", phaseModeBox);
    qualityAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        audioProcessor.apvts, "linphase_quality", qualityBox);

    phaseModeBox.onChange = [this] { updateLatencyLabelAndQualityVisibility(); };
    updateLatencyLabelAndQualityVisibility();

    graph.onBandSelected = [this] (int band) { refreshAttachmentsForBand (band); };
    refreshAttachmentsForBand (0);
    graph.selectedBand = 0;

    startTimer (200);
    setSize (900, 560);
}

EQAudioProcessorEditor::~EQAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void EQAudioProcessorEditor::updateLatencyLabelAndQualityVisibility()
{
    bool isLinear = phaseModeBox.getSelectedItemIndex() == 1;
    qualityBox.setVisible (isLinear);
    qualityLabel.setVisible (isLinear);

    int latencySamples = audioProcessor.getReportedLatencySamples();
    double ms = audioProcessor.currentSampleRate > 0.0
                    ? 1000.0 * (double) latencySamples / audioProcessor.currentSampleRate
                    : 0.0;
    latencyLabel.setText (isLinear
                              ? "Latency: " + juce::String (latencySamples) + " smp (" + juce::String (ms, 1) + " ms)"
                              : "Latency: 0 smp",
                          juce::dontSendNotification);
}

void EQAudioProcessorEditor::timerCallback()
{
    updateLatencyLabelAndQualityVisibility();
}

void EQAudioProcessorEditor::refreshAttachmentsForBand (int band)
{
    BandParamIDs id (band);
    freqAttach.reset();
    gainAttach.reset();
    qAttach.reset();
    typeAttach.reset();
    slopeAttach.reset();
    enabledAttach.reset();
    dynEnabledAttach.reset();
    dynThresholdAttach.reset();
    dynRangeAttach.reset();

    freqAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (audioProcessor.apvts, id.freq, freqSlider);
    gainAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (audioProcessor.apvts, id.gain, gainSlider);
    qAttach    = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (audioProcessor.apvts, id.q, qSlider);
    typeAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (audioProcessor.apvts, id.type, typeBox);
    slopeAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (audioProcessor.apvts, id.slope, slopeSlider);
    enabledAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (audioProcessor.apvts, id.enabled, enabledToggle);

    dynEnabledAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (audioProcessor.apvts, id.dynEnabled, dynEnabledToggle);
    dynThresholdAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (audioProcessor.apvts, id.dynThreshold, dynThresholdSlider);
    dynRangeAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (audioProcessor.apvts, id.dynRange, dynRangeSlider);

    bandLabel.setText ("Band " + juce::String (band + 1), juce::dontSendNotification);
    updateQVsSlopeVisibility();
}

void EQAudioProcessorEditor::updateQVsSlopeVisibility()
{
    int type = typeBox.getSelectedItemIndex();
    bool isCut = (type == 3 /* Low Cut */ || type == 4 /* High Cut */);
    qSlider.setVisible (! isCut);
    slopeSlider.setVisible (isCut);
}

void EQAudioProcessorEditor::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    juce::ColourGradient bg (juce::Colour (0xff14131a), bounds.getTopLeft(),
                              juce::Colour (0xff1f1416), bounds.getBottomRight(), false);
    g.setGradientFill (bg);
    g.fillRect (bounds);

    auto bottom = getLocalBounds().removeFromBottom (110).toFloat();
    g.setColour (juce::Colour (FabColours::kPanelBg));
    g.fillRoundedRectangle (bottom.reduced (4.0f), 6.0f);
    g.setColour (juce::Colour (FabColours::kPanelBrd));
    g.drawRoundedRectangle (bottom.reduced (4.0f), 6.0f, 1.0f);
}

void EQAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();
    auto bottom = area.removeFromBottom (110);
    auto top = area.removeFromTop (30);

    top.reduce (8, 2);
    phaseModeLabel.setBounds (top.removeFromLeft (45));
    phaseModeBox.setBounds (top.removeFromLeft (110));
    top.removeFromLeft (12);
    qualityLabel.setBounds (top.removeFromLeft (50));
    qualityBox.setBounds (top.removeFromLeft (100));
    top.removeFromLeft (12);
    latencyLabel.setBounds (top.removeFromLeft (220));

    graph.setBounds (area.reduced (8));

    bottom.reduce (12, 8);
    bandLabel.setBounds (bottom.removeFromLeft (70));
    typeBox.setBounds (bottom.removeFromLeft (120).reduced (0, 30));
    enabledToggle.setBounds (bottom.removeFromLeft (60).reduced (0, 40));
    freqSlider.setBounds (bottom.removeFromLeft (100));
    gainSlider.setBounds (bottom.removeFromLeft (100));
    auto qArea = bottom.removeFromLeft (100);
    qSlider.setBounds (qArea);
    slopeSlider.setBounds (qArea);

    bottom.removeFromLeft (8);
    dynEnabledToggle.setBounds (bottom.removeFromLeft (50).reduced (0, 40));
    auto threshArea = bottom.removeFromLeft (100);
    dynThresholdLabel.setBounds (threshArea.removeFromBottom (14));
    dynThresholdSlider.setBounds (threshArea);
    auto rangeArea = bottom.removeFromLeft (100);
    dynRangeLabel.setBounds (rangeArea.removeFromBottom (14));
    dynRangeSlider.setBounds (rangeArea);
}
