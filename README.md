# Aggregate Audio

Windows 10/11 x64 多设备聚合 ASIO 驱动原型。将不同音频设备的输入、输出声道组合成一个 ASIO 设备，供 64 位 DAW 使用。

项目仓库：https://github.com/wanan522z/AggregateAudio-asio

powered by WanAn522z

## 功能

- 按物理设备展示 Kernel Streaming（KS）输入、输出引脚，逐声道选择参与聚合的通道。
- 分别设置每台设备的 KS 缓冲，同时设置全局 ASIO 主机缓冲和采样率。
- 直接通过 `KsCreatePin` 打开设备；支持 WaveRT 映射循环缓冲和硬件通知，其他设备使用异步 KS 数据包。
- 优先通过 KS 格式交集采用驱动返回的实际容器、有效位数和声道布局，支持 24-in-32、20-in-24 等 PCM；旧驱动保留格式回退。
- 设备组内按通道名称自然排序，忽略英文大小写，数字按数值排列，同一端点的声道保持相邻。
- 每个引脚使用独立工作线程和预分配的无锁环形缓冲；通过线性重采样和缓冲占用反馈补偿设备时钟差异。
- 支持自动检测设备缓冲、设备故障恢复请求、托盘入口、会话日志和只读诊断。
- 勾选、保存、探测及 ASIO 打开时检查 KS 引脚占用；检测到占用会提示释放设备，并停止格式和缓冲重试。
- 配置窗口支持 DPI 缩放、键盘导航和自定义滚动条；Settings 页面提供仓库链接与作者署名。

## 安装与使用

使用打包生成的 `dist\AggregateAudio-v19-x64-Setup.exe`。安装程序包含 Microsoft x64 Visual C++ 运行库，注册 64 位 ASIO 驱动，并创建快捷方式和卸载入口。ASIO 注册名、宿主可见描述和 COM 显示名统一为 `Aggregate Audio`。

1. 关闭 DAW 和配置窗口，运行 Setup 安装。
2. 打开 Aggregate Audio 配置程序，展开设备并勾选需要的输入、输出声道。
3. 选中声道行，设置该设备的 KS 缓冲；设置全局 ASIO 主机缓冲和采样率。
4. 保存设置，在 DAW 中选择 Aggregate Audio。

配置保存到 `%LOCALAPPDATA%\AggregateASIO\settings.ini`，安装包不包含本机音频配置。保存配置会向正在运行的主机请求 ASIO 重置；如果 DAW 忽略请求，请在 DAW 中重新打开音频设备。

“自动检测设备缓冲”会逐个打开选中设备的引脚，向输出发送数字静音并丢弃采集数据，选择短时探测中可工作的较小缓冲。它只是初始建议，不能代替实际 DAW 负载和物理回环延迟测试。

设备占用检查读取 KS 引脚实例数，不创建测试音频流。实例容量已满的引脚会提示“设备被占用，请释放后再试”；本驱动和检测程序也会互相保护，避免同时打开同一引脚。自动检测需要先停止 DAW 并释放设备。缺少全局实例数属性时会继续查询局部实例数，兼容 E2x2 OTG 等驱动；若设备驱动完全不支持实例数查询，则在打开时处理其返回的占用错误。用户态程序无法拦截设备驱动自身的内核崩溃。此次 USB 驱动崩溃分析与兼容修正见 [修复记录](docs/device-busy-fix.md)。

ASIO 启动后配置程序驻留托盘。点击托盘图标可以恢复窗口；ASIO 运行时关闭窗口会保留托盘入口。Settings 页面展示本次会话日志，返回设备页会保留尚未保存的编辑。

## 编译

需要 Visual Studio 2022 C++ 工具链、Windows 10/11 SDK 和 CMake 3.24+，目标架构为 x64：

```powershell
cmake -S . -B build\device-groups-v3 -G "Visual Studio 17 2022" -A x64
cmake --build build\device-groups-v3 --config Release
```

主要输出位于 `build\device-groups-v3\Release`：

- `AggregateAudio.dll`：ASIO 驱动。
- `AggregateAudioConfig.exe`：配置程序，与驱动放在同一目录。
- `AggregateAudioDiagnostics.exe`：只读诊断工具。

手动注册需在输出目录打开管理员 PowerShell：

```powershell
regsvr32 .\AggregateAudio.dll
# 取消注册
regsvr32 /u .\AggregateAudio.dll
```

注册会写入 ASIO 和 COM 注册表项。通过 Setup 安装时不需要手动注册。

## 验证与诊断

项目包含模拟设备测试，覆盖 ASIO 输出交接、KS 采集和播放队列、时钟、缓冲恢复、设备目录、配置迁移、故障恢复、界面和托盘生命周期。按需运行：

```powershell
ctest --test-dir build\device-groups-v3 -C Release --output-on-failure
```

这些测试不能证明真实硬件的声音质量或物理延迟。界面修改建议手动检查设备列表和 Settings 日志的滚轮、横向滚动、滑块拖动，以及 100%、150%、200% 缩放。

诊断运行中的 DAW（参数为主机进程 ID 和观察秒数）：

```powershell
.\AggregateAudioDiagnostics.exe <host-process-id> 15
```

诊断包括回调间隔、静音和无效采样、各流的帧计数、溢出、欠载、恢复丢帧和延迟估计。驱动通过共享内存发布计数，不录制音频，不在音频线程写日志。延迟估计不包含 DAW 插件延迟，也不是物理回环测量。

开发构建中的 `catalog_inspect.exe --access E2x2` 可只读检查指定名称设备的全局/局部实例数、属性错误码及占用判断，不创建音频流。

`catalog_inspect.exe --formats Livejack` 只读查看原始格式范围和 48 kHz 格式交集。`ks_open_check.exe Livejack` 则明确创建停止状态的引脚以验证连接，不启动音频、不提交采集/播放数据，也不读写个人配置。此连接检查不能验证实际音频稳定性。v19 的硬件证据与测试范围见 [兼容修复记录](docs/ks-format-compatibility-v19.md)。

## 打包

打包入口为 `packaging\build-installer.ps1`，使用 Inno Setup 7.1.0 和 Microsoft 签名的 x64 运行库。脚本校验发布文件的 SHA256 和 x64 架构，再生成离线 Setup、校验文件和包清单。详细参数与隔离安装验证见 [打包说明](packaging/README.md)，用户说明见 [中文安装指南](packaging/INSTALL-zh-CN.txt)。

`build`、`dist` 和本机 IDE 配置不提交到 Git 仓库。

## 限制与许可

- 仅支持 Windows x64 和 64 位 ASIO 主机；设备是否能打开取决于其驱动实际接受的 KS 格式。
- 支持 64～2048 帧的二次幂 ASIO 缓冲及 32～384 kHz 常见采样率，设备实际格式可能使用接近的采样率。
- 不保证低于 10 ms 的单向延迟。较小缓冲、断连恢复和取消行为仍需在实际设备上验证。
- 设备故障会请求 DAW 重置驱动，具体是否重开由主机决定。
- 线性重采样可补偿时钟漂移，但不提供设备之间的字时钟同步。
- KS 工厂并不与 Windows 友好名称一一对应；相同名称不能证明是同一声道。
- 安装程序和产品二进制未签名。
- `third_party/asio-sdk` 使用其随附许可，提供 GPLv3 和专有许可选项。分发前请确认适用许可；详见 SDK 的许可文件及 `packaging/THIRD-PARTY-NOTICES.txt`。
