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
    irm https://raw.githubusercontent.com/rayyan0x/skipbuild/main/scripts/install.ps1 | iex

.EXAMPLE
    irm https://raw.githubusercontent.com/rayyan0x/skipbuild/main/scripts/install.ps1 | iex -Version v0.1.0

.PARAMETER Version
    Version tag to install (default: latest)

.PARAMETER InstallDir
    Installation directory (default: $env:LOCALAPPDATA\skipbuild\bin)

.PARAMETER BuildFromSource
    If no prebuilt asset is available, explicitly allow a source build.
#>

param(
    [Parameter(Mandatory=$false)]
    [string]$Version = "latest",

    [Parameter(Mandatory=$false)]
    [string]$InstallDir = "$env:LOCALAPPDATA\skipbuild\bin",

    [Parameter(Mandatory=$false)]
    [switch]$BuildFromSource
)

$ErrorActionPreference = 'Stop'

$Repo = "rayyan0x/skipbuild"
$Asset = "skipbuild-windows-x64.exe"

# Resolve version
if ($Version -eq "latest") {
    try {
        $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -ErrorAction Stop
        $Version = $release.tag_name -replace '^v', ''
    } catch {
        Write-Error "Failed to determine latest version: $_"
        exit 1
    }
}

$Url = "https://github.com/rayyan0x/skipbuild/releases/download/v$Version/skipbuild-windows-x64.exe"
$SumsUrl = "https://github.com/rayyan0x/skipbuild/releases/download/v$Version/SHA256SUMS"

Write-Host "Installing skipbuild v$Version to $InstallDir"

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

    # Verify checksum if available
    try {
        Invoke-WebRequest -Uri $SumsUrl -OutFile $SumFile -UseBasicParsing -ErrorAction Stop
        $expected = (Get-Content $SumFile | Where-Object { $_ -like "*skipbuild-windows-x64.exe" }) -split '\s+' | Select-Object -First 1
        $actual = (Get-FileHash -Path $TempFile -Algorithm SHA256).Hash.ToLower()
        if ($expected -and $expected -ne $actual) {
            Write-Error "Checksum mismatch! Expected $expected, got $actual"
            exit 1
        }
        if ($expected) { Write-Host "Checksum verified" } else { Write-Warning "Asset not listed in SHA256SUMS; skipping verification" }
    } catch {
        Write-Warning "Could not verify checksum (SHA256SUMS not available): $_"
    }

    # Install
    $dest = Join-Path $InstallDir "skipbuild.exe"
    Move-Item -Path $TempFile -Destination $dest -Force
    Write-Host "Installed skipbuild.exe to $dest"

    # Add to user PATH if not already present
    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if ($userPath -notlike "*$InstallDir*") {
        $newPath = $userPath + ";" + $InstallDir
        [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
        Write-Host "Added $InstallDir to user PATH"
        Write-Host "Restart your shell for PATH changes to take effect"
    } else {
        Write-Host "$InstallDir already in PATH"
    }

} finally {
    if (Test-Path $TempFile) { Remove-Item -Force $TempFile }
    if (Test-Path $SumFile) { Remove-Item -Force $SumFile }
}