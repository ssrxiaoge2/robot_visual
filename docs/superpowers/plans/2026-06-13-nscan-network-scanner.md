# N-ScanHub 网络扫码枪硬件测试 实现计划

> **面向 AI 代理的工作者：** 按任务顺序实现并使用复选框（`- [ ]`）跟踪进度。每完成一个任务先编译验证，再进入下一任务。

**目标：** 在完全保留现有“扫码器调试”面板与 `DeviceManager::testScanner()` 行为的前提下，新增独立的“N-ScanHub SDK 调试”面板和可复用扫码类。`NScanScheduler` 提供带超时、重试和结构化返回值的同步 `scan()` 方法，用于验证扫码枪能够正常连接、触发并读取条码；测试 UI 通过调用适配层异步执行该同步方法，保证界面不阻塞。

**架构：** `NScanScheduler` 是纯同步 SDK 封装，自身不创建线程、不持有 UI 状态、不负责业务流程；每次 `scan()` 在当前调用线程内完成“按需连接/复用连接 → 发送现场已验证的 `41`（可通过 `ScanOptions::triggerCommand` 覆盖） → 限时读取 → 失败时断开重连 → 返回 `ScanResult`”。厂商 Demo 明确警告频繁连接/断开会导致读取失败，因此成功连接跨扫码调用复用，析构或显式 `shutdown()` 时释放。`DeviceManager` 持有 Scheduler，并复用单一 `QThread + NScanTestWorker` 调用同步接口；工作线程空闲时等待任务，程序退出时统一释放。`MainWindow` 只负责主动扫码按钮、状态和结果显示。后续其他模块可在自己的工作线程直接调用同一个同步接口。

**技术栈：** Qt 6（Widgets/Core）、CMake、N-ScanHub v1.0.05，Linux/Windows x64 优先。

**关键约束：**

- 旧扫码器 UI、`Config::scannerIP`、`testScanner()`、`scannerStatusChanged` 全部保持原样。
- 新面板使用独立控件、状态和信号，不复用旧扫码器指示灯，避免两套状态互相覆盖。
- `NScanScheduler::scan()` 是同步阻塞接口，但必须提供有限读取超时和有限重试次数。
- `NScanScheduler` 不创建线程；调用方负责确保不在 UI 线程直接调用同步 `scan()`。
- UI 测试路径由 `DeviceManager` 的常驻空闲工作线程调用 `scan()`，避免高频扫码反复创建和销毁系统线程。
- 成功连接应按厂商 Demo 建议跨扫码调用复用；发送、读取或连接异常后关闭状态不明的 socket，再重连重试。
- 本期只实现主动单次扫码测试，不做连接常驻、后台轮询和连续扫码。
- 首版不把扫码结果推进整线流程；硬件测试稳定后再单独设计业务接入。
- 代码增加必要的中文注释，重点解释同步接口约束、调用线程责任、重试语义、资源释放和原始数据解析。
- 新增控件如使用自定义颜色，深色/浅色样式必须统一纳入 `MainWindow::applyTheme(bool dark)`；不得只写死深色 QSS。

**当前已核对的 SDK 行为：**

- 网络连接：`nl_connectToService(char *serviceIp, int port, int *socket)`。
- 厂家 Demo 默认向数据端口 `30000` 发送 `01 54 04`；现场设备配置页及验证脚本确认本机启动读码指令为单字节 `41`。
- 结果读取：`nl_readFromSocket(socket, timeoutMs, buffer, &length)`。
- 网络关闭：Linux 头文件声明 `nl_CloseClientSocket(int *socket)`；实现时必须分别核对 Linux/Windows 头文件签名，不能照抄 demo 动态加载包装函数中的旧签名。
- 图像端口 `36520` 与 `nl_getNetImgData` 本期不使用。

---

## 文件清单

### 新增文件

| 文件 | 职责 |
|------|------|
| `src/nscanscheduler.h` | 声明 `ScanOptions`、`ScanResult` 和同步 `scan()` 接口 |
| `src/nscanscheduler.cpp` | 实现 SDK 连接、触发、限时读取、重试、解析和资源释放，不创建线程 |

### 修改文件

| 文件 | 职责变化 |
|------|----------|
| `CMakeLists.txt` | 加入新源码、SDK 头文件与平台库、Linux RPATH 和 Windows DLL 部署 |
| `src/devicemanager.h` | 持有 `NScanScheduler`，声明 UI 测试的异步请求和结果信号 |
| `src/devicemanager.cpp` | 实现临时 `NScanTestWorker/QThread` 调用适配、忙碌门禁和日志转发 |
| `src/mainwindow.h` | 声明新面板控件、主动扫码槽和 UI 状态更新方法 |
| `src/mainwindow.cpp` | 新增独立 N-ScanHub 面板、信号连接和深浅主题样式；旧面板不改 |
| `changelog/CHANGELOG.md` | 记录网络扫码枪硬件测试能力 |
| `README.md` | 可选：补充动态库、端口及部署说明 |

### 不修改文件

- `src/mainwindow.ui`：当前界面由 `MainWindow::initUI()` 动态构建，新面板沿用该模式。
- `src/themeswitch.h/.cpp`：`ThemeSwitch` 只发出主题切换信号；新增控件颜色仍由 `MainWindow::applyTheme()` 统一处理。
- 旧扫码器面板、`DeviceManager::testScanner()`、`Config::scannerIP` 相关代码保持原样。

## 阶段总览

| 阶段 | 目标 | 独立验证出口 |
|------|------|--------------|
| 阶段一 | 配置 SDK 构建和运行时依赖 | CMake configure/build 成功，动态库可解析 |
| 阶段二 | 实现同步扫码类、超时、重试和结构化结果 | 同步 harness 验证成功/超时/重试/错误和 socket 释放 |
| 阶段三 | DeviceManager 增加非阻塞 UI 调用适配 | 日志/结果信号可验证、UI 主线程不阻塞且旧扫码逻辑不变 |
| 阶段四 | 新增 UI 和主题适配 | 无硬件验证按钮状态、UI 不阻塞及深浅主题 |
| 阶段五 | 集成、真机和文档验收 | 完成动态库、真机扫码、稳定性及回归清单 |

---

## 阶段一：配置 N-ScanHub SDK 构建与运行时依赖

**文件：**
- 修改：`CMakeLists.txt`

- [x] **步骤 1.1：配置 Linux x64/x86 SDK**

定义 N-ScanHub 根目录和公共头文件目录，根据 `CMAKE_SIZEOF_VOID_P` 选择 `x64` 或 `x32`：

```cmake
set(NSCAN_SDK_ROOT "${CMAKE_SOURCE_DIR}/3rd/NScanHubSdk")

if(UNIX AND NOT APPLE)
    set(NSCAN_PLATFORM_ROOT "${NSCAN_SDK_ROOT}/N-ScanHubForLinux_v1.0.05")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(NSCAN_ARCH x64)
    else()
        set(NSCAN_ARCH x32)
    endif()

    target_include_directories(wh-robot-visual PRIVATE
        "${NSCAN_PLATFORM_ROOT}/include"
    )
    target_link_libraries(wh-robot-visual PRIVATE
        "${NSCAN_PLATFORM_ROOT}/lib/${NSCAN_ARCH}/N-ScanHub.so"
    )
endif()
```

使用 `set_property(TARGET ... APPEND PROPERTY BUILD_RPATH ...)` 和 `INSTALL_RPATH` 追加扫码 SDK 与 `libusb` 路径，不能覆盖现有 Orbbec RPATH。

确认 SDK 依赖名为 `libusb-1.0.so.0`。若 SDK 目录只有 `libusb-1.0.so`，构建/部署阶段创建正确文件名的副本或软链接；不要假设目标机系统库必然兼容。

- [x] **步骤 1.2：配置 Windows MSVC x64/x86 SDK**

Windows 根目录：

```cmake
${NSCAN_SDK_ROOT}/N-ScanHubForWindows_v1.0.05/N-ScanHubForWindows
```

按位数选择 `lib/x64` 或 `lib/x86`，链接 `N-ScanHub.lib`。使用 `add_custom_command(TARGET ... POST_BUILD)` 将同目录的 `N-ScanHub.dll` 复制到 `$<TARGET_FILE_DIR:wh-robot-visual>`。

MinGW 没有匹配的 import library，本期明确为“不支持或仅做条件跳过”，不能尝试把 MSVC `.lib` 直接链接到 MinGW。

- [x] **步骤 1.3：避免非支持平台编译失败**

通过平台条件定义 `NSCAN_SDK_AVAILABLE`：

```cmake
target_compile_definitions(wh-robot-visual PRIVATE NSCAN_SDK_AVAILABLE=1)
```

仅在成功配置 SDK 的平台定义。非支持平台仍允许 UI 工程编译，Scheduler 返回“当前平台未配置 N-ScanHub SDK”的明确错误。

- [x] **步骤 1.4：构建配置验证**

执行 CMake configure，确认：

- Linux x64 选择 `lib/x64/N-ScanHub.so`。
- 原有 Huayan、Orbbec 链接配置未被覆盖。
- 构建目录中可解析 `N-ScanHub.so` 及 `libusb-1.0.so.0`。
- Windows MSVC 构建目录能获得 `N-ScanHub.dll`。

- [x] **阶段一验证：构建配置与动态库检查**

不依赖 UI 和真机，完成 CMake configure/build；Linux 直接对 SDK 的 `N-ScanHub.so` 执行 `ldd`，确认其 `libusb-1.0.so.0` 依赖可解析，Windows MSVC 确认 import library 可链接且 DLL 复制规则生效。由于阶段一尚未引用 SDK 符号，不要求主程序的 `ldd` 已出现 `N-ScanHub.so`。

**阶段完成标准：** 后续源码和 UI 尚未接入时，也能独立证明 SDK 构建及运行时路径配置正确。

---

## 阶段二：实现同步扫码类与 SDK 网络封装

**文件：**
- 创建：`src/nscanscheduler.h`
- 创建：`src/nscanscheduler.cpp`
- 修改：`CMakeLists.txt`

- [x] **步骤 2.1：创建扫码类并加入工程**

创建 `src/nscanscheduler.h/.cpp` 并加入 `PROJECT_SOURCES`。扫码类不继承 `QObject`，不包含信号、槽或线程生命周期；它只提供普通同步 C++ 接口。

- [x] **步骤 2.2：声明同步请求和结构化结果**

建议接口：

```cpp
class NScanScheduler
{
public:
    struct ScanOptions {
        QString ip;
        quint16 port = 30000;
        int timeoutMs = 2000;
        int maxAttempts = 3;
        int retryIntervalMs = 500;
    };

    struct ScanResult {
        enum class Status {
            Success,
            Timeout,
            ConnectError,
            SendError,
            ReadError,
            InvalidConfig,
            SdkUnavailable
        };

        Status status = Status::InvalidConfig;
        QString code;
        QByteArray rawData;
        QString errorMessage;
        int attempts = 0;

        bool isSuccess() const { return status == Status::Success; }
    };

    ScanResult scan(const ScanOptions &options);
};
```

`scan()` 为同步方法：返回前完成全部 SDK 调用和资源释放。接口注释必须明确“禁止在 UI 线程直接调用”。

- [x] **步骤 2.3：实现参数校验和平台降级**

校验 IP、端口、`timeoutMs > 0`、`maxAttempts >= 1`、`retryIntervalMs >= 0`。未定义 `NSCAN_SDK_AVAILABLE` 时返回 `SdkUnavailable`，保证非支持平台仍可编译运行。

- [x] **步骤 2.4：实现单次尝试**

每次尝试严格执行：

```text
连接 IP:端口
→ 发送 `ScanOptions::triggerCommand`（默认单字节 41）
→ nl_readFromSocket 限时等待
→ 按 realLen 构造 QByteArray
→ 成功时保留连接；失败时关闭 socket
→ 返回本次状态
```

要求：

- 触发命令使用 `QByteArray` 原始字节及明确长度发送；默认单字节 `41`，允许调用方覆盖。
- 接收缓冲区首版至少 4096 字节。
- 不依赖 `\0` 结尾，只按 `realLen` 构造结果。
- Linux/Windows 分别按当前头文件签名调用 `nl_CloseClientSocket`。
- 使用局部 socket 和统一收尾路径；成功、超时或错误都必须关闭。

- [x] **步骤 2.5：实现有限重试**

`scan()` 最多执行 `maxAttempts` 次。仅 Timeout、ConnectError、SendError、ReadError 可重试；InvalidConfig 和 SdkUnavailable 直接返回。每次失败后先关闭 socket，再在当前调用线程等待 `retryIntervalMs`，然后重新连接和触发。等待可使用 `std::this_thread::sleep_for`，但不得创建新线程。

重试发生在调用 `scan()` 的当前线程中，扫码类自身不创建线程。`attempts` 返回实际尝试次数，最终错误保留最后一次失败原因。

- [x] **步骤 2.6：实现结果解析**

- 原始数据完整保存在 `rawData`。
- 显示值去除尾部 `\r`、`\n`、`\0`，不删除中间字节。
- 首选 UTF-8；无效 UTF-8 时回退 Latin-1。
- 空结果不算成功，归入 Timeout 或 ReadError，具体依据 SDK 返回码确定。
- 本期仅验证能否读到条码，不加入长度、前缀、数据库或业务校验。

- [x] **步骤 2.7：并发调用边界**

优先让 `scan()` 的 socket 和缓冲区全部使用局部变量，使不同实例互不共享状态。若 SDK 文档不能确认全局线程安全，在类内增加静态或共享 `QMutex` 串行化 SDK 调用；这不创建线程，只避免多个模块同时进入 SDK。

- [x] **阶段二验证：同步扫码类独立验证**

使用最小命令行 harness 或临时测试入口直接调用同步 `scan()`：

1. 非法参数返回 InvalidConfig，不调用 SDK。
2. 不可达 IP 按配置重试并返回 ConnectError/Timeout，`attempts` 正确。
3. 成功连接可复用；失败重连和 `shutdown()` 均正确关闭 socket，无句柄积累。
4. mock/fake 返回条码时得到 Success、code 和 rawData。
5. 验证扫码类自身未创建 `QThread/std::thread`。

**阶段完成标准：** 不依赖 DeviceManager 和 UI，即可验证完整同步流程“按需连接、触发、限时读取、失败重连、资源释放、返回结果”，接口可直接提供给后续模块调用。

---

## 阶段三：DeviceManager 增加非阻塞测试调用适配

**文件：**
- 修改：`src/devicemanager.h`
- 修改：`src/devicemanager.cpp`

- [x] **步骤 3.1：持有同步扫码类并提供 getter**

`DeviceManager` 持有 `NScanScheduler`，供后续模块获取同步接口：

```cpp
NScanScheduler *nscanScheduler() const;
```

根据所有权模式使用普通成员或 `std::unique_ptr`。`NScanScheduler` 不继承 `QObject` 时，不使用 QObject parent 管理。

- [x] **步骤 3.2：声明 UI 测试异步入口**

新增仅供测试面板使用的接口：

```cpp
void startNScanTest(const NScanScheduler::ScanOptions &options);
bool nscanTestRunning() const;
```

新增独立信号，不复用旧 `scannerStatusChanged`：

```cpp
void nscanTestStarted();
void nscanTestFinished(const NScanScheduler::ScanResult &result);
void nscanTestLog(const QString &message);
```

为跨线程传递 `ScanResult` 注册 Qt metatype，或由 DeviceManager 将结果拆成 Qt 内置信号参数。

- [x] **步骤 3.3：在 devicemanager.cpp 内实现临时 NScanTestWorker**

`NScanTestWorker` 只是 UI 调用适配器，不属于 `NScanScheduler`。它在复用工作线程中调用：

```cpp
const auto result = scheduler->scan(options);
```

然后发出完成信号。线程生命周期：

```text
startNScanTest
→ busy 门禁
→ 创建临时 QThread + NScanTestWorker
→ thread.started 执行同步 scan()
→ Worker 返回 ScanResult
→ DeviceManager 转发结果和日志
→ Worker deleteLater / thread.quit
→ thread.finished / thread.deleteLater
→ 清除 busy，允许下一次测试
```

正常 UI 路径禁止 `wait()`；仅应用退出且测试尚未结束时允许按“单次超时 × 最大尝试次数 + 重试间隔 + 安全余量”有界等待。

- [x] **步骤 3.4：实现忙碌门禁和完整状态日志**

`m_nscanTestRunning` 防止连续点击创建多个线程。日志至少覆盖：开始、当前配置、最终状态、实际尝试次数、条码长度、原始十六进制摘要和错误信息。不要在日志中假装每次重试都由 UI 发起。

- [x] **步骤 3.5：确认旧逻辑零改动**

- `Config::scannerIP` 默认值和赋值逻辑保持不变。
- `DeviceManager::testScanner()` 仍探测旧的 `scannerIP:8080`。
- `scannerStatusChanged` 仍只服务旧 `m_indScanner`。
- `applyConfig()` 不自动运行 N-ScanHub 扫码。

- [x] **阶段三验证：管理层异步适配与旧逻辑回归**

1. 直接调用 `nscanScheduler()->scan()` 仍是同步接口，且 Scheduler 内无线程代码。
2. 调用 `startNScanTest()` 时主事件循环持续响应。
3. 同一时间只存在一个临时测试线程。
4. 成功、超时、重试耗尽和 SDK 错误都能通过 `nscanTestFinished` 返回。
5. 任务完成后 busy 清零，工作线程继续空闲等待，无 `QThread: Destroyed while thread is still running`。
6. 旧 `testScanner()` 和旧状态信号行为不变。

**阶段完成标准：** DeviceManager 完成“同步扫码能力 → UI 非阻塞测试调用”的适配，同时同步接口仍可直接供其他后台模块调用。

---

## 阶段四：新增完整 N-ScanHub SDK 验证面板

**文件：**
- 修改：`src/mainwindow.h`
- 修改：`src/mainwindow.cpp`

- [x] **步骤 4.1：声明验证面板控件**

新增独立成员，统一使用 `m_nscan...` 命名：

```cpp
QLineEdit       *m_nscanIpEdit;
QLineEdit       *m_nscanPortEdit;
QLineEdit       *m_nscanTimeoutEdit;
QLineEdit       *m_nscanAttemptsEdit;
QLineEdit       *m_nscanRetryIntervalEdit;
QPushButton     *m_nscanTriggerBtn;
QPushButton     *m_nscanClearBtn;
DeviceIndicator *m_nscanIndicator;
QLineEdit       *m_nscanResultEdit;
QLabel          *m_nscanAttemptsLabel;
QLabel          *m_nscanRawLabel;
int              m_nscanSuccessCount = 0;
```

默认端口 `30000`、单次超时 `2000 ms`、最大尝试 `3` 次、重试间隔 `500 ms`（与厂家 Demo 重连等待一致）。

- [x] **步骤 4.2：构建独立验证面板**

新增 `initNScanPanel(QVBoxLayout *leftPanel)`，放在旧 `initScannerPanel()` 之后，旧函数完全不改。面板标题为“N-ScanHub SDK 调试”，包含：

1. IP 和端口。
2. 单次超时、最大尝试次数、重试间隔。
3. “主动扫码”按钮和独立状态指示灯。
4. 最新条码、实际尝试次数、原始数据十六进制摘要。
5. 成功计数和清空结果。

结果框只读，并用 tooltip 显示完整长码；原始摘要不可撑宽左侧面板。

- [x] **步骤 4.3：主动扫码按钮调用 DeviceManager 测试入口**

点击后从 UI 组装 `NScanScheduler::ScanOptions`，调用 `DeviceManager::startNScanTest(options)`。MainWindow 不直接调用同步 `scan()`，也不创建线程、不调用任何 SDK 函数。

完整 UI 验证流程：

```text
点击主动扫码
→ 按钮禁用，显示“扫码中”
→ DeviceManager 复用工作线程调用同步 scan()
→ scan() 内连接、触发、限时读取并按配置重试
→ 返回 ScanResult
→ UI 显示条码或最终错误、实际尝试次数
→ 任务完成后按钮恢复，工作线程保持空闲
```

- [x] **步骤 4.4：处理结构化结果**

收到 `nscanTestFinished` 后：

- Success：显示条码，成功计数加一，状态指示为成功。
- Timeout：显示“扫码超时（已尝试 N 次）”，不计成功。
- ConnectError/SendError/ReadError：显示明确失败阶段。
- InvalidConfig/SdkUnavailable：显示配置或平台错误。
- 始终显示 `attempts`、错误信息和原始数据摘要。
- 清空按钮只清 UI 结果和计数，不影响旧扫码器面板。

- [x] **步骤 4.5：按钮门禁和异常恢复**

`nscanTestStarted` 时禁用主动扫码以及参数输入；任务完成后恢复，工作线程保持空闲等待下一次任务。快速重复点击不能创建并发任务。窗口关闭时由 DeviceManager 完成测试线程收尾。

- [x] **步骤 4.6：主题与尺寸适配**

优先依赖全局 QSS。所有新增有色控件（主动扫码按钮、状态文字、成功/超时/失败提示、原始数据标签）必须在 `MainWindow::applyTheme(bool dark)` 中同时定义深色和浅色样式；主题切换时立即刷新，不能残留上一主题颜色。`ThemeSwitch` 自身无需修改。

检查左侧 `QScrollArea` 最大宽度 `340` 下：

- IP、端口和参数控件不被挤压。
- 长条码和原始十六进制摘要不撑宽窗口。
- 与华沿 SDK、AGV 面板的行高、间距和按钮高度一致。

- [x] **阶段四验证：完整无硬件 UI 流程**

使用不可达 IP 和 fake transport 验证：

1. 点击“主动扫码”后按钮和参数立即禁用，任务结束后恢复。
2. 超时及失败期间窗口可拖动、主题可切换、旧面板可操作。
3. 内部重试期间 UI 始终只显示一个进行中的测试任务。
4. Success/Timeout/各类 Error 显示正确，实际尝试次数正确。
5. 清空按钮只清结果和计数。
6. 深色和浅色主题下所有新增控件清晰可读。
7. 左侧 340 px 滚动区域布局无截断。

**阶段完成标准：** 无需真机即可跑通“UI 发起 → 复用工作线程 → 同步 scan() → 超时/重试 → 结构化结果 → UI 恢复”的完整验证流程。

---

## 阶段五：集成、真机和文档验收

**文件：**
- 验证/必要时修改：`CMakeLists.txt`
- 验证/必要时修改：`src/nscanscheduler.h`、`src/nscanscheduler.cpp`
- 验证/必要时修改：`src/devicemanager.h`、`src/devicemanager.cpp`
- 验证/必要时修改：`src/mainwindow.h`、`src/mainwindow.cpp`
- 修改：`changelog/CHANGELOG.md`
- 可选修改：`README.md`

### 5.1 编译、离线和调用边界验证

- [ ] **步骤 5.1：全量编译**

从干净或现有构建目录重新 configure/build，确认 `ScanOptions/ScanResult`、Qt metatype、DeviceManager 常驻 Worker 信号和 UI 接线均编译通过。

- [ ] **步骤 5.2：动态库加载检查**

Linux 使用 `ldd` 检查生成程序：

```bash
ldd <build-dir>/wh-robot-visual | rg "N-ScanHub|libusb|not found"
```

预期 `N-ScanHub.so` 与 `libusb-1.0.so.0` 均有解析路径且无 `not found`。

- [ ] **步骤 5.3：同步接口独立验证**

通过 harness 或后台测试线程直接调用 `NScanScheduler::scan()`：

- 验证 timeout、maxAttempts、retryIntervalMs 生效。
- 验证结果包含 status、code、rawData、errorMessage、attempts。
- 验证成功连接被复用，失败时 socket 被关闭重连，`shutdown()` 后连接释放。
- 代码检索确认 `nscanscheduler.*` 不包含 `QThread/std::thread/QtConcurrent`。

- [ ] **步骤 5.4：UI 非阻塞离线验证**

使用不可达测试 IP 验证：

- 点击主动扫码后窗口仍可拖动、切换主题、操作旧面板。
- 内部同步 scan() 超时和重试期间，UI 主线程不等待。
- 任务完成后主动扫码按钮恢复，工作线程保持空闲等待下一次任务。
- 快速重复点击不会创建多个测试线程或 socket 任务。
- 关闭窗口不会出现 `QThread: Destroyed while thread is still running`。

- [ ] **步骤 5.5：最小可测试边界**

若直接 SDK 调用导致离线测试困难，在 `NScanScheduler` 内抽出薄的 SDK transport 函数表或接口，用 fake 验证成功、超时、连接失败、发送失败、读取失败、先失败后重试成功和重试耗尽。测试替身只替换 SDK I/O，不改变 `scan()` 的同步语义。

---

### 5.2 真机硬件验收

- [ ] **步骤 6.1：确认现场网络参数**

确认 PC 与扫码枪同网段，扫码枪数据服务端口为 `30000`，防火墙允许访问。记录设备型号和固件版本。

- [ ] **步骤 6.2：验证触发协议**

连接后默认发送现场已验证的单字节 `41`，并保留参数覆盖能力。记录：

- 成功响应格式。
- 无码时 SDK 返回码和长度。
- 是否包含 `\r\n`、协议头或校验字节。
- 一次触发是否可能返回多个条码。

若协议不同，只调整 `NScanScheduler` 内的触发/解析常量，不让设备协议细节进入 MainWindow 或 DeviceManager。

- [ ] **步骤 6.3：稳定性测试**

完成以下验收：

1. 连续主动扫码 20 次，每次 UI 测试线程均正常退出，无线程残留。
2. 单次触发 100 次，UI 始终响应，无重复计数和旧结果重放。
3. 无条码时按配置完成超时和重试，失败连接被关闭重建，最终结果和 attempts 正确。
4. 间隔点击主动扫码运行至少 10 分钟，无并发错误和内存持续增长。
5. 扫码过程中拔网线，有限时间内报告错误；恢复网络后再次主动扫码可成功。
6. 扫码过程中关闭主窗口，当前同步调用按有限超时返回，socket 和 UI 测试线程完整退出。
7. 验证数字、字母、长条码以及包含中文内容的二维码显示不截断。
8. 旧扫码器面板的 `IP:8080` 测试行为与改造前一致。
9. 深色和浅色主题下新增面板均清晰可读，扫码中/成功/超时/失败颜色随主题正确刷新。

- [ ] **步骤 6.4：根据真机结果固化解析规则**

仅在拿到真实响应样本后决定是否去协议头、拆分多码或调整编码。保留原始 `QByteArray` 信号和十六进制日志，避免后续设备升级时丢失诊断能力。

---

### 5.3 文档与变更记录

**文件：**
- 修改：`changelog/CHANGELOG.md`
- 可选修改：`README.md`

- [ ] **步骤 7.1：更新 CHANGELOG**

记录：

- 新增 N-ScanHub 网络扫码枪 SDK 调试面板。
- `NScanScheduler` 提供不创建线程的同步 `scan()`，支持超时、有限重试和结构化结果。
- DeviceManager 复用单一工作线程承载扫码任务，空闲时等待事件，高频调用不反复创建线程且 UI 不阻塞。
- 支持主动单次扫码、重试参数、实际尝试次数和原始结果诊断。
- 原有扫码器调试面板保持不变。

- [ ] **步骤 7.2：记录部署要求**

在 README 或部署说明中记录 N-ScanHub 动态库、`libusb`、Windows DLL、默认端口 `30000` 和触发命令的现场验证状态。

- [ ] **阶段五验证：最终集成出口**

汇总阶段一至四的验证结果，并完成真机扫码、断网恢复、100 次主动触发、主题回归和旧扫码面板回归。CHANGELOG/README 必须与实际部署方式一致。

**阶段完成标准：** 构建、运行时依赖、线程释放、UI、主题、真机协议和旧功能回归全部通过；任何失败项记录具体平台、设备型号和复现条件。

---

## 完成标准

- 新旧两套扫码调试功能并存，旧行为无回归。
- `NScanScheduler` 提供完整同步 `scan()`，自身不创建线程，并可供其他模块调用。
- 同步接口支持有限超时、有限重试、结构化状态和原始数据返回。
- DeviceManager 的 UI 测试适配保证网络超时、重试和断网期间主窗口持续响应。
- DeviceManager 忙碌门禁阻止重复点击和并发测试任务。
- 成功 socket 按 SDK 建议复用，异常及退出时正确关闭；每次 UI 测试结束时 Worker 和 QThread 有序释放。
- Linux x64 完成编译、动态库解析和真机扫码验证；Windows MSVC 至少完成构建和 DLL 部署验证。
- 真机已确认单字节 `41` 可触发扫码；继续确认返回数据格式并固化解析规则。

## 范围外

- 删除或重构旧扫码器面板。
- 接入扫码图像端口 `36520`。
- USB/串口枚举模式。
- 网络设备自动搜索和参数修改。
- 扫码结果自动推进 `LineOrchestrator`。
- 条码业务校验、数据库查询、去重策略和工位绑定。
