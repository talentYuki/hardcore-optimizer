# LeakOptimizator

Агрессивная системная утилита для Windows, которая выжимает максимум FPS и
стабильности в играх. Работает на уровне системных механизмов Windows (WinAPI,
WMI, ETW). Все твики — штатные и полностью обратимые (кнопка **Restore** +
JSON-слепок реестра).

> Для работы многих твиков требуются права администратора (UAC-манифест
> `requireAdministrator`).

---

## Возможности

### Системные твики (только WinAPI)

| Твик | Механизм |
|------|----------|
| Очистка Standby-памяти | `NtSetSystemInformation` (purge standby list) |
| Приоритет игры | `SetPriorityClass` → `HIGH_PRIORITY_CLASS` |
| Запрет «фоновых» ядер | `SetProcessAffinityMask` |
| Отключение Xbox Game Bar | реестр `HKCU\Software\Microsoft\GameBar` |
| Отключение Game DVR / записи | реестр `GameDVR` / `GameConfigStore` |
| Ускорение мыши | реестр `Control Panel\Mouse` |
| Nagle-алгоритм | реестр `Tcpip\Parameters\Interfaces` (все GUID) |
| Фоновые службы (WSearch, SysMain, DiagTrack, MapsBroker) | `SCManager` + `ChangeServiceConfig` |
| Таймер высокого разрешения | `timeBeginPeriod(1)` + `bcdedit /set disabledynamictick yes` |

### Производительность процессора и GPU (AMD / Intel)

| Твик | Действие |
|------|----------|
| Детект вендора CPU (AMD/Intel) и частоты | чтение реестра `CentralProcessor` |
| План питания «Максимальная производительность» | `powercfg /setactive SCHEME_MIN` |
| Разпарковка ядер (мин. состояние 100%) | `powercfg … PROCTHROTTLEMIN 100` |
| Агрессивный boost | `powercfg … PERFBOOSTMODE 2` |
| HAGS — аппаратное планирование GPU | реестр `GraphicsDrivers\HwSchMode=2` (после перезагрузки) |
| Игровой режим Windows | `GameBar\AllowAutoGameMode=1` |
| Инструкция XMP/DOCP и разгон | подробная памятка по BIOS |

> ⚠️ **Важно про разгон.** Реальный разгон CPU (умножитель, напряжение) и
> включение XMP/DOCP выполняются **в BIOS/UEFI** (или утилитой вендора: AMD
> Ryzen Master / Intel XTU). Из пользовательского приложения это безопасно
> сделать нельзя — прямой запись в MSR с поднятием напряжения рискует
> повредить железо. Поэтому здесь разгон представлен как безопасная
> Windows-оптимизация + подробная инструкция. XMP/DOCP — заводские безопасные
> профили памяти, их включение безопасно.

### Пресеты рекомендуемых настроек ПК

|c#| Пресет | Состав |
|--|--------|--------|
| 1 | **Максимальный FPS** | все твики + план питания + ядра + boost + HAGS + Game Mode |
| 2 | **Стабильный frametime (онлайн)** | таймер, Nagle, план питания, ядра, boost, приоритет |
| 3 | **Тихий / стриминг** | Game Bar/DVR, службы, standby, Game Mode |
| 4 | **Рекомендуемые (безопасные)** | Game Mode, план питания, разпарковка ядер |

### Многопоточность (`std::thread`)
- поток мониторинга FPS (ETW-потребитель событий `Microsoft-Windows-DxgKrnl`);
- поток температур CPU/GPU (WMI `MSAcpi_ThermalZoneTemperature`);
- поток загрузки ядер/ОЗУ (`GetSystemProcessorPerformanceInformation`);
- отдельный рабочий поток для твиков (очередь задач) — GUI не блокируется.

### Оверлей
- прозрачное окно поверх игры: `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST`;
- клики «проходят насквозь» (click-through);
- FPS, frametime, температура CPU/GPU, загрузка CPU, занятая RAM;
- темы: **Neon Blue**, **Toxic Green**, **Blood Red**, **Rust Orange**, **Monochrome**;
- системный **Color Picker** для акцентного цвета; прозрачность 0–255;
- сохранение темы и позиции в JSON.

### Бэкап и восстановление
- `LeakOptimizator.backup.json` — слепок всех затронутых значений реестра;
- кнопка **Restore** откатывает реестр, приоритет, маску ядер, службы, таймер,
  план питания, парковку ядер, boost и HAGS.

---

## Структура проекта

```
main.cpp          GUI, точка входа, пресеты, фоновые потоки (ETW, WMI, твики)
optimizer.h/cpp   системные твики (память, приоритет, реестр, службы, таймер,
                  производительность CPU/GPU, инструкция XMP)
overlay.h/cpp     прозрачный оверлей (GDI)
hardware.h/cpp    детект железа (вендор AMD/Intel), температуры, загрузка ядер
theme.h/cpp       цветовые схемы + JSON
backup.h/cpp      сохранение/восстановление реестра
json_util.h/cpp   мини-JSON парсер/писатель (без внешних зависимостей)
resources/        app.rc, app.manifest (UAC), app.ico
CMakeLists.txt    сборка
build.bat         сборка через vcvars64 + CMake (NMake)
leakshop-landing.html  ч/б лендинг магазина
```

---

## Требования для сборки

| Зависимость | Назначение |
|-------------|-----------|
| **Windows 10 / 11** (x64) | целевая ОС |
| **Visual Studio 2022 Build Tools** (MSVC v143, Windows 10 SDK) | компилятор + SDK |
| **CMake ≥ 3.20** | генерация проекта |

Внешних библиотек, кроме системных (WinAPI, WMI, ETW, COM), нет — всё
линкуется из Windows SDK. **MinGW/GCC не поддерживается** (в CMake есть
проверка на MSVC).

---

## Сборка

### Вариант 1 — `build.bat` (на этой машине)

```cmd
build.bat Release
```
Исполняемый файл: `build\bin\LeakOptimizator.exe`.

### Вариант 2 — CMake + Visual Studio

```cmd
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### Вариант 3 — Ninja

```cmd
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

---

## Запуск

1. Запустите `LeakOptimizator.exe` **от имени администратора**.
2. Введите PID игры в поле «PID игры» (0 = текущий процесс).
3. Выберите пресет и нажмите **Применить пресет** (или **Применить всё**).
4. Оверлей покажет FPS/температуры/загрузку поверх игры.
5. Откат всего — кнопка **ВОССТАНОВИТЬ (Rollback)**.

---

## Диагностика FPS-монитора

FPS считаются по real-time ETW-сессии поставщика `Microsoft-Windows-DxgKrnl`.
Если сессия не открылась (нет прав), FPS останется 0 — температуры и загрузка
продолжат работать, твики не ломаются.

---

## Примечания

- Настройки хранятся рядом с exe: `LeakOptimizator.theme.json`,
  `LeakOptimizator.backup.json`.
- HAGS (`HwSchMode=2`) вступает в силу **после перезагрузки**.
- Отключение служб `SysMain`/`WSearch` замедляет поиск/предзагрузку — это
  намеренное «агрессивное» поведение, откатывается кнопкой Restore.