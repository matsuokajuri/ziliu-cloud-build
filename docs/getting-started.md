# 开始试用字流

[← 项目首页](../README.md) · [构建与开发](development.md) · [反馈问题](../CONTRIBUTING.md)

字流目前面向 **Windows 11 原生 x64**。ARM64、32 位应用兼容性和其他操作系统不在当前测试包的支持承诺内。测试包未签名，交互与安装生命周期仍待验收；请在隔离测试环境中评估。

## 获取方式

截至 2026-10-06，本仓库的 [Releases 页面](https://github.com/matsuokajuri/ziliu-cloud-build/releases)尚无正式发布条目。不要把源码 ZIP 当成可直接安装的软件。

当前可用入口是[自行构建](development.md)，或查看 [Windows build and strict tests](https://github.com/matsuokajuri/ziliu-cloud-build/actions/workflows/build.yml) 工作流的测试产物：

1. 打开对应 `main` 提交的成功运行，确认提交 SHA 和运行结果。
2. 如果产物尚未过期，登录 GitHub，在运行页面底部下载 `windows-x64-<commit SHA>` artifact。
3. 解压 artifact，在 `packages` 目录查看未签名 ZIP、安装器及相应 SHA-256 文件。ZIP 名称包含 `unsigned-test-only`。
4. 先阅读包内的 `UNSIGNED-TEST-ONLY.txt` 和安装说明，再决定是否在测试环境安装。

产物当前只保留 **1 天**，总大小上限为 400 MiB，可能已经过期。失败运行的证据 artifact 不代表其中有可用安装包；若没有包，请从源码构建或等待后续发布。

## 安装前检查

- 使用 Windows 11 x64 和 64 位 PowerShell。
- 注册输入法需要管理员权限；应由实际使用字流的管理员账户安装。通过另一管理员账户凭据为标准用户安装，当前 alpha 安装器不支持。
- 将下载文件的 SHA-256 与同次构建提供的校验文件核对。哈希用于检测损坏，不证明发布者身份，也不等同于代码签名。
- Windows SmartScreen 可能提示未知发布者或阻止运行。不要全局关闭 SmartScreen。

在 PowerShell 中查看文件哈希：

```powershell
Get-FileHash -Algorithm SHA256 -LiteralPath .\Ziliu-0.1.0-alpha.20261005.997f466-win11-x64-unsigned-test-only.zip
```

ZIP 解压后的根目录包含 `Install-Ziliu.ps1`。以下命令只验证包内容，不安装或注册输入法：

```powershell
powershell -NoProfile -File .\Install-Ziliu.ps1 -VerifyOnly
```

## 测试安装与卸载

通过包内安装器安装时，先阅读其提示。使用 ZIP 时，在解压后的包根目录，打开实际使用字流账户的管理员 64 位 PowerShell：

```powershell
powershell -NoProfile -File .\Install-Ziliu.ps1
```

若系统策略阻止脚本，先检查脚本与系统策略；本指南不要求更改全局执行策略。安装完成后注销登录或重启，重新打开应用，在 Windows 输入法切换器中选择字流；输入法列表以系统实际注册结果为准。

程序文件位于 `%ProgramFiles%\Ziliu\<版本>`；用户设置、主题和 Rime 数据位于 `%LOCALAPPDATA%\Ziliu`。同一 alpha 版本目录不会被覆盖。升级后，已加载的输入法 DLL 与 Broker 仍可能是旧版本，需注销或重启后再评估。

安装器安装的版本通过 Windows“已安装的应用”卸载。ZIP 安装的版本应从**已安装的版本目录**运行其 `Uninstall-Ziliu.ps1`，使用管理员 64 位 PowerShell。卸载保留用户数据；若 DLL 正被应用占用，重启后再重试卸载，勿手动删除版本目录。

这些是现有脚本的预期流程；CI 没有执行真实安装、升级、卸载或重启验收。

## 如何反馈

请记录包版本、源提交 SHA、Windows 版本、目标应用及最小操作步骤。输入异常可用“今天测试中文输入”等合成文字复现；不要上传个人词库、真实聊天、账号信息或包含私人内容的桌面截图。

进入 [Issues](https://github.com/matsuokajuri/ziliu-cloud-build/issues) 搜索已有问题，再按[贡献指南](../CONTRIBUTING.md)提交复现信息。
