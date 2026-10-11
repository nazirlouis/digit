$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot
$output = Join-Path $project 'test-artifacts\local-stop'
New-Item -ItemType Directory -Force $output | Out-Null
# Exercise the real poll/cancel handoff with mocked model and action services.
$source = Get-Content -Raw -LiteralPath (Join-Path $project 'src\local_stop.cpp')
$pollStart = $source.IndexOf('void poll() {')
$controlEnd = $source.IndexOf('void audioFault()', $pollStart)
if ($pollStart -lt 0 -or $controlEnd -lt 0) { throw 'Local Stop control functions not found' }
Set-Content -LiteralPath (Join-Path $output 'local_stop_control.inc') -Value $source.Substring($pollStart, $controlEnd - $pollStart)
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
& (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
& cl /nologo /std:c++17 /EHsc /W4 "/I$project\src" "/I$output" "/Fe:$output\test_local_stop.exe" "/Fo:$output\test_local_stop.obj" "$PSScriptRoot\test_local_stop.cpp"
if ($LASTEXITCODE -ne 0) { throw 'Local Stop coordination compilation failed' }
& "$output\test_local_stop.exe"
if ($LASTEXITCODE -ne 0) { throw 'Local Stop coordination tests failed' }
