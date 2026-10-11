$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot
$output = Join-Path $project 'test-artifacts\camera-broker'
New-Item -ItemType Directory -Force $output | Out-Null
$source = Get-Content -Raw (Join-Path $project 'src\camera_service.cpp')
$start = $source.IndexOf('SemaphoreHandle_t frameMutex')
$end = $source.IndexOf('// XIAO', $start)
Set-Content -LiteralPath (Join-Path $output 'camera_capture.inc') -Value $source.Substring($start,$end-$start)
$start = $source.IndexOf('bool copyLatestJpeg(')
$end = $source.IndexOf('}  // namespace camera_service', $start)
Set-Content -LiteralPath (Join-Path $output 'camera_copy.inc') -Value $source.Substring($start,$end-$start)
Copy-Item (Join-Path $project 'src\camera_service.h') $output
Copy-Item (Join-Path $PSScriptRoot 'test_camera_broker.cpp') $output
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
& (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
Push-Location $output
try {
    & cl /nologo /std:c++17 /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS /I. /Fe:test_camera_broker.exe test_camera_broker.cpp
    if ($LASTEXITCODE -ne 0) { throw 'Camera broker test compilation failed' }
    & .\test_camera_broker.exe
    if ($LASTEXITCODE -ne 0) { throw 'Camera broker tests failed' }
} finally { Pop-Location }
