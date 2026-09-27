// optimizer.cpp — реализация агрессивных твиков через чистый WinAPI.
#include "optimizer.h"

#include <tlhelp32.h>
#include <winnt.h>
#include <winternl.h>
#include <winsvc.h>
#include <mmsystem.h>

#include <cstdio>

namespace opt {

// Глобальное хранилище «последней ошибки» (перезаписывается каждым твиком).
static std::wstring g_lastError;

std::wstring lastErrorText() { return g_lastError; }

static void setError(const wchar_t* what, DWORD code = ::GetLastError()) {
    wchar_t buf[64];
    std::swprintf(buf, 64, L"%s (код %lu)", what, code);
    g_lastError = buf;
}
static void clearError() { g_lastError.clear(); }

// ---------------------------------------------------------------------------
// 1. Очистка Standby-кэша памяти через NtSetSystemInformation.
//    Классика игровых оптимизаторов. Значения: SystemMemoryListInformation=80,
//    SystemMemoryListCommand::MemoryPurgeStandbyList=4.
// ---------------------------------------------------------------------------
bool clearStandbyMemory() {
    clearError();
    typedef NTSTATUS (NTAPI *tNtSetSystemInformation)(SYSTEM_INFORMATION_CLASS,
                                                      PVOID, ULONG);
    static auto pfn = reinterpret_cast<tNtSetSystemInformation>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                       "NtSetSystemInformation"));
    if (!pfn) { setError(L"Не найден NtSetSystemInformation"); return false; }

    // Параметр — команда (4 = PurgeStandbyList).
    ULONG command = 4;
    LONG st = pfn(static_cast<SYSTEM_INFORMATION_CLASS>(80),
                  &command, sizeof(command));
    if (st != 0) { setError(L"Ошибка очистки standby-памяти", static_cast<DWORD>(st)); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// 2. Приоритет и привязка игрового процесса.
// ---------------------------------------------------------------------------
// Ищем первый видимый игровой процесс по имени exe. Приложению передаётся
// PID; если 0 — используем процесс самой утилиты.
static DWORD g_gamePid = 0;

DWORD currentGamePid() { return g_gamePid; }

static HANDLE openProcess(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION |
                           PROCESS_SUSPEND_RESUME, FALSE, pid);
    if (!h) setError(L"Не удалось открыть процесс");
    return h;
}

bool setGamePriority(DWORD gamePid) {
    clearError();
    if (gamePid == 0) gamePid = GetCurrentProcessId();
    g_gamePid = gamePid;
    HANDLE h = openProcess(gamePid);
    if (!h) return false;
    bool ok = SetPriorityClass(h, HIGH_PRIORITY_CLASS) != 0;
    if (!ok) setError(L"Не удалось повысить приоритет");
    CloseHandle(h);
    return ok;
}

bool setGameAffinity(DWORD gamePid, bool disableBackgroundCores) {
    clearError();
    if (gamePid == 0) gamePid = GetCurrentProcessId();
    HANDLE h = openProcess(gamePid);
    if (!h) return false;

    if (!disableBackgroundCores) {
        // Возвращаем полную маску из групп доступности.
        DWORD_PTR sys = 0, proc = 0;
        GetProcessAffinityMask(h, &proc, &sys);
        bool ok = sys && SetProcessAffinityMask(h, sys) != 0;
        CloseHandle(h);
        if (!ok) setError(L"Не удалось снять маску приоритета");
        return ok;
    }

    // Запрещаем «фоновым» ядрам (вторая половина логических) получать потоки.
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    unsigned total = si.dwNumberOfProcessors;
    // Маска всех логических процессоров (до 64 поддерживаем для простоты).
    if (total > 64) total = 64;
    DWORD_PTR all = (total == 64) ? ~static_cast<DWORD_PTR>(0)
                                  : (static_cast<DWORD_PTR>(1) << total) - 1;

    unsigned keep = (total + 1) / 2;            // оставляем первую половину
    DWORD_PTR mask = (keep >= 64) ? all
        : (static_cast<DWORD_PTR>(1) << keep) - 1;

    bool ok = SetProcessAffinityMask(h, mask) != 0;
    if (!ok) setError(L"Не удалось задать маску приоритета");
    CloseHandle(h);
    return ok;
}

// ---------------------------------------------------------------------------
// 3. Твики реестра. Все значения завязываем на HKLM/HKCU по стандартным путям.
// ---------------------------------------------------------------------------
static bool writeDword(HKEY base, const wchar_t* path, const wchar_t* name,
                       DWORD value) {
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(base, path, 0, nullptr, 0,
                              KEY_SET_VALUE | KEY_WOW64_64KEY,
                              nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) return false;
    rc = RegSetValueExW(key, name, 0, REG_DWORD,
                        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

bool disableGameBar() {
    clearError();
    bool ok = true;
    ok &= writeDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\GameBar",
                     L"AllowAutoGameMode", 0);
    ok &= writeDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\GameBar",
                     L"AutoGameModeEnabled", 0);
    ok &= writeDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\GameBar",
                     L"UseNexusForGameBarEnabled", 0);
    if (!ok) setError(L"Ошибка записи реестра Game Bar");
    return ok;
}

bool disableGameDvr() {
    clearError();
    bool ok = true;
    ok &= writeDword(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR",
        L"AppCaptureEnabled", 0);
    ok &= writeDword(HKEY_CURRENT_USER, L"System\\GameConfigStore",
        L"GameDVR_Enabled", 0);
    ok &= writeDword(HKEY_CURRENT_USER, L"System\\GameConfigStore",
        L"GameDVR_FSEBehaviorMode", 2);
    if (!ok) setError(L"Ошибка записи реестра Game DVR");
    return ok;
}

bool disableMouseAccel() {
    clearError();
    // MouseSpeed=1 + пороги 0/0 убирает усиление движения («калибровку»).
    bool ok = true;
    ok &= writeDword(HKEY_CURRENT_USER, L"Control Panel\\Mouse", L"MouseSpeed", 1);
    ok &= writeDword(HKEY_CURRENT_USER, L"Control Panel\\Mouse", L"MouseThreshold1", 0);
    ok &= writeDword(HKEY_CURRENT_USER, L"Control Panel\\Mouse", L"MouseThreshold2", 0);
    if (!ok) setError(L"Ошибка записи реестра мыши");
    return ok;
}

bool enableMouseAccel() {
    clearError();
    bool ok = true;
    ok &= writeDword(HKEY_CURRENT_USER, L"Control Panel\\Mouse", L"MouseSpeed", 1);
    ok &= writeDword(HKEY_CURRENT_USER, L"Control Panel\\Mouse", L"MouseThreshold1", 6);
    ok &= writeDword(HKEY_CURRENT_USER, L"Control Panel\\Mouse", L"MouseThreshold2", 10);
    return ok;
}

// ---------------------------------------------------------------------------
// 4. Nagle-алгоритм: применяем TcpAckFrequency=1, TCPNoDelay=1 и
//    TcpDelAckTicks=0 ко всем GUID-интерфейсам Tcpip.
// ---------------------------------------------------------------------------
static bool applyTcpFlags(DWORD ackFreq, DWORD noDelay, DWORD delAckTicks) {
    HKEY ifaceNet = HKEY_LOCAL_MACHINE;
    const wchar_t* basePath =
        L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces";
    HKEY iface = nullptr;
    if (RegOpenKeyExW(ifaceNet, basePath, 0, KEY_READ | KEY_WOW64_64KEY,
                      &iface) != ERROR_SUCCESS) {
        setError(L"Не найден раздел TCP-интерфейсов");
        return false;
    }
    wchar_t subName[256];
    bool anyWritten = false;
    for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(iface, i, subName, &len, nullptr, nullptr,
                          nullptr, nullptr) != ERROR_SUCCESS) break;
        std::wstring full = std::wstring(basePath) + L"\\" + subName;
        if (writeDword(HKEY_LOCAL_MACHINE, full.c_str(), L"TcpAckFrequency", ackFreq) &&
            writeDword(HKEY_LOCAL_MACHINE, full.c_str(), L"TCPNoDelay", noDelay) &&
            writeDword(HKEY_LOCAL_MACHINE, full.c_str(), L"TcpDelAckTicks", delAckTicks)) {
            anyWritten = true;
        }
    }
    RegCloseKey(iface);
    if (!anyWritten) setError(L"Не найден ни один TCP-интерфейс");
    return anyWritten;
}

bool disableNagle() {
    clearError();
    return applyTcpFlags(1, 1, 0);
}

bool enableNagle() {
    clearError();
    // Стандартные значения Windows: ack каждые 2 пакета, задержка 1 (тик=100мс).
    return applyTcpFlags(2, 0, 1);
}

// ---------------------------------------------------------------------------
// 5. Службы.
// ---------------------------------------------------------------------------
const std::vector<ServiceJob>& targetServices() {
    static const std::vector<ServiceJob> svc = {
        { L"WSearch",          L"Поиск Windows (WSearch)" },
        { L"SysMain",          L"SysMain (PreFetch/Superfetch)" },
        { L"DiagTrack",        L"DiagTrack (Телеметрия)" },
        { L"MapsBroker",       L"MapsBroker (Карты Windows)" },
    };
    return svc;
}

// Устанавливает Start-режим службы и (при старте!=0) пытается её остановить.
static bool applyService(const ServiceJob& svc, DWORD startValue, bool wantRunning) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) { setError(L"Ошибка доступа к менеджеру служб"); return false; }

    SC_HANDLE s = OpenServiceW(scm, svc.name, SERVICE_ALL_ACCESS);
    if (!s) {
        // Служба отсутствует на этой системе — считаем успехом (нечего трогать).
        CloseServiceHandle(scm);
        return true;
    }

    bool ok = false;
    // Меняем автозапуск на уровень startValue (4=Запрещено, 2=Авто).
    if (ChangeServiceConfigW(s, SERVICE_NO_CHANGE, startValue, SERVICE_NO_CHANGE,
                             nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                             nullptr)) {
        ok = true;
    } else {
        setError(L"Не удалось изменить автозапуск службы");
    }

    // Управление текущим состоянием.
    if (wantRunning) {
        SERVICE_STATUS ss{};
        if (ok) StartServiceW(s, 0, nullptr); // может быть уже запущена
    } else {
        SERVICE_STATUS ss{};
        ControlService(s, SERVICE_CONTROL_STOP, &ss); // если запущена
    }
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return ok;
}

bool disableBackgroundServices() {
    clearError();
    bool all = true;
    for (const auto& s : targetServices())
        all &= applyService(s, SERVICE_DISABLED, false);

    // Ставим Start=4 (Запрещено) дополнительно через реестр, т.к. некоторые
    // службы не дают отключить себя через SCM без прав.
    for (const auto& s : targetServices()) {
        std::wstring path = L"SYSTEM\\CurrentControlSet\\Services\\" + std::wstring(s.name);
        all &= writeDword(HKEY_LOCAL_MACHINE, path.c_str(), L"Start", 4);
    }
    return all;
}

bool restoreBackgroundServices() {
    clearError();
    bool all = true;
    for (const auto& s : targetServices()) {
        std::wstring path = L"SYSTEM\\CurrentControlSet\\Services\\" + std::wstring(s.name);
        writeDword(HKEY_LOCAL_MACHINE, path.c_str(), L"Start", SERVICE_AUTO_START);
        all &= applyService(s, SERVICE_AUTO_START, true);
    }
    return all;
}

// ---------------------------------------------------------------------------
// 6. Таймер высокого разрешения.
// ---------------------------------------------------------------------------
// timeBeginPeriod/timeEndPeriod — парные вызовы для текущей нити. Держим
// счётчик, чтобы не откатить раньше, чем нужно.
static bool g_timerArmed = false;
typedef UINT (WINAPI *TimePeriodProc)(UINT);
static TimePeriodProc g_winmmTimeBegin = nullptr;
static TimePeriodProc g_winmmTimeEnd = nullptr;

bool setHighResolutionTimer() {
    clearError();
    if (!g_winmmTimeBegin)
        g_winmmTimeBegin = reinterpret_cast<TimePeriodProc>(
            GetProcAddress(GetModuleHandleW(L"winmm.dll"), "timeBeginPeriod"));
    if (!g_winmmTimeBegin) { setError(L"Нет winmm"); return false; }

    if (g_winmmTimeBegin(1) != TIMERR_NOERROR) {
        setError(L"Не удалось установить разрешение таймера 1 мс");
        return false;
    }
    g_timerArmed = true;

    // Отключаем динамический тик (только Windows 10/11, требует админ-прав).
    // Фон вызова — минимальный процесс powershell.exe.
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    std::wstring cmd =
        L"powershell.exe -NoProfile -Command "
        L"bcdedit /set disabledynamictick yes";
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return true;
}

bool restoreTimerSettings() {
    clearError();
    if (g_timerArmed) {
        if (!g_winmmTimeEnd)
            g_winmmTimeEnd = reinterpret_cast<TimePeriodProc>(
                GetProcAddress(GetModuleHandleW(L"winmm.dll"), "timeEndPeriod"));
        if (g_winmmTimeEnd) g_winmmTimeEnd(1);
        g_timerArmed = false;
    }

    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    std::wstring cmd =
        L"powershell.exe -NoProfile -Command "
        L"bcdedit /set disabledynamictick no";
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 7. Производительность процессора и GPU (безопасные Windows-настройки).
// ---------------------------------------------------------------------------
namespace {

// Запускает одну команду без окна и ждёт завершения. Возвращает true, если
// процесс запустился (exit-код не проверяем — у powercfg бывают «шумные»).
bool runHidden(const std::wstring& command) {
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"powershell.exe -NoProfile -Command " + command;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace

// План питания «Максимальная производительность» (высокая производительность).
bool setHighPerformancePowerPlan() {
    clearError();
    // SCHEME_MIN — системный алиас High Performance (8c5e7fda-...).
    if (!runHidden(L"powercfg /setactive SCHEME_MIN")) {
        setError(L"Не удалось активировать план питания");
        return false;
    }
    return true;
}

bool restoreBalancedPowerPlan() {
    clearError();
    return runHidden(L"powercfg /setactive SCHEME_BALANCED");
}

// Минимальное состояние процессора 100% — ядра не «паркуются» (не выключаются
// из планировщика). Это устраняет микрофризы от включения/выключения ядер.
bool maximizeProcessorPerformance() {
    clearError();
    bool ok = true;
    ok &= runHidden(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PROCTHROTTLEMIN 100");
    ok &= runHidden(L"powercfg /setactive SCHEME_CURRENT");
    if (!ok) setError(L"Не удалось настроить парковку ядер");
    return ok;
}

bool restoreProcessorPerformance() {
    clearError();
    bool ok = true;
    ok &= runHidden(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PROCTHROTTLEMIN 5");
    ok &= runHidden(L"powercfg /setactive SCHEME_CURRENT");
    return ok;
}

// PERFBOOSTMODE=2 — «агрессивный» boost (раскрывает все ядра для нагрузки).
bool disablePowerThrottling() {
    clearError();
    bool ok = true;
    ok &= runHidden(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PERFBOOSTMODE 2");
    ok &= runHidden(L"powercfg /setactive SCHEME_CURRENT");
    if (!ok) setError(L"Не удалось настроить boost-режим");
    return ok;
}

bool restorePowerThrottling() {
    clearError();
    bool ok = true;
    ok &= runHidden(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PERFBOOSTMODE 0");
    ok &= runHidden(L"powercfg /setactive SCHEME_CURRENT");
    return ok;
}

// HAGS (Hardware Accelerated GPU Scheduling): HwSchMode=2 включает аппаратное
// планирование очередей GPU — снижает задержку. Требует перезагрузки.
bool enableHags() {
    clearError();
    if (!writeDword(HKEY_LOCAL_MACHINE,
                    L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
                    L"HwSchMode", 2)) {
        setError(L"Не удалось записать HwSchMode=2");
        return false;
    }
    return true;
}

bool restoreHags() {
    clearError();
    if (!writeDword(HKEY_LOCAL_MACHINE,
                    L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
                    L"HwSchMode", 1)) {
        setError(L"Не удалось записать HwSchMode=1");
        return false;
    }
    return true;
}

// Игровой режим Windows (разрешаем автоигровой режим).
bool enableGameMode() {
    clearError();
    bool ok = true;
    ok &= writeDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\GameBar",
                     L"AllowAutoGameMode", 1);
    ok &= writeDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\GameBar",
                     L"AutoGameModeEnabled", 1);
    if (!ok) setError(L"Не удалось включить игровой режим");
    return ok;
}

// Текст инструкции по XMP/DOCP и разгону (BIOS). Никаких рискованных MSR.
std::wstring overclockXmpGuidance() {
    return
        L"РАЗГОН CPU И XMP/DOCP — БИОС (не программа!)\r\n\r\n"
        L"1. Перезагрузись и войди в UEFI/BIOS (Del / F2 / F10).\r\n"
        L"2. XMP/DOCP (память):\r\n"
        L"   Intel: AI Tweaker / Extreme Memory Profile (XMP) -> Profile 1\r\n"
        L"   AMD:   OC / Memory -> DOCP / EXPO -> Profile 1\r\n"
        L"3. Разгон CPU (осторожно, риск для железа!):\r\n"
        L"   Intel K: умножитель (Core Ratio) + настройка LLC;\r\n"
        L"   AMD Ryzen: включи PBO (Precision Boost Overdrive) -> Enabled/Advanced;\r\n"
        L"4. Обязательно включи ReBAR/SAM выше разрешения PCIe.\r\n"
        L"5. Тестируй стабильность: OCCT / Cinebench / AIDA64.\r\n"
        L"\r\nПрофили XMP — это заводские безопасные профили памяти (обычно 3200-"
        L"6000 МГц), включать их безопасно. Ручной разгон CPU повышает температуру"
        L"и потребление — делай его только если знаешь, что делаешь.";
}

} // namespace opt