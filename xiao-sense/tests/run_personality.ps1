$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot
$output = Join-Path $project 'test-artifacts\personality'
New-Item -ItemType Directory -Force $output | Out-Null
foreach ($file in 'personality_events.cpp','personality_events.h','reaction_player.h','robot_actions.h','speech_commands.h','wake_word.h','zumo_link.h','audio_cues.h','emotion_voice.h','reaction_catalog.h') {
    Copy-Item -LiteralPath (Join-Path $project "src\$file") -Destination (Join-Path $output $file)
}
foreach ($file in 'personality_mocks.h','test_personality_events.cpp') {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination (Join-Path $output $file)
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'personality_audio_mock.h') -Destination (Join-Path $output 'audio_engine.h')
Set-Content -LiteralPath (Join-Path $output 'Arduino.h') -Value "#pragma once`n#include <cstdint>`nuint32_t millis();"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
& (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
Push-Location $output
try {
    & cl /nologo /std:c++17 /EHsc /W4 /WX /D_CRT_SECURE_NO_WARNINGS /I. /Fe:test_personality_events.exe test_personality_events.cpp
    if ($LASTEXITCODE -ne 0) { throw 'Personality event compilation failed' }
    foreach ($case in 'startup','freshness','expiry','busy_startup','stop','speech','exact_result','queue_expiry','queue_takeover','fault','fault_expiry','fault_after_safety_stop','fault_after_user_stop','fault_reconnect','ownership','wrap') {
        & .\test_personality_events.exe $case
        if ($LASTEXITCODE -ne 0) { throw "Personality event test failed: $case" }
    }
} finally { Pop-Location }
