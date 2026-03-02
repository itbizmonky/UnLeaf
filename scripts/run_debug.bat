@echo off
:: UnLeaf Service - Debug Mode (Console)
:: Runs the service in console mode for testing

echo ============================================
echo  UnLeaf Service - Debug Mode
echo ============================================
echo.

set "SCRIPT_DIR=%~dp0"
set "BUILD_DIR=%SCRIPT_DIR%..\build\Release"
set "SERVICE_EXE=%BUILD_DIR%\UnLeaf_Service.exe"

if not exist "%SERVICE_EXE%" (
    echo [ERROR] UnLeaf_Service.exe not found.
    echo Please build the project first.
    pause
    exit /b 1
)

echo Starting in debug mode...
echo Press Ctrl+C to stop.
echo.

"%SERVICE_EXE%" --debug
pause
