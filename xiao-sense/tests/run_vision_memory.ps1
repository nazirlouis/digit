$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot
$output = Join-Path $project 'test-artifacts\vision-memory'
New-Item -ItemType Directory -Force $output | Out-Null
foreach ($file in 'vision_service.cpp','vision_service.h') {
    Copy-Item -LiteralPath (Join-Path $project "src\$file") -Destination (Join-Path $output $file)
}
foreach ($file in 'vision_memory_mocks.h','test_vision_memory.cpp') {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination (Join-Path $output $file)
}
foreach ($header in 'Arduino.h','camera_service.h','esp_camera.h','esp_heap_caps.h','esp_jpeg_dec.h','esp_partition.h','pedestrian_detect.hpp','battery_detector.h','object_model_config.h','wake_word.h','speech_commands.h') {
    Set-Content -LiteralPath (Join-Path $output $header) -Value '#include "vision_memory_mocks.h"'
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
& (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
Push-Location $output
try {
    & cl /nologo /std:c++17 /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS /I. /Fe:test_vision_memory.exe /Fo:test_vision_memory.obj test_vision_memory.cpp
    if ($LASTEXITCODE -ne 0) { throw 'Vision lifecycle compilation failed' }
    & .\test_vision_memory.exe
    if ($LASTEXITCODE -ne 0) { throw 'Vision lifecycle tests failed' }
} finally {
    Pop-Location
}
