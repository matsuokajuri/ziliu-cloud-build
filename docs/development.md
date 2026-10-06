# 构建与开发

[← 项目首页](../README.md) · [架构](architecture.md) · [贡献指南](../CONTRIBUTING.md)

## 环境与依赖

当前构建脚本使用 Visual Studio 2026（18.x）和 MSVC v145，第一方 C++ 代码采用 C++23。请准备：

| 工具 | 要求 |
| --- | --- |
| Windows 开发环境 | x64；交互验收需 Windows 11 |
| Visual Studio 2026 | MSVC x64 工具、C++ 标准库、MSBuild 与 WinUI 项目所需组件 |
| Windows SDK | CI 使用 `10.0.26100.0`；本地脚本默认 `10.0.28000.0`，需安装实际选择的版本 |
| CMake / CTest | CMake 至少 3.28；需能识别所选生成器，本地脚本使用 NMake |
| Git | 支持 submodule；保留固定依赖版本与换行规则 |
| Python | Python 3，用于仓库测试与 CI 检查 |
| 7-Zip | `7z.exe` 在 PATH 中，用于解压官方 librime 运行时 |
| NuGet 源 | 默认 nuget.org，可通过 `ZILIU_NUGET_SOURCES` 指定已准备的离线源 |
| Inno Setup 6 | 仅生成安装器时需要 `ISCC.exe` |

WinUI 项目固定使用 `Microsoft.WindowsAppSDK` 2.2.0 和 `Microsoft.Windows.CppWinRT` 3.0.260715.1。构建会下载公共依赖并恢复 NuGet 包，不会自动安装开发工具。

## 从干净 checkout 构建

以下命令使用 **cmd.exe**，运行位置为仓库根目录。新 checkout 采用 Windows CRLF 的 Rime Ice 数据和 `.gitattributes` 指定的 LF 第一方覆盖层：

```bat
git -c core.autocrlf=true clone --recurse-submodules https://github.com/matsuokajuri/ziliu-cloud-build.git
cd ziliu-cloud-build
git config core.autocrlf true
git submodule status
set ZILIU_WINDOWS_SDK_VERSION=10.0.26100.0
powershell -NoProfile -File scripts\fetch-librime-runtime.ps1
scripts\build-local.cmd Release
```

`build-local.cmd` 会配置、构建常规目标（包括 WinUI 3 设置），再运行 CTest。输出位于 `build/local-x64-Release/bin`，测试状态位于该构建目录中。可用 `Debug` 替换 `Release`。构建不会安装或注册输入法。

`ZILIU_DEPENDENCY_ROOT` 默认是当前仓库；仅在复用已准备的固定依赖与运行时缓存时设置它。缺少真实 librime 运行时可能退回确定性 stub 引擎，不能把这种结果当成真实 Rime 验证。`CMakePresets.json` 的默认预设关闭 `ZILIU_ENABLE_RIME`，也不等价于上述完整构建路径。

## 固定数据的验证边界

源码指纹依赖 Rime Ice 的 CRLF 与第一方覆盖层的 LF 字节。编译缓存指纹还依赖 **ja-JP 用户 locale（LCID 1041）**：固定的 Windows Boost.Regex 二进制在生成英文 prism 时使用受 locale 影响的字节大小写转换。

CI 在一次性 runner 上选择并检查此 locale。不同 locale 的本地环境可能无法通过冻结缓存测试；请用相同条件的隔离开发环境验证，勿为通过测试更改产品设置、重写固定哈希或绕过检查。

额外运行 Python 测试：

```bat
python -B -m unittest discover -s tests -p "test_*.py" -v
```

完整门禁以 [CI 脚本](../scripts/ci/windows-build.ps1)和 [CTest 校验脚本](../scripts/ci/assert-ctest.py)为准，不能用跳过、缺失测试或 stub 输出代替真实测试通过。

## 生成未签名测试包

先完成 Release 构建，再在 **PowerShell** 的仓库根目录执行：

```powershell
.\scripts\package-alpha.ps1 -DependencyRoot .
```

默认输入为 `build/local-x64-Release/bin`，输出为 `build/release`。打包脚本会整理允许的程序文件、资源、许可与校验信息。若需要安装器，在已安装 Inno Setup 6 的环境执行：

```powershell
$packageZip = '.\build\release\Ziliu-0.1.0-alpha.20261005.997f466-win11-x64-unsigned-test-only.zip'
$packageHash = (Get-FileHash -LiteralPath $packageZip -Algorithm SHA256).Hash
.\scripts\package-alpha-installer.ps1 -PackageZip $packageZip -ExpectedSha256 $packageHash
```

当前产物仍为 unsigned test-only。安装与卸载流程及限制见[试用指南](getting-started.md)。

## 公共 CI 做了什么

[Windows build and strict tests](https://github.com/matsuokajuri/ziliu-cloud-build/actions/workflows/build.yml) 在 `main` push、pull request 或手动触发时运行，使用 `windows-2025-vs2026` runner：

1. 检出固定 submodule，确认 SDK、工具链与冻结测试 locale。
2. 校验官方 librime 压缩包 SHA-256，恢复固定 NuGet 包，构建全部常规 Release 目标。
3. 运行 CTest 与 Python 单元测试，检查 CTest 计划与结果一致，失败、跳过或缺失测试均阻止通过。
4. 生成未签名 ZIP 和安装器；检查 ZIP 路径、包清单及 x64 PE 文件头，不进行安装或注册。
5. 保留有大小上限的构建证据与包，当前期限为 1 天、上限 400 MiB；仍受账户 Actions 配额约束。

首页 badge 查询 `main` 的真实工作流状态。每次运行只能证明其对应提交和实际执行范围，不能替代 Windows 11 TSF、应用兼容性、焦点 / 隐私字段、DPI、安装生命周期和重启的交互验收。

## 依赖可追溯性

| 依赖 | 固定来源 |
| --- | --- |
| librime 源码 | `33e78140250125871856cdc5b42ddc6a5fcd3cd4` |
| Rime Ice | `b681a34f788795034b3b288830f4861980bc8b0d` |
| 官方 librime 运行时 | 1.17.0，`rime-33e7814-Windows-msvc-x64.7z` |

运行时压缩包 SHA-256：

```text
7478c7caa4ff6b37de86daba1f7ce4a994a4f5ba24872a820fb2b3a9b01fed15
```

流水线使用经过校验的上游二进制，不重编译 librime。更新依赖前，请阅读[第三方声明](../THIRD_PARTY_NOTICES.md)和[数据来源规则](../data/ziliu/README.md)，保留出处、许可、版本与回归证据。
