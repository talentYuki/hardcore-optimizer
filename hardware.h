#pragma once
// ============================================================================
// hardware.h — детект железа, температуры и загрузки ядер процессора.
//
// Собирает «технический паспорт» машины (CPU, видеокарта, ОЗУ) и в цикле
// опрашивает живые показатели: загрузку каждого ядра (через GetSystemTimes /
// GetSystemProcessorPerformanceInformation) и температуры CPU/GPU (через WMI:
// MSAcpi_ThermalZoneTemperature и данные ядем видеодрайвера).
// ============================================================================

#include <windows.h>

#include <string>
#include <vector>

namespace hw {

// Статичный снимок железа.
struct HardwareInfo {
    std::wstring cpuName;      // наименование процессора
    std::wstring cpuVendor;    // "Intel" / "AMD" / "Неизвестно"
    unsigned     cpuCores = 0; // физических ядер
    unsigned     cpuLogical = 0; // логических потоков
    unsigned     cpuBaseMhz = 0; // номинальная частота (МГц)
    unsigned     cpuMaxMhz = 0;  // максимальная частота (МГц)
    std::wstring gpuName;      // основная видеокарта
    std::wstring ramText;      // человекочитаемый размер ОЗУ ("32 ГБ")

    unsigned long long totalRamBytes = 0;
};

// Живые показатели (обновляются потоком мониторинга).
struct LiveStats {
    // Загрузка каждого логического потока в процентах (0..100).
    std::vector<double> coreLoad;
    // Суммарная загрузка CPU в процентах.
    double cpuTotal = 0.0;
    // Показатель «theoretical ms per frame» — 1000/FPS, отдаётся из оверлея.
    double frametime = 0.0;
    // Последние известные температуры, °C. -1 означает «недоступно».
    double cpuTemp = -1.0;
    double gpuTemp = -1.0;
    // Используемая оперативная память, байт.
    unsigned long long ramUsed = 0;
};

// Заполняет HardwareInfo текущими данными системы (CPU, GPU, ОЗУ).
HardwareInfo queryHardware();

// Начинает/прекращает опрос загрузки ядер (вызов из потока мониторинга).
// Функция читает счётчики по всем логическим потокам и кладёт результат
// в переданный LiveStats. Структура защищена снаружи (см. main.cpp).
void sampleCpuLoad(LiveStats& out);

// Читает температуру процессора через WMI (MSAcpi_ThermalZoneTemperature).
// Возвращает градусы Цельсия или -1 при ошибке/отсутствии поддержки.
double readCpuTempViaWmi();

// Читает температуру видеокарты через WMI-DXGIDisplayInfo (современные
// драйверы). Возвращает градусы Цельсия или -1 при недоступности.
double readGpuTempViaWmi();

// Количество логических потоков системы (обёртка над ::GetSystemInfo).
unsigned logicalProcessors();

} // namespace hw