@echo off
setlocal enabledelayedexpansion

set CMAKE_EXE="C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

cd /d "%~dp0"

echo Cleaning build directory...
if exist build rmdir /s /q build
mkdir build
cd build

echo.
echo Configuring CMake...
%CMAKE_EXE% .. -G "Visual Studio 18 2026" -A x64
if errorlevel 1 (
    echo.
    echo [ERROR] CMake configuration failed
    pause
    exit /b 1
)

echo.
echo Building Release configuration...
%CMAKE_EXE% --build . --config Release 2>&1
if errorlevel 1 (
    echo.
    echo [ERROR] Build failed
    pause
    exit /b 1
)

echo.
echo ========================================
echo Build completed successfully!
echo Output: build\Release\
echo ========================================
dir Release\*.exe 2>nul

pause
