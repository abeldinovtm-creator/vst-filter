#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

/** Рисует бегущую спектрограмму (FFT) как полупрозрачную заливку на фоне EQ-графика. */
class SpectrumAnalyzer : public juce::Component, private juce::Timer
{
public:
    explicit SpectrumAnalyzer (EQAudioProcessor& p) : processor (p)
    {
        startTimerHz (30);
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
        if (! processor.nextFFTBlockReady.load())
            return;

        {
            const juce::ScopedLock lock (processor.fftLock);
            processor.window.multiplyWithWindowingTable (processor.fftData, (size_t) EQAudioProcessor::fftSize);
            processor.fft.performFrequencyOnlyForwardTransform (processor.fftData);
            processor.nextFFTBlockReady.store (false);
        }

        auto bounds = getLocalBounds().toFloat();
        path.clear();

        const auto sr = processor.currentSampleRate;
        const int numBins = EQAudioProcessor::fftSize / 2;
        bool started = false;

        for (int i = 1; i < numBins; ++i)
        {
            float freq = (float) (i * sr / EQAudioProcessor::fftSize);
            if (freq < 20.0f || freq > 20000.0f)
                continue;

            float x = bounds.getX() + bounds.getWidth() *
                        (std::log (freq / 20.0f) / std::log (20000.0f / 20.0f));

            float magnitude = processor.fftData[i];
            float db = juce::Decibels::gainToDecibels (magnitude, -100.0f);
            float y = juce::jmap (db, -100.0f, 0.0f, bounds.getBottom(), bounds.getY() + bounds.getHeight() * 0.15f);
            y = juce::jlimit (bounds.getY(), bounds.getBottom(), y);

            if (! started) { path.startNewSubPath (x, y); started = true; }
            else            path.lineTo (x, y);
        }

        repaint();
    }

    EQAudioProcessor& processor;
    juce::Path path;
};
