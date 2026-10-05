# NONSTOP — audio-reactive glitch overlay for Windows

Test build. Listens to system audio (WASAPI loopback) and bends the whole
screen to the music in real time: RGB-split, slices, waves, melt, ripples,
glass rain, zoom punch, ghost words, duplication cascade. Click-through:
mouse and keyboard reach the game underneath.

Current version: v0.1.0-megatest. Dirty prototype, everything may change.

## Requirements

- Windows 10/11 64-bit
- Game in **borderless windowed** mode (exclusive fullscreen is not covered)
- Visual C++ Redistributable 2022 x64
- Music playing through the default output device

## Quick start

1. Download `nonstop-megatest.zip` from **Releases**, unpack anywhere.
2. Run `nonstop_diagnostics.exe`.
3. Play music. The `NONSTOP` mini window controls everything:
   POWER slider, per-effect checkboxes, Flash/Word tests, Streamer mode.
4. Hotkeys: `Ctrl+Alt+X` / `Ctrl+Shift+F12` — hide the overlay
   (in HELL MODE: always force-hide + disarm).
5. For friends to see the rave on stream: Discord must share
   the whole **SCREEN**, not the game window. Streamer mode hides
   the overlay from all captures.

## HELL MODE

Separate red toggle in the FX tab, 100–500% overdrive: repeat-synced
screen invert (max 10 flips/sec), deeper zoom, mirror band on drops,
opaque composite allowed.

> EPILEPSY WARNING: HELL MODE strobes up to 10x/sec. Do not enable
> if you or any viewer has photosensitive epilepsy. A fullscreen
> warning with explicit confirmation appears on every enable.
> The panic hotkey kills it instantly.

## Build from source

```bat
cmake -S . -B build
cmake --build build --config Release
```

Needs MSVC 2022+, CMake 3.20+, Windows SDK 10. Result:
`build\Release\nonstop_diagnostics.exe`. Deps are fetched
automatically (Dear ImGui, PFFFT, nlohmann/json).

## Layout

- `src/audio` — WASAPI loopback capture, FFT analyzer, phase + repeat detectors
- `src/overlay` — transparent click-through overlay, D3D11 FX chain,
  desktop-duplication screen capture
- `src/core` — panic hotkeys
- `src/ui` — mini remote (the only settings UI)
- `nonstop.md` — original product spec (Russian, ТЗ)

## Status

Mega-test prototype. No installer, no config file yet (settings live
in-session), no telemetry, no network activity at all.
License: TBD (all rights reserved for now).

---

# NONSTOP — аудио-реактивный глитч-оверлей для Windows

Тестовый билд. Слушает системный звук (WASAPI loopback) и гнёт весь
экран в такт музыке в реальном времени: RGB-разъезд, срезы, волны,
плавление, кольца, дождь по стеклу, зум-панч, слова-призраки, каскад
дублирования. Click-through: мышь и клавиатура проходят в игру.

Текущая версия: v0.1.0-megatest. Грязный прототип, всё может поменяться.

## Требования

- Windows 10/11 64-bit
- Игра в режиме **borderless windowed** (эксклюзивный fullscreen не перекрывается)
- Visual C++ Redistributable 2022 x64
- Музыка играет через устройство вывода по умолчанию

## Быстрый старт

1. Скачай `nonstop-megatest.zip` из **Releases**, распакуй куда угодно.
2. Запусти `nonstop_diagnostics.exe`.
3. Включи музыку. Всё управляется из мини-окна `NONSTOP`:
   слайдер POWER, галки эффектов, тесты Flash/Word, Streamer mode.
4. Хоткеи: `Ctrl+Alt+X` / `Ctrl+Shift+F12` — спрятать оверлей
   (в HELL MODE: всегда жёстко гасит + выключает режим).
5. Чтобы кенты видели рейв на стриме: в Discord шарь весь **ЭКРАН**,
   а не окно игры. Streamer mode прячет оверлей из всех захватов.

## HELL MODE

Отдельный красный тумблер во вкладке FX, овердрайв 100–500%:
инверсия под репиты (макс 10 флипов/сек), глубже зум, зеркальная
полоса на дропе, разрешена непрозрачность.

> ПРЕДУПРЕЖДЕНИЕ ОБ ЭПИЛЕПСИИ: HELL MODE стробит до 10 раз/сек.
> Не включай, если у тебя или зрителей есть фоточувствительная
> эпилепсия. При каждом включении — фулскрин-предупреждение
> с явным подтверждением. Паник-хоткей гасит мгновенно.

## Сборка из сурсов

```bat
cmake -S . -B build
cmake --build build --config Release
```

Нужны MSVC 2022+, CMake 3.20+, Windows SDK 10. Результат:
`build\Release\nonstop_diagnostics.exe`. Зависимости тянутся сами
(Dear ImGui, PFFFT, nlohmann/json).

## Структура

- `src/audio` — WASAPI loopback захват, FFT-анализатор, детекторы фаз и репитов
- `src/overlay` — прозрачный click-through оверлей, D3D11 цепочка эффектов,
  захват экрана (desktop duplication)
- `src/core` — паник-хоткеи
- `src/ui` — мини-пульт (единственное меню настроек)
- `nonstop.md` — исходное ТЗ продукта

## Статус

Мега-тестовый прототип. Нет установщика, конфиг-файла пока нет
(настройки живут в сессии), нет телеметрии, никакой сетевой
активности вообще.
Лицензия: TBD (all rights reserved for now).
