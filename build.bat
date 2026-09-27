@echo off
rem ===========================================================================
rem build.bat - build Hardcore Optimizer via vcvars64 + CMake (NMake Makefiles)
rem Usage: build.bat [Debug|Release]
rem ===========================================================================
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo ERROR vcvars64.bat & exit /b 1 )

set CFG=%~1
if "%CFG%"=="" set CFG=Release

echo === CMake configure (%CFG%) ===
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=%CFG%
if errorlevel 1 ( echo ERROR configure & exit /b 1 )

echo === Build ===
cmake --build build
if errorlevel 1 ( echo ERROR build & exit /b 1 )

echo === DONE: build\HardcoreOptimizer.exe ===
endlocal