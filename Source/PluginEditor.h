#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "SpectrumAnalyzer.h"

/**
    Центральный виджет: сетка частот/дБ, кривая суммарной АЧХ EQ,
    draggable-точки для каждой полосы (X = частота, Y = gain, колесо = Q).
*/
class EQGraphComponent : public juce::Component, private juce::Timer
{
public:
    explicit EQGraphComponent (EQAudioProcessor& p);

    void paint (juce::Graphics& g) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;

    int selectedBand = -1;
    std::function<void(int)> onBandSelected;

private:
    void timerCallback() override { repaint(); }

    float xToFreq (float x) const;
    float freqToX (float freq) const;
    float yToGain (float y) const;
    float gainToY (float gain) const;
    int   findBandNear (juce::Point<float> pos) const;

    void drawGrid (juce::Graphics& g);
    void drawResponseCurve (juce::Graphics& g);
    void drawBandPoints (juce::Graphics& g);

    EQAudioProcessor& processor;
    SpectrumAnalyzer spectrum;

    // Кривая АЧХ считается на UI-потоке из своих собственных FilterBand, а не из
    // processor.bandsLeftRight — те принадлежат аудио-потоку и мутируются им на
    // каждый блок, поэтому чтение их отсюда было гонкой данных и дёргало кривую.
    std::array<FilterBand, numBands> displayBands;
    double displaySampleRate = 0.0;
    void updateDisplayBands();

    static constexpr float minFreq = 20.0f, maxFreq = 20000.0f;
    static constexpr float minDb = -24.0f, maxDb = 24.0f;

    int draggingBand = -1;
};

class EQAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit EQAudioProcessorEditor (EQAudioProcessor&);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    EQAudioProcessor& audioProcessor;

    EQGraphComponent graph;

    // Панель параметров выбранной полосы (freq/gain/Q/type/enabled)
    juce::Slider freqSlider, gainSlider, qSlider, slopeSlider;
    juce::ComboBox typeBox;
    juce::ToggleButton enabledToggle { "On" };
    juce::Label bandLabel { {}, "Band 1" };

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> freqAttach, gainAttach, qAttach, slopeAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> typeAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> enabledAttach;

    // Q используется всеми типами, кроме Low Cut/High Cut, для которых вместо
    // резонанса показывается выбор крутизны ската (12/24/48 dB/oct).
    void updateQVsSlopeVisibility();

    // Global phase controls (не привязаны к отдельной полосе)
    juce::ComboBox phaseModeBox, qualityBox;
    juce::Label phaseModeLabel { {}, "Phase" }, qualityLabel { {}, "Quality" }, latencyLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> phaseModeAttach, qualityAttach;
    void updateLatencyLabelAndQualityVisibility();

    void refreshAttachmentsForBand (int band);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EQAudioProcessorEditor)
};
