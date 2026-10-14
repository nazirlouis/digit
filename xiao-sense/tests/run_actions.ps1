$ErrorActionPreference='Stop'
$project=Split-Path $PSScriptRoot
$output=Join-Path $project 'test-artifacts\actions'
New-Item -ItemType Directory -Force (Join-Path $output 'freertos') | Out-Null
foreach($file in 'robot_actions.cpp','robot_actions.h','action_console.h') { Copy-Item (Join-Path $project "src\$file") $output }
Copy-Item (Join-Path $project 'src\zumo_link.h') (Join-Path $output 'real_zumo_link.h')
Copy-Item (Join-Path $PSScriptRoot 'robot_tools_mocks.h') $output
Copy-Item (Join-Path $PSScriptRoot 'test_robot_actions.cpp') $output
foreach($header in 'Arduino.h','audio_engine.h','zumo_link.h','reaction_player.h','personality_events.h','robot_session.h','local_stop.h','vision_service.h','wake_word.h') { Set-Content (Join-Path $output $header) '#include "robot_tools_mocks.h"' }
Set-Content (Join-Path $output 'freertos\semphr.h') '#include "robot_tools_mocks.h"'
$linkOutput=Join-Path $output 'link'
New-Item -ItemType Directory -Force $linkOutput | Out-Null
foreach($file in 'zumo_link.cpp','zumo_link.h','reaction_catalog.h','dance_music.h') { Copy-Item (Join-Path $project "src\$file") $linkOutput }
Copy-Item (Join-Path $PSScriptRoot 'link_arduino_mock.h') (Join-Path $linkOutput 'Arduino.h')
Copy-Item (Join-Path $PSScriptRoot 'test_zumo_link.cpp') $linkOutput
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products * -property installationPath
& (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
Push-Location $output
try {
 & cl /nologo /std:c++17 /EHsc /W4 /I. /D_CRT_SECURE_NO_WARNINGS /Fe:test_robot_actions.exe /Fo:test_robot_actions.obj test_robot_actions.cpp
 if($LASTEXITCODE -ne 0){throw 'Robot tool compilation failed'}
 & .\test_robot_actions.exe
 if($LASTEXITCODE -ne 0){throw 'Robot tool tests failed'}
 & cl /nologo /std:c++17 /EHsc /W4 "/I$linkOutput" /D_CRT_SECURE_NO_WARNINGS "/Fe:$linkOutput\test_zumo_link.exe" "/Fo:$linkOutput\test_zumo_link.obj" "$linkOutput\test_zumo_link.cpp"
 if($LASTEXITCODE -ne 0){throw 'UART compilation failed'}
 & "$linkOutput\test_zumo_link.exe"
 if($LASTEXITCODE -ne 0){throw 'UART tests failed'}
} finally { Pop-Location }
