# EQ — параметрический эквалайзер (JUCE, VST3/AU)

Параметрический эквалайзер, до 16 полос одновременно:
- Клик по пустому месту графика создаёт новую точку (как в FabFilter Pro-Q), драг = частота/gain, колесо мыши = Q, двойной клик = вкл/выкл полосы
- Индивидуальная АЧХ-кривая каждой активной полосы отображается на графике (выбранная — ярче остальных)
- Живой спектр-анализатор фона (FFT): post-EQ всегда, pre-EQ (вход) — по тумблеру "I/O"
- Авто-поиск резонансов: полоса сама находит самый выраженный резонанс в спектре и гасит его (Auto), все найденные резонансы подсвечиваются на графике
- Типы фильтров на полосу: Bell, Low Shelf, High Shelf, Low Cut, High Cut, Notch
- **Phase Mode: Zero Latency (IIR, без задержки) / Linear Phase (FIR-свёртка, фаза не крутится у полос)**
- **Linear Phase Quality: Low/Medium/High/Maximum — размер FFT-ядра (1024–8192), компромисс задержка/точность**
- Полная автоматизация параметров из хоста (AudioProcessorValueTreeState)
- Reset — сброс всех параметров к дефолту; Save/Load — сохранение/загрузка пресетов (.xml на диск)

## Как работает Linear Phase

Обычный EQ (IIR-биквады) всегда сдвигает фазу рядом с точкой среза — это то,
что называют "фазовым вращением", источник характерного "смазывания"
транзиентов при агрессивной коррекции. Linear Phase режим решает это иначе:

1. Берём ту же самую суммарную АЧХ, что рисуется на графике.
2. Строим FIR-фильтр с нулевой фазой по этой АЧХ (обратное FFT от
   желаемой магнитуды с фазой = 0).
3. Циклически сдвигаем ядро, чтобы сделать его причинным — отсюда и
   берётся задержка (латентность = половина размера FFT-ядра).
4. Ядро скармливается `juce::dsp::Convolution`, которая делает быструю
   блочную свёртку в реальном времени.

Пересчёт ядра идёт в фоновом потоке (`LinearPhaseEQ::BuilderThread`) с
debounce ~60мс, чтобы не грузить аудио-поток и не пересчитывать FFT на
каждое микро-движение автоматизации. Задержка репортится хосту через
`setLatencySamples()`, так что PDC (plugin delay compensation) сработает
автоматически — трек с Linear Phase EQ останется в фазе с остальными.

**Компромисс качество/задержка** (при 44.1 кГц):
| Quality  | FFT size | Задержка     |
|----------|---------:|-------------:|
| Low      |     1024 |   ~11.6 мс   |
| Medium   |     2048 |   ~23.2 мс   |
| High     |     4096 |   ~46.4 мс   |
| Maximum  |     8192 |   ~92.9 мс   |

Zero Latency режим (по умолчанию) — это исходный каскад IIR-биквадов
без дополнительной задержки, для трекинга/живой работы.

## Требования

- CMake ≥ 3.22
- Компилятор с поддержкой C++17:
  - **Windows**: Visual Studio 2022 (Desktop C++ workload)
  - **macOS**: Xcode ≥ 14 + командные строки (`xcode-select --install`)
- Git (для автоматической загрузки JUCE через FetchContent)

Сеть нужна один раз — CMake сам скачает JUCE 7.0.12 при первой конфигурации.
Если хочешь без сети — скачай JUCE вручную (https://juce.com/get-juce) и замени блок
`FetchContent` в `CMakeLists.txt` на `add_subdirectory(путь/до/JUCE)`.

## Сборка — Windows

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

VST3 появится в:
`build/EQ_artefacts/Release/VST3/EQ.vst3`

Скопируй в `C:\Program Files\Common Files\VST3\`.

## Сборка — macOS

```bash
cmake -B build -G Xcode
cmake --build build --config Release
```

VST3 и AU появятся в:
`build/EQ_artefacts/Release/VST3/EQ.vst3`
`build/EQ_artefacts/Release/AU/EQ.component`

JUCE сам скопирует их в системные плагин-папки
(`~/Library/Audio/Plug-Ins/VST3` и `~/Library/Audio/Plug-Ins/Components`),
т.к. в CMakeLists стоит `COPY_PLUGIN_AFTER_BUILD TRUE`.

## Структура проекта

```
EQ/
├── CMakeLists.txt          — конфиг сборки, тянет JUCE, настраивает VST3/AU/Standalone
└── Source/
    ├── PluginProcessor.h/.cpp  — DSP-движок, параметры (APVTS), 16 полос EQ, FFT для анализатора
    ├── PluginEditor.h/.cpp     — GUI: график АЧХ, draggable-точки, панель параметров
    ├── FilterBand.h/.cpp       — одна полоса фильтра (biquad, RBJ cookbook через juce::dsp)
    ├── LinearPhaseEQ.h/.cpp    — FIR-движок Linear Phase режима (FFT-синтез ядра + Convolution)
    └── SpectrumAnalyzer.h/.cpp — фоновая FFT-визуализация спектра (header-only)
```

## Автор

Tlek Abeldinov

## Что дальше можно добавить

- Плавающая линия текущего значения при наведении (readout freq/gain/Q как тултип)
- Mid/Side и Stereo/L/R режимы на полосу
- Более крутые наклоны Low/High Cut (сейчас Butterworth высокого порядка уже есть в `FilterBand`,
  просто прокинь выбор наклона 12/24/48 dB/oct в `typeBox` или отдельный контрол)
- Список/браузер сохранённых пресетов (сейчас Save/Load — просто диалог выбора файла)
