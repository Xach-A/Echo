<#
.SYNOPSIS
  Configure, build, test and package Echo.

.DESCRIPTION
  Uses CMake + Ninja with either LLVM-MinGW (clang) or MSVC, whichever is
  available. LLVM-MinGW is found on PATH or in the winget package folder.

.EXAMPLE
  ./build.ps1                     # Release build
  ./build.ps1 -Config Debug
  ./build.ps1 -Test               # build + run unit tests
  ./build.ps1 -Package            # build + tests + portable zip + installer
#>
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [switch]$Test,
    [switch]$Package,
    [switch]$Clean,
    [string]$BuildDir = ''
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $root "build/$($Config.ToLower())" }

function Find-Tool([string]$name, [string[]]$extraDirs) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($d in $extraDirs) {
        if ($d -and (Test-Path (Join-Path $d $name))) { return (Join-Path $d $name) }
    }
    return $null
}

# --- locate CMake / Ninja (PATH, pip --user Scripts, or Visual Studio) ---
$pyScripts = @()
foreach ($v in 'Python314', 'Python313', 'Python312', 'Python311') {
    $pyScripts += Join-Path $env:APPDATA "Python/$v/Scripts"
}
$cmake = Find-Tool 'cmake.exe' $pyScripts
$ninja = Find-Tool 'ninja.exe' $pyScripts
if (-not $cmake) { throw 'CMake not found. Install with: python -m pip install --user cmake ninja' }
if (-not $ninja) { throw 'Ninja not found. Install with: python -m pip install --user ninja' }
$env:PATH = "$(Split-Path $cmake);$(Split-Path $ninja);$env:PATH"

# --- locate a compiler: prefer an active MSVC environment, else LLVM-MinGW ---
$compilerArgs = @()
if ($env:VCINSTALLDIR -and (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Host 'Using MSVC from the current developer environment'
    $compilerArgs = @('-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl')
} else {
    $clang = Get-Command 'x86_64-w64-mingw32-clang++.exe' -ErrorAction SilentlyContinue
    if (-not $clang) {
        $pkg = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Microsoft/WinGet/Packages') -Directory -Filter 'MartinStorsjo.LLVM-MinGW*' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($pkg) {
            $bin = Get-ChildItem $pkg.FullName -Directory | ForEach-Object { Join-Path $_.FullName 'bin' } | Where-Object { Test-Path $_ } | Select-Object -First 1
            if ($bin) { $env:PATH = "$bin;$env:PATH" }
        }
        $clang = Get-Command 'x86_64-w64-mingw32-clang++.exe' -ErrorAction SilentlyContinue
    }
    if (-not $clang) {
        throw 'No compiler found. Install LLVM-MinGW: winget install MartinStorsjo.LLVM-MinGW.UCRT --scope user (or run from a VS Developer PowerShell).'
    }
    Write-Host "Using LLVM-MinGW: $($clang.Source)"
    $compilerArgs = @('-DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang', '-DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++',
                      '-DCMAKE_RC_COMPILER=llvm-windres')
}

if ($Clean -and (Test-Path $BuildDir)) { Remove-Item -Recurse -Force $BuildDir }

& $cmake -S $root -B $BuildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config" @compilerArgs
if ($LASTEXITCODE) { throw 'CMake configure failed' }
& $cmake --build $BuildDir --parallel
if ($LASTEXITCODE) { throw 'Build failed' }

if ($Test -or $Package) {
    & (Join-Path $BuildDir 'bin/echo_tests.exe')
    if ($LASTEXITCODE) { throw 'Unit tests failed' }
}

if ($Package) {
    & (Join-Path $root 'installer/package.ps1') -BuildDir $BuildDir
    if ($LASTEXITCODE) { throw 'Packaging failed' }
}

Write-Host "Build complete: $BuildDir/bin"
