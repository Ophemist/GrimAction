[CmdletBinding()]
param(
    [string]$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Grim Dawn',
    [Parameter(Mandatory = $true)][string]$ReleasePath
)

$ErrorActionPreference = 'Stop'
$release = Get-Item -LiteralPath $ReleasePath
$injector = Join-Path $release.FullName 'injector\GdTpc.Injector.exe'
$runtime = Join-Path $release.FullName 'gdtpc_runtime_collision.dll'
& (Join-Path $PSScriptRoot 'Test-ReleaseManifest.ps1') -ReleasePath $release.FullName -ExpectedGate 2 -ExpectedVariant camera-collision -ExpectedGameStateWritesEnabled 1
& $injector --collision-stop $GameRoot $runtime
if ($LASTEXITCODE -ne 0) { throw "Camera-collision logical stop reported exit code $LASTEXITCODE. Do not retry; exit Grim Dawn normally." }
Write-Output 'PASS: camera-collision logical stop confirmed verified restoration and writer completion. Exit Grim Dawn normally.'
