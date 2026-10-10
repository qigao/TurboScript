param(
  [Parameter(Mandatory=$true)][ValidateSet('windows-x64', 'linux-x64', 'macos-arm64', 'android-arm64-v8a')][string]$Rid,
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
    <PackageReference Include="Salts.Native" Version="*-*" />
    <PackageReference Include="SaltsUtils.Native" Version="*-*" />
    <PackageReference Include="CHttp.Native" Version="*-*" />
    <PackageReference Include="TurboDB.Native" Version="*" />
    <PackageReference Include="Qigao.Re2c.Binary" Version="*" />
  </ItemGroup>
</Project>
'@ | Set-Content -LiteralPath $project -Encoding utf8NoBOM
dotnet restore $project --packages $packages --configfile "$repositoryRoot/cmake/vcpkg-cache.nuget.config" --no-cache --force-evaluate
if ($LASTEXITCODE -ne 0) { throw 'Failed to restore the latest native SDKs' }

$hostOs = if ($IsWindows) { 'windows' } elseif ($IsMacOS) { 'macos' } elseif ($IsLinux) { 'linux' } else { throw 'Unsupported build host' }
$hostArch = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
$hostRid = "$hostOs-$hostArch"
$versions = @{}

$assets = Get-Content -LiteralPath (Join-Path $restoreRoot 'obj/project.assets.json') -Raw | ConvertFrom-Json -AsHashtable
function Get-PackageRoot([string]$name, [string]$directory, [string]$packageRid = $Rid) {
  $keys = @($assets.libraries.Keys | Where-Object { $_.StartsWith("$name/", [StringComparison]::OrdinalIgnoreCase) })
  if ($keys.Count -ne 1) { throw "Expected one resolved $name package" }
  $versions[$name] = $keys[0].Split('/', 2)[1]
  Write-Host "Restored $($keys[0]) for $packageRid"
  return Join-Path (Join-Path $packages $assets.libraries[$keys[0]].path) "$directory/$packageRid"
}

$roots = [ordered]@{
  SALTS_ROOT = Get-PackageRoot 'Salts.Native' 'sdk'
  SALTS_UTILS_ROOT = Get-PackageRoot 'SaltsUtils.Native' 'sdk'
  CHTTP_ROOT = Get-PackageRoot 'CHttp.Native' 'sdk'
  RE2C_ROOT = Get-PackageRoot 'Qigao.Re2c.Binary' 'tools' $hostRid
}
# TurboDB currently publishes Linux, Windows and Android SDKs only.
if ($Rid -ne 'macos-arm64') {
  $roots.TURBODB_ROOT = Get-PackageRoot 'TurboDB.Native' 'sdk'
}
$re2cName = if ($IsWindows) { 're2c.exe' } else { 're2c' }
$requiredFiles = @{
  SALTS_ROOT = 'lib/cmake/Salts/SaltsConfig.cmake'
  SALTS_UTILS_ROOT = 'lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake'
  CHTTP_ROOT = 'lib/cmake/Chttp/ChttpConfig.cmake'
  TURBODB_ROOT = 'lib/cmake/TurboDB/TurboDBConfig.cmake'
  RE2C_ROOT = "bin/$re2cName"
}
foreach ($name in $roots.Keys) {
  $file = Join-Path $roots[$name] $requiredFiles[$name]
  if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing restored SDK file: $file" }
}
if (-not $IsWindows) {
  & chmod +x (Join-Path $roots.RE2C_ROOT "bin/$re2cName")
  if ($LASTEXITCODE -ne 0) { throw 'Failed to make restored re2c executable' }
}

# Publish roots only after the complete dependency set has been restored.
foreach ($name in $roots.Keys) {
  [Environment]::SetEnvironmentVariable($name, $roots[$name], 'Process')
  if (-not $Local) { "$name=$($roots[$name])" >> $env:GITHUB_ENV }
}

# Clear an inherited database root on a platform without a database SDK.
if ($Rid -eq 'macos-arm64') {
  [Environment]::SetEnvironmentVariable('TURBODB_ROOT', '', 'Process')
  if (-not $Local) { 'TURBODB_ROOT=' >> $env:GITHUB_ENV }
}
$releasePackages = @{
  SALTS_SDK_RELEASE = 'Salts.Native'
  SALTS_UTILS_SDK_RELEASE = 'SaltsUtils.Native'
  CHTTP_SDK_RELEASE = 'CHttp.Native'
  TURBODB_SDK_RELEASE = 'TurboDB.Native'
}
foreach ($name in $releasePackages.Keys) {
  $version = $versions[$releasePackages[$name]]
  [Environment]::SetEnvironmentVariable($name, $version, 'Process')
  if (-not $Local) { "$name=$version" >> $env:GITHUB_ENV }
}
