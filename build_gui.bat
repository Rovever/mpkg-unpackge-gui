@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo ============================================
echo  mpkg GUI build (MSVC only, no 3rd-party libs)
echo ============================================

where cl.exe >nul 2>nul
if errorlevel 1 (
    if exist "%ProgramFiles%\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
        call "%ProgramFiles%\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul
    ) else (
        echo cl.exe not found. Run this from "Developer Command Prompt for VS".
        pause
        exit /b 1
    )
)

if /i "%~1"=="test" goto :test

where rc.exe >nul 2>nul
if errorlevel 1 (
    echo rc.exe not found. Run this from "Developer Command Prompt for VS".
    pause
    exit /b 1
)

echo Compiling icon resource ...
rc.exe /nologo /fo mpkg_gui.res mpkg_gui.rc
if errorlevel 1 goto :failed

echo Building mpkg_gui.exe ...
cl.exe /nologo /std:c++17 /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE /Fe:mpkg_gui.exe mpkg_gui.cpp mpkg_gui.res /link /SUBSYSTEM:WINDOWS
if errorlevel 1 goto :failed
echo Done: %~dp0mpkg_gui.exe
pause
exit /b 0

:test
echo Building and running regression tests ...
cl.exe /nologo /std:c++17 /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE /Fe:tests\self_test.exe tests\self_test.cpp
if errorlevel 1 goto :failed
tests\self_test.exe
if errorlevel 1 goto :failed
echo All tests passed.
pause
exit /b 0

:failed
echo BUILD or TEST FAILED
pause
exit /b 1
