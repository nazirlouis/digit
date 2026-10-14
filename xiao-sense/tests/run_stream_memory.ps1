$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot
$output = Join-Path $project 'test-artifacts\stream-memory'
New-Item -ItemType Directory -Force $output | Out-Null
$source = Get-Content -LiteralPath (Join-Path $project 'src\web_server.cpp') -Raw
$start = $source.IndexOf('esp_err_t streamHandler(httpd_req_t* request) {')
$end = $source.IndexOf('esp_err_t geminiImageHandler(', $start)
if ($start -lt 0 -or $end -le $start) { throw 'Stream handler source boundaries were not found' }
Set-Content -LiteralPath (Join-Path $output 'stream_handler.cpp') -Value $source.Substring($start, $end - $start)
Copy-Item (Join-Path $project 'src\camera_frame_limits.h') $output
foreach ($file in 'stream_memory_mocks.h','test_stream_memory.cpp') {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination (Join-Path $output $file)
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
& (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
Push-Location $output
try {
    & cl /nologo /std:c++17 /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS /I. /Fe:test_stream_memory.exe test_stream_memory.cpp
    if ($LASTEXITCODE -ne 0) { throw 'Camera stream ownership compilation failed' }
    & .\test_stream_memory.exe
    if ($LASTEXITCODE -ne 0) { throw 'Camera stream ownership tests failed' }
} finally {
    Pop-Location
}
