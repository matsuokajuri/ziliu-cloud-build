# 字流 Ziliu cloud build

Windows Chinese input method source and an unsigned test-build pipeline.
This repository includes current first-party source, synthetic tests, required
resources, packaging scripts and fixed public dependency references.

## Build

The GitHub Actions workflow uses the standard `windows-2025-vs2026` runner,
MSVC v145 and its installed Windows SDK 10.0.26100.0. The local script default
remains SDK 10.0.28000.0. The CI SDK choice is a compatibility build whose actual
result must be established by the corresponding Actions run.

The workflow checks out fixed submodules, verifies the librime 1.17.0 archive
SHA-256, restores the existing pinned NuGet package versions, builds all normal
Release targets, runs the complete 33-test CTest suite and Python unit tests,
and packages an unsigned ZIP and installer. Failed, skipped or missing tests
fail the job. ZIP manifests and x64 PE headers are verified without installation.
Artifacts expire after one day and are bounded to 400 MiB. Storage remains
subject to the account's Actions quota and budget.

For a prepared Windows development environment:

```
set ZILIU_WINDOWS_SDK_VERSION=10.0.26100.0
powershell -File scripts\fetch-librime-runtime.ps1
scripts\build-local.cmd Release
```

`ZILIU_NUGET_SOURCES` defaults to nuget.org and can select an approved offline feed.
The frozen Rime source fingerprint requires a Windows CRLF Rime Ice checkout
plus LF first-party overlays; CI and `.gitattributes` preserve those reviewed
bytes. A fingerprint mismatch is a failure, never a reason to regenerate the pin.

## Dependencies and licenses

- librime: `33e78140250125871856cdc5b42ddc6a5fcd3cd4`
- Rime Ice: `b681a34f788795034b3b288830f4861980bc8b0d`
- Official prebuilt librime archive SHA-256:
  `7478c7caa4ff6b37de86daba1f7ce4a994a4f5ba24872a820fb2b3a9b01fed15`

The workflow uses the verified upstream librime binary; it does not rebuild it.
See [LICENSE](LICENSE), [third-party notices](THIRD_PARTY_NOTICES.md), and
[data provenance](data/ziliu/README.md).

## Test-build limits

Artifacts are unsigned early test builds. A green Windows Server runner covers
only the build and tests it actually executes. It does not certify interactive
Windows 11 TSF input, application compatibility, privacy/focus behavior, DPI,
installation, upgrades, uninstallation or reboot behavior. This workflow does
not install or register the input method and does not claim those acceptance gates.
