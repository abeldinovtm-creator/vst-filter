#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include <cmath>
#include <vector>

/** Рисует бегущую спектрограмму (FFT) как полупрозрачную заливку на фоне EQ-графика.
    По умолчанию показывает только post-EQ (выход). Через setShowPrePost(true)
    дополнительно накладывается pre-EQ (вход) — тонкой приглушённой линией. */
class SpectrumAnalyzer : public juce::Component, private juce::Timer
{
public:
    explicit SpectrumAnalyzer (EQAudioProcessor& p) : processor (p)
    {
        startTimerHz (60);
    }

    void setShowPrePost (bool shouldShow) { showPrePost = shouldShow; }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();

        if (showPrePost && ! prePath.isEmpty())
        {
            g.setColour (juce::Colours::lightgrey.withAlpha (0.35f));
            g.strokePath (prePath, juce::PathStrokeType (1.0f));
        }

        if (postPath.isEmpty())
            return;

        g.setColour (juce::Colours::cyan.withAlpha (0.15f));
        auto fillPath = postPath;
        fillPath.lineTo (bounds.getBottomRight());
        fillPath.lineTo (bounds.getBottomLeft());
        fillPath.closeSubPath();
        g.fillPath (fillPath);

        g.setColour (juce::Colours::cyan.withAlpha (0.5f));
        g.strokePath (postPath, juce::PathStrokeType (1.2f));
    }

private:
    void timerCallback() override
    {
        auto bounds = getLocalBounds().toFloat();
        int width = juce::jmax (1, (int) bounds.getWidth());

        updateChannel (processor.nextFFTBlockReady, processor.fftData, processor.fftLock,
                       postSmoothedDb, postPath, bounds, width);

        // pre-канал считаем только когда включён тумблер — незачем тратить
        // CPU на путь, который всё равно не рисуется.
        if (showPrePost)
            updateChannel (processor.nextPreFFTBlockReady, processor.preFftData, processor.preFftLock,
                           preSmoothedDb, prePath, bounds, width);

        repaint();
    }

    // Общий пайплайн (окно+FFT+интерполяция+attack/release+путь), одинаковый
    // для pre- и post-канала — отличаются только тем, чьи буферы/флаги/состояние
    // сглаживания передаются.
    void updateChannel (std::atomic<bool>& readyFlag, float* rawFftData, juce::CriticalSection& lock,
                       std::vector<float>& smoothedDbState, juce::Path& outPath,
                       juce::Rectangle<float> bounds, int width)
    {
        if ((int) smoothedDbState.size() != width)
            smoothedDbState.assign ((size_t) width, -100.0f);

        std::vector<float> targetDb ((size_t) width, -100.0f);

        if (readyFlag.load())
        {
            {
                const juce::ScopedLock lock2 (lock);
                processor.window.multiplyWithWindowingTable (rawFftData, (size_t) EQAudioProcessor::fftSize);
                processor.fft.performFrequencyOnlyForwardTransform (rawFftData);
                readyFlag.store (false);
            }

            const auto sr = processor.currentSampleRate;
            const int numBins = EQAudioProcessor::fftSize / 2;

            // Бины FFT расположены ЛИНЕЙНО по частоте (шаг ~sr/fftSize ≈ 21.5Hz
            // при fftSize=2048/44.1кГц). Берём СЫРУЮ магнитуду каждого бина без
            // усреднения по частотному окну — оно съедает настоящую зернистость
            // сигнала (особенно на ВЧ). Отсутствие "дыр" между пикселями на НЧ
            // (где на бин приходится много пикселей) обеспечивает не усреднение,
            // а линейная интерполяция ниже.
            std::vector<float> binDb ((size_t) numBins, -100.0f);
            for (int i = 1; i < numBins; ++i)
            {
                float magnitude = rawFftData[i];

                // performFrequencyOnlyForwardTransform() возвращает НЕнормализованную
                // магнитуду (масштаб ~fftSize/2 для синусоиды на пике бина). Плюс
                // Hann-окно имеет когерентное усиление 0.5 — при его применении пик
                // магнитуды падает ещё вдвое (в 4 раза по мощности). Итоговый
                // опорный уровень для "0dBFS" — fftSize/4, а не fftSize.
                constexpr float hannCoherentGainCorrection = 4.0f;
                binDb[(size_t) i] = juce::Decibels::gainToDecibels (magnitude, -100.0f)
                                   - juce::Decibels::gainToDecibels ((float) EQAudioProcessor::fftSize
                                                                     / hannCoherentGainCorrection);
            }

            // Линейная интерполяция между двумя соседними бинами по дробному
            // индексу — без неё, при округлении индекса до целого, на низких
            // частотах (где один бин приходится на много пикселей) получалась
            // "лестница"/квадратные ступени вместо гладкой кривой.
            for (int px = 0; px < width; ++px)
            {
                float t = (float) px / (float) juce::jmax (1, width - 1);
                float freq = 20.0f * std::pow (20000.0f / 20.0f, t);

                float binIndexF = juce::jlimit (1.0f, (float) (numBins - 1),
                                                freq * (float) EQAudioProcessor::fftSize / (float) sr);
                int binLo = (int) std::floor (binIndexF);
                int binHi = juce::jmin (numBins - 1, binLo + 1);
                float frac = binIndexF - (float) binLo;

                targetDb[(size_t) px] = binDb[(size_t) binLo] * (1.0f - frac)
                                       + binDb[(size_t) binHi] * frac;
            }
        }
        // Если нового FFT-блока нет (тишина/стоп), targetDb остаётся на -100dB —
        // огибающая ниже плавно спадает к тишине, а не замирает на последнем кадре.

        // Attack/release сглаживание: быстро откликается на рост, плавно спадает.
        constexpr float attackMs = 40.0f, releaseMs = 350.0f;
        constexpr float tickMs = 1000.0f / 60.0f;
        float attackCoeff  = std::exp (-tickMs / attackMs);
        float releaseCoeff = std::exp (-tickMs / releaseMs);

        for (int px = 0; px < width; ++px)
        {
            float target = targetDb[(size_t) px];
            float& cur = smoothedDbState[(size_t) px];
            float coeff = (target > cur) ? attackCoeff : releaseCoeff;
            cur = coeff * cur + (1.0f - coeff) * target;
        }

        outPath.clear();
        bool started = false;
        for (int px = 0; px < width; ++px)
        {
            float x = bounds.getX() + (float) px;
            float y = juce::jmap (smoothedDbState[(size_t) px], -100.0f, 0.0f,
                                  bounds.getBottom(), bounds.getY() + bounds.getHeight() * 0.15f);
            y = juce::jlimit (bounds.getY(), bounds.getBottom(), y);

            if (! started) { outPath.startNewSubPath (x, y); started = true; }
            else            outPath.lineTo (x, y);
        }
    }

    EQAudioProcessor& processor;
    bool showPrePost = false;

    juce::Path postPath;
    std::vector<float> postSmoothedDb;

    juce::Path prePath;
    std::vector<float> preSmoothedDb;
};
