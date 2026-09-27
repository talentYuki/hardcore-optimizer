// overlay.cpp — реализация прозрачного игрового оверлея (GDI).
#include "overlay.h"

#include <dwmapi.h>

#include <cstdio>

// Цветовые стили для разных элементов (без фона — полупрозрачность layering).
namespace {

// Главное окно оверлея: дочернее рисование GDI прямо в память окна.
COLORREF toColorRef(const theme::Color& c) {
    return RGB(c.r(), c.g(), c.b());
}

} // namespace

// ---------------------------------------------------------------------------
OverlayWindow::OverlayWindow() {
    // Шрифт подбираем системным, крупным, чётким — лучше всего для оверлея.
    m_font = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    m_fontTitle = CreateFontW(-17, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

OverlayWindow::~OverlayWindow() {
    if (m_font)      DeleteObject(m_font);
    if (m_fontTitle) DeleteObject(m_fontTitle);
    if (m_hwnd)      DestroyWindow(m_hwnd);
}

void OverlayWindow::setTheme(const theme::Theme& t) { m_theme = t; requestMetricsUpdate(); }

// Стили окна: прозрачное, всегда поверх, без рамки.
bool OverlayWindow::create(const std::wstring& title) {
    WNDCLASSW wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LeakOptOverlay";
    if (!RegisterClassW(&wc)) {
        // Окно уже зарегистрировано в этом процессе — значит, окно существует.
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }

    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"LeakOptOverlay", title.c_str(),
        WS_POPUP, m_winX, m_winY, m_winW, m_winH,
        nullptr, nullptr, wc.hInstance, this);
    if (!m_hwnd) return false;

    // Аппаратный «слой» прозрачности окна.
    SetLayeredWindowAttributes(m_hwnd, RGB(0, 0, 0), static_cast<BYTE>(m_opacity),
                               LWA_ALPHA);

    // При статусе «нерабочий стол» уберём transparency для совместимости.
    return true;
}

LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                        LPARAM lParam) {
    OverlayWindow* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<OverlayWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    switch (msg) {
        case WM_PAINT: {
            if (self) self->onPaint();
            return 0;
        }
        case WM_DISPLAYCHANGE:
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        case WM_DESTROY:
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void OverlayWindow::setClickThrough(bool enabled) {
    m_clickThrough = enabled;
    LONG_PTR ex = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    LONG_PTR was = ex;
    if (enabled) ex |= WS_EX_TRANSPARENT;
    else         ex &= ~WS_EX_TRANSPARENT;
    if (was != ex) SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex);
}

void OverlayWindow::show(bool visible) {
    ShowWindow(m_hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void OverlayWindow::setOpacity(int alpha) {
    m_opacity = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
    SetLayeredWindowAttributes(m_hwnd, RGB(0, 0, 0),
                               static_cast<BYTE>(m_opacity), LWA_ALPHA);
}

void OverlayWindow::getWindowRect(LONG& x, LONG& y, LONG& w, LONG& h) const {
    RECT r{};
    if (m_hwnd) GetWindowRect(m_hwnd, &r);
    x = r.left; y = r.top; w = r.right - r.left; h = r.bottom - r.top;
}

void OverlayWindow::setWindowRect(LONG x, LONG y, LONG w, LONG h) {
    m_winX = x; m_winY = y; m_winW = w; m_winH = h;
    if (m_hwnd) SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

void OverlayWindow::requestMetricsUpdate() {
    // Ничего критичного вне WM_PAINT не делаем — размеры окна пересчитаем
    // там же, где рисуем, чтобы всегда попадать в габариты.
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// Отрисовка кадра. Тёмная полупрозрачная панель + акцентные цифры.
// ---------------------------------------------------------------------------
void OverlayWindow::onPaint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(m_hwnd, &ps);

    // Фон оверлея — полупрозрачная тёмная плашка (процент альфы задан
    // SetLayeredWindowAttributes отдельно для всего окна).
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    HBRUSH bg = CreateSolidBrush(toColorRef(m_theme.background));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    // Тонкая рамка акцентным цветом для читаемости на любом фоне.
    HPEN pen = CreatePen(PS_SOLID, 1, toColorRef(m_theme.secondary));
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, 1, 1, rc.right - 1, rc.bottom - 1);
    // Рамка завершена.

    // Заголовок.
    HGDIOBJ oldFont = SelectObject(hdc, m_fontTitle);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, toColorRef(m_theme.accent));
    std::wstring title = L"LeakOptimizator";
    TextOutW(hdc, 12, 10, title.c_str(), static_cast<int>(title.size()));

    // Подзаголовок: имя игры и линия-разделитель.
    SelectObject(hdc, m_font);
    SetTextColor(hdc, toColorRef(m_theme.text));
    std::wstring status;
    if (m_data && !m_data->gameName.empty()) status = m_data->gameName;
    else status = L"не отслеживается игра";
    TextOutW(hdc, 12, 34, status.c_str(), static_cast<int>(status.size()));

    // Акцентная горизонтальная линия.
    HBRUSH accLine = CreateSolidBrush(toColorRef(m_theme.accent));
    RECT lineRc{ 12, 58, rc.right - 12, 60 };
    FillRect(hdc, &lineRc, accLine);
    DeleteObject(accLine);

    // Левый столбец: FPS и frametime (крупные цифры).
    double fps = m_data ? m_data->fps.load() : 0.0;
    double ft  = m_data ? m_data->frametime.load() : 0.0;

    wchar_t buf[128];
    std::swprintf(buf, 128, L"FPS %4.0f", fps);
    TextOutW(hdc, 12, 70, buf, static_cast<int>(std::wcslen(buf)));

    std::swprintf(buf, 128, L"frame %4.1f ms", ft);
    TextOutW(hdc, 12, 92, buf, static_cast<int>(std::wcslen(buf)));

    // Правый столбец: температуры и загрузка.
    double cpuT = m_data ? m_data->cpuTemp.load() : -1.0;
    double gpuT = m_data ? m_data->gpuTemp.load() : -1.0;
    double ld   = m_data ? m_data->cpuLoad.load() : 0.0;
    double ram  = m_data ? m_data->ramUsedGB.load() : 0.0;

    int colX = 130;
    if (cpuT >= 0.0) std::swprintf(buf, 128, L"CPU %4.1f C", cpuT);
    else             std::swprintf(buf, 128, L"CPU  n/a");
    SetTextColor(hdc, cpuT >= 90.0 ? toColorRef(m_theme.warning) : toColorRef(m_theme.text));
    TextOutW(hdc, colX, 70, buf, static_cast<int>(std::wcslen(buf)));

    if (gpuT >= 0.0) std::swprintf(buf, 128, L"GPU %4.1f C", gpuT);
    else             std::swprintf(buf, 128, L"GPU  n/a");
    SetTextColor(hdc, gpuT >= 85.0 ? toColorRef(m_theme.warning) : toColorRef(m_theme.text));
    TextOutW(hdc, colX, 92, buf, static_cast<int>(std::wcslen(buf)));

    std::swprintf(buf, 128, L"Load %3.0f%%", ld);
    SetTextColor(hdc, toColorRef(m_theme.text));
    TextOutW(hdc, colX, 114, buf, static_cast<int>(std::wcslen(buf)));

    std::swprintf(buf, 128, L"RAM %4.1f", ram);
    SetTextColor(hdc, toColorRef(m_theme.text));
    TextOutW(hdc, colX, 136, buf, static_cast<int>(std::wcslen(buf)));

    // Вернём контекст в исходное состояние и завершим отрисовку.
    SelectObject(hdc, oldFont);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    EndPaint(m_hwnd, &ps);
}

void OverlayWindow::updateViews() {
    if (!m_hwnd) return;
    // Запрос перерисовки — лёгкий, не блокирует поток мониторинга.
    InvalidateRect(m_hwnd, nullptr, FALSE);
    // Отрисовка сразу, чтобы не зависеть от очереди сообщений GUI.
    UpdateWindow(m_hwnd);
}