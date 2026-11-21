param(
  [string]$CompilerRoot,
  [string]$ClangTidy,
  [ValidateSet('all', 'multiwm', 'companion')][string]$Mod = 'all'
)
# Advisory function-size and complexity review (.clang-tidy). Prints the
# functions over the thresholds, most complex first. Never fails the build.
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

# The Windhawk compiler ships no clang-tidy. Use one from PATH, or the one the
# VS Code C/C++ extension bundles.
if (-not $ClangTidy) {
  $onPath = Get-Command clang-tidy.exe -ErrorAction SilentlyContinue
  if ($onPath) {
    $ClangTidy = $onPath.Source
  } else {
    $ClangTidy = Get-ChildItem "$env:USERPROFILE\.vscode\extensions" -Directory -Filter 'ms-vscode.cpptools-*' -ErrorAction SilentlyContinue |
      Sort-Object Name -Descending |
      ForEach-Object { Join-Path $_.FullName 'LLVM\bin\clang-tidy.exe' } |
      Where-Object { Test-Path -LiteralPath $_ } |
      Select-Object -First 1
  }
}
if (-not $ClangTidy) { throw 'No clang-tidy found. Pass -ClangTidy, or install the VS Code C/C++ extension.' }

# Parse with the Windhawk compiler's own headers and target, as the real build
# does. A clang-tidy newer than the compiler reports errors inside the
# compiler's intrinsic headers; they don't affect MultiWM's code, so only
# warnings in src/ are shown.
$probe = ('' | & $compilerPath -std=c++23 -target x86_64-w64-mingw32 -E -x c++ - -v 2>&1) | ForEach-Object { "$_" }
$systemIncludes = @()
$inList = $false
foreach ($line in $probe) {
  if ($line -match 'search starts here') { $inList = $true; continue }
  if ($line -match 'End of search list') { break }
  if ($inList -and $line -notmatch '#include') { $systemIncludes += $line.Trim() }
}
$arguments = @('--target=x86_64-w64-mingw32', '-std=c++23', '-nostdinc', '-nostdinc++') +
  ($systemIncludes | ForEach-Object { "-isystem$_" }) +
  @('-DUNICODE', '-D_UNICODE', '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00', '-D_WIN32_IE=0x0A00',
    '-DNTDDI_VERSION=0x0A000008', '-D__USE_MINGW_ANSI_STDIO=0', '-DWH_MOD', '-DWH_EDITING',
    '-include', $apiHeader)

$repository = Split-Path -Parent $PSScriptRoot
$sources = @{
  multiwm = 'src/multiwm/multiwm.cpp'
  companion = 'src/companion/multiwm-companion.cpp'
}
$mods = if ($Mod -eq 'all') { @('multiwm', 'companion') } else { @($Mod) }
$findings = @{}
foreach ($name in $mods) {
  $output = & $ClangTidy (Join-Path $repository $sources[$name]) --quiet -- @arguments 2>&1 | ForEach-Object { "$_" }
  for ($i = 0; $i -lt $output.Count; $i++) {
    if ($output[$i] -notmatch '^(.*[\\/]src[\\/][^:]*):(\d+):\d+: warning: function ''(\w+)'' (.*) \[readability-') { continue }
    $location = ($Matches[1] -replace '.*[\\/]src[\\/]', 'src/' -replace '\\', '/') + ':' + $Matches[2]
    $function = $Matches[3]
    $complexity = if ($Matches[4] -match 'complexity of (\d+)') { [int]$Matches[1] } else { 0 }
    $notes = @()
    for ($j = $i + 1; $j -lt [Math]::Min($i + 6, $output.Count); $j++) {
      if ($output[$j] -match 'warning:') { break }
      if ($output[$j] -match 'note: (\d+) (lines|statements|nesting level)') { $notes += "$($Matches[1]) $($Matches[2])" }
    }
    $key = "$location $function"
    if (-not $findings.ContainsKey($key)) {
      $findings[$key] = [pscustomobject]@{ Location = $location; Function = $function; Complexity = 0; Size = '' }
    }
    $entry = $findings[$key]
    $entry.Complexity = [Math]::Max($entry.Complexity, $complexity)
    if ($notes) { $entry.Size = (@($entry.Size, ($notes -join ', ')) | Where-Object { $_ }) -join '; ' }
  }
}

if (-not $findings.Count) {
  Write-Output 'No functions over the .clang-tidy thresholds.'
  exit 0
}
Write-Output "$($findings.Count) functions over the .clang-tidy thresholds (complexity 25, 120 lines, nesting 4):"
$findings.Values | Sort-Object Complexity -Descending | Format-Table -AutoSize Location, Function, Complexity, Size |
  Out-String -Width 200 | Write-Output
exit 0
