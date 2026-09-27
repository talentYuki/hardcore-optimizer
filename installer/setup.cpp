// ============================================================================
// setup.cpp — однофайловый установщик LeakOptimizator.
//
// Устанавливает приложение в каталог пользователя (без прав администратора):
//   %LocalAppData%\Programs\LeakOptimizator\LeakOptimizator.exe
// извлекая его из встроенного RCDATA-ресурса (#101).
// Создаёт ярлык в меню «Пуск», добавляет запись «Программы и компоненты»
// (HKCU) для корректного удаления. Запуск с ключом "/u" удаляет приложение.
// ============================================================================

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <shobjidl.h>

#include <string>

// Идентификаторы контролов.
enum {
    IDC_EDIT_DIR = 1001,
    IDC_BTN_BROWSE = 1002,
    IDC_BTN_INSTALL = 1003,
    IDC_BTN_CANCEL = 1004,
    IDC_STATIC_STATUS = 1005,
};

namespace {

const wchar_t kAppName[] = L"LeakOptimizator";
// Путь к exe, который запишем в уник info and will be appended to install dir.
const wchar_t kExeName[] = L"LeakOptimizator.exe";

// Извлекает встроенный RCDATA-ресурс (IDR_APPEXE) в файл по пути filePath.
bool ExtractEmbeddedApp(const std::wstring& filePath) {
    HRSRC hrsc = FindResourceW(nullptr, MAKEINTRESOURCEW(101), RT_RCDATA);
    if (!hrsc) return false;
    HGLOBAL hglob = LoadResource(nullptr, hrsc);
    if (!hglob) return false;
    void* data = LockResource(hglob);
    DWORD size = SizeofResource(nullptr, hrsc);
    if (!data || size == 0) return false;

    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(hFile, data, size, &written, nullptr);
    CloseHandle(hFile);
    return ok && written == size;
}

// Определяет каталог установки по умолчанию: %LocalAppData%\Programs\LeakOptimizator
std::wstring DefaultInstallDir() {
    wchar_t local[ MAX_PATH ];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
                                   SHGFP_TYPE_CURRENT, local))) {
        return std::wstring(local) + L"\\Programs\\LeakOptimizator";
    }
    return L"C:\\LeakOptimizator";
}

// Создаёт все каталоги вдоль пути (делегируем SHCreateDirectoryExW).
bool EnsureDirs(const std::wstring& path) {
    return SHCreateDirectoryExW(nullptr, path.c_str(), nullptr) == ERROR_SUCCESS ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

// Создаёт ярлык kAppName.lnk в стартовом меню, указывающий на installed exe.
bool CreateStartMenuShortcut(const std::wstring& exePath) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return false;

    IShellLinkW* psl = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&psl)))) {
        psl->SetPath(exePath.c_str());
        size_t slash = exePath.find_last_of(L'\\');
        std::wstring workDir = (slash != std::wstring::npos)
                                   ? exePath.substr(0, slash) : L"";
        psl->SetWorkingDirectory(workDir.c_str());
        psl->SetDescription(L"LeakOptimizator — системный твикер для игр");

        IPersistFile* pf = nullptr;
        if (SUCCEEDED(psl->QueryInterface(IID_PPV_ARGS(&pf)))) {
            wchar_t startMenu[ MAX_PATH ];
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_STARTMENU, nullptr,
                                           SHGFP_TYPE_CURRENT, startMenu))) {
                std::wstring lnk = std::wstring(startMenu) +
                    L"\\Programs\\LeakOptimizator.lnk";
                ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
            }
            pf->Release();
        }
        psl->Release();
    }
    CoUninitialize();
    return ok;
}

// Пишет запись «Программы и компоненты» для корректного удаления.
bool WriteUninstallEntry(const std::wstring& installDir) {
    HKEY key = nullptr;
    std::wstring subkey =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LeakOptimizator";
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;

    std::wstring exePath = installDir + L"\\" + kExeName;
    std::wstring uninst = L"\"" + exePath + L"\" /u";

    const wchar_t* displayIcon = exePath.c_str();
    const wchar_t* installLocation = installDir.c_str();
    const wchar_t* displayVersion = L"1.0.0";
    const wchar_t* publisher = L"LEAKSHOP";
    const wchar_t* displayName = L"LeakOptimizator 1.0";
    DWORD estimate = 2; // размер в КБ (грубо)

    RegSetValueExW(key, L"DisplayName", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(displayName),
                   static_cast<DWORD>((wcslen(displayName) + 1) * sizeof(wchar_t)));
    RegSetValueExW(key, L"DisplayVersion", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(displayVersion),
                   static_cast<DWORD>((wcslen(displayVersion) + 1) * sizeof(wchar_t)));
    RegSetValueExW(key, L"Publisher", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(publisher),
                   static_cast<DWORD>((wcslen(publisher) + 1) * sizeof(wchar_t)));
    RegSetValueExW(key, L"InstallLocation", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(installLocation),
                   static_cast<DWORD>((wcslen(installLocation) + 1) * sizeof(wchar_t)));
    RegSetValueExW(key, L"DisplayIcon", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(displayIcon),
                   static_cast<DWORD>((wcslen(displayIcon) + 1) * sizeof(wchar_t)));
    RegSetValueExW(key, L"UninstallString", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(uninst.c_str()),
                   static_cast<DWORD>((uninst.size() + 1) * sizeof(wchar_t)));
    RegSetValueExW(key, L"EstimatedSize", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&estimate), sizeof(DWORD));
    RegCloseKey(key);
    return true;
}

// Удаляет установленное приложение (по ключам HKCU).
bool UninstallPerUser() {
    HKEY key = nullptr;
    std::wstring subkey =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LeakOptimizator";
    std::wstring installDir;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_QUERY_VALUE,
                      &key) == ERROR_SUCCESS) {
        wchar_t buf[ MAX_PATH ];
        DWORD size = sizeof(buf);
        if (RegQueryValueExW(key, L"InstallLocation", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS)
            installDir = buf;
        RegCloseKey(key);
    }

    bool ok = true;
    // Удаляем файл приложения.
    if (!installDir.empty()) {
        std::wstring exePath = installDir + L"\\" + kExeName;
        DeleteFileW(exePath.c_str());
        RemoveDirectoryW(installDir.c_str()); // если пуст
    }
    // Удаляем ярлык.
    {
        wchar_t startMenu[ MAX_PATH ];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_STARTMENU, nullptr,
                                       SHGFP_TYPE_CURRENT, startMenu))) {
            std::wstring lnk = std::wstring(startMenu) +
                L"\\Programs\\LeakOptimizator.lnk";
            DeleteFileW(lnk.c_str());
        }
    }
    // Удаляем запись об удалении.
    if (RegDeleteKeyW(HKEY_CURRENT_USER, subkey.c_str()) != ERROR_SUCCESS)
        ok = false;
    return ok;
}

} // namespace

// ---------------------------------------------------------------------------
// Мини-окно установщика.
// ---------------------------------------------------------------------------
HWND g_edit = nullptr;
HWND g_status = nullptr;

static void SetStatus(const wchar_t* text) {
    SetWindowTextW(g_status, text);
}

static void DoInstall() {
    wchar_t dirBuf[ MAX_PATH ];
    GetWindowTextW(g_edit, dirBuf, MAX_PATH);
    std::wstring dir = dirBuf;
    if (dir.empty()) dir = DefaultInstallDir();

    if (!EnsureDirs(dir)) {
        SetStatus(L"Ошибка: не удалось создать каталог.");
        return;
    }
    std::wstring exePath = dir + L"\\" + kExeName;
    if (!ExtractEmbeddedApp(exePath)) {
        SetStatus(L"Ошибка: не извлечь приложение из установщика.");
        return;
    }
    CreateStartMenuShortcut(exePath);
    WriteUninstallEntry(dir);

    SetStatus(L"Готово.");
    MessageBoxW(nullptr,
        (L"LeakOptimizator установлен.\n\nПуть: " + dir + L"\nЯрлык добавлен в меню «Пуск».").c_str(),
        L"LeakOptimizator", MB_OK | MB_ICONINFORMATION);
}

LRESULT CALLBACK SetupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_BTN_INSTALL) {
                DoInstall();
                return 0;
            }
            if (id == IDC_BTN_CANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == IDC_BTN_BROWSE) {
                BROWSEINFOW bi{};
                bi.hwndOwner = hwnd;
                bi.lpszTitle = L"Выберите каталог установки";
                bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
                if (pidl) {
                    wchar_t path[ MAX_PATH ];
                    if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(g_edit, path);
                    CoTaskMemFree(pidl);
                }
                return 0;
            }
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Точка входа.
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    // Режим удаления.
    if (GetCommandLineW() && wcsstr(GetCommandLineW(), L"/u")) {
        bool ok = UninstallPerUser();
        MessageBoxW(nullptr,
                    ok ? L"LeakOptimizator удалён." : L"Приложение не найдено/уже удалено.",
                    L"LeakOptimizator", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = SetupWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    wc.lpszClassName = L"LeakSetup";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, L"LeakSetup",
                                L"LeakOptimizator — установка",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 420, 200,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    CreateWindowExW(0, L"STATIC", L"Каталог установки:",
                    WS_CHILD | WS_VISIBLE, 14, 16, 120, 18, hwnd, nullptr, hInst, nullptr);
    g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", DefaultInstallDir().c_str(),
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                             14, 36, 300, 22, hwnd,
                             reinterpret_cast<HMENU>(IDC_EDIT_DIR), hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Обзор...",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP, 320, 36, 78, 24, hwnd,
                    reinterpret_cast<HMENU>(IDC_BTN_BROWSE), hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Установить",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 116, 80, 120, 30, hwnd,
                    reinterpret_cast<HMENU>(IDC_BTN_INSTALL), hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Отмена",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP, 246, 80, 100, 30, hwnd,
                    reinterpret_cast<HMENU>(IDC_BTN_CANCEL), hInst, nullptr);
    g_status = CreateWindowExW(0, L"STATIC", L"",
                               WS_CHILD | WS_VISIBLE, 14, 124, 380, 20, hwnd,
                               reinterpret_cast<HMENU>(IDC_STATIC_STATUS), hInst, nullptr);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}