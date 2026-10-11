$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot
$output = Join-Path $project 'test-artifacts\gemini-config'
$idfJson = Join-Path $env:USERPROFILE '.platformio\packages\framework-espidf\components\json\cJSON'
New-Item -ItemType Directory -Force $output | Out-Null
$source = Get-Content -Raw -LiteralPath (Join-Path $project 'src\web_server.cpp')
$start = $source.IndexOf('esp_err_t sendJsonObject(')
$end = $source.IndexOf('esp_err_t rootHandler(', $start)
$configStart = $source.IndexOf('bool geminiRequestOrigin(')
$configEnd = $source.IndexOf('esp_err_t actionsDebugHandler(', $configStart)
if ($start -lt 0 -or $end -lt $start -or $configStart -lt 0 -or $configEnd -le $configStart) { throw 'Gemini config handlers not found' }
Set-Content -LiteralPath (Join-Path $output 'gemini_config_handlers.inc') -Value ($source.Substring($start, $end - $start) + $source.Substring($configStart, $configEnd - $configStart))
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
& (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
Push-Location $output
try {
    & cl /nologo /W4 /D_CRT_SECURE_NO_WARNINGS /c /TC "/I$idfJson" /Fo:cJSON.obj "$idfJson\cJSON.c"
    if ($LASTEXITCODE -ne 0) { throw 'cJSON compilation failed' }
    & cl /nologo /std:c++17 /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS "/I$output" "/I$idfJson" /Fe:test_gemini_config.exe /Fo:test_gemini_config.obj "$PSScriptRoot\test_gemini_config.cpp" cJSON.obj
    if ($LASTEXITCODE -ne 0) { throw 'Gemini config test compilation failed' }
    & .\test_gemini_config.exe
    if ($LASTEXITCODE -ne 0) { throw 'Gemini config tests failed' }
} finally { Pop-Location }
