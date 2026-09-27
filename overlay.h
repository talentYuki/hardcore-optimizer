#pragma once
// ============================================================================
// overlay.h — прозрачный игровой оверлей поверх всех окон.
//
// Окно создаётся с расширенными стилями WS_EX_LAYERED | WS_EX_TRANSPARENT |
// WS_EX_TOPMOST и не перехватывает клики (клики проходят «насквозь» —
// отдельно управляется глобальным флагом clickthrough). Рисование ведётся
// через GDI (антиалиасинг включён через CLEARTYPE). Поток оверлея обновляет
// данные добавочно, не блокируя мониторинг.
// ============================================================================

#include <windows.h>

#include <atomic>
#include <string>

#include "theme.h"

// Данные одного кадра, которые оверлей показывает. Заполняет поток видео-
// мониторинга; читает поток GUI/оверлея. Защиты не требуется: все поля
// атомарные либо обновляются из одного писателя.
struct OverlayData {
    std::atomic<double> fps         { 0.0 };
    std::atomic<double> frametime   { 0.0 };
    std::atomic<double> cpuTemp     { -1.0 };
    std::atomic<double> gpuTemp     { -1.0 };
    std::atomic<double> cpuLoad     { 0.0 };   // суммарная загрузка CPU, %
    std::atomic<double> ramUsedGB   { 0.0 };   // занятая RAM, ГБ

    std::wstring gameName;                     // имя отслеживаемого процесса (детект)
};

// Управление оверлеем (лёгкая объектная обёртка над HWND).
class OverlayWindow {
public:
    OverlayWindow();
    ~OverlayWindow();

    // Создаёт и показывает окно. Вернёт false, если десктоп не поддерживает
    // аппаратное ускорение (окно остаётся интерактивным как запасной вариант).
    bool create(const std::wstring& title);

    HWND hwnd() const { return m_hwnd; }

    // Показывает/скрывает оверлей.
    void show(bool visible);

    // Рисует текущие OverlayData с заданной темой. Вызывается внутри потока.
    void updateViews();

    void setData(OverlayData* data) { m_data = data; }

    // Прозрачность 0..255 (0 = полностью прозрачно).
    void setOpacity(int alpha);

    // Координаты окна в пикселях (для сохранения в конфиг).
    void getWindowRect(LONG& x, LONG& y, LONG& w, LONG& h) const;
    void setWindowRect(LONG x, LONG y, LONG w, LONG h);

    // Глобальный флаг «клики уходят сквозь окно». По умолчанию true.
    void setClickThrough(bool enabled);

    // Пересчитывает размеры окна под шрифт.
    void requestMetricsUpdate();

    // Текущая тема рисования.
    const theme::Theme& theme() const { return m_theme; }
    void setTheme(const theme::Theme& t);

private:
    // Callback WndProc окна оверлея.
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void onPaint();                 // отрисовка кадра

    HWND               m_hwnd = nullptr;
    OverlayData*       m_data = nullptr;
    theme::Theme       m_theme;
    bool               m_clickThrough = true;
    HFONT              m_font = nullptr;
    HFONT              m_fontTitle = nullptr;
    int                m_opacity = 235;
    LONG               m_winX = 40, m_winY = 40, m_winW = 260, m_winH = 170;
};