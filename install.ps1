<# 
.SYNOPSIS
    Install remake on Windows

.EXAMPLE
    irm https://github.com/yourname/remake/releases/latest/download/install.ps1 | iex
#>

param(
    [string]$Version = "latest",
    [string]$InstallDir = "$env:USERPROFILE\scoop\apps\remake\current"
)

$ErrorActionPreference = "Stop"
$repo = "yourname/remake"

Write-Host "Installing remake..."

if ($Version -eq "latest") {
    $Version = (Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest").tag_name
}

$arch = if ($env:PROCESSOR_ARCHITECTURE -eq "AMD64") { "x86_64" } else { "aarch64" }
$url = "https://github.com/$repo/releases/download/$Version/remake-windows-$arch.zip"

Write-Host "Downloading $url..."
$zip = "$env:TEMP\remake.zip"
Invoke-WebRequest -Uri $url -OutFile $zip

Write-Host "Extracting..."
Expand-Archive -Path $zip -DestinationPath "$env:TEMP\remake" -Force

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
Copy-Item -Path "$env:TEMP\remake\remake.exe" -Destination "$InstallDir\remake.exe" -Force

# Add to PATH if not already
$path = [Environment]::GetEnvironmentVariable("PATH", "User")
if ($path -notlike "*$InstallDir*") {
    [Environment]::SetEnvironmentVariable("PATH", "$path;$InstallDir", "User")
    Write-Host "Added $InstallDir to user PATH"
}

Write-Host "Installed! Restart your shell and run: remake --version"