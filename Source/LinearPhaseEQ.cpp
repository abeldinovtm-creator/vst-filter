#include "LinearPhaseEQ.h"

void LinearPhaseEQ::setQuality (int qualityIndex)
{
    static constexpr int sizes[4] = { 1024, 2048, 4096, 8192 };
    pendingFftSize = sizes[juce::jlimit (0, 3, qualityIndex)];
    if (pendingFftSize != currentFftSize)
    {
        dirty.store (true);
        lastSnapshotChangeMs.store (juce::Time::getMillisecondCounterHiRes());
    }
}

void LinearPhaseEQ::prepare (double sr, int maximumBlockSize)
{
    sampleRate = sr;
    currentFftSize = pendingFftSize;

    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maximumBlockSize, 2 };
    convolution.prepare (spec);

    dirty.store (true);
}

void LinearPhaseEQ::reset()
{
    convolution.reset();
}

void LinearPhaseEQ::process (juce::dsp::AudioBlock<float>& block)
{
    juce::dsp::ProcessContextReplacing<float> ctx (block);
    convolution.process (ctx);
}

void LinearPhaseEQ::updateBandSnapshot (const std::array<BandSnapshot, numBands>& newSnapshot)
{
    bool changed = false;

    {
        const juce::ScopedLock lock (snapshotLock);
        for (int i = 0; i < numBands; ++i)
        {
            const auto& a = snapshot[(size_t) i];
            const auto& b = newSnapshot[(size_t) i];
            if (a.type != b.type || a.enabled != b.enabled
                || std::abs (a.freq - b.freq) > 0.5f
                || std::abs (a.gain - b.gain) > 0.05f
                || std::abs (a.q - b.q) > 0.01f
                || std::abs (a.order - b.order) > 0.01f)
            {
                changed = true;
            }
        }
        snapshot = newSnapshot;
    }

    if (changed)
    {
        dirty.store (true);
        lastSnapshotChangeMs.store (juce::Time::getMillisecondCounterHiRes());
    }
}

void LinearPhaseEQ::rebuildKernelIfDirty()
{
    if (! dirty.load())
        return;

    // Пока изменения продолжают поступать чаще, чем раз в kernelSettleMs, не
    // трогаем текущий (стабильный) kernel вообще — см. комментарий у
    // lastSnapshotChangeMs в LinearPhaseEQ.h.
    double sinceChangeMs = juce::Time::getMillisecondCounterHiRes() - lastSnapshotChangeMs.load();
    if (sinceChangeMs < kernelSettleMs)
        return;

    currentFftSize = pendingFftSize;
    rebuildKernel();
    dirty.store (false);
}

void LinearPhaseEQ::rebuildKernel()
{
    const int N = currentFftSize;
    const int order = (int) std::log2 (N);
    juce::dsp::FFT fft (order);

    std::vector<std::complex<float>> spectrum ((size_t) N, { 0.0f, 0.0f });
    const int half = N / 2;

    juce::Array<BandSnapshot> snap;
    {
        const juce::ScopedLock lock (snapshotLock);
        for (auto& s : snapshot)
            snap.add (s);
    }

    auto magnitudeAt = [&] (double freq) -> double
    {
        double mag = 1.0;
        for (auto& s : snap)
        {
            if (! s.enabled)
                continue;

            // Пересчитываем магнитуду биквада напрямую по параметрам снэпшота,
            // чтобы не трогать живые FilterBand (они принадлежат audio thread).
            FilterBand temp;
            temp.setParameters (s.type, s.freq, s.gain, s.q, s.order);
            mag *= temp.getMagnitudeForFrequency (freq, sampleRate);
        }
        return mag;
    };

    for (int k = 0; k <= half; ++k)
    {
        double freq = (double) k * sampleRate / (double) N;
        float mag = (float) magnitudeAt (freq);
        spectrum[(size_t) k] = { mag, 0.0f };
        if (k > 0 && k < half)
            spectrum[(size_t) (N - k)] = { mag, 0.0f }; // сопряжённо-симметрично, фаза везде 0
    }

    std::vector<std::complex<float>> timeDomain ((size_t) N);
    fft.perform (spectrum.data(), timeDomain.data(), true); // обратное FFT

    juce::AudioBuffer<float> ir (1, N);
    auto* w = ir.getWritePointer (0);

    for (int n = 0; n < N; ++n)
    {
        int srcIndex = (n + half) % N; // циклический сдвиг: центрируем и делаем причинным
        w[n] = timeDomain[(size_t) srcIndex].real(); // JUCE FFT::perform(inverse=true) уже делит на N
    }

    juce::dsp::WindowingFunction<float> window ((size_t) N, juce::dsp::WindowingFunction<float>::blackmanHarris);
    window.multiplyWithWindowingTable (w, (size_t) N);

    convolution.loadImpulseResponse (std::move (ir), sampleRate,
                                      juce::dsp::Convolution::Stereo::no,
                                      juce::dsp::Convolution::Trim::no,
                                      juce::dsp::Convolution::Normalise::no);
}

void LinearPhaseEQ::startBackgroundThread()
{
    builderThread.startThread (juce::Thread::Priority::low);
}

void LinearPhaseEQ::stopBackgroundThread()
{
    builderThread.stopThread (2000);
}
