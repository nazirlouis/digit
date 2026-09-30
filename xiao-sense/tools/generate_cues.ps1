$ErrorActionPreference = 'Stop'
$cueDirectory = Join-Path $PSScriptRoot '..\audio_cues'
New-Item -ItemType Directory -Force $cueDirectory | Out-Null
$voice = New-Object -ComObject SAPI.SpVoice
$voice.Rate = 1
$cues = [ordered]@{
    hello = 'Hello! I am Digit.'
    following = 'I see you. Let us go.'
    lost = 'Where did you go?'
    countdown = 'Three. Two. One.'
    photo = 'Got it!'
    clear = 'The way ahead is clear.'
    obstacle = 'Something is in the way.'
    person = 'I found someone!'
    explore = 'Time to explore.'
}
foreach ($entry in $cues.GetEnumerator()) {
    $stream = New-Object -ComObject SAPI.SpFileStream
    $stream.Format.Type = 18 # 16 kHz, signed 16-bit, mono PCM
    $stream.Open((Join-Path (Resolve-Path $cueDirectory) ($entry.Key + '.wav')), 3, $false)
    try {
        $voice.AudioOutputStream = $stream
        [void]$voice.Speak($entry.Value)
    } finally { $stream.Close() }
}
