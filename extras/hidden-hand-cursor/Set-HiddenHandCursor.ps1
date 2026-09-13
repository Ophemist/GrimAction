[CmdletBinding()]
param(
    # Grim Dawn install folder (contains resources\UI.arc). Detected from Steam when omitted.
    [string]$GameRoot,
    # Dot: a small white dot with a dark edge at the cursor's click point (top-left pixel). Blank: fully transparent, for use
    # with the ReShade ThirdPersonDot effect.
    [ValidateSet('Dot', 'Blank')][string]$Style = 'Dot',
    # Put the original hand cursor back from the backup this script made.
    [switch]$Restore,
    # Work on a copy of UI.arc instead of the game's (testing).
    [string]$ArchivePath
)

# Optional extra, applied by the player: makes Grim Dawn's default hand cursor (cursor/cursordefault.tex and its _lg
# version) fully transparent, so the ReShade aim dot (extras\reshade\ThirdPersonDot.fx, kept on all the time) is the only
# pointer. The attack sword, dialog bubble, merchant bag and controller reticles are unchanged.
#
# This edits resources\UI.arc in the game folder. It keeps a backup next to this script, and -Restore puts it back. Steam's
# "Verify integrity of game files" also restores the original. Game updates may undo it; run it again afterwards.
# No game art is shipped: the blank textures are made from your own UI.arc at run time.
$ErrorActionPreference = 'Stop'
$backupDir = Join-Path $PSScriptRoot 'backup'
$cursorNames = @('cursordefault.tex', 'cursordefault_lg.tex')

function Find-GameRoot {
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        $steam = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
        foreach ($value in @($steam.SteamPath, $steam.InstallPath)) {
            if (-not $value) { continue }
            $libraries = @($value -replace '/', '\')
            $vdf = Join-Path $libraries[0] 'steamapps\libraryfolders.vdf'
            if (Test-Path -LiteralPath $vdf) {
                $libraries += [regex]::Matches((Get-Content -Raw -LiteralPath $vdf), '"path"\s+"([^"]+)"') | ForEach-Object { $_.Groups[1].Value -replace '\\\\', '\' }
            }
            foreach ($library in $libraries) {
                $candidate = Join-Path $library 'steamapps\common\Grim Dawn'
                if (Test-Path -LiteralPath (Join-Path $candidate 'resources\UI.arc')) { return $candidate }
            }
        }
    }
    return 'C:\Program Files (x86)\Steam\steamapps\common\Grim Dawn'
}

if (-not $GameRoot) { $GameRoot = Find-GameRoot }
$archiveTool = Join-Path $GameRoot 'ArchiveTool.exe'
if (-not $ArchivePath) { $ArchivePath = Join-Path $GameRoot 'resources\UI.arc' }
foreach ($required in $archiveTool, $ArchivePath) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Not found: $required. Pass -GameRoot with your Grim Dawn folder." }
}
if (Get-Process -Name 'Grim Dawn' -ErrorAction SilentlyContinue) { throw 'Close Grim Dawn first.' }

$backup = Join-Path $backupDir 'UI.arc'
if ($Restore) {
    if (-not (Test-Path -LiteralPath $backup)) { throw 'No backup found. Use Steam > Grim Dawn > Properties > Installed Files > Verify integrity instead.' }
    Copy-Item -LiteralPath $backup -Destination $ArchivePath -Force
    Write-Host 'Original hand cursor restored.' -ForegroundColor Green
    exit 0
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('grimaction-cursor-' + [guid]::NewGuid().ToString('N'))
try {
    $extracted = Join-Path $work 'extracted'
    $blank = Join-Path $work 'blank'
    New-Item -ItemType Directory -Force -Path $extracted, (Join-Path $blank 'cursor') | Out-Null
    foreach ($name in $cursorNames) {
        & $archiveTool $ArchivePath -extract $extracted "cursor/$name" | Out-Null
    }
    # ArchiveTool reports success even for a missing file, so locate what it actually wrote.
    $found = @{}
    foreach ($name in $cursorNames) {
        $file = Get-ChildItem -LiteralPath $extracted -Recurse -File -Filter $name | Select-Object -First 1
        if (-not $file) { throw "cursor/$name was not found in UI.arc; nothing was changed." }
        $found[$name] = $file.FullName
    }

    $changed = $false
    $originalCursor = $false
    foreach ($name in $cursorNames) {
        $bytes = [System.IO.File]::ReadAllBytes($found[$name])
        # TEX header (12 bytes) + DDSR header (128 bytes) + uncompressed 32-bit pixels, no mipmaps.
        if ($bytes.Length -lt 140 -or [System.Text.Encoding]::ASCII.GetString($bytes, 0, 3) -ne 'TEX' -or
            [System.Text.Encoding]::ASCII.GetString($bytes, 12, 4) -ne 'DDSR') { throw "Unexpected texture format in $name; nothing was changed." }
        $width = [BitConverter]::ToUInt32($bytes, 12 + 16); $height = [BitConverter]::ToUInt32($bytes, 12 + 12)
        $bits = [BitConverter]::ToUInt32($bytes, 12 + 88)
        if ($bits -ne 32 -or $bytes.Length -ne 140 + $width * $height * 4) { throw "Unexpected texture layout in $name; nothing was changed." }
        # Pixels are BGRA rows from the top. The engine places the texture's top-left pixel on the mouse position, so the
        # dot sits there: white core, dark edge on its right and bottom (the corner clips the other two sides).
        $core = [int][Math]::Max(1, $width / 16); $edge = [int][Math]::Max(1, $width / 32)
        $pixels = New-Object byte[] ($width * $height * 4)
        if ($Style -eq 'Dot') {
            for ($y = 0; $y -lt $core + $edge; $y++) {
                for ($x = 0; $x -lt $core + $edge; $x++) {
                    $o = ($y * $width + $x) * 4
                    $value = if ($x -lt $core -and $y -lt $core) { 255 } else { 0 }
                    $pixels[$o] = $value; $pixels[$o + 1] = $value; $pixels[$o + 2] = $value; $pixels[$o + 3] = if ($value -eq 255) { 255 } else { 200 }
                }
            }
        }
        # A texture this script wrote (either style) has nothing outside the dot's corner square; the game's hand does.
        for ($y = 0; $y -lt $height; $y++) {
            for ($x = 0; $x -lt $width; $x++) {
                if ($x -lt $core + $edge -and $y -lt $core + $edge) { continue }
                if ($bytes[140 + ($y * $width + $x) * 4 + 3] -ne 0) { $originalCursor = $true; break }
            }
            if ($originalCursor) { break }
        }
        for ($i = 0; $i -lt $pixels.Length; $i++) { if ($bytes[140 + $i] -ne $pixels[$i]) { $changed = $true; $bytes[140 + $i] = $pixels[$i] } }
        [System.IO.File]::WriteAllBytes((Join-Path $blank "cursor\$name"), $bytes)
    }
    if (-not $changed) { Write-Host "The hand cursor is already set to $Style." -ForegroundColor Yellow; exit 0 }

    # Back up only an archive that still has the game's hand: the first run, or after a game update or Steam verify replaced
    # UI.arc. Switching styles must never overwrite the original backup with an already-edited archive.
    New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
    if ($originalCursor) {
        Copy-Item -LiteralPath $ArchivePath -Destination $backup -Force
    }
    elseif (-not (Test-Path -LiteralPath $backup)) {
        throw 'UI.arc already has an edited cursor but no backup exists here. Verify game files in Steam first, then run this again.'
    }

    # ArchiveTool ignores an absolute base and stores the whole path as the entry name; run it from the base folder with
    # relative paths so the entries are named cursor/<file>, replacing the originals.
    Push-Location -LiteralPath $blank
    try { & $archiveTool $ArchivePath -replace 'cursor' '.' 9 | Out-Null; $replaceExit = $LASTEXITCODE }
    finally { Pop-Location }
    if ($replaceExit -ne 0) {
        Copy-Item -LiteralPath $backup -Destination $ArchivePath -Force
        throw "ArchiveTool failed to update UI.arc (exit $replaceExit); the original was put back."
    }

    # Verify: read both textures back and require them to match exactly what was written.
    $check = Join-Path $work 'check'
    New-Item -ItemType Directory -Force -Path $check | Out-Null
    foreach ($name in $cursorNames) {
        & $archiveTool $ArchivePath -extract $check "cursor/$name" | Out-Null
        $readBack = Get-ChildItem -LiteralPath $check -Recurse -File -Filter $name | Select-Object -First 1
        if (-not $readBack) { Copy-Item -LiteralPath $backup -Destination $ArchivePath -Force; throw "Verification could not read $name back; the original UI.arc was put back." }
        $bytes = [System.IO.File]::ReadAllBytes($readBack.FullName)
        $blankBytes = [System.IO.File]::ReadAllBytes((Join-Path $blank "cursor\$name"))
        if ([Convert]::ToBase64String($bytes) -ne [Convert]::ToBase64String($blankBytes)) {
            Copy-Item -LiteralPath $backup -Destination $ArchivePath -Force
            throw "Verification failed for $name; the original UI.arc was put back."
        }
    }
    if ($Style -eq 'Dot') { Write-Host 'Hand cursor replaced with a dot.' -ForegroundColor Green }
    else { Write-Host 'Hand cursor hidden. Keep the ReShade ThirdPersonDot effect on so you always have a pointer.' -ForegroundColor Green }
    Write-Host 'To undo: run this again with -Restore, or verify game files in Steam.'
}
finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
