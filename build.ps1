param(
  [ValidateSet('windows-release', 'windows-fast')]
  [string]$Preset = 'windows-release',
  [switch]$Test,
  [switch]$Install
)

$ErrorActionPreference = 'Stop'
$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
if (-not $cmake) {
  $roots = @(
    'C:\Program Files\CMake\bin\cmake.exe',
    'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
    'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
  )
  $cmakePath = $roots | Where-Object { Test-Path $_ } | Select-Object -First 1
  if (-not $cmakePath) {
    throw 'CMake 3.24+ is required. Install Kitware.CMake or the Visual Studio CMake component.'
  }
} else {
  $cmakePath = $cmake.Source
}

& $cmakePath --preset $Preset
& $cmakePath --build --preset $Preset
if ($Test) { & $cmakePath --build --preset $Preset --target rc_core_tests; & $cmakePath --build --preset $Preset --target RUN_TESTS }
if ($Install) { & $cmakePath --install "out/build/$Preset" --config Release }
