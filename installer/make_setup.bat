@echo off
rem ===========================================================================
rem make_setup.bat - build single-file installer for LeakOptimizator
rem Prereq: build\bin\LeakOptimizator.exe already built (run build.bat Release).
rem Output: installer\LeakOptimizator_setup.exe
rem ===========================================================================
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo ERROR vcvars64 & exit /b 1 )

cd installer

if not exist "..\build\bin\LeakOptimizator.exe" (
  echo ERROR: build\bin\LeakOptimizator.exe not found. Run build.bat Release first.
  exit /b 1
)

echo === RC (embed app exe as resource) ===
rc.exe /fo setup.res /I "..\build\bin" setup.rc
if errorlevel 1 ( echo RC FAIL & exit /b 1 )

echo === CL (compile setup.cpp) ===
cl.exe /nologo /O2 /EHsc /W3 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /c setup.cpp /Fosetup.obj
if errorlevel 1 ( echo CL FAIL & exit /b 1 )

echo === LINK ===
link.exe /nologo /SUBSYSTEM:WINDOWS /OUT:LeakOptimizator_setup.exe setup.obj setup.res shell32.lib ole32.lib uuid.lib user32.lib gdi32.lib advapi32.lib shlwapi.lib
if errorlevel 1 ( echo LINK FAIL & exit /b 1 )

echo === SETUP_OK: installer\LeakOptimizator_setup.exe ===
endlocal