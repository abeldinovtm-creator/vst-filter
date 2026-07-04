#include "FilterBand.h"

void FilterBand::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;

    for (auto& f : stagesL) f.prepare (spec);
    for (auto& f : stagesR) f.prepare (spec);
    for (auto& f : stagesL2) f.prepare (spec);
    for (auto& f : stagesR2) f.prepare (spec);

    scratchBuffer.setSize ((int) spec.numChannels, (int) spec.maximumBlockSize);

    detector.prepare (spec);
    detectorScratch.realloc ((size_t) spec.maximumBlockSize);
    detectorScratchSize = (int) spec.maximumBlockSize;

    updateCoefficients();
}

void FilterBand::reset()
{
    for (auto& f : stagesL) f.reset();
    for (auto& f : stagesR) f.reset();
    for (auto& f : stagesL2) f.reset();
    for (auto& f : stagesR2) f.reset();

    detector.reset();
    envelopeDb = -100.0f;
}

void FilterBand::setParameters (FilterType type, float frequencyHz, float gainDb, float qVal, float slopeOrder)
{
    currentType = type;
    freq = juce::jlimit (10.0f, 22000.0f, frequencyHz);
    gain = gainDb;
    q = juce::jlimit (0.05f, 18.0f, qVal);
    order = juce::jlimit (1.0f, 8.0f, slopeOrder);
    updateCoefficients();
}

int FilterBand::designCutStages (FilterType type, float freq, double sampleRate, int order,
                                  std::array<juce::dsp::IIR::Coefficients<float>::Ptr, maxStages>& dest)
{
    auto qVals = (type == FilterType::LowCut)
        ? juce::dsp::FilterDesign<float>::designIIRHighpassHighOrderButterworthMethod (freq, sampleRate, order)
        : juce::dsp::FilterDesign<float>::designIIRLowpassHighOrderButterworthMethod (freq, sampleRate, order);

    int n = juce::jmin ((int) qVals.size(), (int) dest.size());
    for (int i = 0; i < n; ++i)
        dest[(size_t) i] = qVals[(size_t) i];
    return n;
}

void FilterBand::updateCoefficients()
{
    using Coeffs = juce::dsp::IIR::Coefficients<float>;

    activeStages = 1;
    activeStages2 = 0;
    slopeBlend = 0.0f;

    switch (currentType)
    {
        case FilterType::Bell:
            coeffs[0] = Coeffs::makePeakFilter (sampleRate, freq, q, juce::Decibels::decibelsToGain (gain));
            break;

        case FilterType::LowShelf:
            coeffs[0] = Coeffs::makeLowShelf (sampleRate, freq, q, juce::Decibels::decibelsToGain (gain));
            break;

        case FilterType::HighShelf:
            coeffs[0] = Coeffs::makeHighShelf (sampleRate, freq, q, juce::Decibels::decibelsToGain (gain));
            break;

        case FilterType::Notch:
            coeffs[0] = Coeffs::makeNotch (sampleRate, freq, q);
            break;

        case FilterType::LowCut:
        case FilterType::HighCut:
        {
            // Основной каскад: order = floor(order). Если order нецелый, добавляем
            // второй каскад с order = ceil(order) и кроссфейдим их по дробной части,
            // чтобы крутизна ската менялась плавно, а не ступенями по 12 dB/oct.
            int orderLow  = juce::jmax (1, (int) std::floor (order));
            int orderHigh = juce::jmax (1, (int) std::ceil  (order));

            activeStages = designCutStages (currentType, freq, sampleRate, orderLow, coeffs);

            if (orderHigh != orderLow)
            {
                activeStages2 = designCutStages (currentType, freq, sampleRate, orderHigh, coeffs2);
                slopeBlend = order - (float) orderLow;
            }
            break;
        }
    }

    if (currentType != FilterType::LowCut && currentType != FilterType::HighCut)
    {
        stagesL[0].coefficients = coeffs[0];
        stagesR[0].coefficients = coeffs[0];
    }
    else
    {
        for (int i = 0; i < activeStages; ++i)
        {
            stagesL[(size_t) i].coefficients = coeffs[(size_t) i];
            stagesR[(size_t) i].coefficients = coeffs[(size_t) i];
        }
        for (int i = 0; i < activeStages2; ++i)
        {
            stagesL2[(size_t) i].coefficients = coeffs2[(size_t) i];
            stagesR2[(size_t) i].coefficients = coeffs2[(size_t) i];
        }
    }
}

void FilterBand::process (juce::dsp::AudioBlock<float>& block)
{
    if (! enabled)
        return;

    auto numCh = block.getNumChannels();

    if (activeStages2 > 0 && slopeBlend > 0.0f)
    {
        auto numSamples = block.getNumSamples();
        juce::dsp::AudioBlock<float> scratch (scratchBuffer);
        auto scratchBlock = scratch.getSubBlock (0, numSamples);
        scratchBlock.copyFrom (block);

        for (int stage = 0; stage < activeStages; ++stage)
        {
            if (numCh > 0)
            {
                auto chBlock = block.getSingleChannelBlock (0);
                juce::dsp::ProcessContextReplacing<float> ctx (chBlock);
                stagesL[(size_t) stage].process (ctx);
            }
            if (numCh > 1)
            {
                auto chBlock = block.getSingleChannelBlock (1);
                juce::dsp::ProcessContextReplacing<float> ctx (chBlock);
                stagesR[(size_t) stage].process (ctx);
            }
        }

        for (int stage = 0; stage < activeStages2; ++stage)
        {
            if (numCh > 0)
            {
                auto chBlock = scratchBlock.getSingleChannelBlock (0);
                juce::dsp::ProcessContextReplacing<float> ctx (chBlock);
                stagesL2[(size_t) stage].process (ctx);
            }
            if (numCh > 1)
            {
                auto chBlock = scratchBlock.getSingleChannelBlock (1);
                juce::dsp::ProcessContextReplacing<float> ctx (chBlock);
                stagesR2[(size_t) stage].process (ctx);
            }
        }

        block.multiplyBy (1.0f - slopeBlend);
        block.addProductOf (scratchBlock, slopeBlend);
        return;
    }

    for (int stage = 0; stage < activeStages; ++stage)
    {
        if (numCh > 0)
        {
            auto chBlock = block.getSingleChannelBlock (0);
            juce::dsp::ProcessContextReplacing<float> ctx (chBlock);
            stagesL[(size_t) stage].process (ctx);
        }
        if (numCh > 1)
        {
            auto chBlock = block.getSingleChannelBlock (1);
            juce::dsp::ProcessContextReplacing<float> ctx (chBlock);
            stagesR[(size_t) stage].process (ctx);
        }
    }
}

void FilterBand::setDynamicsParameters (bool dynamicsEnabled, float thresholdDb, float rangeDb)
{
    dynEnabled = dynamicsEnabled;
    dynThreshold = thresholdDb;
    dynRange = rangeDb;
}

float FilterBand::computeDynamicsGainOffsetDb (const juce::dsp::AudioBlock<const float>& dryBlock, double sr)
{
    if (! dynEnabled || dynRange == 0.0f)
        return 0.0f;

    auto numSamples = (int) dryBlock.getNumSamples();
    auto numChannels = (int) dryBlock.getNumChannels();

    if (numSamples <= 0 || numChannels <= 0)
        return 0.0f;

    if (detectorScratchSize < numSamples)
    {
        detectorScratch.realloc ((size_t) numSamples);
        detectorScratchSize = numSamples;
    }

    for (int n = 0; n < numSamples; ++n)
    {
        float sum = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            sum += dryBlock.getSample (ch, n);
        detectorScratch[n] = sum / (float) numChannels;
    }

    detector.coefficients = juce::dsp::IIR::Coefficients<float>::makeBandPass (sr, freq, juce::jmax (q, 0.3f));

    double sumOfSquares = 0.0;
    for (int n = 0; n < numSamples; ++n)
    {
        float y = detector.processSample (detectorScratch[n]);
        sumOfSquares += (double) y * (double) y;
    }

    float rms = std::sqrt ((float) (sumOfSquares / (double) numSamples));
    float levelDb = juce::Decibels::gainToDecibels (rms, -100.0f);

    double blockMs = 1000.0 * (double) numSamples / sr;
    bool attacking = levelDb > envelopeDb;
    float coeff = (float) std::exp (-blockMs / (attacking ? 15.0 : 150.0));

    envelopeDb = coeff * envelopeDb + (1.0f - coeff) * levelDb;

    float excess = envelopeDb - dynThreshold;
    float absRange = std::abs (dynRange);
    float clamped = juce::jlimit (0.0f, absRange, excess);
    return clamped * (dynRange < 0.0f ? -1.0f : 1.0f);
}

float FilterBand::getMagnitudeForFrequency (double frequencyHz, double sr) const
{
    double mag = 1.0;
    for (int i = 0; i < activeStages; ++i)
        if (coeffs[(size_t) i] != nullptr)
            mag *= coeffs[(size_t) i]->getMagnitudeForFrequency (frequencyHz, sr);

    if (activeStages2 > 0 && slopeBlend > 0.0f)
    {
        double mag2 = 1.0;
        for (int i = 0; i < activeStages2; ++i)
            if (coeffs2[(size_t) i] != nullptr)
                mag2 *= coeffs2[(size_t) i]->getMagnitudeForFrequency (frequencyHz, sr);

        mag = (1.0 - (double) slopeBlend) * mag + (double) slopeBlend * mag2;
    }

    return (float) mag;
}
