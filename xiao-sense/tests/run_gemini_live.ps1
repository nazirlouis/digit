$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot
$output = Join-Path $project 'test-artifacts\gemini-live'
$idfJson = Join-Path $env:USERPROFILE '.platformio\packages\framework-espidf\components\json\cJSON'
New-Item -ItemType Directory -Force $output | Out-Null
$vswhere = Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -property installationPath
& (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
Push-Location $output
try {
 & cl /nologo /W4 /D_CRT_SECURE_NO_WARNINGS /c /TC "/I$idfJson" /Fo:cJSON.obj "$idfJson\cJSON.c"
 if ($LASTEXITCODE -ne 0) { throw 'cJSON compilation failed' }
 foreach ($test in @('test_gemini_live','test_gemini_transport')) {
  & cl /nologo /std:c++17 /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS "/I$idfJson" "/Fe:$test.exe" "/Fo:$test.obj" "$PSScriptRoot\$test.cpp" cJSON.obj
  if ($LASTEXITCODE -ne 0) { throw "$test compilation failed" }
  & ".\$test.exe"
  if ($LASTEXITCODE -ne 0) { throw "$test failed" }
 }
} finally { Pop-Location }
