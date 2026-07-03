#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    // Дефолтные частоты банд, разложенные логарифмически по спектру
    constexpr float defaultFreqs[numBands] = { 60.f, 150.f, 400.f, 1000.f, 2500.f, 6000.f, 10000.f, 15000.f };
}

FabEQAudioProcessor::FabEQAudioProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
}

FabEQAudioProcessor::~FabEQAudioProcessor()
{
    linearPhaseEQ.stopBackgroundThread();
}

juce::AudioProcessorValueTreeState::ParameterLayout FabEQAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    juce::StringArray typeChoices { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch" };

    params.push_back (std::make_unique<juce::AudioParameterChoice>(
        "phase_mode", "Phase Mode",
        juce::StringArray { "Zero Latency", "Linear Phase" }, 0));

    params.push_back (std::make_unique<juce::AudioParameterChoice>(
        "linphase_quality", "Linear Phase Quality",
        juce::StringArray { "Low", "Medium", "High", "Maximum" }, 2));

    for (int i = 0; i < numBands; ++i)
    {
        BandParamIDs id (i);

        params.push_back (std::make_unique<juce::AudioParameterFloat>(
            id.freq, "Band " + juce::String (i + 1) + " Freq",
            juce::NormalisableRange<float> (20.0f, 20000.0f, 1.0f, 0.3f), defaultFreqs[i]));

        params.push_back (std::make_unique<juce::AudioParameterFloat>(
            id.gain, "Band " + juce::String (i + 1) + " Gain",
            juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f));

        params.push_back (std::make_unique<juce::AudioParameterFloat>(
            id.q, "Band " + juce::String (i + 1) + " Q",
            juce::NormalisableRange<float> (0.05f, 18.0f, 0.01f, 0.3f), 0.707f));

        params.push_back (std::make_unique<juce::AudioParameterChoice>(
            id.type, "Band " + juce::String (i + 1) + " Type", typeChoices, 0));

        params.push_back (std::make_unique<juce::AudioParameterBool>(
            id.enabled, "Band " + juce::String (i + 1) + " Enabled", i == 0 || i == numBands - 1));

        // Крутизна ската для Low Cut/High Cut, непрерывно 6..48 dB/oct (1..8 полюсов)
        params.push_back (std::make_unique<juce::AudioParameterFloat>(
            id.slope, "Band " + juce::String (i + 1) + " Slope",
            juce::NormalisableRange<float> (6.0f, 48.0f, 0.1f), 12.0f));
    }

    return { params.begin(), params.end() };
}

void FabEQAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = (juce::uint32) samplesPerBlock;
    spec.numChannels = 2;

    for (auto& band : bandsLeftRight)
    {
        band.prepare (spec);
        band.reset();
    }

    linearPhaseEQ.prepare (sampleRate, samplesPerBlock);
    linearPhaseEQ.setQuality ((int) apvts.getRawParameterValue ("linphase_quality")->load());
    if (! linearPhaseThreadStarted)
    {
        linearPhaseEQ.startBackgroundThread();
        linearPhaseThreadStarted = true;
    }

    juce::zeromem (fifoBuffer, sizeof (fifoBuffer));
    fifoIndex = 0;
}

bool FabEQAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void FabEQAudioProcessor::pushNextSampleIntoFifo (float sample)
{
    if (fifoIndex == fftSize)
    {
        if (! nextFFTBlockReady.load())
        {
            const juce::ScopedLock lock (fftLock);
            juce::zeromem (fftData, sizeof (fftData));
            memcpy (fftData, fifoBuffer, sizeof (fifoBuffer));
            nextFFTBlockReady.store (true);
        }
        fifoIndex = 0;
    }

    fifoBuffer[fifoIndex++] = sample;
}

void FabEQAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    LinearPhaseEQ::BandSnapshot snapshot[numBands];

    // Обновляем параметры каждой полосы из APVTS перед обработкой блока
    for (int i = 0; i < numBands; ++i)
    {
        BandParamIDs id (i);
        auto freq = apvts.getRawParameterValue (id.freq)->load();
        auto gain = apvts.getRawParameterValue (id.gain)->load();
        auto q    = apvts.getRawParameterValue (id.q)->load();
        auto type = (int) apvts.getRawParameterValue (id.type)->load();
        auto en   = apvts.getRawParameterValue (id.enabled)->load() > 0.5f;
        auto slopeDbPerOct = apvts.getRawParameterValue (id.slope)->load();
        float order = juce::jlimit (1.0f, 8.0f, slopeDbPerOct / 6.0f); // dB/oct -> число полюсов

        FilterType ft = filterTypeFromIndex (type);

        bandsLeftRight[(size_t) i].enabled = en;
        bandsLeftRight[(size_t) i].setParameters (ft, freq, gain, q, order);

        snapshot[i] = { ft, freq, gain, q, en, order };
    }

    auto mode = getPhaseMode();

    // Ставим latency один раз при смене режима/качества, не каждый блок
    // (частый вызов setLatencySamples заставляет хост пересчитывать PDC)
    int wantedLatency = getReportedLatencySamples();
    if (mode != lastReportedMode || wantedLatency != lastReportedLatency)
    {
        setLatencySamples (wantedLatency);
        lastReportedMode = mode;
        lastReportedLatency = wantedLatency;
    }

    juce::dsp::AudioBlock<float> block (buffer);

    if (mode == PhaseMode::Linear)
    {
        linearPhaseEQ.setQuality ((int) apvts.getRawParameterValue ("linphase_quality")->load());
        std::array<LinearPhaseEQ::BandSnapshot, numBands> arr;
        for (int i = 0; i < numBands; ++i) arr[(size_t) i] = snapshot[i];
        linearPhaseEQ.updateBandSnapshot (arr);
        linearPhaseEQ.process (block);
    }
    else
    {
        for (auto& band : bandsLeftRight)
            band.process (block);
    }

    // Питаем спектр-анализатор сэмплами после обработки (пост-EQ отклик)
    auto* readPtr = buffer.getReadPointer (0);
    for (int n = 0; n < buffer.getNumSamples(); ++n)
        pushNextSampleIntoFifo (readPtr[n]);
}

FabEQAudioProcessor::PhaseMode FabEQAudioProcessor::getPhaseMode() const
{
    return (PhaseMode) (int) apvts.getRawParameterValue ("phase_mode")->load();
}

int FabEQAudioProcessor::getReportedLatencySamples() const
{
    if (getPhaseMode() != PhaseMode::Linear)
        return 0;

    static constexpr int sizes[4] = { 1024, 2048, 4096, 8192 };
    int qualityIndex = (int) apvts.getRawParameterValue ("linphase_quality")->load();
    return sizes[juce::jlimit (0, 3, qualityIndex)] / 2;
}

void FabEQAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void FabEQAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* FabEQAudioProcessor::createEditor()
{
    return new FabEQAudioProcessorEditor (*this);
}

// Точка входа для JUCE plugin wrappers (VST3/AU/Standalone)
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FabEQAudioProcessor();
}
