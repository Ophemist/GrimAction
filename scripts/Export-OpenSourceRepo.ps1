[CmdletBinding()]
param(
    # Destination working copy of the public repository. Created if missing; tracked files are replaced from the allowlist.
    [string]$Destination = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'GrimAction')
)

# Copies the public subset of this workspace into the GrimAction repository folder. ALLOWLIST ONLY: anything not named
# here (research dumps, disassembly, telemetry, session handoffs, agent notes, build outputs) stays private. After copying,
# the destination is scanned for personal paths and forbidden files, and the export fails if any are found.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$destinationFull = [System.IO.Path]::GetFullPath($Destination)
if ($destinationFull.StartsWith([System.IO.Path]::GetFullPath($projectRoot) + [char]92, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The export destination must be outside the private workspace.'
}

# Source-relative path -> destination-relative path.
$files = [ordered]@{
    'LICENSE' = 'LICENSE'
    'THIRD-PARTY-NOTICES.txt' = 'THIRD-PARTY-NOTICES.txt'
    'Directory.Build.props' = 'Directory.Build.props'
    'docs\REPO_README.md' = 'README.md'
    'docs\PLAYER_GUIDE.md' = 'docs\PLAYER_GUIDE.md'
    'docs\DESIGN.md' = 'docs\DESIGN.md'
    'docs\BUILDING.md' = 'docs\BUILDING.md'
    'docs\ANTIVIRUS.md' = 'docs\ANTIVIRUS.md'
    'config\supported-builds.json' = 'config\supported-builds.json'
    'config\runtime.example.ini' = 'config\runtime.example.ini'
    'config\prohibited-releases.json' = 'config\prohibited-releases.json'
    'scripts\Build-Detours.ps1' = 'scripts\Build-Detours.ps1'
    'scripts\Build-NativeValidation.ps1' = 'scripts\Build-NativeValidation.ps1'
    'scripts\Build-PlayerPackage.ps1' = 'scripts\Build-PlayerPackage.ps1'
    'scripts\Export-OpenSourceRepo.ps1' = 'scripts\Export-OpenSourceRepo.ps1'
    'scripts\Test-SupportedBuild.ps1' = 'scripts\Test-SupportedBuild.ps1'
    'scripts\Test-ReleaseManifest.ps1' = 'scripts\Test-ReleaseManifest.ps1'
    'scripts\Test-ReleaseNotProhibited.ps1' = 'scripts\Test-ReleaseNotProhibited.ps1'
    'scripts\Run-CameraCollision.ps1' = 'scripts\Run-CameraCollision.ps1'
    'scripts\Stop-CameraCollision.ps1' = 'scripts\Stop-CameraCollision.ps1'
    'package\Start-GrimAction.ps1' = 'package\Start-GrimAction.ps1'
    'package\Stop-GrimAction.ps1' = 'package\Stop-GrimAction.ps1'
    'package\Start GrimAction.cmd' = 'package\Start GrimAction.cmd'
    'package\Stop GrimAction.cmd' = 'package\Stop GrimAction.cmd'
    'extras\reshade\ThirdPersonDot.fx' = 'extras\reshade\ThirdPersonDot.fx'
    'extras\hidden-hand-cursor\Set-HiddenHandCursor.ps1' = 'extras\hidden-hand-cursor\Set-HiddenHandCursor.ps1'
    'extras\hidden-hand-cursor\Dot Cursor.cmd' = 'extras\hidden-hand-cursor\Dot Cursor.cmd'
    'extras\hidden-hand-cursor\Restore Hand Cursor.cmd' = 'extras\hidden-hand-cursor\Restore Hand Cursor.cmd'
    'src\GdTpc.Injector\GdTpc.Injector.csproj' = 'src\GdTpc.Injector\GdTpc.Injector.csproj'
    'src\GdTpc.Injector\Program.cs' = 'src\GdTpc.Injector\Program.cs'
    'src\GdTpc.ValidateBuild\GdTpc.ValidateBuild.csproj' = 'src\GdTpc.ValidateBuild\GdTpc.ValidateBuild.csproj'
    'src\GdTpc.ValidateBuild\Program.cs' = 'src\GdTpc.ValidateBuild\Program.cs'
    'third_party\detours\LICENSE.md' = 'third_party\detours\LICENSE.md'
    'third_party\detours\README.md' = 'third_party\detours\README.md'
    'third_party\detours\CREDITS.TXT' = 'third_party\detours\CREDITS.TXT'
    'third_party\detours\Makefile' = 'third_party\detours\Makefile'
    'third_party\detours\system.mak' = 'third_party\detours\system.mak'
}
# Whole directories copied by extension (top level only).
$directories = @(
    @{ Source = 'src\native'; Destination = 'src\native'; Include = @('*.cpp', '*.h') }
    @{ Source = 'third_party\detours\src'; Destination = 'third_party\detours\src'; Include = @('*') }
    @{ Source = 'third_party\detours\include'; Destination = 'third_party\detours\include'; Include = @('*') }
)
# Native files that are research-only or retired and must not be published.
$nativeExcluded = @()

$gitignore = @'
artifacts/
**/bin/
**/obj/
.vs/
*.user
*.suo
third_party/detours/bin.X64/
third_party/detours/lib.X64/
'@

$thirdPartyReadme = @'
# Third-party source

## Microsoft Detours

- Upstream: https://github.com/microsoft/Detours
- Version: `v4.0.1`
- Pinned commit: `e4bfd6b03e50de46b47abfbd1e46b384f0c5f833`
- License: MIT (`detours/LICENSE.md`)
- Contents: the upstream `src` and `include` folders plus the build makefiles; samples are omitted.
- Build: `scripts/Build-Detours.ps1`. Linked into the runtime DLL for its single camera-update hook.
'@

New-Item -ItemType Directory -Force -Path $destinationFull | Out-Null
# Remove previously exported content (everything except .git) so files dropped from the allowlist disappear too.
# Files first, then empty folders best-effort: an editor watching the folder can hold a directory handle open.
Get-ChildItem -LiteralPath $destinationFull -File -Recurse -Force | Where-Object { $_.FullName -notmatch '\\\.git(\\|$)' } | Remove-Item -Force
Get-ChildItem -LiteralPath $destinationFull -Directory -Recurse -Force | Where-Object { $_.FullName -notmatch '\\\.git(\\|$)' } |
    Sort-Object { $_.FullName.Length } -Descending | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }

$utf8 = [System.Text.UTF8Encoding]::new($false)
foreach ($entry in $files.GetEnumerator()) {
    $source = Join-Path $projectRoot $entry.Key
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Allowlisted file is missing: $($entry.Key)" }
    $target = Join-Path $destinationFull $entry.Value
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath $source -Destination $target
}
foreach ($directory in $directories) {
    $source = Join-Path $projectRoot $directory.Source
    $target = Join-Path $destinationFull $directory.Destination
    New-Item -ItemType Directory -Force -Path $target | Out-Null
    foreach ($file in Get-ChildItem -Path (Join-Path $source '*') -File -Include $directory.Include) {
        if ($nativeExcluded -contains $file.Name) { continue }
        Copy-Item -LiteralPath $file.FullName -Destination $target
    }
}
[System.IO.File]::WriteAllText((Join-Path $destinationFull '.gitignore'), $gitignore, $utf8)
[System.IO.File]::WriteAllText((Join-Path $destinationFull 'third_party\README.md'), $thirdPartyReadme, $utf8)

# Leak scan: personal paths, account names, private document names and binary/research files must not be exported.
# Assembled from pieces so this script does not match its own scan.
$patterns = @(('C:' + '\Users\'), ('war' + 're'), ('@' + 'gmail'), ('CLAUDE' + '_HANDOFF'), ('SESSION' + '_HANDOFF'), ('IMPLEMENTATION_PLAN' + '_HISTORY'), ('artifacts' + '\disassembly'))
$problems = New-Object System.Collections.Generic.List[string]
foreach ($file in Get-ChildItem -LiteralPath $destinationFull -File -Recurse -Force | Where-Object { $_.FullName -notmatch '\\\.git\\' }) {
    if ($file.Extension -in '.exe', '.dll', '.lib', '.obj', '.pdb', '.csv', '.dmp', '.arc', '.arz') { $problems.Add("binary/data file: $($file.FullName)"); continue }
    $text = [System.IO.File]::ReadAllText($file.FullName)
    foreach ($pattern in $patterns) {
        if ($text.IndexOf($pattern, [StringComparison]::OrdinalIgnoreCase) -ge 0) { $problems.Add("'$pattern' in $($file.FullName)") }
    }
}
if ($problems.Count -gt 0) { throw "Export blocked; private content found:`n  " + ($problems -join "`n  ") }

$count = @(Get-ChildItem -LiteralPath $destinationFull -File -Recurse -Force | Where-Object { $_.FullName -notmatch '\\\.git\\' }).Count
Write-Output "PASS: exported $count public files to $destinationFull; leak scan clean."
