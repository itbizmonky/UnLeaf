#Requires -RunAsAdministrator
<#
.SYNOPSIS
    UnLeaf development code signing certificate generator.
.DESCRIPTION
    Creates a self-signed code signing certificate (RSA 2048 / SHA-256)
    and exports it as PFX (private key + cert) and CER (public cert only).
.PARAMETER Subject
    Certificate subject CN. Default: "UnLeaf Development"
.PARAMETER OutputPath
    Directory for output files. Default: certs\ under project root.
.PARAMETER Password
    PFX export password. Required.
.PARAMETER ValidYears
    Certificate validity in years. Default: 5
#>
param(
    [string]$Subject = "UnLeaf Development",
    [string]$OutputPath,
    [Parameter(Mandatory = $true)]
    [string]$Password,
    [int]$ValidYears = 5
)

$ErrorActionPreference = "Stop"

# Resolve output directory
if (-not $OutputPath) {
    $OutputPath = Join-Path (Split-Path $PSScriptRoot -Parent) "certs"
}
if (-not (Test-Path $OutputPath)) {
    New-Item -ItemType Directory -Path $OutputPath -Force | Out-Null
}

$pfxFile = Join-Path $OutputPath "UnLeaf_Dev.pfx"
$cerFile = Join-Path $OutputPath "UnLeaf_Dev.cer"

# Check for existing files
if ((Test-Path $pfxFile) -or (Test-Path $cerFile)) {
    Write-Warning "Certificate files already exist in $OutputPath"
    $reply = Read-Host "Overwrite? (y/N)"
    if ($reply -ne 'y') {
        Write-Host "Aborted."
        exit 0
    }
}

Write-Host "[CERT] Creating self-signed code signing certificate..."
Write-Host "[CERT] Subject : CN=$Subject"
Write-Host "[CERT] Algorithm: RSA 2048 / SHA256"
Write-Host "[CERT] Valid    : $ValidYears years"

# Create certificate in CurrentUser\My
$notAfter = (Get-Date).AddYears($ValidYears)
$cert = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject "CN=$Subject" `
    -KeyAlgorithm RSA `
    -KeyLength 2048 `
    -HashAlgorithm SHA256 `
    -CertStoreLocation Cert:\CurrentUser\My `
    -NotAfter $notAfter

Write-Host "[CERT] Certificate created: $($cert.Thumbprint)"

# Export PFX (private key + certificate)
$securePassword = ConvertTo-SecureString -String $Password -Force -AsPlainText
Export-PfxCertificate -Cert $cert -FilePath $pfxFile -Password $securePassword | Out-Null
Write-Host "[CERT] PFX exported: $pfxFile"

# Export CER (public certificate only)
Export-Certificate -Cert $cert -FilePath $cerFile | Out-Null
Write-Host "[CERT] CER exported: $cerFile"

# Clean up from cert store (optional - keeps store tidy)
Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -Force

Write-Host ""
Write-Host "========================================"
Write-Host "[CERT] Certificate generation complete!"
Write-Host "========================================"
Write-Host ""
Write-Host "Next steps:"
Write-Host ""
Write-Host "  1. (Optional) Trust the certificate locally (Admin PowerShell):"
Write-Host "     Import-Certificate -FilePath `"$cerFile`" -CertStoreLocation Cert:\LocalMachine\Root"
Write-Host ""
Write-Host "  2. Configure signing (choose one):"
Write-Host ""
Write-Host "     A. Environment variables (current session):"
Write-Host "        `$env:UNLEAF_SIGN_PFX = `"$pfxFile`""
Write-Host "        `$env:UNLEAF_SIGN_PASSWORD = `"$Password`""
Write-Host ""
Write-Host "     B. Local config file:"
Write-Host "        Copy scripts\sign_config.local.ps1.template -> scripts\sign_config.local.ps1"
Write-Host "        Copy scripts\sign_config.local.bat.template -> scripts\sign_config.local.bat"
Write-Host "        Edit with your PFX path and password."
Write-Host ""
Write-Host "  3. Build with signing:"
Write-Host "     .\build.bat  (or .\build.ps1)"
