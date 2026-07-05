#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
    // Дефолтные частоты банд, разложенные логарифмически по спектру
    constexpr float defaultFreqs[numBands] = { 60.f, 150.f, 400.f, 1000.f, 2500.f, 6000.f, 10000.f, 15000.f };
}

EQAudioProcessor::EQAudioProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
}

EQAudioProcessor::~EQAudioProcessor()
{
    linearPhaseEQ.stopBackgroundThread();
}

juce::AudioProcessorValueTreeState::ParameterLayout EQAudioProcessor::createParameterLayout()
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
            id.enabled, "Band " + juce::String (i + 1) + " Enabled", false));

        // Не показывается как отдельный контрол в UI — просто помечает, что
        // полоса когда-либо была активирована кликом по графику. См. комментарий
        // у BandParamIDs::used в PluginProcessor.h. Изначально пусто — как в
        // FabFilter, ни одной точки на старте, пока пользователь сам не кликнет.
        params.push_back (std::make_unique<juce::AudioParameterBool>(
            id.used, "Band " + juce::String (i + 1) + " Used", false));

        // Крутизна ската для Low Cut/High Cut, непрерывно 6..48 dB/oct (1..8 полюсов)
        params.push_back (std::make_unique<juce::AudioParameterFloat>(
            id.slope, "Band " + juce::String (i + 1) + " Slope",
            juce::NormalisableRange<float> (6.0f, 48.0f, 0.1f), 12.0f));

        // ---- Dynamic EQ ----
        params.push_back (std::make_unique<juce::AudioParameterBool>(
            id.dynEnabled, "Band " + juce::String (i + 1) + " Dyn Enabled", false));

        params.push_back (std::make_unique<juce::AudioParameterFloat>(
            id.dynThreshold, "Band " + juce::String (i + 1) + " Dyn Threshold",
            juce::NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -24.0f));

        params.push_back (std::make_unique<juce::AudioParameterFloat>(
            id.dynRange, "Band " + juce::String (i + 1) + " Dyn Range",
            juce::NormalisableRange<float> (-30.0f, 30.0f, 0.1f), 0.0f));

        // ---- Auto-resonance ----
        params.push_back (std::make_unique<juce::AudioParameterBool>(
            id.autoResonance, "Band " + juce::String (i + 1) + " Auto Resonance", false));
    }

    return { params.begin(), params.end() };
}

void EQAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
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

    dryBuffer.setSize (2, samplesPerBlock, false, false, true);

    linearPhaseDynGainSmoother.reset (sampleRate, 0.03); // ~30мс рамп
    linearPhaseDynGainSmoother.setCurrentAndTargetValue (1.0f);

    for (int i = 0; i < numBands; ++i)
    {
        BandParamIDs id (i);

        auto& freqSm  = freqSmoothers[(size_t) i];
        auto& gainSm  = gainSmoothers[(size_t) i];
        auto& qSm     = qSmoothers[(size_t) i];
        auto& orderSm = orderSmoothers[(size_t) i];

        freqSm.reset  (sampleRate, paramSmoothingSeconds);
        gainSm.reset  (sampleRate, paramSmoothingSeconds);
        qSm.reset     (sampleRate, paramSmoothingSeconds);
        orderSm.reset (sampleRate, paramSmoothingSeconds);

        auto slopeDbPerOct = apvts.getRawParameterValue (id.slope)->load();

        freqSm.setCurrentAndTargetValue  (apvts.getRawParameterValue (id.freq)->load());
        gainSm.setCurrentAndTargetValue  (apvts.getRawParameterValue (id.gain)->load());
        qSm.setCurrentAndTargetValue     (apvts.getRawParameterValue (id.q)->load());
        orderSm.setCurrentAndTargetValue (juce::jlimit (1.0f, 8.0f, slopeDbPerOct / 6.0f));

        auto& detectorFreqSm = detectorFreqSmoothers[(size_t) i];
        auto& detectorQSm    = detectorQSmoothers[(size_t) i];

        detectorFreqSm.reset (sampleRate, detectorSmoothingSeconds);
        detectorQSm.reset    (sampleRate, detectorSmoothingSeconds);

        detectorFreqSm.setCurrentAndTargetValue (apvts.getRawParameterValue (id.freq)->load());
        detectorQSm.setCurrentAndTargetValue    (apvts.getRawParameterValue (id.q)->load());
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

    juce::zeromem (preFifoBuffer, sizeof (preFifoBuffer));
    preFifoIndex = 0;

    juce::zeromem (resonanceFifoBuffer, sizeof (resonanceFifoBuffer));
    resonanceFifoIndex = 0;
    detectedResonanceCount.store (0);
    for (auto& f : detectedResonanceFreqHz) f.store (0.0f);
    for (auto& f : currentAutoTrackedFreq) f.store (0.0f);
}

bool EQAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void EQAudioProcessor::pushNextSampleIntoFifo (float sample)
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

void EQAudioProcessor::pushNextPreSampleIntoFifo (float sample)
{
    if (preFifoIndex == fftSize)
    {
        if (! nextPreFFTBlockReady.load())
        {
            const juce::ScopedLock lock (preFftLock);
            juce::zeromem (preFftData, sizeof (preFftData));
            memcpy (preFftData, preFifoBuffer, sizeof (preFifoBuffer));
            nextPreFFTBlockReady.store (true);
        }
        preFifoIndex = 0;
    }

    preFifoBuffer[preFifoIndex++] = sample;
}

void EQAudioProcessor::pushNextDrySampleIntoResonanceFifo (float sample)
{
    if (resonanceFifoIndex == resonanceFftSize)
    {
        if (! nextResonanceFFTBlockReady.load())
        {
            const juce::ScopedLock lock (resonanceFftLock);
            juce::zeromem (resonanceFftData, sizeof (resonanceFftData));
            memcpy (resonanceFftData, resonanceFifoBuffer, sizeof (resonanceFifoBuffer));
            nextResonanceFFTBlockReady.store (true);
        }
        resonanceFifoIndex = 0;
    }

    resonanceFifoBuffer[resonanceFifoIndex++] = sample;
}

void EQAudioProcessor::updateResonancePeaks()
{
    if (! nextResonanceFFTBlockReady.load())
        return;

    {
        const juce::ScopedLock lock (resonanceFftLock);
        resonanceWindow.multiplyWithWindowingTable (resonanceFftData, (size_t) resonanceFftSize);
        resonanceFft.performFrequencyOnlyForwardTransform (resonanceFftData);
        nextResonanceFFTBlockReady.store (false);
    }

    const int numBins = resonanceFftSize / 2;
    constexpr float minFreqHz = 40.0f, maxFreqHz = 18000.0f;
    constexpr float minProminenceDb = 6.0f;

    std::vector<float> dbSpectrum ((size_t) numBins);
    for (int i = 0; i < numBins; ++i)
        dbSpectrum[(size_t) i] = juce::Decibels::gainToDecibels (resonanceFftData[i], -100.0f);

    // Локально сглаженное среднее — приближённая "спектральная огибающая",
    // относительно которой ищем именно ВЫСТУПАЮЩИЕ пики, а не просто самые
    // громкие частоты (иначе на плотном миксе резонансом сочтётся весь бас).
    int smoothRadius = juce::jmax (2, numBins / 64);
    std::vector<float> localAvg ((size_t) numBins);
    for (int i = 0; i < numBins; ++i)
    {
        int lo = juce::jmax (0, i - smoothRadius);
        int hi = juce::jmin (numBins - 1, i + smoothRadius);
        double sum = 0.0;
        int count = 0;
        for (int k = lo; k <= hi; ++k) { sum += dbSpectrum[(size_t) k]; ++count; }
        localAvg[(size_t) i] = (float) (sum / count);
    }

    struct Candidate { float freq; float prominence; };
    std::vector<Candidate> candidates;

    for (int i = 2; i < numBins - 2; ++i)
    {
        float freq = (float) (i * currentSampleRate / resonanceFftSize);
        if (freq < minFreqHz || freq > maxFreqHz)
            continue;

        bool isLocalMax = dbSpectrum[(size_t) i] > dbSpectrum[(size_t) (i - 1)]
                         && dbSpectrum[(size_t) i] >= dbSpectrum[(size_t) (i + 1)];
        if (! isLocalMax)
            continue;

        float prominence = dbSpectrum[(size_t) i] - localAvg[(size_t) i];
        if (prominence < minProminenceDb)
            continue;

        candidates.push_back ({ freq, prominence });
    }

    std::sort (candidates.begin(), candidates.end(),
              [] (const Candidate& a, const Candidate& b) { return a.prominence > b.prominence; });

    // Разносим по минимальному расстоянию (~25% по частоте, примерно треть
    // октавы). 5% было слишком мало: две Auto-полосы могли "нацелиться" на
    // почти соседние частоты одного и того же широкого баса/резонанса, и их
    // узкие (но не бесконечно узкие) вырезы по -8dB каждый накладывались друг
    // на друга, суммируясь в один куда более глубокий и широкий common-провал
    // вместо двух отдельных аккуратных вырезов.
    std::vector<float> picked;
    for (auto& c : candidates)
    {
        bool tooClose = false;
        for (float p : picked)
        {
            if (std::abs (std::log (c.freq / p)) < std::log (1.25f))
            {
                tooClose = true;
                break;
            }
        }

        if (! tooClose)
            picked.push_back (c.freq);

        if ((int) picked.size() >= maxResonancePeaks)
            break;
    }

    for (int i = 0; i < maxResonancePeaks; ++i)
        detectedResonanceFreqHz[(size_t) i].store (i < (int) picked.size() ? picked[(size_t) i] : 0.0f);
    detectedResonanceCount.store ((int) picked.size());
}

namespace
{
    // Гранула обновления dynamics-детектора и коэффициентов биквадов в
    // zero-latency режиме. Если считать динамическую поправку и сразу менять
    // коэффициенты фильтра раз на весь буфер (256-1024+ сэмплов), резкий скачок
    // огибающей на транзиенте даёт мгновенную подмену коэффициентов и слышимый
    // "треск" на границе блока. Пересчитывая маленькими кусками, тот же скачок
    // растягивается на несколько мелких шагов и перестаёт щёлкать.
    constexpr int dynSubBlockSize = 64;
}

void EQAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    auto totalNumSamples = buffer.getNumSamples();

    // Резонансы ищем по ИСХОДНОМУ (до-EQ) сигналу, пока band'ы ещё не успели
    // его обработать — иначе как только Auto-полоса начинает резать резонанс,
    // он бы пропал из анализа и детектор потерял бы собственную цель. Тот же
    // сырой сигнал заодно кормит отдельный pre-EQ пайплайн для одновременного
    // pre/post отображения в спектр-анализаторе.
    {
        auto* dryReadPtr = buffer.getReadPointer (0);
        for (int n = 0; n < totalNumSamples; ++n)
        {
            pushNextDrySampleIntoResonanceFifo (dryReadPtr[n]);
            pushNextPreSampleIntoFifo (dryReadPtr[n]);
        }
    }

    // Полосы с включённым Auto разбирают найденные резонансы по порядку
    // выраженности: первая (по индексу 0..7) Auto-полоса получает самый
    // заметный резонанс, вторая — следующий, и т.д.
    int autoSlotForBand[numBands];
    {
        int slot = 0;
        for (int i = 0; i < numBands; ++i)
        {
            BandParamIDs id (i);
            bool autoOn = apvts.getRawParameterValue (id.autoResonance)->load() > 0.5f;
            autoSlotForBand[i] = autoOn ? slot++ : -1;
        }
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
        // Свёртка обрабатывает буфер целиком за один проход — зиппер-шумов от
        // покусочной смены коэффициентов тут нет, поэтому dynamics меряем один
        // раз на весь блок, как раньше.
        dryBuffer.setSize (2, totalNumSamples, false, false, true);
        for (int ch = 0; ch < buffer.getNumChannels() && ch < dryBuffer.getNumChannels(); ++ch)
            dryBuffer.copyFrom (ch, 0, buffer, ch, 0, totalNumSamples);
        const juce::AudioBuffer<float>& constDryBuffer = dryBuffer;
        juce::dsp::AudioBlock<const float> dryBlock (constDryBuffer);

        LinearPhaseEQ::BandSnapshot snapshot[numBands];
        float totalDynOffsetDb = 0.0f;

        for (int i = 0; i < numBands; ++i)
        {
            BandParamIDs id (i);
            auto type = (int) apvts.getRawParameterValue (id.type)->load();
            auto en   = apvts.getRawParameterValue (id.enabled)->load() > 0.5f;
            auto slopeDbPerOct = apvts.getRawParameterValue (id.slope)->load();
            float rawOrder = juce::jlimit (1.0f, 8.0f, slopeDbPerOct / 6.0f);

            auto dynEnabledParam   = apvts.getRawParameterValue (id.dynEnabled)->load() > 0.5f;
            auto dynThresholdParam = apvts.getRawParameterValue (id.dynThreshold)->load();
            auto dynRangeParam     = apvts.getRawParameterValue (id.dynRange)->load();

            // Auto: полоса сама настраивается на найденный резонанс (фиксированное
            // узкое Q, гашение через Dynamic EQ с внутренними fixed threshold/range
            // — см. константы autoResonance* в PluginProcessor.h). Если резонанс
            // ещё не найден (слот пуст), не трогаем freq-таргет вообще — держим
            // последнее известное значение вместо скачка на 0Hz.
            bool autoOn = apvts.getRawParameterValue (id.autoResonance)->load() > 0.5f;
            int autoSlot = autoSlotForBand[i];
            bool autoHasTarget = autoOn && autoSlot >= 0 && autoSlot < detectedResonanceCount.load()
                                 && detectedResonanceFreqHz[(size_t) autoSlot].load() > 0.0f;

            float freqRawTarget = autoHasTarget ? detectedResonanceFreqHz[(size_t) autoSlot].load()
                                                 : apvts.getRawParameterValue (id.freq)->load();
            float qRawTarget = autoOn ? autoResonanceQ : apvts.getRawParameterValue (id.q)->load();

            // Те же сглаженные freq/gain/q/order, что и в ZeroLatency-ветке (см.
            // paramSmoothingSeconds в PluginProcessor.h). Без этого dynamics-детектор
            // здесь ретюнится на новую частоту/Q мгновенно и без сглаживания, а его
            // огибающая теперь идёт в общий постфактум-гейн на весь микс (см. ниже),
            // так что даже короткий transient от ретюна детектора слышен на всей
            // фонограмме, а не только в узкой полосе band'а.
            auto& freqSm  = freqSmoothers[(size_t) i];
            auto& gainSm  = gainSmoothers[(size_t) i];
            auto& qSm     = qSmoothers[(size_t) i];
            auto& orderSm = orderSmoothers[(size_t) i];

            if (autoHasTarget || ! autoOn)
                freqSm.setTargetValue (freqRawTarget);
            gainSm.setTargetValue  (apvts.getRawParameterValue (id.gain)->load());
            qSm.setTargetValue     (qRawTarget);
            orderSm.setTargetValue (rawOrder);

            float freq  = freqSm.skip (totalNumSamples);
            float gain  = gainSm.skip (totalNumSamples);
            float q     = qSm.skip (totalNumSamples);
            float order = orderSm.skip (totalNumSamples);

            currentAutoTrackedFreq[(size_t) i].store (autoOn ? freq : 0.0f);

            // Детектор dynamics намеренно следует за freq/q гораздо медленнее
            // основного фильтра (см. detectorSmoothingSeconds) — иначе при
            // перетаскивании точки в графике он мгновенно ретюнится вслед за
            // курсором и "подхватывает" по пути громкие соседние частоты.
            auto& detectorFreqSm = detectorFreqSmoothers[(size_t) i];
            auto& detectorQSm    = detectorQSmoothers[(size_t) i];
            if (autoHasTarget || ! autoOn)
                detectorFreqSm.setTargetValue (freqRawTarget);
            detectorQSm.setTargetValue (qRawTarget);
            float detectorFreq = detectorFreqSm.skip (totalNumSamples);
            float detectorQ    = detectorQSm.skip (totalNumSamples);

            bool dynEnabled     = autoOn ? true : dynEnabledParam;
            float dynThreshold  = autoOn ? autoResonanceThresholdDb : dynThresholdParam;
            float dynRange      = autoOn ? autoResonanceRangeDb : dynRangeParam;

            FilterType ft = filterTypeFromIndex (type);

            auto& band = bandsLeftRight[(size_t) i];
            band.enabled = en;
            band.setDynamicsParameters (dynEnabled, dynThreshold, dynRange);
            band.setParameters (ft, freq, gain, q, order);

            float dynOffset = band.computeDynamicsGainOffsetDb (dryBlock, currentSampleRate, detectorFreq, detectorQ);
            currentDynGainOffsetDb[(size_t) i].store (dynOffset);

            if (en)
                totalDynOffsetDb += dynOffset;

            // Kernel строится по статическому gain (без dynOffset) — см. комментарий
            // у linearPhaseDynGainSmoother в PluginProcessor.h. Иначе dirty-флаг в
            // LinearPhaseEQ триггерится почти на каждый callback, пока dynamics
            // активен, и FFT-kernel пересобирается непрерывно.
            snapshot[i] = { ft, freq, gain, q, en, order };
        }

        linearPhaseEQ.setQuality ((int) apvts.getRawParameterValue ("linphase_quality")->load());
        std::array<LinearPhaseEQ::BandSnapshot, numBands> arr;
        for (int i = 0; i < numBands; ++i) arr[(size_t) i] = snapshot[i];
        linearPhaseEQ.updateBandSnapshot (arr);
        linearPhaseEQ.process (block);

        // Быстрая dynamics-модуляция накладывается постфактум, а не запекается
        // в kernel: суммарная поправка со всех активных полос -> смуженный
        // scalar-гейн на весь буфер.
        linearPhaseDynGainSmoother.setTargetValue (juce::Decibels::decibelsToGain (totalDynOffsetDb));
        linearPhaseDynGainSmoother.applyGain (buffer, totalNumSamples);
    }
    else
    {
        for (int start = 0; start < totalNumSamples; start += dynSubBlockSize)
        {
            int len = juce::jmin (dynSubBlockSize, totalNumSamples - start);

            // dryBuffer подготовлен на dynSubBlockSize в prepareToPlay, так что
            // setSize здесь только уменьшает заявленный размер и не аллоцирует
            // память в аудио-потоке.
            dryBuffer.setSize (2, len, false, false, true);
            for (int ch = 0; ch < buffer.getNumChannels() && ch < dryBuffer.getNumChannels(); ++ch)
                dryBuffer.copyFrom (ch, 0, buffer, ch, start, len);
            const juce::AudioBuffer<float>& constDryBuffer = dryBuffer;
            juce::dsp::AudioBlock<const float> dryBlock (constDryBuffer);

            for (int i = 0; i < numBands; ++i)
            {
                BandParamIDs id (i);
                auto type = (int) apvts.getRawParameterValue (id.type)->load();
                auto en   = apvts.getRawParameterValue (id.enabled)->load() > 0.5f;
                auto slopeDbPerOct = apvts.getRawParameterValue (id.slope)->load();
                float rawOrder = juce::jlimit (1.0f, 8.0f, slopeDbPerOct / 6.0f);

                auto dynEnabledParam   = apvts.getRawParameterValue (id.dynEnabled)->load() > 0.5f;
                auto dynThresholdParam = apvts.getRawParameterValue (id.dynThreshold)->load();
                auto dynRangeParam     = apvts.getRawParameterValue (id.dynRange)->load();

                // Auto: см. подробный комментарий в Linear Phase-ветке выше — тот же
                // механизм (найденный резонанс -> freq/Q фиксируются, гашение через
                // Dynamic EQ с внутренними fixed threshold/range).
                bool autoOn = apvts.getRawParameterValue (id.autoResonance)->load() > 0.5f;
                int autoSlot = autoSlotForBand[i];
                bool autoHasTarget = autoOn && autoSlot >= 0 && autoSlot < detectedResonanceCount.load()
                                     && detectedResonanceFreqHz[(size_t) autoSlot].load() > 0.0f;

                float freqRawTarget = autoHasTarget ? detectedResonanceFreqHz[(size_t) autoSlot].load()
                                                     : apvts.getRawParameterValue (id.freq)->load();
                float qRawTarget = autoOn ? autoResonanceQ : apvts.getRawParameterValue (id.q)->load();

                // Раздвигаем скачок ручного изменения ручки/автоматизации на
                // paramSmoothingSeconds, а не применяем его как мгновенную подмену
                // коэффициентов между суб-блоками — см. комментарий в PluginProcessor.h.
                auto& freqSm  = freqSmoothers[(size_t) i];
                auto& gainSm  = gainSmoothers[(size_t) i];
                auto& qSm     = qSmoothers[(size_t) i];
                auto& orderSm = orderSmoothers[(size_t) i];

                if (autoHasTarget || ! autoOn)
                    freqSm.setTargetValue (freqRawTarget);
                gainSm.setTargetValue  (apvts.getRawParameterValue (id.gain)->load());
                qSm.setTargetValue     (qRawTarget);
                orderSm.setTargetValue (rawOrder);

                float freq  = freqSm.skip (len);
                float gain  = gainSm.skip (len);
                float q     = qSm.skip (len);
                float order = orderSm.skip (len);

                currentAutoTrackedFreq[(size_t) i].store (autoOn ? freq : 0.0f);

                // Детектор dynamics следует за freq/q медленнее основного фильтра
                // (см. detectorSmoothingSeconds) — тот же resон, что и в Linear
                // Phase-ветке, хоть здесь эффект и локален для полосы.
                auto& detectorFreqSm = detectorFreqSmoothers[(size_t) i];
                auto& detectorQSm    = detectorQSmoothers[(size_t) i];
                if (autoHasTarget || ! autoOn)
                    detectorFreqSm.setTargetValue (freqRawTarget);
                detectorQSm.setTargetValue (qRawTarget);
                float detectorFreq = detectorFreqSm.skip (len);
                float detectorQ    = detectorQSm.skip (len);

                bool dynEnabled    = autoOn ? true : dynEnabledParam;
                float dynThreshold = autoOn ? autoResonanceThresholdDb : dynThresholdParam;
                float dynRange     = autoOn ? autoResonanceRangeDb : dynRangeParam;

                FilterType ft = filterTypeFromIndex (type);

                auto& band = bandsLeftRight[(size_t) i];
                band.enabled = en;
                band.setDynamicsParameters (dynEnabled, dynThreshold, dynRange);
                band.setParameters (ft, freq, gain, q, order);

                float dynOffset = band.computeDynamicsGainOffsetDb (dryBlock, currentSampleRate, detectorFreq, detectorQ);
                currentDynGainOffsetDb[(size_t) i].store (dynOffset);

                band.setParameters (ft, freq, gain + dynOffset, q, order);
            }

            auto subBlock = block.getSubBlock ((size_t) start, (size_t) len);
            for (auto& band : bandsLeftRight)
                band.process (subBlock);
        }
    }

    // Питаем спектр-анализатор сэмплами после обработки (пост-EQ отклик)
    auto* readPtr = buffer.getReadPointer (0);
    for (int n = 0; n < totalNumSamples; ++n)
        pushNextSampleIntoFifo (readPtr[n]);
}

EQAudioProcessor::PhaseMode EQAudioProcessor::getPhaseMode() const
{
    return (PhaseMode) (int) apvts.getRawParameterValue ("phase_mode")->load();
}

int EQAudioProcessor::getReportedLatencySamples() const
{
    if (getPhaseMode() != PhaseMode::Linear)
        return 0;

    static constexpr int sizes[4] = { 1024, 2048, 4096, 8192 };
    int qualityIndex = (int) apvts.getRawParameterValue ("linphase_quality")->load();
    return sizes[juce::jlimit (0, 3, qualityIndex)] / 2;
}

void EQAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void EQAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* EQAudioProcessor::createEditor()
{
    return new EQAudioProcessorEditor (*this);
}

// Точка входа для JUCE plugin wrappers (VST3/AU/Standalone)
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new EQAudioProcessor();
}
