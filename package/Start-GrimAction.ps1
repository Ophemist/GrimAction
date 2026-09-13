[CmdletBinding()]
param(
    # Grim Dawn install folder (the one containing x64\Grim Dawn.exe). Optional: detected from Steam when omitted.
    [string]$GameRoot,
    # Skip the "press Enter at the main menu" pause (for players who start this after the menu is already up).
    [switch]$NoPrompt
)

# GrimAction player launcher. Verifies the package files, checks your settings, finds Grim Dawn, and loads the camera runtime
# into the running game. Nothing is installed or copied into the game folder.
$ErrorActionPreference = 'Stop'
$packageRoot = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $packageRoot 'bin'
$injector = Join-Path $bin 'GdTpc.Injector.exe'
$runtime = Join-Path $bin 'gdtpc_runtime_collision.dll'
$settings = Join-Path $packageRoot 'settings\runtime.ini'
$gamePathFile = Join-Path $packageRoot 'settings\game-path.txt'
$logs = Join-Path $packageRoot 'logs'

function Stop-WithMessage([string]$Message) {
    Write-Host ''
    Write-Host "GrimAction did not start: $Message" -ForegroundColor Red
    exit 1
}

# 1. Package integrity: every shipped binary and script must match the manifest it was built with.
$manifestPath = Join-Path $bin 'package-manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath)) { Stop-WithMessage 'bin\package-manifest.json is missing. Re-extract the download.' }
$manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
foreach ($entry in $manifest.files) {
    $file = Join-Path $packageRoot $entry.path
    if (-not (Test-Path -LiteralPath $file)) { Stop-WithMessage "$($entry.path) is missing. Re-extract the download." }
    # Files from a downloaded zip carry the internet zone mark; clear it so Windows does not block the DLL.
    Unblock-File -LiteralPath $file -ErrorAction SilentlyContinue
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) {
        Stop-WithMessage "$($entry.path) does not match this release (damaged download or modified file). Re-extract the download."
    }
}
Write-Host "GrimAction $($manifest.version): package files verified."

# 2. Settings: checked with the runtime's own parser before anything touches the game.
if (-not (Test-Path -LiteralPath $settings)) { Stop-WithMessage 'settings\runtime.ini is missing. Copy it back from the download.' }
$ErrorActionPreference = 'Continue'
& $injector --check-config $runtime $settings 2>&1 | ForEach-Object { Write-Host "  $_" }
$checked = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($checked -ne 0) {
    Stop-WithMessage 'settings\runtime.ini has an invalid value (see the message above). Fix it or restore the original from the download; every key must be present.'
}

# 3. Find Grim Dawn: -GameRoot, then settings\game-path.txt, then Steam libraries, then the default Steam folder.
function Test-GameRoot([string]$Path) {
    return $Path -and (Test-Path -LiteralPath (Join-Path $Path 'x64\Grim Dawn.exe'))
}
function Find-SteamGameRoot {
    $candidates = New-Object System.Collections.Generic.List[string]
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        $steam = (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue)
        foreach ($value in @($steam.SteamPath, $steam.InstallPath)) {
            if (-not $value) { continue }
            $steamRoot = $value -replace '/', '\'
            $candidates.Add($steamRoot)
            $libraryFile = Join-Path $steamRoot 'steamapps\libraryfolders.vdf'
            if (Test-Path -LiteralPath $libraryFile) {
                foreach ($match in [regex]::Matches((Get-Content -Raw -LiteralPath $libraryFile), '"path"\s+"([^"]+)"')) {
                    $candidates.Add(($match.Groups[1].Value -replace '\\\\', '\'))
                }
            }
        }
    }
    foreach ($library in $candidates) {
        $root = Join-Path $library 'steamapps\common\Grim Dawn'
        if (Test-GameRoot $root) { return $root }
    }
    return $null
}
if (-not $GameRoot -and (Test-Path -LiteralPath $gamePathFile)) {
    $GameRoot = (Get-Content -LiteralPath $gamePathFile | Where-Object { $_.Trim() -and -not $_.TrimStart().StartsWith('#') } | Select-Object -First 1)
    if ($GameRoot) { $GameRoot = $GameRoot.Trim().Trim('"') }
}
if (-not (Test-GameRoot $GameRoot)) { $GameRoot = Find-SteamGameRoot }
if (-not (Test-GameRoot $GameRoot)) { $GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Grim Dawn' }
if (-not (Test-GameRoot $GameRoot)) {
    Stop-WithMessage 'Grim Dawn was not found. Put your Grim Dawn folder (the one containing x64\Grim Dawn.exe) on one line in settings\game-path.txt.'
}
Write-Host "Grim Dawn: $GameRoot"

# 4. The game must already be running and sitting at the main menu.
if (-not (Get-Process -Name 'Grim Dawn' -ErrorAction SilentlyContinue)) {
    Stop-WithMessage 'Grim Dawn is not running. Start the game (64-bit), wait for the main menu, then run this again.'
}
if (-not $NoPrompt) {
    Write-Host ''
    Read-Host 'Wait until the Grim Dawn main menu is showing, then press Enter here'
}

# 5. Load the runtime.
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$log = Join-Path $logs ('session-' + [DateTimeOffset]::Now.ToString('yyyyMMdd-HHmmss') + '.csv')
# Windows PowerShell 5.1 turns a native program's error output into terminating errors under 'Stop'; the exit code is what
# decides here, so let the injector's messages through as text.
$ErrorActionPreference = 'Continue'
& $injector --collision $GameRoot $runtime $settings $log 2>&1 | ForEach-Object { Write-Host "  $_" }
$code = $LASTEXITCODE
if ($code -eq 0) {
    Write-Host ''
    Write-Host 'GrimAction is running. Load your character and press F8 for third person.' -ForegroundColor Green
    Write-Host 'Run "Stop GrimAction" if you want the normal camera back without quitting (optional; quitting the game is safe).'
    exit 0
}
switch ($code) {
    3 { Stop-WithMessage 'a required file is missing. Re-extract the download.' }
    4 { Stop-WithMessage 'this Grim Dawn version is not supported by this GrimAction release (the game was probably updated). Nothing was loaded; check for a GrimAction update.' }
    5 { Stop-WithMessage 'Grim Dawn (64-bit) is not running.' }
    6 { Stop-WithMessage 'GrimAction is already loaded in this game session. To start it again, quit Grim Dawn completely and relaunch the game.' }
    7 { Stop-WithMessage 'Windows did not allow access to Grim Dawn. Run the game and GrimAction as the same user (both normal, or both as administrator).' }
    default { Stop-WithMessage "GrimAction could not be activated (code $code). It is inactive for this session. Do not run Start again now: quit Grim Dawn normally, relaunch, and try once more. If it keeps happening, report the message above." }
}
