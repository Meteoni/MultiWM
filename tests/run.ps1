param(
  [string]$CompilerRoot,
  [ValidateSet('all', 'layout', 'reconciliation', 'companion', 'restore')][string]$Suite = 'all'
)
$ErrorActionPreference = 'Stop'

if (-not $CompilerRoot) {
  $compiler = Get-Command clang++.exe -ErrorAction SilentlyContinue
  if (-not $compiler) { throw 'Pass -CompilerRoot with the Windhawk Compiler directory.' }
  $CompilerRoot = Split-Path -Parent (Split-Path -Parent $compiler.Source)
}
$compilerPath = Join-Path $CompilerRoot 'bin/clang++.exe'
$apiHeader = Join-Path $CompilerRoot 'include/windhawk_api.h'
if (-not (Test-Path -LiteralPath $compilerPath) -or -not (Test-Path -LiteralPath $apiHeader)) {
  throw 'CompilerRoot must contain bin/clang++.exe and include/windhawk_api.h.'
}

$repository = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $repository 'output/tests'
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
$isolatedRunner = Join-Path $buildDirectory 'isolated_runner.exe'
& $compilerPath '-std=c++23' '-target' 'x86_64-w64-mingw32' '-D_WIN32_WINNT=0x0A00' `
  '-municode' '-static' (Join-Path $PSScriptRoot 'isolated_runner.cpp') '-o' $isolatedRunner
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed: isolated test runner' }
$suites = if ($Suite -eq 'all') { @('layout', 'reconciliation', 'companion', 'restore') } else { @($Suite) }
# Every suite runs even when an earlier one fails, so one failure doesn't hide
# the others' results; all failures are reported together at the end.
$failures = @()
foreach ($name in $suites) {
  $source = Join-Path $PSScriptRoot ($name + '_tests.cpp')
  $executable = Join-Path $buildDirectory ($name + '_tests.exe')
  $compilerArguments = @(
    '-std=c++23', '-target', 'x86_64-w64-mingw32', '-DUNICODE', '-D_UNICODE',
    '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00', '-D_WIN32_IE=0x0A00',
    '-DNTDDI_VERSION=0x0A000008', '-D__USE_MINGW_ANSI_STDIO=0', '-DWH_MOD', '-DWH_EDITING',
    '-include', $apiHeader, $source, '-lole32', '-loleaut32', '-luuid', '-ldwmapi',
    '-lversion', '-lgdi32', '-ladvapi32', '-static', '-o', $executable
  )
  & $compilerPath @compilerArguments
  if ($LASTEXITCODE -ne 0) {
    $failures += "compilation failed: $name"
    continue
  }
  & $isolatedRunner $executable
  if ($LASTEXITCODE -ne 0) { $failures += "regression failed: $name (exit code $LASTEXITCODE)" }
}
if ($failures) { throw ($failures -join '; ') }
