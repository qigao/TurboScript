param(
  [Parameter(Mandatory=$true)]
  [ValidateSet('windows-x64', 'linux-x64', 'macos-arm64', 'android-arm64-v8a')]
  [string]$Rid,
  [switch]$Local
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($env:GITHUB_TOKEN)) {
  throw 'GITHUB_TOKEN is required'
}
if (-not $Local -and (-not $env:RUNNER_TEMP -or -not $env:GITHUB_ENV)) {
  throw 'RUNNER_TEMP and GITHUB_ENV are required in CI'
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$packages = if ($env:QIGAO_NUGET_PACKAGES) {
  $env:QIGAO_NUGET_PACKAGES
} else {
  Join-Path $repositoryRoot 'stage/nuget'
}
$packages = [IO.Path]::GetFullPath($packages)

$pythonCommand = Get-Command python3 -ErrorAction SilentlyContinue
if (-not $pythonCommand) {
  $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
}
if (-not $pythonCommand) {
  throw 'Python 3 is required for curl-based GitHub package restoration'
}
$python = $pythonCommand.Path

$localEnvFile = $null
$previousGitHubEnv = $env:GITHUB_ENV
if ($Local) {
  $localEnvFile = [IO.Path]::GetTempFileName()
  $env:GITHUB_ENV = $localEnvFile
}

try {
  $arguments = @(
    (Join-Path $PSScriptRoot 'github_packages.py'),
    'restore',
    '--rid', $Rid,
    '--packages', $packages
  )
  & $python @arguments
  if ($LASTEXITCODE -ne 0) {
    throw 'Failed to restore native SDKs from GitHub Packages with curl'
  }

  if ($Local) {
    foreach ($line in Get-Content -LiteralPath $localEnvFile) {
      if ([string]::IsNullOrWhiteSpace($line)) { continue }
      $pair = $line -split '=', 2
      if ($pair.Count -ne 2) {
        throw "Invalid environment line from package restore: $line"
      }
      [Environment]::SetEnvironmentVariable($pair[0], $pair[1], 'Process')
    }
    if ($env:RE2C_ROOT) {
      $re2cBin = Join-Path $env:RE2C_ROOT 'bin'
      $env:PATH = "$re2cBin$([IO.Path]::PathSeparator)$env:PATH"
    }
  }
}
finally {
  if ($Local) {
    if ($null -eq $previousGitHubEnv) {
      Remove-Item Env:GITHUB_ENV -ErrorAction SilentlyContinue
    } else {
      $env:GITHUB_ENV = $previousGitHubEnv
    }
    Remove-Item -LiteralPath $localEnvFile -Force -ErrorAction SilentlyContinue
  }
}
