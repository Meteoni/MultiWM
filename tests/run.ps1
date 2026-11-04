param(
  [string]$CompilerRoot,
  [ValidateSet('all', 'layout', 'reconciliation')][string]$Suite = 'all'
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
$suites = if ($Suite -eq 'all') { @('layout', 'reconciliation') } else { @($Suite) }
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
  if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $name" }
  & $executable
  if ($LASTEXITCODE -ne 0) { throw "Regression failed: $name" }
}
