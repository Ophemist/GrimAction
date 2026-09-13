[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ReleasePath
)

# Refuses to let an injecting script use a runtime image that failed a live gate.
#
# "Never inject that release again" was previously only prose in the handoff. This enforces it, and
# enforces it by SHA-256 rather than by directory name, so a renamed folder, a copy, or a rebuilt
# reproducibility twin is refused too.
#
# Injection only. Logical stop must never consult this list: whatever is resident in a live process
# still has to be stoppable, and a prohibited image is exactly the case where that matters most.

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$listPath = Join-Path $projectRoot 'config\prohibited-releases.json'
if (-not (Test-Path -LiteralPath $listPath -PathType Leaf)) {
    throw "The prohibited-release list is missing: $listPath. Refusing to inject without it."
}

$list = Get-Content -Raw -LiteralPath $listPath | ConvertFrom-Json
if ($list.schemaVersion -ne 1) { throw "Unsupported prohibited-release schema: $($list.schemaVersion)" }

$release = [System.IO.Path]::GetFullPath($ReleasePath)
if (-not (Test-Path -LiteralPath $release -PathType Container)) { throw "Release directory does not exist: $release" }

# Hash every file actually present, so the check does not depend on the release's own manifest
# being honest about what it contains.
$present = @{}
foreach ($file in Get-ChildItem -File -Recurse -LiteralPath $release) {
    $present[(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()] = $file.Name
}

foreach ($entry in $list.prohibited) {
    $banned = ([string]$entry.runtimeSha256).ToLowerInvariant()
    if ($present.ContainsKey($banned)) {
        throw @"
REFUSED: this release contains a prohibited runtime image and must never be injected.
  file        : $($present[$banned])
  sha256      : $banned
  recorded    : $($entry.recordedUtc)
  reason      : $($entry.reason)
  superseded by: $($entry.supersededBy)
Use the superseding release. If you believe this entry is wrong, change config\prohibited-releases.json deliberately; do not bypass this script.
"@
    }
}

Write-Output "PASS: no prohibited runtime image found in $release"
