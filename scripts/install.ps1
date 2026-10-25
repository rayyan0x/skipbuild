<#PSScriptInfo
.VERSION 1.0
.GUID 00000000-0000-0000-0000-000000000000
.AUTHOR rayyan0x
.COMPANYNAME
.COPYRIGHT
.TAGS skipbuild, install
.LICENSEURI
.PROJECTURI https://github.com/rayyan0x/skipbuild
.ICONURI
.EXTERNALMODULEDEPENDENCIES
.REQUIREDSCRIPTS
.EXTERNALSCRIPTDEPENDENCIES
.RELEASENOTES
#>

<#
.SYNOPSIS
    Install skipbuild on Windows.

.DESCRIPTION
    Downloads the latest skipbuild release and installs it to %LOCALAPPDATA%\skipbuild\bin.

.EXAMPLE
    irm https://raw.githubusercontent.com/rayyan0x/skipbuild/main/scripts/install.ps1 -OutFile install.ps1; .\install.ps1

.EXAMPLE
    .\install.ps1 -Version 0.6.2

.PARAMETER Version
    Version tag to install (default: latest)

.PARAMETER InstallDir
    Installation directory (default: $env:LOCALAPPDATA\skipbuild\bin)
#>

param(
    [Parameter(Mandatory=$false)]
    [string]$Version = "latest",

    [Parameter(Mandatory=$false)]
    [string]$InstallDir = "$env:LOCALAPPDATA\skipbuild\bin"
)

$ErrorActionPreference = 'Stop'

$Repo = "rayyan0x/skipbuild"

# Detect the real OS architecture. A 32-bit PowerShell on 64-bit Windows reports
# x86 in PROCESSOR_ARCHITECTURE, so prefer PROCESSOR_ARCHITEW6432.
$arch = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
switch ($arch) {
    'AMD64' { $Arch = 'x64' }
    'ARM64' { $Arch = 'arm64' }
    default { throw "Unsupported architecture: $arch" }
}
$Asset = "skipbuild-windows-$Arch.exe"

# Resolve version
if ($Version -eq "latest") {
    try {
        $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -ErrorAction Stop
        $Version = $release.tag_name -replace '^v', ''
    } catch {
        throw "Failed to determine latest version: $_"
    }
}

$Url = "https://github.com/rayyan0x/skipbuild/releases/download/v$Version/$Asset"
$SumsUrl = "https://github.com/rayyan0x/skipbuild/releases/download/v$Version/SHA256SUMS"

Write-Host "Installing skipbuild v$Version to $InstallDir"

# Exact PATH-entry comparison (a wildcard match breaks on usernames containing
# bracket characters).
function Test-PathContains([string]$PathValue, [string]$Dir) {
    if ([string]::IsNullOrEmpty($PathValue)) { return $false }
    $target = $Dir.TrimEnd('\')
    foreach ($entry in $PathValue -split ';') {
        if ($entry.TrimEnd('\') -ieq $target) { return $true }
    }
    return $false
}

# Create install directory
if (-not (Test-Path $InstallDir)) {
    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
}

# Download binary
$TempFile = [IO.Path]::GetTempFileName()
$SumFile = "$TempFile.sums"
try {
    Write-Host "Downloading from $Url"
    Invoke-WebRequest -Uri $Url -OutFile $TempFile -UseBasicParsing -ErrorAction Stop

    # Verification is mandatory: a missing sums file or an unlisted asset must
    # not silently install an unverified binary.
    Invoke-WebRequest -Uri $SumsUrl -OutFile $SumFile -UseBasicParsing -ErrorAction Stop
    $expected = Get-Content $SumFile | ForEach-Object {
        $parts = $_ -split '\s+', 2
        if ($parts.Count -eq 2 -and $parts[1].TrimStart('*') -eq $Asset) { $parts[0] }
    } | Select-Object -First 1
    if (-not $expected) {
        throw "Asset $Asset is not listed in SHA256SUMS; refusing to install an unverified binary."
    }
    $actual = (Get-FileHash -Path $TempFile -Algorithm SHA256).Hash.ToLower()
    if ($expected -ne $actual) {
        throw "Checksum mismatch for $Asset (expected $expected, got $actual)!"
    }
    Write-Host "Checksum verified"

    # Install
    $dest = Join-Path $InstallDir "skipbuild.exe"
    Move-Item -Path $TempFile -Destination $dest -Force
    Write-Host "Installed skipbuild.exe to $dest"

    # Make it usable in THIS window immediately
    if (-not (Test-PathContains $env:PATH $InstallDir)) {
        $env:PATH = "$InstallDir;$env:PATH"
    }

    # Persist for future terminals
    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if (-not (Test-PathContains $userPath $InstallDir)) {
        [Environment]::SetEnvironmentVariable("Path", ($userPath + ";" + $InstallDir), "User")
        Write-Host "Added $InstallDir to your user PATH (applies to new terminals)."
    }

    Write-Host ""
    Write-Host "skipbuild is ready." -ForegroundColor Green
    Write-Host "  Verify:  skipbuild --version"
    Write-Host "  CMake:   paste the 4-line snippet from the README 'Integration' section once."

} finally {
    if (Test-Path $TempFile) { Remove-Item -Force $TempFile }
    if (Test-Path $SumFile) { Remove-Item -Force $SumFile }
}