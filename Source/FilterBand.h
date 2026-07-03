#pragma once
#include <juce_dsp/juce_dsp.h>

static constexpr int numBands = 8;

/** Тип фильтра для одной полосы EQ. */
enum class FilterType
{
    Bell = 0,
    LowShelf,
    HighShelf,
    LowCut,     // Butterworth, крутизна задаётся order (непрерывно, 6..48 dB/oct)
    HighCut,
    Notch
};

/**
    Одна полоса параметрического EQ.
    Инкапсулирует IIR-фильтр (juce::dsp) + пересчёт коэффициентов по RBJ cookbook.
    Для LowCut/HighCut используется каскад биквадов. Order (крутизна) непрерывный:
    при нецелом значении параллельно считаются две Butterworth-цепочки соседних целых
    order и их выход кроссфейдится, поэтому крутизна ската меняется плавно.
*/
class FilterBand
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();

    // Обрабатывает блок audio (моно-канал контекста через ProcessContextReplacing снаружи)
    void process (juce::dsp::AudioBlock<float>& block);

    void setParameters (FilterType type, float frequencyHz, float gainDb, float q, float slopeOrder = 2.0f);

    // Для отрисовки АЧХ в редакторе: комплексный отклик на заданной частоте
    float getMagnitudeForFrequency (double frequencyHz, double sampleRate) const;

    bool enabled = true;

    static constexpr int maxStages = 4;

private:
    void updateCoefficients();
    static int designCutStages (FilterType type, float freq, double sampleRate, int order,
                                 std::array<juce::dsp::IIR::Coefficients<float>::Ptr, maxStages>& dest);

    FilterType currentType = FilterType::Bell;
    float freq = 1000.0f;
    float gain = 0.0f;
    float q = 0.707f;
    float order = 2.0f; // для low/high cut: число полюсов (непрерывно), 6 dB/oct на полюс

    double sampleRate = 44100.0;

    // Основной каскад (order = floor(order))
    std::array<juce::dsp::IIR::Filter<float>, maxStages> stagesL, stagesR;
    std::array<juce::dsp::IIR::Coefficients<float>::Ptr, maxStages> coeffs;
    int activeStages = 1;

    // Второй каскад (order = ceil(order)), используется только при нецелом order
    // для плавного кроссфейда крутизны
    std::array<juce::dsp::IIR::Filter<float>, maxStages> stagesL2, stagesR2;
    std::array<juce::dsp::IIR::Coefficients<float>::Ptr, maxStages> coeffs2;
    int activeStages2 = 0;
    float slopeBlend = 0.0f; // 0 = только основной каскад, 1 = только второй

    juce::AudioBuffer<float> scratchBuffer;
};
