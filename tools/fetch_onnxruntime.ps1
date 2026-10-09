<#
.SYNOPSIS
  Downloads the official ONNX Runtime (CPU, x64) release from Microsoft's
  GitHub, verifies its SHA-256, and copies the runtime DLLs into Echo's
  'ai' folder. Run this only if you want AI voice conversion.

.DESCRIPTION
  Source:  https://github.com/microsoft/onnxruntime/releases (MIT license)
  Size:    about 83 MB download, about 18 MB installed
  Privacy: official Windows builds of ONNX Runtime can emit ETW trace events
           (collected only if a Windows diagnostic trace session is active).
           Echo calls DisableTelemetryEvents() right after loading it.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\fetch_onnxruntime.ps1
  powershell -ExecutionPolicy Bypass -File tools\fetch_onnxruntime.ps1 -Destination "C:\Path\To\Echo\ai"
#>
param(
    [string]$Destination = (Join-Path (Split-Path $PSScriptRoot -Parent) 'ai'),
    [string]$Version = '1.30.0',
    [string]$Sha256 = 'c6ba983baf5681af108599675d2a89c2d145512d02de28aed0bff177cd0ba949'
)
$ErrorActionPreference = 'Stop'
$url = "https://github.com/microsoft/onnxruntime/releases/download/v$Version/onnxruntime-win-x64-$Version.zip"
Write-Host "This will download ONNX Runtime $Version (~83 MB, MIT license) from:"
Write-Host "  $url"
$answer = Read-Host 'Continue? (y/N)'
if ($answer -notmatch '^[yY]') { Write-Host 'Cancelled.'; exit 1 }

$tmp = Join-Path ([IO.Path]::GetTempPath()) "echo-ort-$([guid]::NewGuid())"
New-Item -ItemType Directory -Force $tmp | Out-Null
try {
    $zip = Join-Path $tmp 'ort.zip'
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    if ($hash -ne $Sha256.ToLower()) { throw "SHA-256 mismatch: expected $Sha256, got $hash" }
    Expand-Archive $zip -DestinationPath $tmp
    $lib = Join-Path $tmp "onnxruntime-win-x64-$Version\lib"
    New-Item -ItemType Directory -Force $Destination | Out-Null
    Copy-Item (Join-Path $lib 'onnxruntime.dll') $Destination -Force
    Copy-Item (Join-Path $lib 'onnxruntime_providers_shared.dll') $Destination -Force
    Copy-Item (Join-Path $tmp "onnxruntime-win-x64-$Version\LICENSE") (Join-Path $Destination 'ONNXRUNTIME-LICENSE.txt') -Force
    Copy-Item (Join-Path $tmp "onnxruntime-win-x64-$Version\ThirdPartyNotices.txt") (Join-Path $Destination 'ONNXRUNTIME-ThirdPartyNotices.txt') -Force
    Write-Host "Installed ONNX Runtime $Version to $Destination (hash verified). Restart Echo."
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
