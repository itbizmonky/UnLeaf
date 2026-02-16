@echo off
REM UnLeaf code signing wrapper for CMake / build.bat
REM Loads local config if present, then calls sign.ps1

REM Load local config (sets UNLEAF_SIGN_PFX / UNLEAF_SIGN_PASSWORD)
if exist "%~dp0sign_config.local.bat" (
    call "%~dp0sign_config.local.bat"
)

REM Forward all arguments to sign.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sign.ps1" %*
exit /b %errorlevel%
