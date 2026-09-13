[CmdletBinding()]
param(
    [string]$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Grim Dawn',
    [Parameter(Mandatory = $true)][string]$ReleasePath
)

# Runs only an explicitly named immutable camera-collision release. This never builds or installs
# anything and is not authorization to launch or inject; each live process needs fresh permission.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$release = Get-Item -LiteralPath $ReleasePath
$injector = Join-Path $release.FullName 'injector\GdTpc.Injector.exe'
$runtime = Join-Path $release.FullName 'gdtpc_runtime_collision.dll'
$config = Join-Path $release.FullName 'runtime.ini'
foreach ($required in @($injector,$runtime,$config)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Camera-collision release is incomplete: $required" }
}
& (Join-Path $PSScriptRoot 'Test-ReleaseManifest.ps1') -ReleasePath $release.FullName -ExpectedGate 2 -ExpectedVariant camera-collision -ExpectedGameStateWritesEnabled 1
& (Join-Path $PSScriptRoot 'Test-ReleaseNotProhibited.ps1') -ReleasePath $release.FullName

$logRoot = Join-Path $projectRoot 'artifacts\observation\collision'
New-Item -ItemType Directory -Path $logRoot -Force | Out-Null
$stamp = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$log = Join-Path $logRoot ("collision-"+$stamp+'.csv')
if (Test-Path -LiteralPath $log) { throw "Refusing to reuse an existing log path: $log" }
$sidecar = $log -replace '\.csv$','-run.json'
$record = [ordered]@{
    startedUtc=$stamp; status='attempted'; release=$release.FullName
    injectorSha256=(Get-FileHash -LiteralPath $injector -Algorithm SHA256).Hash.ToLowerInvariant()
    runtimeSha256=(Get-FileHash -LiteralPath $runtime -Algorithm SHA256).Hash.ToLowerInvariant()
    configSha256=(Get-FileHash -LiteralPath $config -Algorithm SHA256).Hash.ToLowerInvariant()
    log=$log; injectorExitCode=$null
}
$record | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $sidecar -Encoding utf8
& $injector --collision $GameRoot $runtime $config $log
$exit = $LASTEXITCODE
$record.injectorExitCode = $exit
$record.status = if ($exit -eq 0) { 'injected' } else { 'failed-closed' }
$record | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $sidecar -Encoding utf8
if ($exit -ne 0) { throw "Camera-collision injector exited with code $exit. Follow its residency/exit instruction exactly." }
Write-Output "PASS: camera-collision runtime reported ready. Evidence: $log"
