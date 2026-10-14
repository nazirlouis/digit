$ErrorActionPreference='Stop'
$project=Split-Path $PSScriptRoot
$output=Join-Path $project 'test-artifacts\gesture-chirp'
New-Item -ItemType Directory -Force $output | Out-Null
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products * -property installationPath
& (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
Push-Location $output
try {
  & cl /nologo /std:c++17 /EHsc /W4 /WX "/I$project\src" /Fe:test_gesture_chirp.exe /Fo:.\ "$PSScriptRoot\test_gesture_chirp.cpp" "$project\src\emotion_voice.cpp"
  if($LASTEXITCODE -ne 0){throw 'Chirp compilation failed'}
  & .\test_gesture_chirp.exe "$output\expression-chirps.wav"
  if($LASTEXITCODE -ne 0){throw 'Chirp tests failed'}
} finally { Pop-Location }
