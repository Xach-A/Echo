<#
.SYNOPSIS
  Stages a release build, creates the portable ZIP and (if Inno Setup is
  installed) the installer, plus SHA-256 checksums.

.EXAMPLE
  ./installer/package.ps1 -BuildDir build/release
#>
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build\release')
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$BuildDir = Resolve-Path $BuildDir

# Version and name from the generated header.
$hdr = Get-Content (Join-Path $BuildDir 'generated\Version.h') -Raw
$version = [regex]::Match($hdr, 'ECHO_VERSION_STRING "([^"]+)"').Groups[1].Value
$name = [regex]::Match($hdr, 'ECHO_APP_NAME "([^"]+)"').Groups[1].Value
if (-not $version) { throw 'Could not read version from Version.h' }

$pkg = Join-Path $root 'build\package'
$stage = Join-Path $pkg 'stage'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item (Join-Path $BuildDir "bin\$name.exe") $stage
Copy-Item (Join-Path $BuildDir 'bin\echo_hwtest.exe') $stage
foreach ($f in 'LICENSE', 'README.md', 'THIRD_PARTY_NOTICES.md') { Copy-Item (Join-Path $root $f) $stage }
Copy-Item -Recurse (Join-Path $root 'docs') (Join-Path $stage 'docs')
Copy-Item -Recurse (Join-Path $root 'presets') (Join-Path $stage 'presets')
New-Item -ItemType Directory -Force (Join-Path $stage 'tools') | Out-Null
Copy-Item (Join-Path $root 'tools\fetch_onnxruntime.ps1') (Join-Path $stage 'tools')
New-Item -ItemType Directory -Force (Join-Path $stage 'ai') | Out-Null
Set-Content -Path (Join-Path $stage 'ai\README.txt') -Encoding utf8 -Value @'
Optional: place onnxruntime.dll (and onnxruntime_providers_shared.dll) from the
official Microsoft ONNX Runtime release here to enable AI voice conversion.
Run ..\tools\fetch_onnxruntime.ps1 to download and verify it. See ..\docs\AI_VOICE.md.
'@

# Portable ZIP (settings still go to %APPDATA%\Echo).
$zip = Join-Path $pkg "$name-$version-portable-win64.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Host "Portable ZIP: $zip"

# Installer
$iscc = @(
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
    'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
    'C:\Program Files\Inno Setup 6\ISCC.exe'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($iscc) {
    & $iscc /Q "/DAppName=$name" "/DAppVersion=$version" "/DStageDir=$stage" "/DOutputDir=$pkg" (Join-Path $PSScriptRoot 'Echo.iss')
    if ($LASTEXITCODE) { throw 'Inno Setup failed' }
    Write-Host "Installer: $(Join-Path $pkg "$name-$version-setup-win64.exe")"
} else {
    Write-Warning 'Inno Setup not found; skipped the installer (winget install JRSoftware.InnoSetup --scope user).'
}

# Checksums
$sums = Get-ChildItem $pkg -File | Where-Object { $_.Extension -in '.zip', '.exe' } |
    ForEach-Object { "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name }
Set-Content -Path (Join-Path $pkg 'SHA256SUMS.txt') -Value $sums -Encoding ascii
$sums | ForEach-Object { Write-Host $_ }
