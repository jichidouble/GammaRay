# GammaRay MCP（Qt 运行时监测与验证）

本目录把 GammaRay 的 Launcher、Probe、Client 和 RemoteModel 封装成一个无界面的
[Model Context Protocol](https://modelcontextprotocol.io/) stdio 服务。它基于 GammaRay
`v3.4.0`，当前本机基线为 Qt `6.11.1`、MinGW `13.1.0`、CMake `3.30.5` 和 Ninja
`1.12.1`。

## 能力与边界

`gammaray-mcp` 复用原生 GammaRay 协议，不依赖操作系统级屏幕抓取或 OCR。对于 QWidget
应用，它可由目标进程的 Widget Inspector 直接渲染选中的控件或其顶层窗口，并通过 MCP
返回内存中的 PNG：

- 启动 Qt 程序、附加本地 PID，或连接已有 GammaRay Probe；
- 获取实时 QObject 树，并按稳定路径选择对象；
- 读取 QObject 属性、Qt 日志和 GammaRay 问题扫描结果；
- 按 QObject 路径抓取特定 QWidget，或抓取该控件所在的顶层窗口，以检查运行时 GUI 状态；
- 根据 warning/critical/fatal 数量及问题严重度输出可机器判定的运行时验收结果；
- 单个 MCP 服务进程同时维护一个目标会话，切换目标前需调用 `gammaray_disconnect`。

这是运行时语义监测，不替代单元测试、ASan、静态分析或性能采样。Probe 必须与目标的
Qt 次版本、编译器 ABI、架构和 debug/release 模式兼容。当前构建会生成
`qt6_11-GNU-x86_64` 与 `qt6_11-GNU-x86_64d` 两套 Probe。

## 构建与运行

在仓库根目录执行：

```powershell
.\scripts\build-qt611-mingw.ps1
```

完整安装可增加 `-Install`。只启动 stdio MCP 服务：

```powershell
.\scripts\run-mcp.ps1
```

要在本机验证 GUI 截图通道，先构建测试目标，再运行端到端脚本：

```powershell
cmake --build build-qt6.11.1-mingw --target gammaray-mcp-gui-test-target
.\scripts\test-mcp-gui-capture.ps1
```

`run-mcp.ps1` 不向 stdout 输出辅助文本；stdout 专用于一行一条的 UTF-8 JSON-RPC
消息，运行日志只进入 stderr。服务实现 MCP `2025-11-25`，并兼容
`2025-06-18`、`2024-11-05` 客户端初始化。

将 [mcp-config.example.json](mcp-config.example.json) 的服务条目合并到支持
`mcpServers` 的客户端配置即可。其他客户端使用相同 command/args：命令为
`powershell.exe`，参数指向 `scripts/run-mcp.ps1`。

## 工具

| 工具 | 作用 |
| --- | --- |
| `gammaray_status` | 查看 GammaRay/Qt 版本、Probe 和会话状态 |
| `gammaray_launch` | 直接启动可执行文件并注入 Probe（不经过 shell） |
| `gammaray_attach` | 按 PID 注入并附加 |
| `gammaray_connect` | 连接已有 `tcp://` Probe |
| `gammaray_disconnect` | 断开客户端，不终止目标程序 |
| `gammaray_list_objects` | 读取有深度/数量上限的 QObject 树快照 |
| `gammaray_get_properties` | 用对象路径读取实时属性 |
| `gammaray_get_messages` | 读取目标 Qt 消息 |
| `gammaray_run_diagnostics` | 运行绑定、连接、线程亲和性等问题检查器 |
| `gammaray_validate_runtime` | 按显式阈值输出 `passed` 与失败原因 |
| `gammaray_grab_widget` | 将指定 `objectPath` 的 QWidget 作为 `image/png` MCP 内容返回 |
| `gammaray_grab_window` | 将指定 QWidget 所在顶层窗口作为 `image/png` MCP 内容返回 |

截图工具的输入是 `gammaray_list_objects` 返回的 `objectPath`。它们的默认输出上限是
1920×1080，可通过 `maxWidth` 和 `maxHeight` 调整（只会等比缩小）；返回内容包括文本
元数据以及 MCP `image` 项，`mimeType` 为 `image/png`。`gammaray_grab_widget` 抓取目标
控件自身，`gammaray_grab_window` 抓取包含该控件的顶层 QWidget。二者需要目标加载 Qt
Widgets；对 Qt Quick、非 QWidget QObject、对象已销毁或 Widget Inspector 不可用的情况会
返回可诊断的工具错误。

推荐流程：`gammaray_status` → `gammaray_launch`/`attach`/`connect` →
`gammaray_list_objects` → `gammaray_grab_widget`/`gammaray_grab_window` →
属性/日志/诊断/验收 → `gammaray_disconnect`。

## 安全

启动和注入属于高权限本地操作。MCP host 应在调用 `launch`、`attach`、`connect`
前显示参数并取得用户确认。服务不执行 shell 字符串，但被启动的程序及传入环境变量仍应
视为不可信输入。远程 Probe 建议仅监听可信网络；本服务启动的 Probe 默认绑定
`127.0.0.1`。

GUI 截图可能包含用户数据、令牌、文件路径或其他敏感内容。MCP host 应像处理日志和对象属性
一样，先向用户展示待抓取的目标和参数，并避免把返回的 PNG 发送到不受信任的外部服务。

GammaRay 及本 MCP 可执行文件按 `GPL-2.0-or-later` 提供；商业闭源集成应向 KDAB
确认相应许可。
