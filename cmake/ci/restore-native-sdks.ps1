param(
  [Parameter(Mandatory=$true)][ValidateSet('windows-x64', 'linux-x64')][string]$Rid,
  [switch]$Local
)
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($env:GITHUB_TOKEN)) { throw 'GITHUB_TOKEN is required' }
if (-not $Local -and (-not $env:RUNNER_TEMP -or -not $env:GITHUB_ENV)) {
  throw 'RUNNER_TEMP and GITHUB_ENV are required in CI'
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$restoreRoot = Join-Path $repositoryRoot 'build/native-sdk'
$packages = if ($env:QIGAO_NUGET_PACKAGES) { $env:QIGAO_NUGET_PACKAGES } else { Join-Path $repositoryRoot 'stage/nuget' }
$packages = [IO.Path]::GetFullPath($packages)
$project = Join-Path $restoreRoot 'turboscript-native-sdk-restore.csproj'
New-Item -ItemType Directory -Path $restoreRoot -Force | Out-Null
@'
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
    <RestorePackagesWithLockFile>false</RestorePackagesWithLockFile>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Salts.Native" Version="[2.3.0-rc.4]" />
    <PackageReference Include="SaltsUtils.Native" Version="[4.3.0-rc.2]" />
    <PackageReference Include="CHttp.Native" Version="[2.1.0-rc.1]" />
    <PackageReference Include="Qigao.Re2c.Binary" Version="*" />
  </ItemGroup>
</Project>
'@ | Set-Content -LiteralPath $project -Encoding utf8NoBOM
dotnet restore $project --packages $packages --configfile "$repositoryRoot/cmake/vcpkg-cache.nuget.config" --no-cache --force-evaluate
if ($LASTEXITCODE -ne 0) { throw 'Failed to restore the selected native SDK baseline' }

$assets = Get-Content -LiteralPath (Join-Path $restoreRoot 'obj/project.assets.json') -Raw | ConvertFrom-Json -AsHashtable
function Get-PackageRoot([string]$name, [string]$directory) {
  $keys = @($assets.libraries.Keys | Where-Object { $_.StartsWith("$name/", [StringComparison]::OrdinalIgnoreCase) })
  if ($keys.Count -ne 1) { throw "Expected one resolved $name package" }
  Write-Host "Restored $($keys[0]) for $Rid"
  return Join-Path (Join-Path $packages $assets.libraries[$keys[0]].path) "$directory/$Rid"
}

$roots = [ordered]@{
  SALTS_ROOT = Get-PackageRoot 'Salts.Native' 'sdk'
  SALTS_UTILS_ROOT = Get-PackageRoot 'SaltsUtils.Native' 'sdk'
  CHTTP_ROOT = Get-PackageRoot 'CHttp.Native' 'sdk'
  RE2C_ROOT = Get-PackageRoot 'Qigao.Re2c.Binary' 'tools'
}
$re2cName = if ($Rid -eq 'windows-x64') { 're2c.exe' } else { 're2c' }
$requiredFiles = @{
  SALTS_ROOT = 'lib/cmake/Salts/SaltsConfig.cmake'
  SALTS_UTILS_ROOT = 'lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake'
  CHTTP_ROOT = 'lib/cmake/Chttp/ChttpConfig.cmake'
  RE2C_ROOT = "bin/$re2cName"
}
foreach ($name in $roots.Keys) {
  $file = Join-Path $roots[$name] $requiredFiles[$name]
  if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing restored SDK file: $file" }
}
if ($Rid -eq 'linux-x64') {
  & chmod +x (Join-Path $roots.RE2C_ROOT "bin/$re2cName")
  if ($LASTEXITCODE -ne 0) { throw 'Failed to make restored re2c executable' }
}

# Publish roots only after the complete dependency set has been restored.
foreach ($name in $roots.Keys) {
  [Environment]::SetEnvironmentVariable($name, $roots[$name], 'Process')
  if (-not $Local) { "$name=$($roots[$name])" >> $env:GITHUB_ENV }
}
