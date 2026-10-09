<#
.SYNOPSIS
  Generates speech test material with the Windows built-in text-to-speech
  voices (48 kHz, 16-bit, mono WAV) for the noise-suppression and echo-
  cancellation tests. Nothing is downloaded; the files are generated locally
  and are not committed to the repository.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\make_speech_samples.ps1 -OutDir build\release\test-speech
#>
param([string]$OutDir = (Join-Path $PSScriptRoot '..\build\release\test-speech'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Speech
New-Item -ItemType Directory -Force $OutDir | Out-Null
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(48000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
$texts = @{
    'speech_a.wav' = @('Microsoft David Desktop', 'The quick brown fox jumps over the lazy dog. Please call me back when you get a chance, I will be at home all evening. We should test the microphone before the stream starts.')
    'speech_b.wav' = @('Microsoft Zira Desktop', 'Good morning everyone, welcome to the meeting. Today we will review the audio settings and check that the noise suppression keeps every word clear and natural.')
}
$voices = (New-Object System.Speech.Synthesis.SpeechSynthesizer).GetInstalledVoices() | ForEach-Object { $_.VoiceInfo.Name }
foreach ($file in $texts.Keys) {
    $voice, $text = $texts[$file]
    $synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
    if ($voices -contains $voice) { $synth.SelectVoice($voice) }
    $path = Join-Path $OutDir $file
    $synth.SetOutputToWaveFile($path, $fmt)
    $synth.Rate = 0
    $synth.Speak($text)
    $synth.Dispose()
    Write-Host "wrote $path"
}
