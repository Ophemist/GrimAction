[CmdletBinding()]
param([string]$GameRoot)

# GrimAction: returns the camera to normal and ends the session's logging while the game keeps running. Optional; quitting
# Grim Dawn is also safe. The runtime stays loaded but inactive until the game exits.
$ErrorActionPreference = 'Stop'
$packageRoot = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $packageRoot 'bin'
$injector = Join-Path $bin 'GdTpc.Injector.exe'
$runtime = Join-Path $bin 'gdtpc_runtime_collision.dll'
$gamePathFile = Join-Path $packageRoot 'settings\game-path.txt'

function Test-GameRoot([string]$Path) { return $Path -and (Test-Path -LiteralPath (Join-Path $Path 'x64\Grim Dawn.exe')) }
if (-not $GameRoot -and (Test-Path -LiteralPath $gamePathFile)) {
    $GameRoot = (Get-Content -LiteralPath $gamePathFile | Where-Object { $_.Trim() -and -not $_.TrimStart().StartsWith('#') } | Select-Object -First 1)
    if ($GameRoot) { $GameRoot = $GameRoot.Trim().Trim('"') }
}
if (-not (Test-GameRoot $GameRoot)) {
    $process = Get-Process -Name 'Grim Dawn' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($process -and $process.Path) { $GameRoot = Split-Path -Parent (Split-Path -Parent $process.Path) }
}
if (-not (Test-GameRoot $GameRoot)) {
    Write-Host 'Grim Dawn is not running (or its folder was not found). Nothing to stop.' -ForegroundColor Yellow
    exit 1
}
$ErrorActionPreference = 'Continue'
& $injector --collision-stop $GameRoot $runtime 2>&1 | ForEach-Object { Write-Host "  $_" }
$code = $LASTEXITCODE
if ($code -eq 0) {
    Write-Host 'GrimAction stopped: camera restored. You can keep playing in the normal camera or quit the game.' -ForegroundColor Green
    exit 0
}
if ($code -eq 16) {
    Write-Host 'GrimAction is not loaded in this game session. Nothing to stop.' -ForegroundColor Yellow
    exit 1
}
Write-Host "Stop did not complete (code $code). Do not retry; quit Grim Dawn normally." -ForegroundColor Red
exit 1
