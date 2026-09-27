#pragma once
// ============================================================================
// backup.h — сохранение и восстановление исходных настроек системы.
//
// Перед применением твиков снимаем «слепок» затрагиваемых значений реестра
// в память и дублируем его в JSON-файл. Кнопка Restore откатывает всё:
// верные значения записываются назад, отключённые службы запускаются,
// приоритет процесса сбрасывается, динамический тик таймера возвращается.
// ============================================================================

#include <windows.h>

#include <string>
#include <vector>

namespace backup {

// Полные ключи реестра, которыми управляет оптимизатор. Root — HKCU/HKLM.
enum class Root { HKCU, HKLM };

struct RegValue {
    Root        root;          // корневой раздел
    std::wstring path;         // путь к ключу, напр. L"Software\\Microsoft\\GameBar"
    std::wstring name;         // имя значения
    DWORD       type = 0;      // тип REG_* (может не быть установлен)
    bool        exists = false; // существовало ли значение до применения
    std::vector<unsigned char> data; // сырые данные значения
};

// Снимает слепок ВСЕХ значений, которыми может управлять оптимизатор.
std::vector<RegValue> collectSnapshot();

// Записывает слепок в JSON-файл. Возвращает true при успехе.
bool saveSnapshotToFile(const std::vector<RegValue>& snap, const std::wstring& path);

// Читает слепок из JSON-файла. Возвращает false при ошибке/отсутствии файла.
bool loadSnapshotFromFile(std::vector<RegValue>& snap, const std::wstring& path);

// Восстанавливает значения из слепка (записывает их обратно в реестр).
// Значения, которых раньше не было, удаляются; существовавшие — перезаписываются.
void restoreSnapshot(const std::vector<RegValue>& snap);

// Путь к файлу бэкапа рядом с exe: HardcoreOptimizer.backup.json
std::wstring defaultBackupPath();

// Вернуть процесс-игру обратно в NORMAL_PRIORITY_CLASS (для Restore).
void restoreProcessPriority(HANDLE process);

// Снять принудительную маску приоритета у процесса (для Restore).
void restoreProcessAffinity(HANDLE process);

} // namespace backup