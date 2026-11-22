# Picks a clang-cl that the installed MSVC standard library accepts and
# exports its path as LRVX_CLANG_CL.
#
# The MSVC STL refuses to compile under a clang older than the one it was
# released with (error STL1000). The runner image updates Visual Studio and
# the standalone LLVM on separate schedules, so "whatever clang-cl is on
# PATH" stops building the day the STL moves ahead: MSVC 14.52 wants Clang 22
# and the image still carried LLVM 20.
#
# The clang-cl bundled with Visual Studio comes first, since it is the one
# that STL is released against. The standalone one is next. If neither
# passes, LLVM is upgraded through choco and tried again. Each candidate is
# judged by compiling a file that includes an STL header, not by its version
# number: the requirement lives in the STL and changes with it.
#
# Needs the MSVC environment loaded (scripts/ci-msvc-env.ps1).

$ErrorActionPreference = 'Continue'

$probeDir = Join-Path $env:RUNNER_TEMP 'lrvx-clang-cl-probe'
New-Item -ItemType Directory -Force -Path $probeDir | Out-Null
$probe = Join-Path $probeDir 'probe.cpp'
Set-Content -Path $probe -Encoding ascii -Value @(
  '#include <array>',
  'int main() { std::array<int, 1> a{}; return a[0]; }'
)

function Test-ClangCl {
  param([string]$Path)
  if (-not (Test-Path $Path)) {
    Write-Host "  not present: $Path"
    return $false
  }
  $version = (& $Path --version | Select-Object -First 1)
  & $Path /nologo /std:c++latest /EHsc /c $probe "/Fo$probeDir\probe.obj" 2>&1 |
    Select-Object -First 5 | ForEach-Object { Write-Host "    $_" }
  $ok = ($LASTEXITCODE -eq 0)
  Write-Host ("  {0}: {1} ({2})" -f $(if ($ok) { 'accepted' } else { 'rejected' }), $Path, $version)
  return $ok
}

$standalone = 'C:\Program Files\LLVM\bin\clang-cl.exe'
$candidates = @()
if ($env:VCINSTALLDIR) {
  $candidates += (Join-Path $env:VCINSTALLDIR 'Tools\Llvm\x64\bin\clang-cl.exe')
}
$candidates += $standalone

Write-Host 'Looking for a clang-cl the MSVC STL accepts:'
$chosen = $null
foreach ($candidate in $candidates) {
  if (Test-ClangCl $candidate) { $chosen = $candidate; break }
}

if (-not $chosen) {
  Write-Host 'No installed clang-cl passes; upgrading LLVM through choco.'
  for ($i = 1; $i -le 3; $i++) {
    choco upgrade llvm -y
    if ($LASTEXITCODE -eq 0) { break }
    Write-Host "::warning::choco upgrade llvm failed (attempt $i/3, exit $LASTEXITCODE)"
    if ($i -lt 3) { Start-Sleep -Seconds (20 * $i) }
  }
  if (Test-ClangCl $standalone) { $chosen = $standalone }
}

if (-not $chosen) {
  Write-Host '::error::no clang-cl on this runner compiles against the installed MSVC STL'
  exit 1
}

# Forward slashes: the path goes into CMake, where a backslash is an escape.
$exported = $chosen -replace '\\', '/'
"LRVX_CLANG_CL=$exported" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
Write-Host "clang-cl: $exported"
exit 0
