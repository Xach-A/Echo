<#
.SYNOPSIS
  Downloads the official ONNX Runtime from Microsoft, verifies it, and copies
  the runtime DLLs into Echo's 'ai' folder. Run this only if you want AI voice
  conversion.

.DESCRIPTION
  Default: the CPU build from Microsoft's GitHub releases (MIT license, about
  83 MB download, 18 MB installed).

  -Gpu: the DirectML build from nuget.org (Microsoft.ML.OnnxRuntime.DirectML,
  MIT license, 12 MB download, 17 MB installed). It runs AI voices on the
  graphics card (any DirectX 12 card) and still works on the CPU. It uses the
  DirectML that comes with Windows (System32\DirectML.dll, version 1.15 or
  later on current Windows 11). Only if Windows' copy is older does it offer
  Microsoft's DirectML package (193 MB download, Microsoft's DirectML license,
  which allows Microsoft to collect usage data through Windows).

  Privacy: official Windows builds of ONNX Runtime can emit ETW trace events
  (collected only if a Windows diagnostic trace session is active). Echo calls
  DisableTelemetryEvents() right after loading it.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\fetch_onnxruntime.ps1
  powershell -ExecutionPolicy Bypass -File tools\fetch_onnxruntime.ps1 -Gpu
  powershell -ExecutionPolicy Bypass -File tools\fetch_onnxruntime.ps1 -Destination "C:\Path\To\Echo\ai"
#>
param(
    [string]$Destination = (Join-Path (Split-Path $PSScriptRoot -Parent) 'ai'),
    [switch]$Gpu,
    [string]$Version = '1.30.0',
    [string]$Sha256 = 'c6ba983baf5681af108599675d2a89c2d145512d02de28aed0bff177cd0ba949'
)
$ErrorActionPreference = 'Stop'

# Downloads a nuget.org package and checks it against its published SHA-512.
function Get-NugetPackage([string]$Id, [string]$PackageVersion, [string]$Sha512Base64, [string]$Dir) {
    $lower = $Id.ToLower()
    $url = "https://api.nuget.org/v3-flatcontainer/$lower/$PackageVersion/$lower.$PackageVersion.nupkg"
    $zip = Join-Path $Dir "$lower.zip"
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    $sha = [System.Security.Cryptography.SHA512]::Create()
    $stream = [IO.File]::OpenRead($zip)
    try { $hash = [Convert]::ToBase64String($sha.ComputeHash($stream)) } finally { $stream.Dispose() }
    if ($hash -ne $Sha512Base64) { throw "SHA-512 mismatch for $Id $PackageVersion" }
    $out = Join-Path $Dir $lower
    Expand-Archive $zip -DestinationPath $out
    return $out
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) "echo-ort-$([guid]::NewGuid())"
if ($Gpu) {
    $gpuVersion = '1.24.4'
    Write-Host "This will download ONNX Runtime $gpuVersion with DirectML (~12 MB, MIT license) from nuget.org."
    $answer = Read-Host 'Continue? (y/N)'
    if ($answer -notmatch '^[yY]') { Write-Host 'Cancelled.'; exit 1 }
    New-Item -ItemType Directory -Force $tmp | Out-Null
    try {
        $pkg = Get-NugetPackage 'Microsoft.ML.OnnxRuntime.DirectML' $gpuVersion `
            'ZjOyv495vhfVXoTJp2utRyn8ir1TFIvCj0B9JP8QZGBXS0oFx+lY7ZCZqSdnVShQX6EtxYi2T3VOtYXcmBTV4A==' $tmp
        $lib = Join-Path $pkg 'runtimes\win-x64\native'
        New-Item -ItemType Directory -Force $Destination | Out-Null
        Copy-Item (Join-Path $lib 'onnxruntime.dll') $Destination -Force
        Copy-Item (Join-Path $lib 'onnxruntime_providers_shared.dll') $Destination -Force
        Copy-Item (Join-Path $pkg 'LICENSE') (Join-Path $Destination 'ONNXRUNTIME-LICENSE.txt') -Force
        Copy-Item (Join-Path $pkg 'ThirdPartyNotices.txt') (Join-Path $Destination 'ONNXRUNTIME-ThirdPartyNotices.txt') -Force
        # DirectML: Windows' own copy when it is new enough.
        $system = Join-Path $env:WINDIR 'System32\DirectML.dll'
        $ok = $false
        if (Test-Path $system) {
            $v = (Get-Item $system).VersionInfo
            $ok = ($v.FileMajorPart -gt 1) -or ($v.FileMajorPart -eq 1 -and $v.FileMinorPart -ge 15)
        }
        if ($ok) {
            Write-Host "Using Windows' DirectML ($system)."
        } else {
            Write-Host "Windows' DirectML is missing or older than 1.15. Microsoft's DirectML package is a 193 MB download"
            Write-Host "under Microsoft's DirectML license (it allows Microsoft to collect usage data through Windows)."
            $answer = Read-Host 'Download it? (y/N)'
            if ($answer -match '^[yY]') {
                $dml = Get-NugetPackage 'Microsoft.AI.DirectML' '1.15.4' `
                    '/edn9WkEq8kP1T9l2HKckYq39uPF4ezdR5kI/AK0U1zysIYPerKsubcx1suAm3LD1dTQKFP7j16gIqR7xE7yhQ==' $tmp
                Copy-Item (Join-Path $dml 'bin\x64-win\DirectML.dll') $Destination -Force
                Copy-Item (Join-Path $dml 'LICENSE.txt') (Join-Path $Destination 'DIRECTML-LICENSE.txt') -Force
            } else {
                Write-Host 'Skipped: AI voices will run on the CPU.'
            }
        }
        Write-Host "Installed ONNX Runtime $gpuVersion (DirectML) to $Destination (hash verified). Restart Echo."
    } finally {
        Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    }
    exit 0
}

$url = "https://github.com/microsoft/onnxruntime/releases/download/v$Version/onnxruntime-win-x64-$Version.zip"
Write-Host "This will download ONNX Runtime $Version (~83 MB, MIT license) from:"
Write-Host "  $url"
$answer = Read-Host 'Continue? (y/N)'
if ($answer -notmatch '^[yY]') { Write-Host 'Cancelled.'; exit 1 }

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
    Remove-Item (Join-Path $Destination 'DirectML.dll') -ErrorAction SilentlyContinue # left by a -Gpu install
    Copy-Item (Join-Path $tmp "onnxruntime-win-x64-$Version\LICENSE") (Join-Path $Destination 'ONNXRUNTIME-LICENSE.txt') -Force
    Copy-Item (Join-Path $tmp "onnxruntime-win-x64-$Version\ThirdPartyNotices.txt") (Join-Path $Destination 'ONNXRUNTIME-ThirdPartyNotices.txt') -Force
    Write-Host "Installed ONNX Runtime $Version to $Destination (hash verified). Restart Echo."
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
