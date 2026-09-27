// ============================================================================
// main.cpp — точка входа и главное окно LeakOptimizator.
//
// Собирает всю систему воедино:
//   - главное окно с кнопками твиков, выбором темы, логом операций
//   - фоновые потоки мониторинга (температуры+загрузка через WMI/GetSystemTimes
//     и FPS через ETW-потребитель событий DxgKrnl)
//   - оверлей поверх игры (OverlayWindow)
//   - бэкап/восстановление реестра (backup)
//
// Требует прав администратора (UAC-манифест).
// ============================================================================

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <dwmapi.h>

#include <evntrace.h>
#include <evntcons.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "backup.h"
#include "hardware.h"
#include "optimizer.h"
#include "overlay.h"
#include "theme.h"

// ---------------------------------------------------------------------------
// 1. Идентификаторы элементов управления главного окна.
// ---------------------------------------------------------------------------
namespace ids {
enum {
    BtnApplyAll  = 2001, // «Применить всё»
    BtnRestore   = 2002, // «Восстановить (Rollback)»
    BtnClearMem  = 2003, // «Очистить Standby-память»
    BtnPriority  = 2004, // «HIGH_PRIORITY + обрезка ядер»
    BtnGameBar   = 2005, // «Отключить Game Bar»
    BtnDvr       = 2006, // «Отключить Game DVR»
    BtnMouse     = 2007, // «Убрать ускорение мыши»
    BtnNagle     = 2008, // «Отключить Nagle (TCP)»
    BtnServices  = 2009, // «Отключить фоновые службы»
    BtnTimer     = 2010, // «Таймер 1 мс + disabledynamictick»
    BtnOverlay   = 2011, // Переключатель оверлея
    ComboTheme   = 2012, // Выпадающий список тем
    BtnColor     = 2013, // Системный Color Picker
    EditOpacity  = 2014, // Поле ввода прозрачности 0..255
    BtnOpacity   = 2015, // Применить прозрачность
    EditPid      = 2016, // Поле ввода PID игры
    EditLog      = 2017, // Лог операций
    StaticHw     = 2020, // Инфо о железе
    StaticStats  = 2021, // Живые метрики
    BtnPower     = 2030, // План питания: макс. производительность
    BtnCores     = 2031, // Разпарковка ядер (мин. 100%)
    BtnBoost     = 2032, // Boost-режим агрессивный
    BtnHags      = 2033, // HAGS: аппаратное планирование GPU
    BtnGameMode  = 2034, // Игровой режим Windows
    BtnXmp       = 2035, // Инструкция XMP/DOCP и разгона
    ComboPreset  = 2040, // Пресеты рекомендуемых настроек
    BtnApplyPreset = 2041, // Применить выбранный пресет
};
} // namespace ids

// ---------------------------------------------------------------------------
// 2. Глобальное состояние приложения.
// ---------------------------------------------------------------------------
namespace app {

// Дескриптор главного окна (необходим из обработчиков кнопок).
HWND g_mainHwnd = nullptr;
HWND GetMainHwnd() { return g_mainHwnd; }
void SetMainHwnd(HWND h) { g_mainHwnd = h; }

// Общие «живые» данные оверлея (поля атомарные — читаются из любого потока).
OverlayData            g_overlay;       // данные для оверлея
OverlayWindow          g_overlayWin;    // само окно оверлея
bool                   g_overlayVisible = true;

// Управление нитями.
std::atomic<bool>      g_running{ true };
std::thread            g_monitorThread; // поток: температуры/загрузка/RAM/FPS
std::thread            g_etwThread;     // поток: ETW-потребитель

// Очередь задач твиков для отдельного рабочего потока: GUI не блокируется
// на «медленных» вызовах реестра/служб/WMI-подобных операций.
std::mutex                     g_tweakMutex;
std::condition_variable        g_tweakCv;
std::vector<std::function<void()>> g_tweakQueue;
std::thread                    g_tweakThread;
std::atomic<bool>              g_tweakRunning{ true };

// FPS через ETW: счётчик принятых событий презента и сессия трассировки.
std::atomic<unsigned long long> g_presentCount{ 0 };
TRACEHANDLE            g_sessionTrace = 0;
const wchar_t*         g_sessionName = L"HO_FpsTrace";

// Слепок реестра, снятый перед последним применением (для мгновенного отката).
std::vector<backup::RegValue> g_snapshot;

// Последний целевой PID игры.
DWORD                  g_gamePid = 0;

} // namespace app

// ---------------------------------------------------------------------------
// 3. ETW-потребитель событий поставщика Microsoft-Windows-DxgKrnl.
//    Real-time сессия: каждый пришедший графический event увеличивает счётчик.
//    Поток блокируется внутри ProcessTrace до остановки (см. StopEtw).
// ---------------------------------------------------------------------------
namespace {

// Поставщик Microsoft-Windows-DxgKrnl (ядро графической подсистемы Windows).
const GUID kDxgKrnl = { 0x802EC45A, 0x1E99, 0x4B83,
                        { 0x99, 0x20, 0x87, 0xC9, 0x82, 0x77, 0xBA, 0x9D } };

VOID WINAPI OnGraphicEvent(EVENT_RECORD* rec) {
    if (rec) app::g_presentCount.fetch_add(1, std::memory_order_relaxed);
}

// Запускает real-time трассировку и блокирует до остановки сессии.
void EtwMonitorLoop() {
    // ВАЖНО: g_sessionName — это указатель, поэтому длину считаем через wcslen.
    const size_t nameChars = wcslen(app::g_sessionName) + 1;
    const size_t bufSize = sizeof(EVENT_TRACE_PROPERTIES) +
                           nameChars * sizeof(wchar_t);
    std::vector<BYTE> buffer(bufSize, 0);
    auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data());

    props->Wnode.BufferSize = static_cast<ULONG>(bufSize);
    props->Wnode.Flags      = WNODE_FLAG_TRACED_GUID;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    props->LogFileMode      = EVENT_TRACE_REAL_TIME_MODE;
    props->FlushTimer       = 1;
    props->BufferSize       = 256;
    props->MinimumBuffers   = 2;
    props->MaximumBuffers   = 16;
    // Имя сессии пишем сразу за структурой.
    wcscpy_s(reinterpret_cast<wchar_t*>(props + 1), nameChars,
             app::g_sessionName);

    TRACEHANDLE session = 0;
    ULONG st = StartTraceW(&session, app::g_sessionName, props);
    if (st == ERROR_ALREADY_EXISTS) {
        // Запущена старая сессия — останавливаем и создаём заново.
        ControlTraceW(session, app::g_sessionName, props, EVENT_TRACE_CONTROL_STOP);
        st = StartTraceW(&session, app::g_sessionName, props);
    }
    if (st != ERROR_SUCCESS) return; // нет прав — FPS просто не считается
    app::g_sessionTrace = session;

    // Включаем поставщика DxgKrnl на информационном уровне.
    EnableTraceEx2(session, &kDxgKrnl, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                   TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);

    // Подключаем потребителя в real-time.
    EVENT_TRACE_LOGFILEW logFile{};
    logFile.LoggerName          = const_cast<LPWSTR>(app::g_sessionName);
    logFile.ProcessTraceMode    = PROCESS_TRACE_MODE_REAL_TIME |
                                  PROCESS_TRACE_MODE_EVENT_RECORD;
    logFile.EventRecordCallback = OnGraphicEvent;
    TRACEHANDLE consumer = OpenTraceW(&logFile);
    if (!consumer) {
        ControlTraceW(session, app::g_sessionName, props, EVENT_TRACE_CONTROL_STOP);
        return;
    }

    // Блокирующий вызов: вернётся, когда сессия будет остановлена.
    (void)ProcessTrace(&consumer, 1, nullptr, nullptr);

    CloseTrace(consumer);
    ControlTraceW(session, app::g_sessionName, props, EVENT_TRACE_CONTROL_STOP);
}

// Останавливает ETW-сессию из нити GUI при выходе.
void StopEtw() {
    if (app::g_sessionTrace) {
        EVENT_TRACE_PROPERTIES props{};
        ControlTraceW(app::g_sessionTrace, app::g_sessionName, &props,
                      EVENT_TRACE_CONTROL_STOP);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// 4. Фоновый поток мониторинга: температуры, загрузка, RAM, FPS.
// ---------------------------------------------------------------------------
namespace {

void MonitorLoop() {
    // COM обязателен для WMI-запросов температур на ЭТОМ потоке.
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)hrCom;

    double lastPresent = 0.0;
    auto lastSampleTime = std::chrono::steady_clock::now();
    int tempCounter = 0; // температуры обновляем реже (WMI дорого)

    while (app::g_running.load()) {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - lastSampleTime).count();

        // 4.1. Загрузка ядер и суммарная загрузка CPU.
        hw::LiveStats stats;
        hw::sampleCpuLoad(stats);
        app::g_overlay.cpuLoad.store(stats.cpuTotal);

        // 4.2. RAM.
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms))
            app::g_overlay.ramUsedGB.store(
                static_cast<double>(ms.ullTotalPhys - ms.ullAvailPhys) /
                (1024.0 * 1024.0 * 1024.0));

        // 4.3. Температуры — каждые ~1.2 секунды.
        if (++tempCounter >= 6) {
            tempCounter = 0;
            double cpuT = hw::readCpuTempViaWmi();
            double gpuT = hw::readGpuTempViaWmi();
            if (cpuT >= -50.0) app::g_overlay.cpuTemp.store(cpuT);
            if (gpuT >= -50.0) app::g_overlay.gpuTemp.store(gpuT);
        }

        // 4.4. FPS из счётчика ETW.
        unsigned long long cur = app::g_presentCount.load(std::memory_order_relaxed);
        if (dt >= 0.5) {
            double fps = static_cast<double>(cur - lastPresent) / dt;
            app::g_overlay.fps.store(fps);
            app::g_overlay.frametime.store(fps > 0.0 ? 1000.0 / fps : 0.0);
            lastPresent = static_cast<double>(cur);
            lastSampleTime = now;
        }

        app::g_overlayWin.updateViews();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (SUCCEEDED(hrCom)) CoUninitialize();
}

// PID целевой игры: из поля EditPid (или 0 = текущий процесс).
DWORD ResolveTargetPid() {
    HWND edit = GetDlgItem(app::GetMainHwnd(), ids::EditPid);
    if (edit) {
        wchar_t buf[64];
        if (GetWindowTextW(edit, buf, 64) > 0) {
            long pid = _wtoi(buf);
            if (pid > 0) return static_cast<DWORD>(pid);
        }
    }
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// 4a. Рабочий поток твиков: очередь задач, поставленных GUI.
//     Позволяет не блокировать окно на «медленных» вызовах (реестр, службы).
// ---------------------------------------------------------------------------
namespace {

void TweakWorkerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(app::g_tweakMutex);
            app::g_tweakCv.wait(lock, [] {
                return !app::g_tweakQueue.empty() || !app::g_tweakRunning.load();
            });
            if (app::g_tweakQueue.empty() && !app::g_tweakRunning.load())
                break; // сигнал остановки
            task = std::move(app::g_tweakQueue.front());
            app::g_tweakQueue.erase(app::g_tweakQueue.begin());
        }
        // Задача выполняется вне mutex — не блокируем постановщика.
        task();
    }
}

// Ставит задачу твика в очередь рабочего потока.
void PostTweak(std::function<void()>&& task) {
    {
        std::lock_guard<std::mutex> lock(app::g_tweakMutex);
        app::g_tweakQueue.push_back(std::move(task));
    }
    app::g_tweakCv.notify_one();
}

} // namespace

// ---------------------------------------------------------------------------
// 5. Служебные утилиты GUI: лог, статус-строки.
// ---------------------------------------------------------------------------
namespace {

void Log(const std::wstring& text) {
    HWND logEdit = GetDlgItem(app::GetMainHwnd(), ids::EditLog);
    if (!logEdit) return;
    int len = GetWindowTextLengthW(logEdit);
    SendMessageW(logEdit, EM_SETSEL, len, len);
    SendMessageW(logEdit, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(logEdit, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(L"\r\n"));
}

void SetStatus(const std::wstring& text) {
    HWND st = GetDlgItem(app::GetMainHwnd(), ids::StaticStats);
    if (st) SetWindowTextW(st, text.c_str());
}

// Записывает результат твика в лог.
void Report(const wchar_t* what, bool ok) {
    std::wstring msg = std::wstring(ok ? L"[OK] " : L"[!!] ") + what;
    if (!ok) msg += L" — " + opt::lastErrorText();
    Log(msg);
}

} // namespace

// ---------------------------------------------------------------------------
// 6. Действия кнопок.
// ---------------------------------------------------------------------------
namespace {

// Снимает слепок реестра и сохраняет его в файл — точка отката для Restore.
void SnapshotBeforeChanges() {
    app::g_snapshot = backup::collectSnapshot();
    backup::saveSnapshotToFile(app::g_snapshot, backup::defaultBackupPath());
}

void ApplyAllTweaks() {
    PostTweak([] {
        Log(L">>> Применяем полный комплект твиков...");
        SnapshotBeforeChanges();

        Report(L"Очистка standby-памяти", opt::clearStandbyMemory());
        Report(L"Game Bar выключен", opt::disableGameBar());
        Report(L"Game DVR выключен", opt::disableGameDvr());
        Report(L"Ускорение мыши снято", opt::disableMouseAccel());
        Report(L"Nagle отключён", opt::disableNagle());
        Report(L"Фоновые службы остановлены", opt::disableBackgroundServices());
        Report(L"Таймер 1 мс + disabledynamictick", opt::setHighResolutionTimer());

        // Производительность CPU/GPU.
        Report(L"План питания: макс. производительность", opt::setHighPerformancePowerPlan());
        Report(L"Разпарковка ядер (мин. 100%)", opt::maximizeProcessorPerformance());
        Report(L"Boost-режим: агрессивный", opt::disablePowerThrottling());
        Report(L"Игровой режим Windows", opt::enableGameMode());
        Report(L"HAGS (после перезагрузки)", opt::enableHags());

        app::g_gamePid = ResolveTargetPid();
        bool ok = opt::setGamePriority(app::g_gamePid);
        Report(L"Приоритет HIGH_PRIORITY_CLASS", ok);
        if (ok) Report(L"Запрет фоновых ядер", opt::setGameAffinity(app::g_gamePid, true));
        Log(L">>> Готово. Бэкап: " + backup::defaultBackupPath());
    });
}

void RestoreAll() {
    PostTweak([] {
        Log(L">>> Восстанавливаем исходные настройки...");

        std::vector<backup::RegValue> snap = app::g_snapshot;
        if (snap.empty()) backup::loadSnapshotFromFile(snap, backup::defaultBackupPath());
        if (!snap.empty()) {
            backup::restoreSnapshot(snap);
            Report(L"Записи реестра восстановлены", true);
        } else {
            Report(L"Слепок реестра не найден", false);
        }

        // Приоритет и маска текущего/применённого процесса.
        backup::restoreProcessPriority(nullptr);
        if (app::g_gamePid) {
            HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, app::g_gamePid);
            if (h) { backup::restoreProcessAffinity(h); CloseHandle(h); }
        }

        Report(L"Ускорение мыши возвращено", opt::enableMouseAccel());
        Report(L"Службы возвращены", opt::restoreBackgroundServices());
        Report(L"Таймер возвращён", opt::restoreTimerSettings());

        // Откат производительности CPU/GPU.
        Report(L"План питания: сбалансированный", opt::restoreBalancedPowerPlan());
        Report(L"Минимальное состояние ядер", opt::restoreProcessorPerformance());
        Report(L"Boost-режим стандартный", opt::restorePowerThrottling());
        Report(L"HAGS восстановлен", opt::restoreHags());
        Log(L">>> Все изменения откачены.");
    });
}

// Вводные: HTTP-запросчика нет, всё это только WinAPI.
void DoClearMemory() {
    PostTweak([] {
        SnapshotBeforeChanges();
        Report(L"Standby-память очищена", opt::clearStandbyMemory());
    });
}

void DoGameBar() {
    PostTweak([] {
        SnapshotBeforeChanges();
        Report(L"Game Bar отключён", opt::disableGameBar());
    });
}

void DoDvr() {
    PostTweak([] {
        SnapshotBeforeChanges();
        Report(L"Game DVR отключён", opt::disableGameDvr());
    });
}

void DoMouse() {
    PostTweak([] {
        SnapshotBeforeChanges();
        Report(L"Ускорение мыши снято", opt::disableMouseAccel());
    });
}

void DoNagle() {
    PostTweak([] {
        SnapshotBeforeChanges();
        Report(L"Nagle отключён", opt::disableNagle());
    });
}

void DoServices() {
    PostTweak([] {
        SnapshotBeforeChanges();
        Report(L"Фоновые службы остановлены", opt::disableBackgroundServices());
    });
}

void DoTimer() {
    PostTweak([] {
        SnapshotBeforeChanges();
        Report(L"Таймер настроен", opt::setHighResolutionTimer());
    });
}

void DoPriority() {
    PostTweak([] {
        app::g_gamePid = ResolveTargetPid();
        bool ok = opt::setGamePriority(app::g_gamePid);
        Report(L"Приоритет HIGH_PRIORITY_CLASS", ok);
        if (ok) Report(L"Запрет фоновых ядер", opt::setGameAffinity(app::g_gamePid, false));
        Log(L"Целевой процесс PID=" + std::to_wstring(
            app::g_gamePid ? app::g_gamePid : GetCurrentProcessId()));
    });
}

// ---- Производительность CPU/GPU (безопасные твики) ----
void DoPower() {
    PostTweak([] {
        SnapshotBeforeChanges();
        Report(L"План питания: макс. производительность",
               opt::setHighPerformancePowerPlan());
    });
}

void DoCores() {
    PostTweak([] {
        SnapshotBeforeChanges();
        Report(L"Разпарковка ядер (мин. состояние 100%)",
               opt::maximizeProcessorPerformance());
    });
}

void DoBoost() {
    PostTweak([] {
        SnapshotBeforeChanges();
        Report(L"Boost-режим: агрессивный",
               opt::disablePowerThrottling());
    });
}

void DoHags() {
    PostTweak([] {
        SnapshotBeforeChanges();
        Report(L"HAGS включён (нужна перезагрузка)",
               opt::enableHags());
    });
}

void DoGameMode() {
    PostTweak([] {
        SnapshotBeforeChanges();
        Report(L"Игровой режим Windows включён",
               opt::enableGameMode());
    });
}

void DoXmp(HWND hwnd) {
    // Только инструкция (BIOS): реальный разгон и XMP из Windows не делаем.
    MessageBoxW(hwnd, opt::overclockXmpGuidance().c_str(),
                L"Разгон CPU и XMP/DOCP — инструкция (BIOS)",
                MB_OK | MB_ICONINFORMATION);
}

// ---------------------------------------------------------------------------
// 6a. Пресеты рекомендуемых настроек ПК.
//     Каждый пресет — это проверенная комбинация твиков под свой сценарий.
// ---------------------------------------------------------------------------
const wchar_t* const kPresetNames[] = {
    L"Максимальный FPS",
    L"Стабильный frametime (онлайн)",
    L"Тихий / стриминг",
    L"Рекомендуемые (безопасные)",
};

void ApplyPresetByIndex(HWND hwnd) {
    HWND combo = GetDlgItem(hwnd, ids::ComboPreset);
    int sel = combo ? static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0)) : -1;
    if (sel < 0 || sel >= 4) return;
    PostTweak([sel] {
        SnapshotBeforeChanges();
        DWORD pid = ResolveTargetPid();
        Log(std::wstring(L">>> Пресет: ") + kPresetNames[sel]);

        switch (sel) {
        case 0: // Максимальный FPS — весь агрессивный набор.
            Report(L"Standby-память", opt::clearStandbyMemory());
            Report(L"Game Bar", opt::disableGameBar());
            Report(L"Game DVR", opt::disableGameDvr());
            Report(L"Ускорение мыши", opt::disableMouseAccel());
            Report(L"Nagle", opt::disableNagle());
            Report(L"Фоновые службы", opt::disableBackgroundServices());
            Report(L"Таймер 1 мс", opt::setHighResolutionTimer());
            Report(L"План питания: макс.", opt::setHighPerformancePowerPlan());
            Report(L"Разпарковка ядер", opt::maximizeProcessorPerformance());
            Report(L"Boost агрессивный", opt::disablePowerThrottling());
            Report(L"Игровой режим", opt::enableGameMode());
            Report(L"HAGS (после перезагрузки)", opt::enableHags());
            app::g_gamePid = pid;
            Report(L"Приоритет HIGH", opt::setGamePriority(pid));
            Report(L"Запрет фоновых ядер", opt::setGameAffinity(pid, true));
            break;

        case 1: // Стабильный frametime — только то, что влияет на ровность кадров.
            Report(L"Таймер 1 мс", opt::setHighResolutionTimer());
            Report(L"Nagle", opt::disableNagle());
            Report(L"План питания: макс.", opt::setHighPerformancePowerPlan());
            Report(L"Разпарковка ядер", opt::maximizeProcessorPerformance());
            Report(L"Boost агрессивный", opt::disablePowerThrottling());
            Report(L"Игровой режим", opt::enableGameMode());
            app::g_gamePid = pid;
            Report(L"Приоритет HIGH", opt::setGamePriority(pid));
            Report(L"Запрет фоновых ядер", opt::setGameAffinity(pid, true));
            break;

        case 2: // Тихий / стриминг — убираем фон, но без агрессии к железу.
            Report(L"Game Bar", opt::disableGameBar());
            Report(L"Game DVR", opt::disableGameDvr());
            Report(L"Фоновые службы", opt::disableBackgroundServices());
            Report(L"Standby-память", opt::clearStandbyMemory());
            Report(L"Игровой режим", opt::enableGameMode());
            break;

        case 3: // Рекомендуемые (безопасные) — минимум изменений.
            Report(L"Игровой режим", opt::enableGameMode());
            Report(L"План питания: макс.", opt::setHighPerformancePowerPlan());
            Report(L"Разпарковка ядер", opt::maximizeProcessorPerformance());
            break;
        }
        Log(L">>> Пресет применён.");
    });
}

void DoApplyPreset(HWND hwnd) { ApplyPresetByIndex(hwnd); }

void DoOverlayToggle(HWND hwnd) {
    app::g_overlayVisible = !app::g_overlayVisible;
    app::g_overlayWin.show(app::g_overlayVisible);
    Log(app::g_overlayVisible ? L"Оверлей показан" : L"Оверлей скрыт");
    HWND btn = GetDlgItem(hwnd, ids::BtnOverlay);
    if (btn) SetWindowTextW(btn,
        app::g_overlayVisible ? L"Скрыть оверлей" : L"Показать оверлей");
}

void DoSetOpacity(HWND hwnd) {
    HWND edit = GetDlgItem(hwnd, ids::EditOpacity);
    if (!edit) return;
    wchar_t buf[32];
    int v = (GetWindowTextW(edit, buf, 32) > 0) ? _wtoi(buf) : 235;
    v = v < 0 ? 0 : (v > 255 ? 255 : v);
    app::g_overlayWin.setOpacity(v);
    Log(L"Прозрачность оверлея: " + std::to_wstring(v));
}

void DoPickColor(HWND hwnd) {
    static COLORREF customColors[16] = {0};
    CHOOSECOLORW cc{ sizeof(cc), hwnd };
    theme::Color cur = app::g_overlayWin.theme().accent;
    cc.lpCustColors = customColors;
    cc.rgbResult = RGB(cur.r(), cur.g(), cur.b());
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (ChooseColorW(&cc)) {
        theme::Theme t = app::g_overlayWin.theme();
        t.accent.setRGB(GetRValue(cc.rgbResult), GetGValue(cc.rgbResult),
                        GetBValue(cc.rgbResult));
        app::g_overlayWin.setTheme(t);
        theme::setActiveTheme(t);
        theme::saveThemeToFile(t, theme::defaultThemePath());
        Log(L"Акцентный цвет изменён и сохранён: " + theme::defaultThemePath());
    }
}

void ApplyThemePreset(HWND hwnd) {
    HWND combo = GetDlgItem(hwnd, ids::ComboTheme);
    int sel = combo ? static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0)) : -1;
    if (sel < 0) return;
    const auto& vec = theme::presets();
    if (static_cast<size_t>(sel) >= vec.size()) return;
    theme::Theme t;
    t.applyPreset(vec[sel]);
    app::g_overlayWin.setTheme(t);
    theme::setActiveTheme(t);
    theme::saveThemeToFile(t, theme::defaultThemePath());
    Log(L"Применён пресет «" + vec[sel].name + L"»");
}

} // namespace

// ---------------------------------------------------------------------------
// 7. Генерация элементов главного окна и создание самого окна.
// ---------------------------------------------------------------------------
namespace {

// Компактный хелпер создания дочернего контрола.
HWND MkWnd(HWND parent, const wchar_t* cls, const wchar_t* txt,
           DWORD style, int x, int y, int w, int h, int id) {
    return CreateWindowExW(0, cls, txt, style | WS_CHILD | WS_VISIBLE,
                           x, y, w, h, parent, reinterpret_cast<HMENU>(INT_PTR(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

} // namespace

// Обработчик сообщений главного окна.
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Запускаем фоновые потоки: мониторинг, ETW и рабочий поток твиков.
            app::g_monitorThread = std::thread(MonitorLoop);
            app::g_etwThread    = std::thread(EtwMonitorLoop);
            app::g_tweakThread  = std::thread(TweakWorkerLoop);
            SetTimer(hwnd, 1, 500, nullptr); // живой статус-таймер
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            switch (id) {
                case ids::BtnApplyAll: ApplyAllTweaks();        break;
                case ids::BtnRestore:  RestoreAll();            break;
                case ids::BtnClearMem: DoClearMemory();         break;
                case ids::BtnPriority: DoPriority();            break;
                case ids::BtnGameBar:  DoGameBar();             break;
                case ids::BtnDvr:      DoDvr();                 break;
                case ids::BtnMouse:    DoMouse();               break;
                case ids::BtnNagle:    DoNagle();               break;
                case ids::BtnServices: DoServices();            break;
                case ids::BtnTimer:    DoTimer();               break;
                case ids::BtnOverlay:  DoOverlayToggle(hwnd);   break;
                case ids::BtnColor:    DoPickColor(hwnd);       break;
                case ids::BtnOpacity:  DoSetOpacity(hwnd);      break;
                case ids::BtnPower:    DoPower();               break;
                case ids::BtnCores:    DoCores();               break;
                case ids::BtnBoost:    DoBoost();               break;
                case ids::BtnHags:     DoHags();                break;
                case ids::BtnGameMode: DoGameMode();            break;
                case ids::BtnXmp:      DoXmp(hwnd);             break;
                case ids::BtnApplyPreset: DoApplyPreset(hwnd);  break;
                case ids::ComboTheme:
                    if (HIWORD(wParam) == CBN_SELCHANGE) ApplyThemePreset(hwnd);
                    break;
                default: break;
            }
            return 0;
        }
        case WM_TIMER: {
            wchar_t buf[256];
            std::swprintf(buf, 256,
                L"FPS %4.0f | %4.1f ms | CPU %5.1f C | GPU %5.1f C | Load %3.0f%% | RAM %4.1f ГБ",
                app::g_overlay.fps.load(), app::g_overlay.frametime.load(),
                app::g_overlay.cpuTemp.load(), app::g_overlay.gpuTemp.load(),
                app::g_overlay.cpuLoad.load(), app::g_overlay.ramUsedGB.load());
            SetStatus(buf);
            return 0;
        }
        case WM_DESTROY: {
            KillTimer(hwnd, 1);
            app::g_running = false;      // стоп мониторинга
            app::g_tweakRunning = false; // стоп рабочего потока твиков
            app::g_tweakCv.notify_all();
            StopEtw();
            // Threads отцепляем, а не ждём join(): отдельный WMI-вызов или
            // ProcessTrace теоретически может заблокироваться на секунды;
            // join() в WndProc недопустимо — GUI виснет при закрытии.
            // После выхода из wWinMain процесс завершится, потоки убьются
            // автоматически, никаких твиков они больше не выполняют.
            if (app::g_monitorThread.joinable()) app::g_monitorThread.detach();
            if (app::g_etwThread.joinable())     app::g_etwThread.detach();
            if (app::g_tweakThread.joinable())   app::g_tweakThread.detach();
            app::g_overlayWin.show(false);
            PostQuitMessage(0);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Создаёт главное окно со всеми элементами управления.
static HWND BuildMainWindow(HINSTANCE hInst) {
    // Класс окна.
    WNDCLASSW wc{};
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"LeakOptMain";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    ATOM cls = RegisterClassW(&wc);
    if (cls == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;

    HWND hwnd = CreateWindowExW(0, L"LeakOptMain",
                                L"LeakOptimizator — системный твикер",
                                WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 620, 820,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return nullptr;

    // Тёмный заголовок окна (DWM dark mode) — окно в едином ч/б стиле.
    // DWMWA_USE_IMMERSIVE_DARK_MODE = 20 (Win10 2004+), = 19 на старых.
    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));

    // Регистрируем HWND в глобалах для доступа из Log/SetStatus.
    app::SetMainHwnd(hwnd);

    // Заголовки секций.
    MkWnd(hwnd, L"STATIC", L"ЖЕЛЕЗО", 0, 14, 10, 190, 12, 0);
    MkWnd(hwnd, L"STATIC", L"ТВИКИ", 0, 228, 10, 370, 12, 0);
    MkWnd(hwnd, L"STATIC", L"ОВЕРЛЕЙ", 0, 14, 250, 400, 12, 0);
    MkWnd(hwnd, L"STATIC", L"ПРЕСЕТЫ И ПРОИЗВОДИТЕЛЬНОСТЬ", 0, 14, 418, 400, 12, 0);
    MkWnd(hwnd, L"STATIC", L"МОНИТОРИНГ", 0, 14, 578, 400, 12, 0);

    // Инфо о железе.
    MkWnd(hwnd, L"STATIC", L"", SS_LEFT | WS_BORDER, 14, 26, 196, 120, ids::StaticHw);

    // Кнопки твиков (левая колонка).
    MkWnd(hwnd, L"BUTTON", L"Очистить Standby-память", BS_PUSHBUTTON, 14, 152, 190, 28, ids::BtnClearMem);
    MkWnd(hwnd, L"BUTTON", L"Приоритет HIGH + ядра",   BS_PUSHBUTTON, 14, 184, 190, 28, ids::BtnPriority);
    MkWnd(hwnd, L"STATIC", L"PID игры (0=текущий)",    SS_RIGHT,      14, 214, 84, 18, 0);
    MkWnd(hwnd, L"EDIT",   L"0", ES_NUMBER | WS_BORDER, 100, 213, 104, 20, ids::EditPid);

    // Кнопки твиков (правая колонка).
    MkWnd(hwnd, L"BUTTON", L"Отключить Game Bar",        BS_PUSHBUTTON, 228, 26, 180, 28, ids::BtnGameBar);
    MkWnd(hwnd, L"BUTTON", L"Отключить Game DVR",        BS_PUSHBUTTON, 228, 58, 180, 28, ids::BtnDvr);
    MkWnd(hwnd, L"BUTTON", L"Убрать ускорение мыши",     BS_PUSHBUTTON, 228, 90, 180, 28, ids::BtnMouse);
    MkWnd(hwnd, L"BUTTON", L"Nagle (TCP-оптимизация)",   BS_PUSHBUTTON, 228, 122, 180, 28, ids::BtnNagle);
    MkWnd(hwnd, L"BUTTON", L"Отключить фоновые службы",  BS_PUSHBUTTON, 228, 154, 180, 28, ids::BtnServices);
    MkWnd(hwnd, L"BUTTON", L"Таймер 1 мс + фикс тика",   BS_PUSHBUTTON, 228, 186, 180, 28, ids::BtnTimer);

    // Глобальные действия.
    MkWnd(hwnd, L"BUTTON", L"Применить всё",              BS_PUSHBUTTON, 14, 252, 190, 34, ids::BtnApplyAll);
    MkWnd(hwnd, L"BUTTON", L"ВОССТАНОВИТЬ (Rollback)",     BS_PUSHBUTTON, 228, 222, 180, 34, ids::BtnRestore);

    // Оверлей.
    MkWnd(hwnd, L"BUTTON", L"Скрыть оверлей",  BS_PUSHBUTTON, 14, 268, 130, 28, ids::BtnOverlay);
    MkWnd(hwnd, L"STATIC", L"Прозрачность:",   SS_RIGHT,      14, 304, 90, 18, 0);
    MkWnd(hwnd, L"EDIT",   L"235", ES_NUMBER | WS_BORDER,     108, 302, 40, 20, ids::EditOpacity);
    MkWnd(hwnd, L"BUTTON", L"Применить",       BS_PUSHBUTTON, 152, 302, 90, 22, ids::BtnOpacity);
    HWND comboTheme = MkWnd(hwnd, L"COMBOBOX", L"", WS_BORDER | CBS_DROPDOWNLIST,
                            14, 336, 170, 200, ids::ComboTheme);
    for (const auto& p : theme::presets())
        SendMessageW(comboTheme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.name.c_str()));
    SendMessageW(comboTheme, CB_SETCURSEL, 0, 0);
    MkWnd(hwnd, L"BUTTON", L"Color Picker (акцент)", BS_PUSHBUTTON, 190, 336, 130, 24, ids::BtnColor);

    // Пресеты рекомендуемых настроек.
    HWND comboPreset = MkWnd(hwnd, L"COMBOBOX", L"", WS_BORDER | CBS_DROPDOWNLIST,
                             14, 438, 190, 200, ids::ComboPreset);
    for (const wchar_t* n : kPresetNames)
        SendMessageW(comboPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n));
    SendMessageW(comboPreset, CB_SETCURSEL, 0, 0);
    MkWnd(hwnd, L"BUTTON", L"Применить пресет", BS_PUSHBUTTON, 228, 438, 180, 28, ids::BtnApplyPreset);

    // Производительность CPU/GPU (безопасные твики).
    MkWnd(hwnd, L"BUTTON", L"План питания: макс. произв.", BS_PUSHBUTTON, 14, 470, 190, 28, ids::BtnPower);
    MkWnd(hwnd, L"BUTTON", L"Разпарковка ядер (мин. 100%)", BS_PUSHBUTTON, 228, 470, 180, 28, ids::BtnCores);
    MkWnd(hwnd, L"BUTTON", L"Boost: агрессивный",          BS_PUSHBUTTON, 14, 502, 190, 28, ids::BtnBoost);
    MkWnd(hwnd, L"BUTTON", L"HAGS (планирование GPU)",     BS_PUSHBUTTON, 228, 502, 180, 28, ids::BtnHags);
    MkWnd(hwnd, L"BUTTON", L"Игровой режим Windows",       BS_PUSHBUTTON, 14, 534, 190, 28, ids::BtnGameMode);
    MkWnd(hwnd, L"BUTTON", L"XMP / разгон (инструкция)",   BS_PUSHBUTTON, 228, 534, 180, 28, ids::BtnXmp);

    // Статус и лог.
    MkWnd(hwnd, L"STATIC", L"", SS_LEFT | WS_BORDER, 14, 596, 396, 20, ids::StaticStats);
    MkWnd(hwnd, L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY |
          ES_AUTOVSCROLL | WS_VSCROLL, 14, 624, 570, 160, ids::EditLog);

    return hwnd;
}

// ---------------------------------------------------------------------------
// 8. Точка входа.
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    // COM нужен для WMI (температуры).
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return 1;

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    // Загружаем тему из файла (если есть), иначе — Neon Blue по умолчанию.
    theme::Theme startupTheme;
    if (theme::loadThemeFromFile(startupTheme, theme::defaultThemePath())) {
        theme::setActiveTheme(startupTheme);
    } else {
        theme::Theme def; def.applyPreset(theme::presets()[4]); // Monochrome
        theme::setActiveTheme(def);
    }
    app::g_overlayWin.setTheme(theme::activeTheme());

    // Оверлей создаём до главного окна, чтобы он был поверх.
    if (app::g_overlayWin.create(L"LeakOptimizator — оверлей")) {
        app::g_overlayWin.show(true);
        app::g_overlayWin.setData(&app::g_overlay);
        app::g_overlayWin.setClickThrough(true);
    }

    HWND hwnd = BuildMainWindow(hInst);
    if (!hwnd) {
        CoUninitialize();
        return 1;
    }

    // Заполняем строку железа.
    hw::HardwareInfo info = hw::queryHardware();
    {
        wchar_t buf[1024];
        std::swprintf(buf, 1024,
            L"CPU: %ls\r\nВендор: %ls\r\nЧастота: %u МГц\r\nПотоков: %u\r\nGPU: %ls\r\nRAM: %ls",
            info.cpuName.c_str(), info.cpuVendor.c_str(), info.cpuBaseMhz,
            info.cpuLogical, info.gpuName.c_str(), info.ramText.c_str());
        SetWindowTextW(GetDlgItem(hwnd, ids::StaticHw), buf);
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Цикл сообщений.
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}