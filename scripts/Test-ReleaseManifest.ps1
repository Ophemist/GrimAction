[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ReleasePath,
    [ValidateSet(0, 1, 2)][int]$ExpectedGate = 0,
    [ValidateSet('standard', 'camera-collision')][string]$ExpectedVariant = 'standard',
    [ValidateSet(0, 1)][int]$ExpectedGameStateWritesEnabled = 0
)

# Verifies that an immutable Gate 0 release still matches its own SHA-256 manifest. Existence
# checks alone do not establish immutability, so nothing may run a release that fails here.

$ErrorActionPreference = 'Stop'

# [System.IO.Path]::GetRelativePath does not exist on Windows PowerShell 5.1, so the release
# layout must not depend on the host PowerShell edition.
function Get-ReleaseRelativePath {
    param([string]$Root, [string]$FullName)
    $rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd([char]92)
    $fileFull = [System.IO.Path]::GetFullPath($FullName)
    if (-not $fileFull.StartsWith($rootFull + [char]92, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside the release root: $FullName"
    }
    return $fileFull.Substring($rootFull.Length + 1)
}

$release = [System.IO.Path]::GetFullPath($ReleasePath)
if (-not (Test-Path -LiteralPath $release -PathType Container)) { throw "Release directory does not exist: $release" }
$manifestPath = Join-Path $release 'release-manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "Release is incomplete; no manifest: $manifestPath" }

$manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
if ($manifest.schemaVersion -ne 1) { throw "Unsupported release manifest schema: $($manifest.schemaVersion)" }
if ($manifest.gate -ne $ExpectedGate) { throw "Release gate mismatch: expected $ExpectedGate, manifest declares $($manifest.gate)." }
$actualVariant = if ($null -eq $manifest.variant) { 'standard' } else { [string]$manifest.variant }
if ($actualVariant -ne $ExpectedVariant) { throw "Release variant mismatch: expected $ExpectedVariant, manifest declares $actualVariant." }
if ($manifest.gameStateWritesEnabled -ne $ExpectedGameStateWritesEnabled) {
    throw "Release write-capability mismatch: expected $ExpectedGameStateWritesEnabled, manifest declares $($manifest.gameStateWritesEnabled)."
}
if (-not $manifest.files -or @($manifest.files).Count -eq 0) { throw 'Release manifest lists no files.' }

$expected = @{}
foreach ($entry in $manifest.files) {
    if (-not $entry.path -or -not $entry.sha256) { throw 'Release manifest contains an incomplete file entry.' }
    $full = [System.IO.Path]::GetFullPath((Join-Path $release $entry.path))
    if (-not $full.StartsWith($release, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Release manifest names a path outside the release: $($entry.path)"
    }
    if ($expected.ContainsKey($full)) { throw "Release manifest lists a duplicate path: $($entry.path)" }
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { throw "Release file is missing: $($entry.path)" }
    $actual = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne ([string]$entry.sha256).ToLowerInvariant()) {
        throw "Release file hash mismatch: $($entry.path) expected $($entry.sha256) actual $actual"
    }
    $expected[$full] = $true
}

# The manifest cannot cover itself; every other file present must be described by it.
$unexpected = @(Get-ChildItem -File -Recurse -LiteralPath $release |
    Where-Object { $_.FullName -ne $manifestPath -and -not $expected.ContainsKey($_.FullName) } |
    ForEach-Object { Get-ReleaseRelativePath -Root $release -FullName $_.FullName })
if ($unexpected.Count -gt 0) {
    throw "Release contains files the manifest does not describe: $($unexpected -join ', ')"
}

Write-Output "PASS: Gate $ExpectedGate/$ExpectedVariant release manifest verified; writes=$ExpectedGameStateWritesEnabled; $($expected.Count) files match their recorded SHA-256 at $release"
