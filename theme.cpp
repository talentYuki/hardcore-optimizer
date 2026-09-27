// theme.cpp — цветовые схемы оверлея, активная тема и её сохранение в JSON.
#include "theme.h"

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>

#include "json_util.h"

namespace theme {

// Глобальная активная тема приложения (рисуется всеми окнами).
static Theme g_active;

// ---------------------------------------------------------------------------
// Встроенные пресеты. Цвета подобраны под типичные геймерские палитры.
// ---------------------------------------------------------------------------
const std::vector<Preset>& presets() {
    static const std::vector<Preset> kPresets = {
        { L"Neon Blue",   {0x00, 0xBF, 0xFF}, {0x00, 0x3A, 0x66} }, // 00BFFF / 003A66
        { L"Toxic Green", {0x39, 0xFF, 0x14}, {0x14, 0x66, 0x11} }, // 39FF14 / 146611
        { L"Blood Red",   {0xFF, 0x23, 0x0F}, {0x66, 0x0A, 0x06} }, // FF230F / 660A06
        { L"Rust Orange", {0xFF, 0xA5, 0x00}, {0x66, 0x3C, 0x00} }, // FFA500 / 663C00
        { L"Monochrome",  {0xF2, 0xF2, 0xF2}, {0x40, 0x40, 0x40} }, // F2F2F2 / 404040
    };
    return kPresets;
}

// Применяет пресет: акцент и вторичный цвет, остальные — фиксированные
// нейтральные тона, читаемые поверх тёмной подложки оверлея.
void Theme::applyPreset(const Preset& p) {
    accent    = p.accent;
    secondary = p.secondary;
    text.setRGB(235, 235, 235);
    background.setRGB(10, 10, 12);
    warning.setRGB(255, 60, 60);
}

// Поиск пресета по имени (без учёта регистра).
bool findPreset(const std::wstring& name, Preset& out) {
    if (name.empty()) return false;
    std::wstring needle = name;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    for (const auto& p : presets()) {
        std::wstring pname = p.name;
        std::transform(pname.begin(), pname.end(), pname.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (pname == needle) { out = p; return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Сериализация темы в JSON-структуру.
// ---------------------------------------------------------------------------
static json::Node themeToNode(const Theme& th) {
    json::Node root = json::Node::makeObject();
    auto addColor = [](json::Node& parent, const char* key, const Color& c) {
        json::Node obj = json::Node::makeObject();
        obj.object.emplace_back("r", json::Node::makeNumber(c.r()));
        obj.object.emplace_back("g", json::Node::makeNumber(c.g()));
        obj.object.emplace_back("b", json::Node::makeNumber(c.b()));
        parent.object.emplace_back(key, std::move(obj));
    };
    addColor(root, "accent", th.accent);
    addColor(root, "secondary", th.secondary);
    addColor(root, "text", th.text);
    addColor(root, "background", th.background);
    addColor(root, "warning", th.warning);
    return root;
}

// Десериализация темы из JSON (с подстановкой значений по умолчанию).
static Theme nodeToTheme(const json::Node& root) {
    Theme th;
    th.accent.setRGB(255, 165, 0);
    th.secondary.setRGB(102, 60, 0);
    th.text.setRGB(235, 235, 235);
    th.background.setRGB(10, 10, 12);
    th.warning.setRGB(255, 60, 60);

    auto readColor = [](const json::Node* c, Color& out) {
        if (!c || c->type != json::Node::Type::Object) return;
        const json::Node* r = c->find("r");
        const json::Node* g = c->find("g");
        const json::Node* b = c->find("b");
        out.setRGB(static_cast<unsigned char>((r ? r->asNumber(0) : 0)),
                   static_cast<unsigned char>((g ? g->asNumber(0) : 0)),
                   static_cast<unsigned char>((b ? b->asNumber(0) : 0)));
    };
    readColor(root.find("accent"), th.accent);
    readColor(root.find("secondary"), th.secondary);
    readColor(root.find("text"), th.text);
    readColor(root.find("background"), th.background);
    readColor(root.find("warning"), th.warning);
    return th;
}

// ---------------------------------------------------------------------------
// Путь к файлу темы: <каталог exe>\LeakOptimizator.theme.json
std::wstring defaultThemePath() {
    wchar_t buffer[MAX_PATH];
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) == 0) return L"LeakOptimizator.theme.json";
    // Обрезаем имя exe, оставляя каталог.
    wchar_t* slash = std::wcsrchr(buffer, L'\\');
    if (slash) *(slash + 1) = L'\0';
    std::wstring dir(buffer);
    return dir + L"LeakOptimizator.theme.json";
}

// ---------------------------------------------------------------------------
// Сохранение темы в JSON-файл (UTF-8). Возвращает true при успехе.
// ---------------------------------------------------------------------------
static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), size);
    return out;
}

static std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                   nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
}

bool saveThemeToFile(const Theme& th, const std::wstring& path) {
    std::string text = json::stringify(themeToNode(th));
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    DWORD written = 0;
    if (WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)) {
        ok = (written == static_cast<DWORD>(text.size()));
    }
    CloseHandle(h);
    return ok;
}

// ---------------------------------------------------------------------------
// Загрузка темы из JSON-файла. Возвращает false, если файл отсутствует или
// повреждён (тема остаётся в состоянии по умолчанию).
// ---------------------------------------------------------------------------
bool loadThemeFromFile(Theme& th, const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > 8 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    std::string text(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    BOOL okRead = ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
    CloseHandle(h);
    if (!okRead || read != static_cast<DWORD>(text.size())) return false;

    json::Node root = json::parse(text, nullptr);
    if (root.type != json::Node::Type::Object) return false;
    th = nodeToTheme(root);
    return true;
}

// ---------------------------------------------------------------------------
// Управление глобальной активной темой.
// ---------------------------------------------------------------------------
void setActiveTheme(const Theme& th) { g_active = th; }
const Theme& activeTheme()          { return g_active; }

} // namespace theme