$ErrorActionPreference = "Stop"

$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

Set-Location $PSScriptRoot

Write-Host "Cleaning build directory..."
if (Test-Path build) {
    Remove-Item -Recurse -Force build
}
New-Item -ItemType Directory -Path build | Out-Null
Set-Location build

Write-Host ""
Write-Host "Configuring CMake..."
# Use Visual Studio 18 2026 generator
& $cmake .. -G "Visual Studio 18 2026" -A x64
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] CMake configuration failed"
    exit 1
}

Write-Host ""
Write-Host "Building Release configuration..."
& $cmake --build . --config Release
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] Build failed"
    exit 1
}

Write-Host ""
Write-Host "========================================"
Write-Host "Build completed successfully!"
Write-Host "Output: build\Release\"
Write-Host "========================================"
Get-ChildItem Release\*.exe 2>$null

# --- Code Signing (optional) ---
$signScript = Join-Path $PSScriptRoot "scripts\sign.ps1"
if (Test-Path $signScript) {
    $localConfig = Join-Path $PSScriptRoot "scripts\sign_config.local.ps1"
    if ($env:UNLEAF_SIGN_PFX -or (Test-Path $localConfig)) {
        Write-Host "`nSigning executables..."
        & $signScript -BinDir (Join-Path $PSScriptRoot "build\Release")
    }
}
