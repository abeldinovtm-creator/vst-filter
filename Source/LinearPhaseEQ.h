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

    // Явная общая очередь для конвольвера: loadImpulseResponse() вызывается из
    // BuilderThread, а process() — из аудио-потока. ConvolutionMessageQueue
    // держит собственный фоновый поток (стартует в своём конструкторе) и
    // безопасно синхронизирует смену IR с process(), поэтому нельзя просто
    // положиться на "работает и так" — нужна именно она, а не implicit-очередь,
    // которую Convolution иначе создал бы сама неявно. Порядок объявления важен:
    // convolutionMessageQueue должна инициализироваться раньше convolution.
    juce::dsp::ConvolutionMessageQueue convolutionMessageQueue;
    juce::dsp::Convolution convolution { juce::dsp::Convolution::NonUniform { 512 }, convolutionMessageQueue };

    double sampleRate = 44100.0;
    int currentFftSize = 4096;
    int pendingFftSize = 4096;

    // Снэпшот параметров полос для построения ядра (копия, без указателей на APVTS)
    std::array<BandSnapshot, numBands> snapshot {};
    std::atomic<bool> dirty { true };

    // Пока параметры продолжают меняться (например, тащишь точку в графике),
    // rebuildKernel() не запускается вообще — ждём "тишины" в изменениях хотя бы
    // kernelSettleMs. Причина: сама пересборка (FFT/IFFT/окно) занимает заметное
    // время, а Convolution кроссфейдит старое/новое ядро 50мс (см. JUCE
    // juce_Convolution.cpp, CrossoverMixer). Если dirty остаётся true почти
    // непрерывно (что при live-перетаскивании обычно так), BuilderThread пытается
    // пересобирать kernel каждые ~60мс — новые kernel'ы прилетают быстрее, чем
    // успевает завершиться предыдущий 50мс-кроссфейд, транзишны накладываются
    // друг на друга, и это слышно как провалы/треск ровно во время движения.
    // Дебаунсим: рабочий (последний стабильный) kernel продолжает звучать без
    // изменений, пока не наступит пауза — тогда пересборка происходит один раз
    // и кроссфейдится чисто. Порог взят с запасом: перетаскивание мышью — это не
    // идеально равномерный поток событий, между ними бывают короткие
    // (десятки мс) паузы, и слишком короткий kernelSettleMs иногда ошибочно
    // принимает такую паузу за "остановились", запуская пересборку прямо
    // посреди продолжающегося перетаскивания.
    static constexpr double kernelSettleMs = 220.0;
    std::atomic<double> lastSnapshotChangeMs { 0.0 };

    juce::CriticalSection snapshotLock;

    class BuilderThread : public juce::Thread
    {
    public:
        explicit BuilderThread (LinearPhaseEQ& o) : juce::Thread ("EQ IR Builder"), owner (o) {}
        void run() override
        {
            while (! threadShouldExit())
            {
                owner.rebuildKernelIfDirty();
                wait (20); // опрашиваем чаще, чем kernelSettleMs, чтобы точнее ловить момент "устаканилось"
            }
        }
    private:
        LinearPhaseEQ& owner;
    };

    BuilderThread builderThread { *this };
};
