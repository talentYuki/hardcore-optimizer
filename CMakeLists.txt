cmake_minimum_required(VERSION 3.20)

project(HardcoreOptimizer
        VERSION 1.0.0
        DESCRIPTION "Агрессивная утилита для выжимания FPS и стабильности в играх"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Требуется C++20 (используются std::jthread, std::format, designated init).
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Проверяем, что собираемся под MSVC (Visual Studio 2022 / Build Tools).
if(NOT MSVC)
    message(FATAL_ERROR "HardcoreOptimizer собирается только компилятором MSVC (Visual Studio 2022).")
endif()

# Гарантированно компилируем как Windows-приложение (WinMain, GUI).
add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX)

# Отключаем предупреждения C4996 (небезопасные CRT-функции типа fopen_s)
# и лишний шум от платформенных заголовков.
add_compile_options(/W4 /Zc:__cplusplus /permissive- /utf-8)

# ---------------------------------------------------------------------------
# Основной исполняемый файл (паттерн WIN32 = GUI-подсистема, WinMain).
# ---------------------------------------------------------------------------
add_executable(HardcoreOptimizer WIN32)

# ---------------------------------------------------------------------------
# Библиотеки, линкуемые с приложением.
#   - user32/gdi32:   окно, элементы управления, отрисовка оверлея
#   - comdlg32:       системный диалог выбора цвета (ChooseColor)
#   - advapi32:       реестр (RegXxx), службы (OpenSCManager, ControlService)
#   - ole32/oleaut32: COM и WMI (для чтения температур)
#   - wbemuuid:       IWbem* интерфейсы WMI
#   - dxgi:           определение видеокарт
#   - dwmapi:         DWM (аппаратное ускорение оверлея / эффекты)
#   - shell32:        иконка приложения (LoadIcon)
#   - winmm:          время высокого разрешения (timeBeginPeriod)
# ---------------------------------------------------------------------------
target_link_libraries(HardcoreOptimizer PRIVATE
    user32 gdi32 comdlg32 advapi32 comctl32 shell32 shlwapi
    ole32 oleaut32 wbemuuid uuid dxgi dwmapi winmm)

# ---------------------------------------------------------------------------
# Исходники проекта.
# ---------------------------------------------------------------------------
target_sources(HardcoreOptimizer PRIVATE
    main.cpp
    optimizer.cpp
    overlay.cpp
    hardware.cpp
    theme.cpp
    backup.cpp
    json_util.cpp
)

# Ресурс с иконкой приложения (генерируется автоматически, см. ниже).
set(RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")
if(NOT EXISTS "${RESOURCE_DIR}/app.rc")
    file(WRITE "${RESOURCE_DIR}/app.rc"
        "1 ICON \"app.ico\"\n"
    )
endif()
target_sources(HardcoreOptimizer PRIVATE "${RESOURCE_DIR}/app.rc")

# ---------------------------------------------------------------------------
# UAC-манифест (requireAdministrator) встраиваем ПОСЛЕ линковки через mt.exe.
# Встраивание манифеста через RC-файл (1 24) приводит к CVT1100 при сборке
# данной версией инструментов, поэтому делаем это отдельным шагом.
# ---------------------------------------------------------------------------
if(CMAKE_MT)
    add_custom_command(TARGET HardcoreOptimizer POST_BUILD
        COMMAND "${CMAKE_MT}" -manifest "${RESOURCE_DIR}/app.manifest"
                "-outputresource:$<TARGET_FILE:HardcoreOptimizer>;#1"
        COMMENT "Embedding UAC manifest (requireAdministrator)"
        VERBATIM)
endif()

# ---------------------------------------------------------------------------
# Свойства вывода: GUI-подсистема, имя exe.
# ---------------------------------------------------------------------------
set_target_properties(HardcoreOptimizer PROPERTIES
    OUTPUT_NAME "HardcoreOptimizer"
    WIN32_EXECUTABLE ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Собираем release-версию с полной оптимизацией (заголовочные inline - на
# скорость работы). В Debug ничего не форсируем, чтобы сохранить отладку.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(HardcoreOptimizer PRIVATE /O2 /Ob2 /GL)
    target_link_options(HardcoreOptimizer PRIVATE /LTCG)
endif()

# Кодировка исходников UTF-8 — все строки с русскими комментариями заведомо
# корректны и во время сборки, и в рантайме.