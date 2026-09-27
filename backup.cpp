// backup.cpp — слепок значений реестра и их восстановление (откат твиков).
#include "backup.h"

#include <cstdio>
#include <cwchar>

#include "json_util.h"

namespace backup {

// ---------------------------------------------------------------------------
// Конвертация широких строк в UTF-8 и обратно (для JSON-файла).
// ---------------------------------------------------------------------------
static std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                   nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                   nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), size);
    return out;
}

// ---------------------------------------------------------------------------
// ЕДИНСТВЕННЫЙ источник истины о значениях реестра, которыми управляет
// оптимизатор. Если добавите новый твик — опишите его здесь, и он автомати-
// чески попадёт и в слепок, и в восстановление.
// ---------------------------------------------------------------------------
namespace {

struct KeyTarget {
    Root           root;
    const wchar_t* path;
    const wchar_t* name;
};

const std::vector<KeyTarget>& managedKeys() {
    static const std::vector<KeyTarget> keys = {
        // Xbox Game Bar (HKCU).
        { Root::HKCU, L"Software\\Microsoft\\GameBar",                    L"AllowAutoGameMode" },
        { Root::HKCU, L"Software\\Microsoft\\GameBar",                    L"AutoGameModeEnabled" },
        { Root::HKCU, L"Software\\Microsoft\\GameBar",                    L"UseNexusForGameBarEnabled" },
        // Game DVR / запись и захват.
        { Root::HKCU, L"Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR", L"AppCaptureEnabled" },
        { Root::HKCU, L"System\\GameConfigStore",                          L"GameDVR_Enabled" },
        { Root::HKCU, L"System\\GameConfigStore",                          L"GameDVR_FSEBehaviorMode" },
        // Ускорение мыши.
        { Root::HKCU, L"Control Panel\\Mouse", L"MouseSpeed" },
        { Root::HKCU, L"Control Panel\\Mouse", L"MouseThreshold1" },
        { Root::HKCU, L"Control Panel\\Mouse", L"MouseThreshold2" },
        // Службы (START_TYPE в нулевом значении ключа службы).
        { Root::HKLM, L"SYSTEM\\CurrentControlSet\\Services\\SysMain",    L"Start" },
        { Root::HKLM, L"SYSTEM\\CurrentControlSet\\Services\\WSearch",    L"Start" },
        { Root::HKLM, L"SYSTEM\\CurrentControlSet\\Services\\DiagTrack",  L"Start" },
        { Root::HKLM, L"SYSTEM\\CurrentControlSet\\Services\\MapsBroker", L"Start" },
        // HAGS — аппаратное планирование GPU.
        { Root::HKLM, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode" },
    };
    return keys;
}

HKEY rootHandle(Root root) {
    return (root == Root::HKCU) ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
}

} // namespace

// Открывают ли мы (для записи) ключ с привилегиями владельца ключа.
static LONG openKeyWritable(HKEY base, const std::wstring& path, HKEY* out) {
    return RegOpenKeyExW(base, path.c_str(), 0,
                         KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY,
                         out);
}

// ---------------------------------------------------------------------------
// Чтение одного значения реестра.
// ---------------------------------------------------------------------------
static void readSingleValue(HKEY base, const std::wstring& path,
                            const std::wstring& name, RegValue& out) {
    HKEY key = nullptr;
    if (openKeyWritable(base, path, &key) != ERROR_SUCCESS) return;
    DWORD type = 0, dataSize = 0;
    LONG rc = RegQueryValueExW(key, name.c_str(), nullptr, &type,
                               nullptr, &dataSize);
    if (rc != ERROR_SUCCESS) { RegCloseKey(key); return; }
    out.exists = true;
    out.type = type;
    out.data.resize(dataSize > 0 ? dataSize : 1);
    RegQueryValueExW(key, name.c_str(), nullptr, &out.type,
                     out.data.data(), &dataSize);
    RegCloseKey(key);
}

// ---------------------------------------------------------------------------
// collectSnapshot — слепок всех управляемых значений + всех TCP-интерфейсов.
// ---------------------------------------------------------------------------
std::vector<RegValue> collectSnapshot() {
    std::vector<RegValue> snap;
    snap.reserve(256);

    for (const auto& t : managedKeys()) {
        RegValue val;
        val.root = t.root;
        val.path = t.path;
        val.name = t.name;
        readSingleValue(rootHandle(t.root), t.path, t.name, val);
        snap.push_back(std::move(val));
    }

    // Nagle-ключи есть у каждого сетевого интерфейса: снимем их со всех
    // GUID-подключей, чтобы восстановление затронуло ровно те места, что
    // были изменены.
    HKEY iface = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces",
        0, KEY_READ | KEY_WOW64_64KEY, &iface) == ERROR_SUCCESS) {
        wchar_t subName[256];
        for (DWORD i = 0;; ++i) {
            DWORD len = 256;
            if (RegEnumKeyExW(iface, i, subName, &len, nullptr, nullptr,
                              nullptr, nullptr) != ERROR_SUCCESS) break;
            std::wstring full =
                L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\"
                + std::wstring(subName);
            for (const wchar_t* n : { L"TcpAckFrequency", L"TCPNoDelay", L"TcpDelAckTicks" }) {
                RegValue val;
                val.root = Root::HKLM;
                val.path = full;
                val.name = n;
                readSingleValue(HKEY_LOCAL_MACHINE, full, n, val);
                snap.push_back(std::move(val));
            }
        }
        RegCloseKey(iface);
    }
    return snap;
}

// ---------------------------------------------------------------------------
// Сериализация слепка в JSON.
// ---------------------------------------------------------------------------
static json::Node regValueToNode(const RegValue& v) {
    json::Node n = json::Node::makeObject();
    n.object.emplace_back("root",   json::Node::makeString(
        v.root == Root::HKCU ? "HKCU" : "HKLM"));
    n.object.emplace_back("path",   json::Node::makeString(WideToUtf8(v.path)));
    n.object.emplace_back("name",   json::Node::makeString(WideToUtf8(v.name)));
    n.object.emplace_back("type",   json::Node::makeNumber(v.type));
    n.object.emplace_back("exists", json::Node::makeBool(v.exists));
    json::Node arr = json::Node::makeArray();
    for (unsigned char b : v.data) arr.array.push_back(json::Node::makeNumber(b));
    n.object.emplace_back("data", std::move(arr));
    return n;
}

static RegValue nodeToRegValue(const json::Node& n) {
    RegValue v;
    v.root  = (n.find("root")->asString() == "HKCU") ? Root::HKCU : Root::HKLM;
    v.path  = Utf8ToWide(n.find("path")->asString());
    v.name  = Utf8ToWide(n.find("name")->asString());
    v.type  = static_cast<DWORD>(n.find("type")->asNumber(0));
    v.exists = n.find("exists")->asBool(false);
    if (const json::Node* d = n.find("data"); d && d->type == json::Node::Type::Array) {
        v.data.reserve(d->array.size());
        for (const auto& b : d->array) v.data.push_back(
            static_cast<unsigned char>(b.asNumber(0)));
    }
    return v;
}

bool saveSnapshotToFile(const std::vector<RegValue>& snap,
                        const std::wstring& path) {
    json::Node root = json::Node::makeObject();
    json::Node arr = json::Node::makeArray();
    arr.array.reserve(snap.size());
    for (const auto& v : snap) arr.array.push_back(regValueToNode(v));
    root.object.emplace_back("snapshot", std::move(arr));

    std::string text = json::stringify(root);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    DWORD written = 0;
    if (WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr))
        ok = (written == static_cast<DWORD>(text.size()));
    CloseHandle(h);
    return ok;
}

bool loadSnapshotFromFile(std::vector<RegValue>& snap,
                          const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 ||
        size.QuadPart > (16 * 1024 * 1024)) {
        CloseHandle(h);
        return false;
    }
    std::string text(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok || read != static_cast<DWORD>(text.size())) return false;

    json::Node root = json::parse(text, nullptr);
    const json::Node* arr = root.find("snapshot");
    if (!arr || arr->type != json::Node::Type::Array) return false;

    snap.clear();
    snap.reserve(arr->array.size());
    for (const auto& n : arr->array) snap.push_back(nodeToRegValue(n));
    return true;
}

// ---------------------------------------------------------------------------
// restoreSnapshot — восстанавливает значения из слепка в реестр.
// Значения, которых раньше не было, удаляются; существовавшие — пишутся назад.
// ---------------------------------------------------------------------------
void restoreSnapshot(const std::vector<RegValue>& snap) {
    for (const auto& v : snap) {
        HKEY base = rootHandle(v.root);
        HKEY key = nullptr;
        if (openKeyWritable(base, v.path, &key) != ERROR_SUCCESS) continue;

        if (v.exists) {
            // Возвращаем исходные данные.
            if (!v.data.empty())
                RegSetValueExW(key, v.name.c_str(), 0, v.type, v.data.data(),
                               static_cast<DWORD>(v.data.size()));
        } else {
            // Значения не было — убираем, если наш твик его создавал.
            RegDeleteValueW(key, v.name.c_str());
        }
        RegCloseKey(key);
    }
}

std::wstring defaultBackupPath() {
    wchar_t buffer[MAX_PATH];
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) == 0)
        return L"LeakOptimizator.backup.json";
    wchar_t* slash = std::wcsrchr(buffer, L'\\');
    if (slash) *(slash + 1) = L'\0';
    return std::wstring(buffer) + L"LeakOptimizator.backup.json";
}

// ---------------------------------------------------------------------------
// Откат приоритета и маски приоритета процесса.
// ---------------------------------------------------------------------------
void restoreProcessPriority(HANDLE process) {
    if (process && process != INVALID_HANDLE_VALUE)
        SetPriorityClass(process, NORMAL_PRIORITY_CLASS);
}

void restoreProcessAffinity(HANDLE process) {
    if (process && process != INVALID_HANDLE_VALUE) {
        DWORD_PTR systemMask = 0;
        DWORD_PTR procMask = 0;
        if (GetProcessAffinityMask(process, &procMask, &systemMask) && systemMask)
            SetProcessAffinityMask(process, systemMask);
    }
}

} // namespace backup