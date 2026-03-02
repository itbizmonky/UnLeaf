@echo off
:: UnLeaf Service - Installation Script
:: Run as Administrator

echo ============================================
echo  UnLeaf Service Installer
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

:: Get script directory
set "SCRIPT_DIR=%~dp0"
set "BUILD_DIR=%SCRIPT_DIR%..\build\Release"
set "SERVICE_EXE=%BUILD_DIR%\UnLeaf_Service.exe"

:: Check if service executable exists
if not exist "%SERVICE_EXE%" (
    echo [ERROR] UnLeaf_Service.exe not found at:
    echo         %SERVICE_EXE%
    echo.
    echo Please build the project first.
    pause
    exit /b 1
)

echo Service executable: %SERVICE_EXE%
echo.

:: Check if service already exists
sc query UnLeafService >nul 2>&1
if %errorlevel% equ 0 (
    echo [INFO] Service already exists. Stopping...
    sc stop UnLeafService >nul 2>&1
    timeout /t 2 /nobreak >nul
    echo [INFO] Removing existing service...
    sc delete UnLeafService
    timeout /t 2 /nobreak >nul
)

:: Install service
echo [INFO] Installing UnLeaf Service...
sc create UnLeafService binPath= "%SERVICE_EXE%" start= auto DisplayName= "UnLeaf Service"
if %errorlevel% neq 0 (
    echo [ERROR] Failed to create service.
    pause
    exit /b 1
)

:: Set description
sc description UnLeafService "UnLeaf - EcoQoS Override Engine (Native C++ Edition)"

echo.
echo [SUCCESS] Service installed successfully!
echo.
echo To start the service, run: sc start UnLeafService
echo Or use UnLeaf_Manager.exe
echo.
pause
