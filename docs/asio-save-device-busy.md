# 保存 ASIO 配置时遇到 Windows 输出占用

2026-10-13：用户报告，Aggregate Audio 已选中并分配设备，Windows WASAPI 随后使用该输出，在配置面板保存后 Mini DAW 卡住并退出。

## 日志与代码证据

- Windows Application 日志中，2026-10-13 15:29、15:43、15:47 的旧版 MiniDAW-next.exe 记录 `ucrtbase.dll`、异常 `0xc0000409`、异常数据 `7`（fatal application exit）；20:58 还有 MiniDAW.exe 的无响应报告。
- WER 归档只剩 Report.wer，没有保留可读取的转储。这些历史记录无法单独证明是本次操作，也不能据此给出完整崩溃堆栈。
- 配置程序保存后发出设置事件，ASIO monitor 请求宿主 reset。重新初始化遇到占用时，驱动原先在持有 `api_mutex_` 的调用中执行同步 `MessageBoxW`。
- 同步窗口运行嵌套消息循环。JUCE 自己执行 ASIO reset 时，Mini DAW 的 `opening` 标志并未置位；窗口内再次处理宿主恢复定时器，可能在初始化未返回时销毁相同设备。析构重新获取同一锁，存在死锁或异常终止路径。这是代码检查发现的具体缺陷，尚未在硬件上复现验证。

## 改动

- Aggregate Audio v21：驱动端移除占用错误的同步弹窗，使用 ASIO 错误返回值和 `getErrorMessage` 交给宿主处理。配置程序自己的占用提示继续保留。
- 驱动刷新失败时清除活动声道绑定；磁盘配置保留。
- Mini DAW：驱动对象创建后先检查初始化错误，失败时释放对象并停止后续声道查询和 buffer 打开；防止重复进入 Host::start。占用错误显示中文说明，保留设备绑定和原有自动恢复机制。
- 新日志：`%LOCALAPPDATA%\AggregateASIO\driver-errors.log` 记录失败阶段、HRESULT、进程和线程；`%APPDATA%\MiniDAW\audio-errors.log` 记录宿主打开失败。超过约 1 MiB 时保留上一份日志。

## 验证范围

按用户要求，仅编译与打包，没有运行音频、界面、安装或自动测试。更新已有 ASIO 模拟测试的期望：初始化遇到占用应返回错误且不能弹出模态窗口；没有执行该测试。

用户可关闭软件后安装 v21，再重复原操作。修复预期是显示设备占用并保留配置；Windows 释放设备后由宿主重新尝试恢复。单实例设备的 KS/ASIO 与 WASAPI 同时使用仍受设备驱动限制。
