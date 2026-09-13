[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$detoursRoot = Join-Path $projectRoot 'third_party\detours'
$detoursSource = Join-Path $detoursRoot 'src'
$license = Join-Path $detoursRoot 'LICENSE.md'
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'

if (-not (Test-Path -LiteralPath $license) -or -not (Test-Path -LiteralPath (Join-Path $detoursSource 'detours.cpp'))) {
    throw 'The pinned Microsoft Detours source snapshot is missing.'
}
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Build Tools could not be located.'
}

$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) {
    throw 'The Microsoft x64 C++ workload is not installed.'
}

$devcmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
$command = "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul && nmake /nologo"
Push-Location $detoursSource
try {
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw "Detours build exited with code $LASTEXITCODE." }
}
finally {
    Pop-Location
}

$library = Join-Path $detoursRoot 'lib.X64\detours.lib'
if (-not (Test-Path -LiteralPath $library)) { throw 'Detours build did not produce the x64 library.' }
Write-Host "Pinned Microsoft Detours x64 library: $library"
