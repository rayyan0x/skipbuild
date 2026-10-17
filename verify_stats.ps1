#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Verifies remake.exe statistics tracking works end-to-end via CLI.
.DESCRIPTION
    Creates a temp C++ file, runs remake twice (miss then hit), and verifies stats.
    Uses the SKIPBUILD_DIR environment variable (or legacy BUILDCACHE_DIR) for cache directory.
#>

$ErrorActionPreference = "Stop"

# --- Locate Visual Studio vcvars64.bat ---
function Find-Vcvars {
    # Primary: VS BuildTools (from compile.bat)
    $btPath = "F:\VS_BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    if (Test-Path $btPath) { return $btPath }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { $vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" }
    if (Test-Path $vswhere) {
        $installPath = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($installPath) {
            $vcvars = Join-Path $installPath "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $vcvars) { return $vcvars }
        }
    }
    # Fallback: common default locations
    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    return $null
}

$vcvarsPath = Find-Vcvars
if (-not $vcvarsPath) {
    Write-Error "Could not find vcvars64.bat. Install Visual Studio with C++ workload."
    exit 1
}
Write-Host "Using vcvars: $vcvarsPath" -ForegroundColor Cyan

# --- Import VS environment silently ONCE into this PowerShell process ---
Write-Host "Importing Visual Studio environment..." -ForegroundColor Cyan
$vcvarsOutput = cmd /c "call `"$vcvarsPath`" > nul 2>&1 && set"
foreach ($line in $vcvarsOutput) {
    if ($line -match "^([^=]+)=(.*)$") {
        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
    }
}
Write-Host "Environment imported." -ForegroundColor Green

# --- Locate remake.exe ---
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$remakeExe = "C:\temp\remake.exe"
if (-not (Test-Path $remakeExe)) {
    # Fallback to build output
    $remakeExe = Join-Path $scriptDir "build\Release\remake.exe"
}
if (-not (Test-Path $remakeExe)) {
    Write-Error "remake.exe not found. Build the project first."
    exit 1
}
Write-Host "Using remake: $remakeExe" -ForegroundColor Cyan

# --- Setup isolated temp directory ---
$testDir = [System.IO.Path]::GetTempPath() + "remake_verify_" + [System.Guid]::NewGuid().ToString("N").Substring(0,8)
New-Item -ItemType Directory -Path $testDir | Out-Null
Write-Host "Test directory: $testDir" -ForegroundColor Cyan

# Dummy C++ source
$srcFile = Join-Path $testDir "dummy_test.cpp"
"int main() { return 0; }" | Set-Content -Path $srcFile -Encoding utf8

# Cache dir (separate from test dir)
$cacheDir = Join-Path $testDir "cache"
New-Item -ItemType Directory -Path $cacheDir | Out-Null

# Helper: run remake.exe via cmd.exe with vcvars in same session, cd to test dir first
function Invoke-Remake {
    param([string[]]$CmdArgs)
    Write-Host "DEBUG: CmdArgs.Count = $($CmdArgs.Count), CmdArgs = $CmdArgs" -ForegroundColor DarkGray
    $argStr = ($CmdArgs -join ' ')
    Write-Host "DEBUG Args: [$argStr]" -ForegroundColor DarkGray
    $cmd = "cd /d $testDir && call $vcvarsPath x64 && $remakeExe --cache-dir $cacheDir $argStr"
    Write-Host "DEBUG: $cmd" -ForegroundColor DarkGray
    & cmd.exe /c $cmd
}

function Show-Stats {
    $cmd = "cd /d $testDir && call $vcvarsPath x64 && $remakeExe --cache-dir $cacheDir --stats"
    Write-Host "DEBUG: $cmd" -ForegroundColor DarkGray
    & cmd.exe /c $cmd
}

try {
    Write-Host "`n=== CLEAR STATS ===" -ForegroundColor Green
    Invoke-Remake @("--clear-stats")

    Write-Host "`n=== RUN 1 (expect MISS) ===" -ForegroundColor Green
    Invoke-Remake @("cl", "/c", $srcFile)

    Write-Host "`n=== STATS AFTER RUN 1 ===" -ForegroundColor Yellow
    Show-Stats

    Write-Host "`n=== RUN 2 (expect HIT) ===" -ForegroundColor Green
    Invoke-Remake @("cl", "/c", $srcFile)

    Write-Host "`n=== STATS AFTER RUN 2 ===" -ForegroundColor Yellow
    Show-Stats

    Write-Host "`n=== VERIFICATION ===" -ForegroundColor Green
    $statsOut = & $remakeExe --cache-dir $cacheDir --stats
    Write-Host "Raw stats output:" -ForegroundColor Cyan
    Write-Host $statsOut
    
    $hitLine = ($statsOut | Select-String "Total Hits:").Line
    $missLine = ($statsOut | Select-String "Total Misses:").Line
    $rateLine = ($statsOut | Select-String "Cache Hit Rate:").Line
    
    Write-Host "Parsed lines:" -ForegroundColor Cyan
    Write-Host "Hit line: $hitLine"
    Write-Host "Miss line: $missLine"
    Write-Host "Rate line: $rateLine"
    
    if ($hitLine -match "Total Hits:\s+(\d+)") { $hits = [int]$matches[1] }
    if ($missLine -match "Total Misses:\s+(\d+)") { $misses = [int]$matches[1] }
    if ($rateLine -match "Cache Hit Rate:\s+([\d.]+)%") { $rate = [double]$matches[1] }

    Write-Host "Hits:   $hits"
    Write-Host "Misses: $misses"
    Write-Host "Rate:   $rate%"

    if ($hits -eq 1 -and $misses -eq 1 -and [math]::Abs($rate - 50.0) -lt 0.1) {
        Write-Host "`n✅ SUCCESS: Statistics tracking works correctly (1 Hit, 1 Miss, 50% Hit Rate)" -ForegroundColor Green
        exit 0
    } else {
        Write-Host "`n❌ FAILURE: Unexpected stats values" -ForegroundColor Red
        exit 1
    }
}
finally {
    # Cleanup
    if (Test-Path $srcFile) { Remove-Item -Force $srcFile }
    $objFile = Join-Path $testDir "dummy_test.obj"
    if (Test-Path $objFile) { Remove-Item -Force $objFile }
    if (Test-Path $testDir) { Remove-Item -Recurse -Force $testDir }
    Write-Host "`nCleaned up temp directory." -ForegroundColor Cyan
}