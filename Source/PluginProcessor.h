#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "FilterBand.h"
#include "LinearPhaseEQ.h"

struct BandParamIDs
{
    juce::String freq, gain, q, type, enabled, slope;

    explicit BandParamIDs (int index)
    {
        auto p = juce::String (index);
        freq    = "band" + p + "_freq";
        gain    = "band" + p + "_gain";
        q       = "band" + p + "_q";
        type    = "band" + p + "_type";
        enabled = "band" + p + "_enabled";
        slope   = "band" + p + "_slope";
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

    // ---- Linear Phase ----
    LinearPhaseEQ linearPhaseEQ;
    bool linearPhaseThreadStarted = false;
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
