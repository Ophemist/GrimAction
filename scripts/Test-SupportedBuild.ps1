[CmdletBinding()]
param(
    [string]$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Grim Dawn'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$dotnet = Join-Path $env:USERPROFILE '.dotnet\dotnet.exe'

if (-not (Test-Path -LiteralPath $dotnet)) {
    $dotnetCommand = Get-Command dotnet -ErrorAction SilentlyContinue
    if (-not $dotnetCommand) {
        throw 'A .NET SDK is required to run build validation.'
    }
    $dotnet = $dotnetCommand.Source
}

$project = Join-Path $projectRoot 'src\GdTpc.ValidateBuild\GdTpc.ValidateBuild.csproj'
$manifest = Join-Path $projectRoot 'config\supported-builds.json'
& $dotnet run --project $project --configuration Release -- --game-root $GameRoot --manifest $manifest
if ($LASTEXITCODE -ne 0) {
    throw "Build validation exited with code $LASTEXITCODE."
}
