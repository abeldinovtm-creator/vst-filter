#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "FilterBand.h"
#include "LinearPhaseEQ.h"

struct BandParamIDs
{
    juce::String freq, gain, q, type, enabled, slope;
    juce::String dynEnabled, dynThreshold, dynRange;

    explicit BandParamIDs (int index)
    {
        auto p = juce::String (index);
        freq    = "band" + p + "_freq";
        gain    = "band" + p + "_gain";
        q       = "band" + p + "_q";
        type    = "band" + p + "_type";
        enabled = "band" + p + "_enabled";
        slope   = "band" + p + "_slope";
        dynEnabled   = "band" + p + "_dyn_enabled";
        dynThreshold = "band" + p + "_dyn_threshold";
        dynRange     = "band" + p + "_dyn_range";
    }
};

inline FilterType filterTypeFromIndex (int type)
{
    switch (type)
    {
        case 0: return FilterType::Bell;
        case 1: return FilterType::LowShelf;
        case 2: return FilterType::HighShelf;
        case 3: return FilterType::LowCut;
        case 4: return FilterType::HighCut;
        case 5: return FilterType::Notch;
        default: return FilterType::Bell;
    }
}

class EQAudioProcessor : public juce::AudioProcessor
{
public:
    EQAudioProcessor();
    ~EQAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "EQ"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    std::array<FilterBand, numBands> bandsLeftRight; // одна инстанция FilterBand обрабатывает оба канала внутри
    double currentSampleRate = 44100.0;

    // Сглаживание freq/gain/q/order в ZeroLatency-режиме: без него быстрое
    // движение ручки (или автоматизация) между суб-блоками — это мгновенная
    // подмена коэффициентов биквада на новое значение, и слышимый треск
    // ровно того же типа, что чинили для dynamics-огибающей, только источник
    // скачка теперь ручной, а не envelope follower.
    static constexpr float paramSmoothingSeconds = 0.02f; // 20мс
    std::array<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>, numBands>
        freqSmoothers, gainSmoothers, qSmoothers, orderSmoothers;

    // ---- Dynamic EQ ----
    juce::AudioBuffer<float> dryBuffer;
    // Текущая поправка гейна от dynamics-детектора на каждую полосу. Читается из
    // EQGraphComponent (UI-поток) для "дышащей" кривой без гонки с аудио-потоком.
    std::array<std::atomic<float>, numBands> currentDynGainOffsetDb {};

    // ---- Linear Phase ----
    LinearPhaseEQ linearPhaseEQ;
    bool linearPhaseThreadStarted = false;

    // FFT-kernel в Linear Phase строится только по статическому gain (ручки),
    // без dynamic-offset — иначе dirty-флаг в LinearPhaseEQ триггерится почти
    // каждый callback, пока dynamics активен, и kernel пересобирается непрерывно
    // (дорого + источник щелчков на подмене IR). Быстрая dynamics-модуляция
    // накладывается постфактум простым смуженным гейном поверх результата
    // свёртки — kernel остаётся стабильным, "дыхание" не трогает его вообще.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> linearPhaseDynGainSmoother;
    enum class PhaseMode { ZeroLatency = 0, Linear = 1 };
    PhaseMode getPhaseMode() const;
    int getReportedLatencySamples() const;
    PhaseMode lastReportedMode = PhaseMode::ZeroLatency;
    int lastReportedLatency = 0;

    // Для спектр-анализатора в редакторе: буфер FFT данных, читаемый из UI-потока
    static constexpr int fftOrder = 11; // 2048
    static constexpr int fftSize = 1 << fftOrder;
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann };
    juce::AbstractFifo abstractFifo { fftSize * 2 };
    juce::AudioBuffer<float> fftDrawBuffer;
    std::atomic<bool> nextFFTBlockReady { false };
    float fifoBuffer[fftSize];
    float fftData[2 * fftSize];
    int fifoIndex = 0;
    juce::CriticalSection fftLock;

    void pushNextSampleIntoFifo (float sample);

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EQAudioProcessor)
};
