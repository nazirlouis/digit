$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot
$output = Join-Path $project 'test-artifacts\gemini'
New-Item -ItemType Directory -Force $output | Out-Null
$client = Get-Content -Raw -LiteralPath (Join-Path $project 'src\gemini_client.cpp')
$settingsStart = $client.IndexOf('namespace {')
$settingsEnd = $client.IndexOf('uint32_t nowMs()')
$publicStart = $client.IndexOf('namespace gemini_client {')
$publicEnd = $client.IndexOf('void live(', $publicStart)
if (@($settingsStart,$settingsEnd,$publicStart,$publicEnd) | Where-Object { $_ -lt 0 }) { throw 'Gemini settings helpers not found' }
Set-Content -LiteralPath (Join-Path $output 'gemini_settings_private.inc') -Value ($client.Substring($settingsStart,$settingsEnd-$settingsStart) + [Environment]::NewLine + '}')
Set-Content -LiteralPath (Join-Path $output 'gemini_settings_public.inc') -Value ($client.Substring($publicStart,$publicEnd-$publicStart) + [Environment]::NewLine + '}')
$vswhere = Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -property installationPath
& (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
Push-Location $output
try {
 & cl /nologo /std:c++17 /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS "/I$output" /Fe:test_gemini_settings.exe /Fo:test_gemini_settings.obj "$PSScriptRoot\test_gemini_settings.cpp"
 if ($LASTEXITCODE -ne 0) { throw 'Gemini settings compilation failed' }
 & .\test_gemini_settings.exe
 if ($LASTEXITCODE -ne 0) { throw 'Gemini settings failed' }
} finally { Pop-Location }
