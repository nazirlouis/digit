$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot
$output = Join-Path $project 'test-artifacts\actions'
New-Item -ItemType Directory -Force (Join-Path $output 'freertos') | Out-Null
foreach ($file in 'robot_actions.cpp','robot_actions.h','audio_cues.h') {
    Copy-Item -LiteralPath (Join-Path $project "src\$file") -Destination (Join-Path $output $file)
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'action_mocks.h') -Destination (Join-Path $output 'action_mocks.h')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'test_robot_actions.cpp') -Destination (Join-Path $output 'test_robot_actions.cpp')
foreach ($header in 'Arduino.h','audio_engine.h','camera_service.h','person_follow.h','photo_store.h','speech_commands.h','vision_service.h','wake_word.h','zumo_link.h','freertos/semphr.h') {
    Set-Content -LiteralPath (Join-Path $output $header) -Value '#include "action_mocks.h"'
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
& (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
Push-Location $output
try {
    & cl /nologo /std:c++17 /EHsc /W4 /I. /D_CRT_SECURE_NO_WARNINGS /Fe:test_robot_actions.exe /Fo:test_robot_actions.obj test_robot_actions.cpp
    if ($LASTEXITCODE -ne 0) { throw 'Controller test compilation failed' }
    & .\test_robot_actions.exe
    if ($LASTEXITCODE -ne 0) { throw 'Controller tests failed' }
} finally { Pop-Location }
