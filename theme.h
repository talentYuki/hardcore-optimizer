#pragma once
// ============================================================================
// theme.h — цветовые схемы оверлея и управление текущей темой приложения.
//
// Хранит список пресетов («Neon Blue», «Toxic Green», «Blood Red»,
// «Rust Orange»), поддерживает кастомный акцентный цвет (Color Picker) и
// сохраняет выбранную тему в JSON-файл рядом с исполняемым файлом.
// ============================================================================

#include <string>
#include <vector>

namespace theme {

// Структура цвета в формате 0x00RRGGBB (без альфа-канала — считаем на 100%).
struct Color {
    unsigned int value = 0x00FFA500; // Rust Orange по умолчанию

    Color() = default;
    // Явный конструктор из компонент R/G/B (0..255).
    Color(unsigned char r, unsigned char g, unsigned char b) { setRGB(r, g, b); }

    // Конвертация из компонент R/G/B (0..255) в 0x00RRGGBB.
    void setRGB(unsigned char r, unsigned char g, unsigned char b) {
        value = (static_cast<unsigned int>(r) << 16) |
                (static_cast<unsigned int>(g) << 8) |
                static_cast<unsigned int>(b);
    }

    // "<< 8" не нужно — рисование GDI само игнорирует старший байт.
    unsigned int rgb() const { return value & 0x00FFFFFF; }
    unsigned char r() const { return static_cast<unsigned char>((value >> 16) & 0xFF); }
    unsigned char g() const { return static_cast<unsigned char>((value >> 8) & 0xFF); }
    unsigned char b() const { return static_cast<unsigned char>(value & 0xFF); }
};

// Пресет темы: имя + акцентный цвет (для статус-бара, линий, подписей).
struct Preset {
    std::wstring name;
    Color        accent;
    Color        secondary; // вторичный (фоновый) оттенок
};

// Полный набор цветов, которыми рисуется оверлей.
struct Theme {
    Color accent;    // заголовки, ключевые цифры
    Color secondary; // рамки, разделители
    Color text;      // обычный текст
    Color background; // фон полупрозрачной панели
    Color warning;    // цвет при превышении порога (например, при 90+ градусах)

    // Применяет пресет к текущей теме.
    void applyPreset(const Preset& p);
};

// Возвращает список всех встроенных пресетов.
const std::vector<Preset>& presets();

// Поиск пресета по имени (без учёта регистра). Возвращает false, если не найден.
bool findPreset(const std::wstring& name, Preset& out);

// ---------------------------------------------------------------------------
// Сохранение и загрузка темы в/из JSON.
//   path  — полный путь к файлу (например L"HardcoreOptimizer.theme.json").
// ---------------------------------------------------------------------------
bool saveThemeToFile(const Theme& th, const std::wstring& path);
bool loadThemeFromFile(Theme& th, const std::wstring& path);

// Возвращает путь к файлу темы рядом с exe (HardcoreOptimizer.theme.json).
std::wstring defaultThemePath();

// Устанавливает текущую «активную» тему в глобальном хранилище (для GUI).
void setActiveTheme(const Theme& th);
const Theme& activeTheme();

} // namespace theme