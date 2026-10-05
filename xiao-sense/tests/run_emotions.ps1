$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot
$output = Join-Path $project 'test-artifacts\emotions'
New-Item -ItemType Directory -Force $output | Out-Null
$web = Get-Content -Raw -LiteralPath (Join-Path $project 'src\web_server.cpp')
foreach ($section in @(
    @('face_controls.inc','esp_err_t zumoFaceHandler(', 'bool parseLedValue('),
    @('face_generation.inc','bool currentGeneration(', 'esp_err_t actionsStatusHandler(')
)) {
    $start = $web.IndexOf($section[1])
    $end = $web.IndexOf($section[2], $start)
    if ($start -lt 0 -or $end -lt 0) { throw 'Production face handler not found' }
    Set-Content -LiteralPath (Join-Path $output $section[0]) -Value $web.Substring($start, $end - $start)
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
& (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
Push-Location $output
try {
    foreach ($test in 'test_emotion_voice','test_face_controls') {
        & cl /nologo /std:c++17 /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS "/I$project\src" "/I$output" "/Fe:$test.exe" "$PSScriptRoot\$test.cpp" "$project\src\emotion_voice.cpp"
        if ($LASTEXITCODE -ne 0) { throw "$test compilation failed" }
        & ".\$test.exe"
        if ($LASTEXITCODE -ne 0) { throw "$test failed" }
    }
    & node "$PSScriptRoot\test_face_dashboard.cjs"
    if ($LASTEXITCODE -ne 0) { throw 'Face dashboard tests failed' }
} finally { Pop-Location }
