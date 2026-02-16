<#
.SYNOPSIS
    Sign UnLeaf executables with a code signing certificate.
.DESCRIPTION
    Resolves certificate settings, locates signtool.exe, signs and verifies executables.
    Safe by default: skips signing when no certificate is configured (exit 0).
    Use -Force to make missing certificate an error (exit 1).
.PARAMETER PfxPath
    Path to PFX file. Overrides environment variable / local config.
.PARAMETER PfxPassword
    PFX password. Overrides environment variable / local config.
.PARAMETER BinDir
    Directory containing executables to sign. Default: build\Release
.PARAMETER Files
    Specific file names to sign. Default: *.exe in BinDir.
.PARAMETER TimestampServer
    RFC 3161 timestamp server URL. Default: http://timestamp.digicert.com
.PARAMETER SkipVerify
    Skip signature verification after signing.
.PARAMETER Force
    Fail with exit 1 if no certificate is configured.
#>
param(
    [string]$PfxPath,
    [string]$PfxPassword,
    [string]$BinDir,
    [string[]]$Files,
    [string]$TimestampServer = "http://timestamp.digicert.com",
    [switch]$SkipVerify,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

# --- Certificate Resolution (priority: env var > local config > parameter) ---

# 1. Load local config if it exists
$localConfig = Join-Path $PSScriptRoot "sign_config.local.ps1"
if (Test-Path $localConfig) {
    . $localConfig
    # $PfxPath / $PfxPassword may now be set from the config file
}

# 2. Environment variables override local config
if ($env:UNLEAF_SIGN_PFX) {
    $PfxPath = $env:UNLEAF_SIGN_PFX
}
if ($env:UNLEAF_SIGN_PASSWORD) {
    $PfxPassword = $env:UNLEAF_SIGN_PASSWORD
}

# 3. Check if we have a certificate configured
if (-not $PfxPath) {
    if ($Force) {
        Write-Host "[SIGN][ERROR] No certificate configured. Set UNLEAF_SIGN_PFX or use -PfxPath."
        exit 1
    }
    Write-Host "[SIGN] No certificate configured. Skipping code signing."
    exit 0
}

if (-not (Test-Path $PfxPath)) {
    Write-Host "[SIGN][ERROR] PFX file not found: $PfxPath"
    exit 1
}

if (-not $PfxPassword) {
    Write-Host "[SIGN][ERROR] PFX password not set. Set UNLEAF_SIGN_PASSWORD or use -PfxPassword."
    exit 1
}

# --- Resolve BinDir ---
if (-not $BinDir) {
    $BinDir = Join-Path (Split-Path $PSScriptRoot -Parent) "build\Release"
}
if (-not (Test-Path $BinDir)) {
    Write-Host "[SIGN][ERROR] Binary directory not found: $BinDir"
    exit 1
}

# --- Locate signtool.exe ---
function Find-SignTool {
    $kitsRoot = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
        "${env:ProgramFiles}\Windows Kits\10\bin"
    )
    foreach ($root in $kitsRoot) {
        if (Test-Path $root) {
            $candidates = Get-ChildItem -Path $root -Recurse -Filter "signtool.exe" -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match "x64[\\/]signtool\.exe$" } |
                Sort-Object { $_.Directory.Name } -Descending
            if ($candidates) {
                return $candidates[0].FullName
            }
        }
    }
    return $null
}

$signtool = Find-SignTool
if (-not $signtool) {
    Write-Host "[SIGN][ERROR] signtool.exe not found. Install Windows SDK."
    exit 1
}
Write-Host "[SIGN] Using signtool: $signtool"

# --- Collect files to sign ---
if ($Files) {
    $targetFiles = $Files | ForEach-Object { Join-Path $BinDir $_ } | Where-Object { Test-Path $_ }
} else {
    $targetFiles = Get-ChildItem -Path $BinDir -Filter "*.exe" | Select-Object -ExpandProperty FullName
}

if (-not $targetFiles -or $targetFiles.Count -eq 0) {
    Write-Host "[SIGN][WARN] No executable files found in: $BinDir"
    exit 0
}

# --- Sign each file ---
$failed = $false
foreach ($file in $targetFiles) {
    $fileName = Split-Path $file -Leaf
    Write-Host "[SIGN] Signing: $fileName"

    $signArgs = @(
        "sign",
        "/f", $PfxPath,
        "/p", $PfxPassword,
        "/fd", "SHA256",
        "/tr", $TimestampServer,
        "/td", "SHA256",
        "/d", "UnLeaf"
        $file
    )

    & $signtool $signArgs 2>&1 | ForEach-Object { Write-Host "       $_" }

    if ($LASTEXITCODE -ne 0) {
        Write-Host "[SIGN][ERROR] Failed to sign: $fileName"
        $failed = $true
    }
}

# --- Verify signatures ---
if (-not $SkipVerify -and -not $failed) {
    Write-Host ""
    Write-Host "[SIGN] Verifying signatures..."
    foreach ($file in $targetFiles) {
        $fileName = Split-Path $file -Leaf
        & $signtool verify /pa $file 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[SIGN][WARN] Verification failed for $fileName (expected for self-signed certificates)"
        } else {
            Write-Host "[SIGN][OK] Verified: $fileName"
        }
    }
}

# --- Result ---
Write-Host ""
if ($failed) {
    Write-Host "[SIGN][ERROR] Some files failed to sign."
    exit 1
} else {
    Write-Host "[SIGN][SUCCESS] All files signed."
    exit 0
}
