#pragma once
#include <juce_dsp/juce_dsp.h>
#include "FilterBand.h"

/**
    Linear Phase движок для EQ.

    Идея (как Zero/Natural/Linear Phase режимы в FabFilter Pro-Q):
    вместо каскада IIR-биквадов (которые всегда вносят фазовый сдвиг рядом
    с точкой среза) строим FIR-фильтр с нулевой фазой по желаемой АЧХ:

      1. Берём текущую суммарную магнитудную характеристику EQ (ту же функцию
         getMagnitudeForFrequency, которой рисуется график) на N/2+1 бинах.
      2. Ставим фазу = 0 на каждом бине, зеркалим спектр (комплексно-сопряжённо)
         для второй половины -> обратное FFT даёт РЕАЛЬНЫЙ, но НЕпричинный
         (симметричный вокруг нуля) импульсный отклик.
      3. Циклически сдвигаем на N/2 сэмплов вправо -> получаем причинный FIR
         фильтр. Это и есть источник задержки: N/2 сэмплов латентности.
      4. Окно (Blackman-Harris) на края импульса, чтобы погасить эффект
         Гиббса от резкого обрезания спектра конечным N.
      5. Ядро скармливается juce::dsp::Convolution, которая и делает
         быструю блочную свёртку в реальном времени.

    Пересчёт ядра — недешёвая операция (FFT + IFFT + окно), поэтому она
    выполняется в фоновом потоке с debounce, а не на каждый сэмпл/блок.
*/
class LinearPhaseEQ
{
public:
    // qualityIndex: 0=Low(1024) 1=Medium(2048) 2=High(4096) 3=Maximum(8192)
    void setQuality (int qualityIndex);
    int  getLatencySamples() const { return currentFftSize / 2; }

    void prepare (double sampleRate, int maximumBlockSize);
    void reset();

    // Вызывается из аудио-потока каждый блок: применяет текущую свёртку.
    void process (juce::dsp::AudioBlock<float>& block);

    struct BandSnapshot { FilterType type; float freq, gain, q; bool enabled; float order = 2.0f; };

    // Вызывается из аудио-потока (дёшево — просто копирование POD-структур):
    // передаёт снэпшот параметров полос и помечает ядро "грязным", если
    // что-то заметно изменилось.
    void updateBandSnapshot (const std::array<BandSnapshot, numBands>& newSnapshot);

    // Запускается один раз из processor'а (владеет фоновым потоком).
    void startBackgroundThread();
    void stopBackgroundThread();

private:
    void rebuildKernelIfDirty();
    void rebuildKernel();

    juce::dsp::Convolution convolution { juce::dsp::Convolution::NonUniform { 512 } };

    double sampleRate = 44100.0;
    int currentFftSize = 4096;
    int pendingFftSize = 4096;

    // Снэпшот параметров полос для построения ядра (копия, без указателей на APVTS)
    std::array<BandSnapshot, numBands> snapshot {};
    std::atomic<bool> dirty { true };

    juce::CriticalSection snapshotLock;

    class BuilderThread : public juce::Thread
    {
    public:
        explicit BuilderThread (LinearPhaseEQ& o) : juce::Thread ("FabEQ IR Builder"), owner (o) {}
        void run() override
        {
            while (! threadShouldExit())
            {
                owner.rebuildKernelIfDirty();
                wait (60); // debounce ~60мс, чтобы не пересчитывать на каждый чих автоматизации
            }
        }
    private:
        LinearPhaseEQ& owner;
    };

    BuilderThread builderThread { *this };
};
