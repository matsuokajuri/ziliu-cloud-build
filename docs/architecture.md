# 架构与数据流

[← 项目首页](../README.md) · [构建与开发](development.md) · [路线图](roadmap.md)

![字流进程与模块示意](assets/architecture.svg)

图示描述当前公开源码的常规输入路径，不包含实验模型或云端推理服务。

## 从按键到文字

1. Windows 在应用进程中加载 `ZiliuTIP.dll`。TSF 输入服务处理按键、组合串、光标与提交，并识别应用报告的输入字段类型。
2. TSF 通过本机命名管道与独立 `ZiliuBroker.exe` 通信，发送输入操作并取得会话状态和候选。
3. Broker 管理会话，以 librime 运行时和雾凇拼音数据生成结果；正常字段可使用 Rime 用户学习。
4. 原生候选栏呈现组合串与候选，选词经 TSF 提交给应用。
5. `ZiliuSettings.exe` 提供 WinUI 3 设置、主题管理与快捷菜单，配置保存于本机，供输入服务与 Broker 使用。

## 模块入口

| 目录 | 职责 | 阅读入口 |
| --- | --- | --- |
| `src/tsf` | TSF 生命周期、按键与组合串、字段分类、提交 | [text_service.cpp](../src/tsf/src/text_service.cpp) |
| `src/broker` | Broker 进程、Rime 适配与资源读取 | [broker_main.cpp](../src/broker/src/broker_main.cpp) · [rime_engine.cpp](../src/broker/src/rime_engine.cpp) |
| `src/ipc` | 本机命名管道客户端与服务端 | [pipe_client.cpp](../src/ipc/src/pipe_client.cpp) · [pipe_server.cpp](../src/ipc/src/pipe_server.cpp) |
| `src/core` | 输入会话、协议、设置、主题与实验请求约束 | [session_host.cpp](../src/core/src/session_host.cpp) |
| `src/ui` | 原生候选栏、坐标与布局、主题绘制 | [candidate_window.cpp](../src/ui/src/candidate_window.cpp) |
| `src/settings` | WinUI 3 设置、主题预览与导入 | [MainWindow.xaml](../src/settings/MainWindow.xaml) |
| `tools/register` | TSF 注册与卸载工具 | [register_main.cpp](../tools/register/register_main.cpp) |
| `data/ziliu` | 相对固定雾凇数据的项目覆盖层 | [数据来源](../data/ziliu/README.md) |
| `tests` | 合成测试、输入字段测试页与实验诊断数据 | [CTest 目标](../tests/CMakeLists.txt) |

## 本地数据与字段处理

程序、依赖和只读资源随版本包安装。用户设置、主题、Rime 用户数据分别保存在 `%LOCALAPPDATA%\Ziliu` 下，个人词库不进入 Git。

公开代码包含普通、受限、阻止三类字段处理。密码 / PIN 与被 TSF 禁用的上下文绕过输入服务；私密或未知字段采用受限路径以抑制学习与持久化。分类依赖宿主应用提供的 InputScope 元数据，它是提示信息，不是安全边界；代码与单元测试不能证明所有应用都正确报告字段或所有交互隐私场景都已验收。相关实现见 [input_privacy.h](../src/tsf/src/input_privacy.h) 和[受限 Rime 方案](../data/ziliu/ziliu_private.schema.yaml)。

## 常规路径与实验路径

上下文请求绑定、字典 epoch 与合成排序评测代码用于研究候选排序的可靠性。当前常规 Broker 构建明确关闭 `ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST`，这些诊断机制不代表已启用的 AI 输入功能，见 [Broker 构建定义](../src/broker/CMakeLists.txt)。

公开[合成上下文样例](../tests/data/contextual-ranking-pilot.README.md)是开发诊断材料，既不是用户输入历史，也不是最终基准或产品验收集。模型接入与深层融合仍需独立评测、数据授权、延迟 / 隐私验证和安全回退，参见[路线图](roadmap.md)。
