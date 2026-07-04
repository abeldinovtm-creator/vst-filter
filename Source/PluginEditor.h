#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "SpectrumAnalyzer.h"

namespace FabColours
{
    static constexpr juce::uint32 kAccent   = 0xffe8544a; // коралловый
    static constexpr juce::uint32 kGold     = 0xffe8b84b;
    static constexpr juce::uint32 kPanelBg  = 0xff1c1516;
    static constexpr juce::uint32 kPanelBrd = 0xff3a2226;

    // Оттенки для визуализации правки dynamic EQ: буст относительно статической
    // кривой подсвечивается золотым, даккинг — холодным синим (контраст к kGold/kAccent).
    static constexpr juce::uint32 kDynBoost = kGold;
    static constexpr juce::uint32 kDynCut   = 0xff4aa8e8;

    // Маркеры авто-найденных резонансов на графике — тревожный красно-оранжевый,
    // чтобы явно отличаться от кривой/точек полос.
    static constexpr juce::uint32 kResonance = 0xffff5a3c;
}

/** Тёмная тема с коралловым акцентом: кастомные ручки и тумблеры. */
class FabLookAndFeel : public juce::LookAndFeel_V4
{
public:
    FabLookAndFeel();

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider& slider) override;

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                           bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
};

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
    void timerCallback() override { processor.updateResonancePeaks(); repaint(); }

    float xToFreq (float x) const;
    float freqToX (float freq) const;
    float yToGain (float y) const;
    float gainToY (float gain) const;
    int   findBandNear (juce::Point<float> pos) const;

    void drawGrid (juce::Graphics& g);
    void drawDynamicsOverlay (juce::Graphics& g);
    void drawResponseCurve (juce::Graphics& g);
    void drawBandPoints (juce::Graphics& g);
    void drawResonanceMarkers (juce::Graphics& g);

    // Рабочая частота полосы для отображения: если у полосы включён Auto, ручной
    // freq-параметр игнорируется движком, и точку/кривую нужно рисовать по
    // processor.currentAutoTrackedFreq, а не по APVTS-параметру.
    float displayFreqForBand (int bandIndex) const;

    EQAudioProcessor& processor;
    SpectrumAnalyzer spectrum;

    // Кривая АЧХ считается на UI-потоке из своих собственных FilterBand, а не из
    // processor.bandsLeftRight — те принадлежат аудио-потоку и мутируются им на
    // каждый блок, поэтому чтение их отсюда было гонкой данных и дёргало кривую.
    // displayBands — «живая» кривая с поправкой dynamic EQ (currentDynGainOffsetDb),
    // displayBandsStatic — та же кривая без поправки, нужна как база для сравнения
    // в drawDynamicsOverlay (насколько и в какую сторону дёргает динамика).
    std::array<FilterBand, numBands> displayBands;
    std::array<FilterBand, numBands> displayBandsStatic;
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
    ~EQAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    EQAudioProcessor& audioProcessor;

    FabLookAndFeel lookAndFeel;

    EQGraphComponent graph;

    // Панель параметров выбранной полосы (freq/gain/Q/type/enabled)
    juce::Slider freqSlider, gainSlider, qSlider, slopeSlider;
    juce::ComboBox typeBox;
    juce::ToggleButton enabledToggle { "On" };
    juce::Label bandLabel { {}, "Band 1" };

    // Dynamic EQ controls для выбранной полосы
    juce::ToggleButton dynEnabledToggle { "Dyn" };
    juce::Slider dynThresholdSlider, dynRangeSlider;
    juce::Label dynThresholdLabel { {}, "Threshold" }, dynRangeLabel { {}, "Range" };

    // Auto: полоса сама находит и гасит резонанс (см. EQAudioProcessor::updateResonancePeaks)
    juce::ToggleButton autoResonanceToggle { "Auto" };

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> freqAttach, gainAttach, qAttach, slopeAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> typeAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> enabledAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> dynEnabledAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> dynThresholdAttach, dynRangeAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> autoResonanceAttach;

    // Q используется всеми типами, кроме Low Cut/High Cut, для которых вместо
    // резонанса показывается выбор крутизны ската (12/24/48 dB/oct).
    void updateQVsSlopeVisibility();

    // Global phase controls (не привязаны к отдельной полосе)
    juce::ComboBox phaseModeBox, qualityBox;
    juce::Label phaseModeLabel { {}, "Phase" }, qualityLabel { {}, "Quality" }, latencyLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> phaseModeAttach, qualityAttach;
    void updateLatencyLabelAndQualityVisibility();

    // Авторская подпись в углу интерфейса
    juce::Label creditLabel { {}, "Tlek Abeldinov" };

    void refreshAttachmentsForBand (int band);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EQAudioProcessorEditor)
};
