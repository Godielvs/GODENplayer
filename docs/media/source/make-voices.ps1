$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Speech
$voiceRoot = Join-Path $PSScriptRoot 'voices'
New-Item -ItemType Directory -Force -Path $voiceRoot | Out-Null
$lines = @(
    'Beneath the noise, a quiet current.',
    'A pulse we carry through the blue.',
    'The city folds into the water.',
    'The tide brings every colour through.',
    'Hold this moment in the low light.',
    'Let the rhythm find its room.',
    'We leave a signal in the silence.',
    'And follow where the colours bloom.',
    'Rain leaves a silver line behind.',
    'The glass remembers every light.',
    'Green opens slowly in the morning.',
    'And violet gathers up the night.',
    'We take the long way through the garden.',
    'We let the heavy weather go.',
    'A softer world beneath the surface.',
    'A little room for things to grow.'
)
$voice = [System.Speech.Synthesis.SpeechSynthesizer]::new()
try {
    $voice.SelectVoice('Microsoft Zira Desktop')
    $voice.Rate = -1
    $format = [System.Speech.AudioFormat.SpeechAudioFormatInfo]::new(
        48000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
        [System.Speech.AudioFormat.AudioChannel]::Mono)
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $voice.SetOutputToWaveFile((Join-Path $voiceRoot ('{0:00}.wav' -f $i)), $format)
        $voice.Speak($lines[$i])
        $voice.SetOutputToNull()
    }
} finally { $voice.Dispose() }
ConvertTo-Json -InputObject $lines | Set-Content -LiteralPath (Join-Path $voiceRoot 'lines.json') -Encoding utf8
