@echo off
:: UnLeaf Service - Uninstallation Script
:: Run as Administrator

echo ============================================
echo  UnLeaf Service Uninstaller
echo ============================================
echo.

:: Check for admin privileges
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] This script requires Administrator privileges.
    echo Right-click and select "Run as administrator"
    pause
    exit /b 1
)

:: Check if service exists
sc query UnLeafService >nul 2>&1
if %errorlevel% neq 0 (
    echo [INFO] Service is not installed.
    pause
    exit /b 0
)

:: Stop service if running
echo [INFO] Stopping service...
sc stop UnLeafService >nul 2>&1
timeout /t 3 /nobreak >nul

:: Delete service
echo [INFO] Removing service...
sc delete UnLeafService
if %errorlevel% neq 0 (
    echo [ERROR] Failed to delete service.
    echo The service may be marked for deletion and will be removed on reboot.
    pause
    exit /b 1
)

echo.
echo [SUCCESS] Service uninstalled successfully!
echo.
pause
