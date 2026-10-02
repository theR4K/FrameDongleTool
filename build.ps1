# build.ps1 - builds set_region.exe with the MSVC developer shell.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
$ErrorActionPreference = 'Stop'

$project = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $project

# Locate Visual Studio 2022 (or newer)
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found: install Visual Studio 2022 with the C++ workload" }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "No Visual Studio C++ toolset found" }
Write-Host "Using: $vs"

$devshell = Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
Import-Module $devshell
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

# Enter-VsDevShell changes directory; come back to the project.
Set-Location $project

# This toolset accepts /std:c++latest (not /std:c++23); c++latest is C++23 here.
cl /nologo /std:c++latest /EHsc /O2 set_region.cpp `
    wlanapi.lib advapi32.lib ole32.lib crypt32.lib /Fe:set_region.exe
$code = $LASTEXITCODE
if ($code -ne 0) { throw "cl failed with exit code $code" }
Write-Host "OK: $project\set_region.exe"
