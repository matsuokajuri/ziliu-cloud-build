[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$evidence = Join-Path $env:RUNNER_TEMP 'ziliu-rime-diagnostic'
$build = Join-Path $root 'build/rime-locale-diagnostic'
New-Item -ItemType Directory -Force $evidence | Out-Null
Set-Location $root
Start-Transcript -Path (Join-Path $evidence 'diagnostic.log') | Out-Null
$originalCulture = (Get-Culture).Name
try {
    function Assert-Exit([string]$operation) {
        if ($LASTEXITCODE -ne 0) { throw "$operation failed: exit $LASTEXITCODE" }
    }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products '*' -version '[18.0,19.0)' -property installationPath
    Assert-Exit 'VS detection'
    if (-not $vs) { throw 'VS2026 is required.' }
    $sdk = '10.0.26100.0'
    if (-not (Test-Path "${env:ProgramFiles(x86)}\Windows Kits\10\Include\$sdk\um\Windows.h")) {
        throw 'The installed SDK26100 is required; do not install another SDK.'
    }
    & ./scripts/fetch-librime-runtime.ps1
    $command = Join-Path $env:RUNNER_TEMP 'ziliu-rime-diagnostic-build.cmd'
    @"
@echo off
call "$vs\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 -winsdk=$sdk
if errorlevel 1 exit /b %errorlevel%
cmake --fresh -S "$root" -B "$build" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DZILIU_BUILD_TESTS=ON -DZILIU_ENABLE_RIME=ON "-DZILIU_DEPENDENCY_ROOT=$root"
if errorlevel 1 exit /b %errorlevel%
cmake --build "$build" --target ziliu_rime_epoch_diagnostic_tests
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $command -Encoding ascii
    & cmd.exe /d /c $command
    Assert-Exit 'Targeted diagnostic build'
    $outcomes = @()
    $expectedLocale = @{ 'en-US' = 1033; 'zh-CN' = 2052; 'ja-JP' = 1041 }
    foreach ($culture in @('en-US', 'zh-CN', 'ja-JP')) {
        # Change only this disposable runner user's culture; restore it below.
        # A new native process observes GetUserDefaultLCID, as Boost.Regex does.
        Set-Culture -CultureInfo $culture
        $locale = & python -c 'import ctypes,json; k=ctypes.windll.kernel32; print(json.dumps(dict(user_lcid=k.GetUserDefaultLCID(),system_lcid=k.GetSystemDefaultLCID(),acp=k.GetACP(),oemcp=k.GetOEMCP())))'
        Assert-Exit 'Native locale identity'
        if (($locale | ConvertFrom-Json).user_lcid -ne $expectedLocale[$culture]) {
            throw "Requested culture $culture did not take effect in the new native process."
        }
        $profileRoot = Join-Path $build "test-state/$culture"
        $env:ZILIU_RIME_USER_DATA_DIR = $profileRoot
        & (Join-Path $build 'bin/ziliu_rime_epoch_diagnostic_tests.exe') --fresh-profile
        $testExit = $LASTEXITCODE
        & python scripts/ci/describe-rime-cache.py $profileRoot (Join-Path $evidence "fingerprints-$culture.json")
        Assert-Exit 'Read-only fingerprint diagnostics'
        $outcomes += [ordered]@{ culture = $culture; nativeLocale = ($locale | ConvertFrom-Json); testExit = $testExit }
    }
    [ordered]@{
        purpose = 'Diagnostic only; not full product acceptance'
        sourceCommit = (& git rev-parse HEAD)
        originalCulture = $originalCulture
        image = $env:ImageVersion
        sdk = $sdk
        outcomes = $outcomes
    } | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $evidence 'outcomes.json')
    if ((Get-ChildItem $evidence -File | Measure-Object Length -Sum).Sum -gt 20MB) {
        throw 'Diagnostic evidence exceeded 20 MiB.'
    }
    if (@($outcomes | Where-Object { $_.testExit -ne 0 }).Count -gt 0) {
        throw 'At least one unchanged strict test failed; diagnostic evidence retained, full gate unchanged.'
    }
} finally {
    Set-Culture -CultureInfo $originalCulture
    Stop-Transcript | Out-Null
}
