[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$evidence = Join-Path $env:RUNNER_TEMP 'ziliu-evidence'
New-Item -ItemType Directory -Force $evidence | Out-Null
Set-Location $root
Start-Transcript -Path (Join-Path $evidence 'build.log') | Out-Null
try {
    function Assert-Exit([string]$Operation) {
        if ($LASTEXITCODE -ne 0) { throw "$Operation failed: exit $LASTEXITCODE" }
    }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products '*' -version '[18.0,19.0)' -property installationPath
    Assert-Exit 'VS detection'
    if (-not $vs) { throw 'Visual Studio 2026 is required.' }
    $msbuild = Join-Path $vs 'MSBuild/Current/Bin/amd64/MSBuild.exe'
    $toolset = Join-Path $vs 'MSBuild/Microsoft/VC/*/Platforms/x64/PlatformToolsets/v145/Toolset.props'
    if (-not (Test-Path $toolset)) { throw 'v145 toolset is required.' }
    $sdk = $env:ZILIU_WINDOWS_SDK_VERSION
    if ($sdk -notmatch '^10\.0\.(26100|28000)\.0$') { throw 'Unexpected SDK selection.' }
    $kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
    foreach ($relative in @("Include/$sdk/um/Windows.h", "Lib/$sdk/um/x64/kernel32.lib",
        "Lib/$sdk/ucrt/x64/ucrt.lib", "bin/$sdk/x64/rc.exe", "bin/$sdk/x64/midl.exe")) {
        if (-not (Test-Path (Join-Path $kits $relative))) { throw "Installed SDK missing $relative; no automatic SDK license acceptance." }
    }
    $identity = [ordered]@{
        sourceCommit = (& git rev-parse HEAD)
        imageOS = $env:ImageOS
        imageVersion = $env:ImageVersion
        sdk = $sdk
        toolset = 'v145'
        configuration = 'Release'
        runId = $env:GITHUB_RUN_ID
        runAttempt = $env:GITHUB_RUN_ATTEMPT
        guiAcceptance = 'NOT RUN: independent Windows 11 interactive acceptance required'
    }
    Assert-Exit 'Source identity'
    $identity | ConvertTo-Json | Set-Content (Join-Path $evidence 'identity.json')
    & $msbuild -version -nologo
    Assert-Exit 'MSBuild version'
    & cmake --version
    Assert-Exit 'CMake version'
    $pins = @{
        'third_party/librime' = '33e78140250125871856cdc5b42ddc6a5fcd3cd4'
        'third_party/rime-ice' = 'b681a34f788795034b3b288830f4861980bc8b0d'
    }
    foreach ($path in $pins.Keys) {
        $actual = & git -C $path rev-parse HEAD
        Assert-Exit 'Submodule identity'
        if ($actual -ne $pins[$path]) { throw "Submodule pin mismatch: $path" }
    }
    $pins | ConvertTo-Json | Set-Content (Join-Path $evidence 'submodules.json')
    $manifest = foreach ($file in (& git ls-files)) {
        if (Test-Path -LiteralPath $file -PathType Leaf) {
            '{0}  {1}' -f (Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant(), $file
        }
    }
    $manifest | Set-Content (Join-Path $evidence 'source-manifest.sha256')
    & ./scripts/fetch-librime-runtime.ps1
    $runtime = Join-Path $root '.cache/librime-runtime/dist/lib/rime.dll'
    if (-not (Test-Path $runtime)) { throw 'Real librime runtime missing.' }
    Get-FileHash $runtime -Algorithm SHA256 | Format-List | Out-String | Set-Content (Join-Path $evidence 'rime-runtime.txt')
    # Fixed runtime archive hash is verified by fetch-librime-runtime.ps1.
    $env:ZILIU_DEPENDENCY_ROOT = $root
    & $msbuild src/settings/ZiliuSettings.vcxproj /t:Restore /nologo /v:minimal `
        /p:Configuration=Release /p:Platform=x64 "/p:WindowsTargetPlatformVersion=$sdk" `
        "/p:RestoreSources=$env:ZILIU_NUGET_SOURCES" /p:NuGetAudit=false
    Assert-Exit 'Online NuGet restore'
    # Emit dependency identities, not an MSBuild binlog containing environment values.
    $assetRoots = @('src/settings', 'build/winui3') | Where-Object { Test-Path $_ }
    $assetFiles = @(Get-ChildItem -Path $assetRoots -Filter project.assets.json -Recurse)
    if ($assetFiles.Count -eq 0) { throw 'Restored NuGet dependency graph is missing.' }
    $packageIdentities = foreach ($file in $assetFiles) {
        $assets = Get-Content -Raw $file.FullName | ConvertFrom-Json -AsHashtable
        $assets.libraries.Keys
    }
    $packageIdentities | Sort-Object -Unique | Set-Content (Join-Path $evidence 'nuget-resolved-packages.txt')
    $env:ZILIU_CI_SKIP_EMBEDDED_TESTS = '1'
    & cmd.exe /d /c scripts\build-local.cmd Release
    Assert-Exit 'Full product build'
    $build = Join-Path $root 'build/local-x64-Release'
    $stagedRuntime = Join-Path $build 'bin/rime.dll'
    if ((Get-FileHash $runtime).Hash -ne (Get-FileHash $stagedRuntime).Hash) { throw 'Staged runtime differs.' }
    & ctest --test-dir $build --show-only=json-v1 | Set-Content (Join-Path $evidence 'ctest-plan.json')
    Assert-Exit 'CTest enumeration'
    & ctest --test-dir $build --output-on-failure --no-tests=error --output-junit (Join-Path $evidence 'ctest.xml')
    $testExit = $LASTEXITCODE
    & python scripts/ci/assert-ctest.py (Join-Path $evidence 'ctest-plan.json') (Join-Path $evidence 'ctest.xml')
    Assert-Exit 'Zero-skip CTest gate'
    if ($testExit -ne 0) { throw "CTest failed: $testExit" }
    & python -B -m unittest discover -s tests -p 'test_*.py' -v
    Assert-Exit 'Python tests'
    $packages = Join-Path $evidence 'packages'
    & ./scripts/package-alpha.ps1 -DependencyRoot $root -SourceDirectory (Join-Path $build 'bin') -OutputDirectory $packages
    $zip = @(Get-ChildItem $packages -Filter '*.zip')
    if ($zip.Count -ne 1) { throw 'Expected one alpha ZIP.' }
    # Verify the generated archive in a fresh disposable directory, never install it.
    $verifiedPackage = Join-Path $env:RUNNER_TEMP ('ziliu-package-check-' + [Guid]::NewGuid().ToString('N'))
    $archive = [IO.Compression.ZipFile]::OpenRead($zip[0].FullName)
    try {
        $entryNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName.Replace('\', '/')
            if ($name.StartsWith('/') -or $name.Contains(':') -or
                @($name.Split('/') | Where-Object { $_ -eq '..' }).Count -gt 0 -or
                -not $entryNames.Add($name)) { throw 'Unsafe or duplicate ZIP path.' }
        }
    } finally { $archive.Dispose() }
    try {
        Expand-Archive -LiteralPath $zip[0].FullName -DestinationPath $verifiedPackage
        & (Join-Path $verifiedPackage 'Install-Ziliu.ps1') -VerifyOnly
        foreach ($binary in @('ZiliuBroker.exe', 'ZiliuRegister.exe', 'ZiliuSettings.exe', 'ZiliuTIP.dll')) {
            $bytes = [IO.File]::ReadAllBytes((Join-Path $verifiedPackage "payload/$binary"))
            if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) { throw "Invalid PE: $binary" }
            $pe = [BitConverter]::ToInt32($bytes, 0x3c)
            if ($pe -lt 0 -or $pe + 6 -gt $bytes.Length -or
                [BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550 -or
                [BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664) { throw "Not x64 PE: $binary" }
        }
    } finally {
        if (Test-Path $verifiedPackage) { Remove-Item $verifiedPackage -Recurse -Force }
    }
    & ./scripts/package-alpha-installer.ps1 -PackageZip $zip[0].FullName `
        -ExpectedSha256 (Get-FileHash $zip[0].FullName -Algorithm SHA256).Hash -OutputDirectory $packages
    # Bound artifact size; do not publish databases, PDBs, dumps, or entire build trees.
    $totalBytes = (Get-ChildItem $evidence -Recurse -File | Measure-Object Length -Sum).Sum
    if ($totalBytes -gt 400MB) {
        Remove-Item $packages -Recurse -Force
        throw 'Artifacts exceeded 400 MiB; packages withheld to bound storage.'
    }
    'Release build + strict tests + unsigned packaging passed. Win11 GUI acceptance NOT RUN.' | Add-Content $env:GITHUB_STEP_SUMMARY
} finally {
    $testLog = Join-Path $root 'build/local-x64-Release/Testing/Temporary/LastTest.log'
    if (Test-Path $testLog) { Copy-Item $testLog (Join-Path $evidence 'LastTest.log') }
    Stop-Transcript | Out-Null
}
