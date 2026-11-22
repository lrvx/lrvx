# Loads the x64 MSVC developer environment (vcvars64.bat) into the job, for
# every step after this one.
#
# What the ilammy/msvc-dev-cmd action did, in the open: find Visual Studio
# with vswhere, run vcvars64.bat, and export every variable it set or changed
# through GITHUB_ENV. Done here because that action still targets the Node 20
# runtime, which the runner flags as deprecated on every run.
#
# VCPKG_ROOT is left out on purpose. vcvars points it at the copy of vcpkg
# that ships inside Visual Studio; the jobs use their own pinned checkout, and
# with both in play vcpkg prints a "mismatched VCPKG_ROOT" warning on every
# command.

$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
  Write-Host "::error::vswhere.exe not found at $vswhere"
  exit 1
}

$vs = & $vswhere -latest -products * `
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) {
  Write-Host '::error::no Visual Studio installation with the x64 C++ tools'
  exit 1
}

$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
  Write-Host "::error::vcvars64.bat not found at $vcvars"
  exit 1
}

# Through a .cmd file, not `cmd /c "<quoted path> && set"`: the path has
# spaces, and quoting it through PowerShell into cmd is its own bug farm.
$bat = Join-Path $env:RUNNER_TEMP 'lrvx-vcvars.cmd'
Set-Content -Path $bat -Encoding ascii -Value @(
  '@echo off',
  "call `"$vcvars`" >nul",
  'if errorlevel 1 exit /b 1',
  'set'
)

$before = @{}
Get-ChildItem env: | ForEach-Object { $before[$_.Name] = $_.Value }

$lines = & cmd.exe /d /c $bat
if ($LASTEXITCODE -ne 0) {
  Write-Host "::error::vcvars64.bat failed (exit $LASTEXITCODE)"
  exit 1
}

$exported = 0
foreach ($line in $lines) {
  $eq = $line.IndexOf('=')
  if ($eq -lt 1) { continue }
  $name = $line.Substring(0, $eq)
  $value = $line.Substring($eq + 1)
  if ($name -eq 'VCPKG_ROOT') { continue }
  if ($before.ContainsKey($name) -and $before[$name] -ceq $value) { continue }
  "$name=$value" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
  $exported++
}

if ($exported -eq 0) {
  Write-Host '::error::vcvars64.bat changed nothing; the developer environment was not loaded'
  exit 1
}
Write-Host "MSVC environment from $vs ($exported variables)"
