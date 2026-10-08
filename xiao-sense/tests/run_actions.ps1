$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot
$output = Join-Path $project 'test-artifacts\actions'
New-Item -ItemType Directory -Force (Join-Path $output 'freertos') | Out-Null
foreach ($file in 'robot_actions.cpp','robot_actions.h','audio_cues.h','emotion_voice.h','reaction_catalog.h','reaction_player.h','reaction_player.cpp','action_mission.cpp','action_mission.h','person_follow.cpp') {
    Copy-Item -LiteralPath (Join-Path $project "src\$file") -Destination (Join-Path $output $file)
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'action_mocks.h') -Destination (Join-Path $output 'action_mocks.h')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'test_robot_actions.cpp') -Destination (Join-Path $output 'test_robot_actions.cpp')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'test_action_missions.cpp') -Destination (Join-Path $output 'test_action_missions.cpp')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'test_reaction_player.cpp') -Destination (Join-Path $output 'test_reaction_player.cpp')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'test_person_follow.cpp') -Destination (Join-Path $output 'test_person_follow.cpp')
# Exercise the actual wake cancellation/status functions alongside the action
# controller. The model/audio task remains mocked for these desktop tests.
$wakeSource = Get-Content -Raw -LiteralPath (Join-Path $project 'src\wake_word.cpp')
$cancelStart = $wakeSource.IndexOf('void cancel() {')
$cancelEnd = $wakeSource.IndexOf('void poll() {', $cancelStart)
$snapshotStart = $wakeSource.IndexOf('Status snapshot() {')
$snapshotEnd = $wakeSource.IndexOf('}  // namespace wake_word', $snapshotStart)
if ($cancelStart -lt 0 -or $cancelEnd -lt 0 -or $snapshotStart -lt 0 -or $snapshotEnd -lt 0) {
    throw 'Wake control functions not found'
}
$wakeControl = $wakeSource.Substring($cancelStart, $cancelEnd - $cancelStart) + $wakeSource.Substring($snapshotStart, $snapshotEnd - $snapshotStart)
Set-Content -LiteralPath (Join-Path $output 'wake_control.inc') -Value $wakeControl
$wakePollEnd = $wakeSource.IndexOf('bool testReaction()', $cancelEnd)
if ($wakePollEnd -lt 0) { throw 'Wake polling function not found' }
Set-Content -LiteralPath (Join-Path $output 'wake_poll.inc') -Value $wakeSource.Substring($cancelEnd, $wakePollEnd - $cancelEnd)
$speechSource = Get-Content -Raw -LiteralPath (Join-Path $project 'src\speech_commands.cpp')
$publishStart = $speechSource.IndexOf('void publishResult(')
$publishEnd = $speechSource.IndexOf('void detectTask(', $publishStart)
$listenStart = $speechSource.IndexOf('void listen(')
$submitStart = $speechSource.IndexOf('void submit(', $listenStart)
$pollStart = $speechSource.IndexOf('void poll()', $submitStart)
$controlEnd = $speechSource.IndexOf('} // namespace speech_commands', $pollStart)
if (@($publishStart,$publishEnd,$listenStart,$submitStart,$pollStart,$controlEnd) | Where-Object { $_ -lt 0 }) {
    throw 'Speech control functions not found'
}
Set-Content -LiteralPath (Join-Path $output 'speech_result.inc') -Value $speechSource.Substring($publishStart, $publishEnd - $publishStart)
$speechControl = $speechSource.Substring($listenStart, $submitStart - $listenStart) + $speechSource.Substring($pollStart, $controlEnd - $pollStart)
Set-Content -LiteralPath (Join-Path $output 'speech_control.inc') -Value $speechControl
foreach ($header in 'Arduino.h','audio_engine.h','camera_service.h','person_follow.h','personality_events.h','photo_store.h','speech_commands.h','vision_service.h','wake_word.h','zumo_link.h','freertos/semphr.h') {
    Set-Content -LiteralPath (Join-Path $output $header) -Value '#include "action_mocks.h"'
}
$linkOutput = Join-Path $output 'link'
New-Item -ItemType Directory -Force $linkOutput | Out-Null
foreach ($file in 'zumo_link.cpp','zumo_link.h') {
    Copy-Item -LiteralPath (Join-Path $project "src\$file") -Destination (Join-Path $linkOutput $file)
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'link_arduino_mock.h') -Destination (Join-Path $linkOutput 'Arduino.h')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'test_zumo_link.cpp') -Destination (Join-Path $linkOutput 'test_zumo_link.cpp')
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$directionOutput = Join-Path $output 'direction'
New-Item -ItemType Directory -Force $directionOutput | Out-Null
foreach ($file in 'voice_direction.cpp','voice_direction.h','voice_settings.h') {
    Copy-Item -LiteralPath (Join-Path $project "src\$file") -Destination (Join-Path $directionOutput $file)
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'voice_direction_mocks.h') -Destination $directionOutput
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'test_voice_direction.cpp') -Destination $directionOutput
foreach ($header in 'audio_engine.h','wake_word.h','zumo_link.h','nvs.h') {
    Set-Content -LiteralPath (Join-Path $directionOutput $header) -Value '#include "voice_direction_mocks.h"'
}
$vs = & $vswhere -latest -products * -property installationPath
& (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
Push-Location $output
try {
    & cl /nologo /std:c++17 /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS "/I$directionOutput" "/Fe:$directionOutput\test_voice_direction.exe" "/Fo:$directionOutput\test_voice_direction.obj" "$directionOutput\test_voice_direction.cpp"
    if ($LASTEXITCODE -ne 0) { throw 'Voice direction compilation failed' }
    & "$directionOutput\test_voice_direction.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Voice direction tests failed' }
    & cl /nologo /std:c++17 /EHsc /W4 /I. /D_CRT_SECURE_NO_WARNINGS /DMISSION_TRANSLATION /c /Fo:action_mission.obj action_mission.cpp
    if ($LASTEXITCODE -ne 0) { throw 'Mission controller compilation failed' }
    & cl /nologo /std:c++17 /EHsc /W4 /I. /D_CRT_SECURE_NO_WARNINGS /DMISSION_TRANSLATION /c /Fo:reaction_player.obj reaction_player.cpp
    if ($LASTEXITCODE -ne 0) { throw 'Reaction coordinator compilation failed' }
    & cl /nologo /std:c++17 /EHsc /W4 /I. /D_CRT_SECURE_NO_WARNINGS /DMISSION_TRANSLATION /DFOLLOW_TRANSLATION /c /Fo:person_follow.obj person_follow.cpp
    if ($LASTEXITCODE -ne 0) { throw 'Person follow compilation failed' }
    & cl /nologo /std:c++17 /EHsc /W4 /I. /D_CRT_SECURE_NO_WARNINGS /DMISSION_TRANSLATION /Fe:test_reaction_player.exe /Fo:test_reaction_player.obj test_reaction_player.cpp reaction_player.obj
    if ($LASTEXITCODE -ne 0) { throw 'Reaction tests compilation failed' }
    & .\test_reaction_player.exe
    if ($LASTEXITCODE -ne 0) { throw 'Reaction tests failed' }
    & cl /nologo /std:c++17 /EHsc /W4 /I. /D_CRT_SECURE_NO_WARNINGS /Fe:test_robot_actions.exe /Fo:test_robot_actions.obj test_robot_actions.cpp action_mission.obj person_follow.obj reaction_player.obj
    if ($LASTEXITCODE -ne 0) { throw 'Controller test compilation failed' }
    & .\test_robot_actions.exe
    if ($LASTEXITCODE -ne 0) { throw 'Controller tests failed' }
    & cl /nologo /std:c++17 /EHsc /W4 /I. /D_CRT_SECURE_NO_WARNINGS /Fe:test_action_missions.exe /Fo:test_action_missions.obj test_action_missions.cpp action_mission.obj person_follow.obj reaction_player.obj
    if ($LASTEXITCODE -ne 0) { throw 'Mission tests compilation failed' }
    & .\test_action_missions.exe
    if ($LASTEXITCODE -ne 0) { throw 'Mission tests failed' }
    & cl /nologo /std:c++17 /EHsc /W4 /I. /D_CRT_SECURE_NO_WARNINGS /Fe:test_person_follow.exe /Fo:test_person_follow.obj test_person_follow.cpp action_mission.obj person_follow.obj reaction_player.obj
    if ($LASTEXITCODE -ne 0) { throw 'Person tracking tests compilation failed' }
    & .\test_person_follow.exe
    if ($LASTEXITCODE -ne 0) { throw 'Person tracking tests failed' }
    & cl /nologo /std:c++17 /EHsc /W4 "/I$project\src" /D_CRT_SECURE_NO_WARNINGS /Fe:test_speech_pipeline.exe /Fo:test_speech_pipeline.obj "$PSScriptRoot\test_speech_pipeline.cpp"
    if ($LASTEXITCODE -ne 0) { throw 'Speech pipeline test compilation failed' }
    & .\test_speech_pipeline.exe
    if ($LASTEXITCODE -ne 0) { throw 'Speech pipeline tests failed' }
    & cl /nologo /std:c++17 /EHsc /W4 "/I$project\src" "/I$output" /D_CRT_SECURE_NO_WARNINGS /Fe:test_speech_results.exe /Fo:test_speech_results.obj "$PSScriptRoot\test_speech_results.cpp"
    if ($LASTEXITCODE -ne 0) { throw 'Speech result test compilation failed' }
    & .\test_speech_results.exe
    if ($LASTEXITCODE -ne 0) { throw 'Speech result tests failed' }
    & "C:\Users\nazir\.platformio\penv\Scripts\python.exe" "$PSScriptRoot\test_cue_pack.py"
    if ($LASTEXITCODE -ne 0) { throw 'Cue packing tests failed' }
    & "C:\Users\nazir\.platformio\penv\Scripts\python.exe" "$PSScriptRoot\test_i2c_timeout_patch.py"
    if ($LASTEXITCODE -ne 0) { throw 'Build-local I2C timeout backport tests failed' }
    & cl /nologo /std:c++17 /EHsc /W4 "/I$project\src" /Fe:test_cue_adpcm.exe /Fo:test_cue_adpcm.obj "$PSScriptRoot\test_cue_adpcm.cpp"
    if ($LASTEXITCODE -ne 0) { throw 'Cue playback test compilation failed' }
    & .\test_cue_adpcm.exe (Join-Path $project 'test-artifacts\cues')
    if ($LASTEXITCODE -ne 0) { throw 'Cue playback tests failed' }
    & cl /nologo /std:c++17 /EHsc /W4 "/I$linkOutput" /D_CRT_SECURE_NO_WARNINGS "/Fe:$linkOutput\test_zumo_link.exe" "/Fo:$linkOutput\test_zumo_link.obj" "$linkOutput\test_zumo_link.cpp"
    if ($LASTEXITCODE -ne 0) { throw 'UART bridge test compilation failed' }
    & "$linkOutput\test_zumo_link.exe"
    if ($LASTEXITCODE -ne 0) { throw 'UART bridge tests failed' }
} finally { Pop-Location }
