#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include <vector>

/** Рисует бегущую спектрограмму (FFT) как полупрозрачную заливку на фоне EQ-графика. */
class SpectrumAnalyzer : public juce::Component, private juce::Timer
{
public:
    explicit SpectrumAnalyzer (EQAudioProcessor& p) : processor (p)
    {
        startTimerHz (60);
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        if (path.isEmpty())
            return;

        g.setColour (juce::Colours::cyan.withAlpha (0.15f));
        auto fillPath = path;
        fillPath.lineTo (bounds.getBottomRight());
        fillPath.lineTo (bounds.getBottomLeft());
        fillPath.closeSubPath();
        g.fillPath (fillPath);

        g.setColour (juce::Colours::cyan.withAlpha (0.5f));
        g.strokePath (path, juce::PathStrokeType (1.2f));
    }

private:
    void timerCallback() override
    {
        auto bounds = getLocalBounds().toFloat();
        int width = juce::jmax (1, (int) bounds.getWidth());

        if ((int) smoothedDb.size() != width)
            smoothedDb.assign ((size_t) width, -100.0f);

        std::vector<float> targetDb ((size_t) width, -100.0f);

        if (processor.nextFFTBlockReady.load())
        {
            {
                const juce::ScopedLock lock (processor.fftLock);
                processor.window.multiplyWithWindowingTable (processor.fftData, (size_t) EQAudioProcessor::fftSize);
                processor.fft.performFrequencyOnlyForwardTransform (processor.fftData);
                processor.nextFFTBlockReady.store (false);
            }

            const auto sr = processor.currentSampleRate;
            const int numBins = EQAudioProcessor::fftSize / 2;

            // Один столбец пикселей = максимум по всем бинам, которые в него
            // попадают. На ВЧ лог-шкала схлопывает много соседних (линейно
            // расположенных) бинов в считанные пиксели — без агрегации кривая
            // рисуется зубчатым "шумом" вместо гладкой линии, потому что каждый
            // бин добавляет свою вершину почти в ту же самую точку по X.
            for (int i = 1; i < numBins; ++i)
            {
                float freq = (float) (i * sr / EQAudioProcessor::fftSize);
                if (freq < 20.0f || freq > 20000.0f)
                    continue;

                float xf = bounds.getWidth() * (std::log (freq / 20.0f) / std::log (20000.0f / 20.0f));
                int px = juce::jlimit (0, width - 1, (int) xf);

                // performFrequencyOnlyForwardTransform() возвращает НЕнормализованную
                // магнитуду (масштаб ~fftSize) — без вычитания gainToDecibels(fftSize)
                // почти любой сигнал даёт дБ на ~60+ выше расчётного диапазона, и
                // кривая упирается в потолок независимо от реальной громкости.
                float magnitude = processor.fftData[i];
                float db = juce::Decibels::gainToDecibels (magnitude, -100.0f)
                           - juce::Decibels::gainToDecibels ((float) EQAudioProcessor::fftSize);

                targetDb[(size_t) px] = juce::jmax (targetDb[(size_t) px], db);
            }
        }
        // Если нового FFT-блока нет (тишина/стоп), targetDb остаётся на -100dB —
        // огибающая ниже плавно спадает к тишине, а не замирает на последнем кадре.

        // Attack/release сглаживание: быстро откликается на рост, плавно
        // спадает — визуально даёт привычное поведение анализатора спектра.
        constexpr float attackMs = 30.0f, releaseMs = 300.0f;
        constexpr float tickMs = 1000.0f / 60.0f;
        float attackCoeff  = std::exp (-tickMs / attackMs);
        float releaseCoeff = std::exp (-tickMs / releaseMs);

        for (int px = 0; px < width; ++px)
        {
            float target = targetDb[(size_t) px];
            float& cur = smoothedDb[(size_t) px];
            float coeff = (target > cur) ? attackCoeff : releaseCoeff;
            cur = coeff * cur + (1.0f - coeff) * target;
        }

        path.clear();
        bool started = false;
        for (int px = 0; px < width; ++px)
        {
            float x = bounds.getX() + (float) px;
            float y = juce::jmap (smoothedDb[(size_t) px], -100.0f, 0.0f,
                                  bounds.getBottom(), bounds.getY() + bounds.getHeight() * 0.15f);
            y = juce::jlimit (bounds.getY(), bounds.getBottom(), y);

            if (! started) { path.startNewSubPath (x, y); started = true; }
            else            path.lineTo (x, y);
        }

        repaint();
    }

    EQAudioProcessor& processor;
    juce::Path path;
    std::vector<float> smoothedDb;
};
