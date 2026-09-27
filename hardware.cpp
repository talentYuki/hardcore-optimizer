// hardware.cpp — реализация детекта железа и замера живых показателей.
#include "hardware.h"

#include <comdef.h>
#include <dxgi.h>
#include <objbase.h>
#include <wbemidl.h>

#include <algorithm>
#include <cstdio>
#include <thread>

namespace hw {

// ---------------------------------------------------------------------------
// Утилита: читает значение из реестра HKLM\HARDWARE\DESCRIPTION\System\
// CentralProcessor\0\ProcessorNameString — имя процессора.
// ---------------------------------------------------------------------------
static std::wstring readCpuNameFromRegistry() {
    HKEY key = nullptr;
    std::wstring name;
    wchar_t buf[256] = {0};
    DWORD size = static_cast<DWORD>(sizeof(buf));
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExW(key, L"ProcessorNameString", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS) {
            name = buf;
        }
        RegCloseKey(key);
    }
    return name;
}

// Определяет производителя CPU по VendorIdentifier в реестре.
static std::wstring readCpuVendorFromRegistry() {
    HKEY key = nullptr;
    wchar_t buf[64] = {0};
    DWORD size = static_cast<DWORD>(sizeof(buf));
    std::wstring vendor;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExW(key, L"VendorIdentifier", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS) {
            vendor = buf;
        }
        RegCloseKey(key);
    }
    if (vendor == L"GenuineIntel") return L"Intel";
    if (vendor == L"AuthenticAMD") return L"AMD";
    return vendor.empty() ? L"Неизвестно" : vendor;
}

// Номинальная частота первого ядра (значение "~MHz") в МГц.
static unsigned readCpuBaseMhzFromRegistry() {
    HKEY key = nullptr;
    DWORD mhz = 0, size = sizeof(mhz), type = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, L"~MHz", nullptr, &type,
                         reinterpret_cast<LPBYTE>(&mhz), &size);
        RegCloseKey(key);
    }
    return mhz;
}

// Регистрируем dxgi.lib — уже в проекте; для чтения GPU используем DXGI.
static HRESULT enumeratePrimaryGpu(std::wstring& name) {
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                    reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) return hr;
    IDXGIAdapter1* adapter = nullptr;
    hr = factory->EnumAdapters1(0, &adapter);
    if (SUCCEEDED(hr)) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            name = desc.Description;
        }
        adapter->Release();
    }
    factory->Release();
    return hr;
}

// Количество логических потоков. Предпочитаем стандартный способ
// std::thread::hardware_concurrency(); GetSystemInfo — резерв на случай 0.
unsigned logicalProcessors() {
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        n = si.dwNumberOfProcessors;
    }
    return n;
}

// ---------------------------------------------------------------------------
// queryHardware — полный снимок: CPU (имя/ядра), GPU, ОЗУ.
// ---------------------------------------------------------------------------
HardwareInfo queryHardware() {
    HardwareInfo info;
    info.cpuName = readCpuNameFromRegistry();
    if (info.cpuName.empty()) info.cpuName = L"Неизвестный процессор";
    info.cpuVendor = readCpuVendorFromRegistry();
    info.cpuBaseMhz = readCpuBaseMhzFromRegistry();
    info.cpuMaxMhz = info.cpuBaseMhz;

    info.cpuLogical = logicalProcessors();
    // Физических ядер чисто по WinAPI не получить; аппроксимируем половиной
    // логических (для гипертрейдинга) — честно помечаем в UI как потоки.
    info.cpuCores = (info.cpuLogical + 1) / 2;

    enumeratePrimaryGpu(info.gpuName);
    if (info.gpuName.empty()) info.gpuName = L"Не определено";

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    info.totalRamBytes = ms.ullTotalPhys;
    {
        constexpr double kGig = 1024.0 * 1024.0 * 1024.0;
        double gb = static_cast<double>(info.totalRamBytes) / kGig;
        wchar_t buf[64];
        std::swprintf(buf, 64, L"%.1f ГБ", gb);
        info.ramText = buf;
    }
    return info;
}

// ---------------------------------------------------------------------------
// Загрузка ядер. Используем GetSystemProcessorPerformanceInformation —
// структура, существующая в приватном API, но стабильно работающая на всех
// Windows NT 6+. Собирает время idle и перевод в проценты по разности.
// ---------------------------------------------------------------------------
namespace {
struct CpuSample {
    unsigned long long idle;
    unsigned long long total;
};

// Для дельты между двумя вызовами держим предыдущий снимок в статике.
static std::vector<CpuSample> g_prev;
static FILETIME g_prevIdleOverall{}, g_prevKernel{}, g_prevUser{};
static bool g_hasPrev = false;

bool collectSystemProcessorTime(std::vector<CpuSample>& out) {
    // Запрос выполняется одиночным вызовом (счётчики всех процессоров).
    typedef LONG (WINAPI *PfnGetProcPerfInfo)(DWORD, DWORD, PVOID, PDWORD);
    static auto pfn = reinterpret_cast<PfnGetProcPerfInfo>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                       "NtQuerySystemInformation"));
    if (!pfn) return false;

    // SMT/гипертрейдинг даёт N процессоров.
    unsigned n = logicalProcessors();
    out.assign(n, CpuSample{0, 0});

    // Извлекаем время через структуру SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION.
    struct PerfInfo {
        LARGE_INTEGER idleTime;     // время простоя ядра
        LARGE_INTEGER kernelTime;   // время в ядре (включая idle)
        LARGE_INTEGER userTime;     // время в пользовательском режиме
        LARGE_INTEGER dpcTime;      // DPC-время
        LARGE_INTEGER interruptTime; // время прерываний
        ULONG interruptCount;
        ULONG spare[2];
    };
    std::vector<PerfInfo> perf(n);
    ULONG sizeOut = 0;
    // 8 = SystemProcessorPerformanceInformation.
    LONG st = pfn(8, 0, perf.data(), &sizeOut);
    if (st != 0) return false;
    for (unsigned i = 0; i < n; ++i) {
        out[i].idle = static_cast<unsigned long long>(perf[i].idleTime.QuadPart);
        out[i].total = static_cast<unsigned long long>(perf[i].kernelTime.QuadPart) +
                       static_cast<unsigned long long>(perf[i].userTime.QuadPart);
    }
    return true;
}
} // namespace

void sampleCpuLoad(LiveStats& out) {
    std::vector<CpuSample> now;
    if (!collectSystemProcessorTime(now)) {
        // Падаем в резервный путь — GetSystemTimes (только общая загрузка).
        FILETIME idle{}, kernel{}, user{};
        if (GetSystemTimes(&idle, &kernel, &user) && g_hasPrev) {
            auto toU64 = [](FILETIME ft) {
                return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) |
                       ft.dwLowDateTime;
            };
            unsigned long long idl = 0, ker = 0, usr = 0;
            auto diff = [](unsigned long long a, unsigned long long b) {
                return a > b ? a - b : 0ULL;
            };
            idl = diff(toU64(idle), toU64(g_prevIdleOverall));
            ker = diff(toU64(kernel), toU64(g_prevKernel));
            usr = diff(toU64(user), toU64(g_prevUser));
            unsigned long long total = ker + usr;
            out.cpuTotal = (total) ? (1.0 - static_cast<double>(idl) / static_cast<double>(total)) * 100.0 : 0.0;
        }
        g_prevIdleOverall = idle; g_prevKernel = kernel; g_prevUser = user;
        g_hasPrev = true;
        // Загрузку по ядрам в резервном режиме не отдаём.
        out.coreLoad.clear();
        return;
    }

    // Основной путь: дельта между предыдущим и текущим снимком.
    if (!g_prev.empty() && now.size() == g_prev.size()) {
        out.coreLoad.assign(now.size(), 0.0);
        double totalIdle = 0.0, totalAll = 0.0;
        for (size_t i = 0; i < now.size(); ++i) {
            unsigned long long dIdle = now[i].idle > g_prev[i].idle
                ? now[i].idle - g_prev[i].idle : 0ULL;
            unsigned long long dTotal = now[i].total > g_prev[i].total
                ? now[i].total - g_prev[i].total : 0ULL;
            double load = (dTotal) ? (1.0 - static_cast<double>(dIdle) /
                                       static_cast<double>(dTotal)) * 100.0 : 0.0;
            out.coreLoad[i] = std::clamp(load, 0.0, 100.0);
            totalIdle += static_cast<double>(dIdle);
            totalAll  += static_cast<double>(dTotal);
        }
        out.cpuTotal = (totalAll) ? std::clamp(
            (1.0 - totalIdle / totalAll) * 100.0, 0.0, 100.0) : 0.0;
    }
    g_prev = std::move(now);
}

// ---------------------------------------------------------------------------
// WMI-обёртка: выполняет запрос и возвращает первый строковый результат.
// Забирает температуры, где драйвер их выставляет.
// ---------------------------------------------------------------------------
namespace {

std::wstring wmiQueryFirst(const wchar_t* wql) {
    IWbemLocator* locator = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_IWbemLocator,
                                  reinterpret_cast<void**>(&locator));
    if (FAILED(hr)) return L"";
    IWbemServices* services = nullptr;
    hr = locator->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), nullptr, nullptr,
                                0, 0, 0, nullptr, &services);
    if (FAILED(hr)) { locator->Release(); return L""; }
    CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                      nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                      nullptr, EOAC_NONE);
    IEnumWbemClassObject* enumerator = nullptr;
    hr = services->ExecQuery(_bstr_t(L"WQL"), _bstr_t(wql),
                             WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                             nullptr, &enumerator);
    std::wstring result;
    if (SUCCEEDED(hr)) {
        IWbemClassObject* obj = nullptr;
        ULONG got = 0;
        if (SUCCEEDED(enumerator->Next(WBEM_INFINITE, 1, &obj, &got)) && got) {
            VARIANT val;
            // Поле CurrentTemperature или CurrentClock принимаем первым.
            if (SUCCEEDED(obj->Get(L"CurrentTemperature", 0, &val, nullptr, nullptr)) &&
                val.vt == VT_I4) {
                // MSAcpi хранит температуру в десятых долях Кельвина.
                int tenthsKelvin = val.lVal;
                result = std::to_wstring(tenthsKelvin);
            }
            VariantClear(&val);
            obj->Release();
        }
        enumerator->Release();
    }
    services->Release();
    locator->Release();
    return result;
}

} // namespace

double readCpuTempViaWmi() {
    // MSAcpi_ThermalZoneTemperature: CurrentTemperature = 10 * (Kelvin).
    std::wstring raw = wmiQueryFirst(
        L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature");
    if (raw.empty()) return -1.0;
    try {
        int tenthsKelvin = std::stoi(raw);
        double celsius = static_cast<double>(tenthsKelvin) / 10.0 - 273.15;
        return celsius;
    } catch (...) {
        return -1.0;
    }
}

double readGpuTempViaWmi() {
    // Современные драйверы выставляют CurrentTemperature только через
    // MSAcpi_DeltaTemperature либо вовсе не дают. Честно пробуем оба пути.
    std::wstring raw = wmiQueryFirst(
        L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature "
        L"WHERE InstanceName LIKE '%GPU%'");
    if (raw.empty())
        raw = wmiQueryFirst(
            L"SELECT CurrentTemperature FROM MSAcpi_DeltaTemperature");
    if (raw.empty()) return -1.0;
    try {
        int tenthsKelvin = std::stoi(raw);
        double celsius = static_cast<double>(tenthsKelvin) / 10.0 - 273.15;
        return celsius;
    } catch (...) {
        return -1.0;
    }
}

} // namespace hw