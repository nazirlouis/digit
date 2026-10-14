$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot
$output = Join-Path $project 'test-artifacts\robot-session'
New-Item -ItemType Directory -Force (Join-Path $output 'freertos') | Out-Null
foreach ($file in 'robot_session.cpp','robot_session.h','action_console.cpp','action_console.h','speech_pipeline.h','gemini_client.h','gemini_live.h','gemini_prompt.h','live_audio.h','robot_actions.h','task_policy.h','camera_frame_limits.h') {
    Copy-Item -LiteralPath (Join-Path $project "src\$file") -Destination (Join-Path $output $file)
}
foreach ($file in 'robot_session_mocks.h','test_robot_session.cpp') {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination (Join-Path $output $file)
}
foreach ($header in 'Arduino.h','esp_heap_caps.h','esp_random.h','zumo_link.h','reaction_player.h','audio_engine.h','camera_service.h','wake_word.h') {
    Set-Content -LiteralPath (Join-Path $output $header) -Value '#include "robot_session_mocks.h"'
}
foreach ($header in 'queue.h','semphr.h','FreeRTOS.h') {
    Set-Content -LiteralPath (Join-Path $output "freertos\$header") -Value '#include "robot_session_mocks.h"'
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
& (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
Push-Location $output
try {
    & cl /nologo /std:c++17 /EHsc /W4 /WX /D_CRT_SECURE_NO_WARNINGS /I. /Fe:test_robot_session.exe /Fo:test_robot_session.obj test_robot_session.cpp
    if ($LASTEXITCODE -ne 0) { throw 'Gemini Live lifecycle compilation failed' }
    & .\test_robot_session.exe
    if ($LASTEXITCODE -ne 0) { throw 'Gemini Live lifecycle tests failed' }
} finally { Pop-Location }
