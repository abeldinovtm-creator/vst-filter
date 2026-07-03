#include "PluginEditor.h"

namespace
{
    juce::Colour bandColour (int i)
    {
        static const juce::Colour palette[numBands] = {
            juce::Colours::orange, juce::Colours::yellow, juce::Colours::lime,
            juce::Colours::cyan, juce::Colours::skyblue, juce::Colours::violet,
            juce::Colours::hotpink, juce::Colours::red
        };
        return palette[i % numBands];
    }
}

// ============================== EQGraphComponent ==============================

EQGraphComponent::EQGraphComponent (FabEQAudioProcessor& p)
    : processor (p), spectrum (p)
{
    spectrum.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (spectrum);
    startTimerHz (30);
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

        displayBands[(size_t) i].enabled = en;
        displayBands[(size_t) i].setParameters (filterTypeFromIndex (type), freq, gain, q, order);
    }
}

void EQGraphComponent::drawResponseCurve (juce::Graphics& g)
{
    updateDisplayBands();

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

    g.setColour (juce::Colours::white);
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
    g.fillAll (juce::Colour (0xff1a1a1e));
    drawGrid (g);
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

// ============================== FabEQAudioProcessorEditor ==============================

FabEQAudioProcessorEditor::FabEQAudioProcessorEditor (FabEQAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p), graph (p)
{
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

void FabEQAudioProcessorEditor::updateLatencyLabelAndQualityVisibility()
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

void FabEQAudioProcessorEditor::timerCallback()
{
    updateLatencyLabelAndQualityVisibility();
}

void FabEQAudioProcessorEditor::refreshAttachmentsForBand (int band)
{
    BandParamIDs id (band);
    freqAttach.reset();
    gainAttach.reset();
    qAttach.reset();
    typeAttach.reset();
    slopeAttach.reset();
    enabledAttach.reset();

    freqAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (audioProcessor.apvts, id.freq, freqSlider);
    gainAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (audioProcessor.apvts, id.gain, gainSlider);
    qAttach    = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (audioProcessor.apvts, id.q, qSlider);
    typeAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (audioProcessor.apvts, id.type, typeBox);
    slopeAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (audioProcessor.apvts, id.slope, slopeSlider);
    enabledAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (audioProcessor.apvts, id.enabled, enabledToggle);

    bandLabel.setText ("Band " + juce::String (band + 1), juce::dontSendNotification);
    updateQVsSlopeVisibility();
}

void FabEQAudioProcessorEditor::updateQVsSlopeVisibility()
{
    int type = typeBox.getSelectedItemIndex();
    bool isCut = (type == 3 /* Low Cut */ || type == 4 /* High Cut */);
    qSlider.setVisible (! isCut);
    slopeSlider.setVisible (isCut);
}

void FabEQAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff101012));
}

void FabEQAudioProcessorEditor::resized()
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
}
