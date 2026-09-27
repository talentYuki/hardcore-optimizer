# Hardcore Optimizer

Агрессивная системная утилита для Windows, которая выжимает максимум FPS и
стабильности в играх, работая на уровне системных механизмов Windows (WinAPI,
WMI, ETW). Все твики — штатные, полностью обратимые (есть кнопка **Restore**
и JSON-слепок реестра).

> Для работы многих твиков требуются права администратора (UAC-манифест
> `requireAdministrator`).

---

## Возможности

### Системные твики (только WinAPI)

| Твик | Механизм |
|------|----------|
| Очистка Standby-памяти | `NtSetSystemInformation` (`SystemMemoryListInformation`, purge standby list) |
| Приоритет игры | `SetPriorityClass` → `HIGH_PRIORITY_CLASS` |
| Запрет «фоновых» ядер | `SetProcessAffinityMask` |
| Отключение Xbox Game Bar | реестр `HKCU\Software\Microsoft\GameBar` |
| Отключение Game DVR / записи | реестр `GameDVR` / `GameConfigStore` |
| Ускорение мыши | реестр `Control Panel\Mouse` (пороги 0/0) |
| Nagle-алгоритм | реестр `Tcpip\Parameters\Interfaces` для всех GUID-интерфейсов |
| Фоновые службы (WSearch, SysMain, DiagTrack, MapsBroker) | `OpenSCManager` + `ChangeServiceConfig` |
| Таймер высокого разрешения | `timeBeginPeriod(1)` + `bcdedit /set disabledynamictick yes` |

### Многопоточность (`std::thread`)
- поток мониторинга FPS (ETW-потребитель событий `Microsoft-Windows-DxgKrnl`);
- поток температур CPU/GPU (WMI `MSAcpi_ThermalZoneTemperature`);
- поток загрузки ядер/ОЗУ (`GetSystemProcessorPerformanceInformation`);
- каждый твик выполняется из потока GUI с блокировкой, не трогая овелей.

### Оверлей
- прозрачное окно поверх игры: `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST`;
- клики «проходят насквозь» (click-through);
- FPS, frametime, температура CPU/GPU, загрузка CPU, занятая RAM;
- темы: **Neon Blue**, **Toxic Green**, **Blood Red**, **Rust Orange**;
- системный **Color Picker** для акцентного цвета;
- прозрачность 0–255; сохранение темы и позиции в JSON.

### Бэкап и восстановление
- `HardcoreOptimizer.backup.json` — слепок всех затронутых значений реестра;
- кнопка **Restore** откатывает реестр, приоритет, маску ядер, службы и таймер.

---

## Структура проекта

```
main.cpp          GUI + точка входа + фоновые потоки (ETW, WMI)
optimizer.h/cpp   системные твики (память, приоритет, реестр, службы, таймер)
overlay.h/cpp     прозрачный оверлей (GDI)
hardware.h/cpp    детект железа, температуры, загрузка ядер
theme.h/cpp       цветовые схемы + JSON
backup.h/cpp      сохранение/восстановление реестра
json_util.h/cpp   мини-JSON парсер/писатель (без внешних зависимостей)
resources/        app.rc, app.manifest (UAC), app.ico
CMakeLists.txt    сборка
```

---

## Требования для сборки

| Зависимость | Назначение |
|-------------|-----------|
| **Windows 10 / 11** (x64) | целевая ОС |
| **Visual Studio 2022** (компонент «Разработка классических приложений на C++»: MSVC v143, Windows 10 SDK) | компилятор + SDK |
| **CMake ≥ 3.20** | генерация проекта |
| **Ninja** (опционально) | параллельная сборка |

Никаких внешних библиотек, кроме системных (WinAPI, WMI, ETW, COM) — всё
линкуется из Windows SDK.

Сборка проекта через MSVC обязательна (используется C++20 и WinAPI). Среда
сборки **MinGW/GCC не поддерживается** (`CMakeLists.txt` это проверяет).

---

## Сборка

### Вариант 1 — через CMake + Visual Studio (рекомендуется)

Откройте «Командную строку для разработчиков» (Developer Command Prompt VS2022):

```cmd
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Исполняемый файл появится в `build\bin\HardcoreOptimizer.exe`.

### Вариант 2 — через Ninja

```cmd
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

---

## Запуск

1. Запустите `HardcoreOptimizer.exe` **от имени администратора** (UAC-запрос
   появится автоматически).
2. Введите PID игры в поле «PID игры» (0 = применяется к процессу утилиты).
3. Нажмите **Применить всё** — применяются все твики, снимается слепок.
4. Оверлей покажет текущий FPS/температуры/загрузку прямо поверх игры.
5. Для отката — **ВОССТАНОВИТЬ (Rollback)**.

---

## Диагностика FPS-монитора

FPS считаются по real-time ETW-сессии поставщика `Microsoft-Windows-DxgKrnl`.
Если сессия не открылась (например, из-за отсутствия прав), FPS в оверлее
останется равным 0, а температуры и загрузка продолжат работать — это не
критично и не ломает твики.

---

## Примечания

- Темы и настройки хранятся рядом с exe: `HardcoreOptimizer.theme.json`,
  `HardcoreOptimizer.backup.json`.
- Отключение служб `SysMain`/`WSearch` может замедлить поиск и предзагрузку —
  это намеренное «агрессивное» поведение утилиты, которое можно откатить.
- `bcdedit /set disabledynamictick yes` действует до следующего отката/сброса
  BCD и позволяет ядру использовать фиксированный 1 мс тик.