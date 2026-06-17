# 客户系统 REST API 通信测试 实施计划

> **面向 AI 代理的工作者：** 后续实现必须以本文档为准。每完成一个任务先编译/测试验证，再进入下一任务。如用户需求发生变化，先更新本文档的“需求变更记录”和相关阶段任务，再实施代码。

**目标：** 在 `robot_visual` 工程中新增一个“客户系统通信测试”能力，用于客户现场验证上位机电脑已接入现场 WiFi 后，能通过 RESTful API 访问客户系统并读取日统计接口中的 `actualQty`。本功能定位为现场调试和通信验证，不在本阶段接入整线自动业务流程。

**现场信息：**

- WiFi 名称：`WDAS_PA01`
- WiFi 密码：`1234567890`
- 客户接口：`http://192.168.115.229:5084/api/MesData/day`
- 通信方式：HTTP RESTful API
- 客户程序监听：`0.0.0.0`
- 当前业务关注字段：`actualQty`

**架构：** 新增 `CustomSysScheduler` 作为客户系统通信调度器，负责接口 URL、HTTP GET、连通性判断、JSON 解析和结构化结果输出。`DeviceManager` 统一创建并持有 `CustomSysScheduler`，提供 UI 调用入口并转发日志/状态/数据结果。`MainWindow` 新增独立调试面板，只负责按钮、状态指示灯、数据展示和日志，不直接写 HTTP 通信或 JSON 解析逻辑。整体分层参考现有华沿 SDK 调试面板和调度接入方式。

**技术栈：** Qt 6（Widgets/Core/Network/Test）、CMake、HTTP GET、JSON 解析。

**关键约束：**

- 程序不自动连接 WiFi，不保存 WiFi 密码；现场网络连接由操作系统完成。
- 本期只验证电脑到客户系统接口的通信，不把 `actualQty` 推入整线流程或产线节拍逻辑。
- 新增 UI 风格参考“华沿 SDK 调试”面板，保持现有左侧调试面板视觉一致。
- 新面板必须独立于现有机械臂、AGV、视觉、扫码器和 N-ScanHub 面板，不复用它们的状态指示灯。
- `MainWindow` 不直接负责 HTTP 和 JSON 解析；所有通信调度由 `DeviceManager` 接入。
- 不改动华沿 SDK、AGV、视觉、扫码器、整线编排等既有业务逻辑。
- 代码增加必要中文注释，重点解释客户接口用途、为什么只提取 `actualQty`、错误处理和调度分层。
- 测试直接通过 UI 新增的客户系统通信测试面板完成，不单独新增 `tests/test_customSysScheduler.cpp`。
- 若新增控件使用自定义颜色，深浅主题必须统一纳入 `MainWindow::applyTheme(bool dark)`。
- 修改范围保持最小，不做无关重构。

**客户接口返回示例：**

```json
[
  {
    "id": 604844,
    "statDate": "2026-06-16T00:00:00",
    "lineId": "DRM8S",
    "lineName": "DRM8S",
    "planQty": 10000,
    "actualQty": 1151,
    "okQty": 1140,
    "ngQty": 11
  }
]
```

---

## 需求变更记录

| 日期 | 变更 | 影响 |
|------|------|------|
| 2026-06-16 | 初版需求：新增客户系统通信测试面板，读取 `actualQty` | 新增 Scheduler、DeviceManager 接入、MainWindow 面板，原计划包含 JSON 解析单元测试 |
| 2026-06-16 | 测试方式调整：直接通过 UI 测试面板验证，不新增独立测试文件 | 移除 `tests/test_customSysScheduler.cpp` 计划；阶段四改为 UI 验证和异常路径检查 |
| 2026-06-16 | UI 交互调整：HTTP 为无状态通信，不设置断开按钮；保留“测试连接”和“读取数据” | 面板只显示 actualQty 和解析摘要，完整原始 JSON 打印到底部日志区 |

---

## 文件清单

### 新增文件

| 文件 | 职责 |
|------|------|
| `src/customSysScheduler.h` | 声明客户系统调度器、数据结构、解析结果、信号和槽 |
| `src/customSysScheduler.cpp` | 实现 HTTP GET、连通性测试、接口读取、JSON 解析和错误处理 |

### 修改文件

| 文件 | 职责变化 |
|------|----------|
| `CMakeLists.txt` | 加入新源码，不新增 Qt Test 测试目标 |
| `src/devicemanager.h` | 持有 `CustomSysScheduler`，声明客户系统测试入口和结果信号 |
| `src/devicemanager.cpp` | 创建调度器、转发日志/状态/数据，提供连通性测试和读取接口入口 |
| `src/mainwindow.h` | 声明客户系统面板控件和槽函数 |
| `src/mainwindow.cpp` | 新增客户系统通信测试面板、信号连接、状态更新和主题适配 |

### 不修改文件

- `src/mainwindow.ui`：当前主界面由 `MainWindow::initUI()` 动态构建，新面板沿用该模式。
- `src/huayanScheduler.h/.cpp`：华沿 SDK 调度逻辑不属于本需求。
- `src/agvcontroller.h/.cpp`、`src/lineorchestrator.h/.cpp`、`src/visionclient.h/.cpp`、`src/nscanscheduler.h/.cpp`：不修改既有设备和流程逻辑。

---

## 阶段总览

| 阶段 | 目标 | 独立验证出口 |
|------|------|--------------|
| 阶段一 | 新增客户系统调度器和 JSON 解析 | 解析示例 JSON 得到 `actualQty=1151` |
| 阶段二 | DeviceManager 统一接入调度器 | 可通过 DeviceManager 发起连通性测试和读取请求 |
| 阶段三 | MainWindow 新增调试面板 | UI 显示接口、状态、`actualQty` 和原始返回 |
| 阶段四 | UI 测试路径和异常场景验证 | 通过新增面板验证连通性、读取数据和失败提示 |
| 阶段五 | 集成验证和文档同步 | 编译通过，UI 面板验证通过，需求变更已同步本文档 |

---

## 阶段一：实现 CustomSysScheduler

**文件：**
- 创建：`src/customSysScheduler.h`
- 创建：`src/customSysScheduler.cpp`
- 修改：`CMakeLists.txt`

- [x] **步骤 1.1：声明客户系统数据结构**

定义 `DayRecord`，至少包含：

- `id`
- `statDate`
- `lineId`
- `lineName`
- `planQty`
- `actualQty`
- `okQty`
- `ngQty`

定义 `ParseResult`，包含：

- `bool ok`
- `DayRecord record`
- `QString errorMessage`

说明：当前业务只使用 `actualQty`，其他字段用于现场排查和 UI 辅助展示。

- [x] **步骤 1.2：声明调度器接口**

`CustomSysScheduler` 继承 `QObject`，内部持有 `QNetworkAccessManager`。建议接口：

```cpp
class CustomSysScheduler : public QObject
{
    Q_OBJECT
public:
    explicit CustomSysScheduler(QObject *parent = nullptr);

    QUrl endpoint() const;
    void setEndpoint(const QUrl &endpoint);

    static QUrl defaultEndpoint();
    static ParseResult parseDayReply(const QByteArray &payload);

public slots:
    void testConnectivity();
    void fetchDayData();

signals:
    void requestStarted(const QString &operation);
    void connectivityChecked(bool ok, const QString &statusText, int httpStatus);
    void dayDataReady(const CustomSysScheduler::DayRecord &record,
                      const QString &rawJson);
    void requestFailed(const QString &operation,
                       const QString &errorMessage,
                       const QString &rawJson);
    void logMessage(const QString &message);
};
```

- [x] **步骤 1.3：实现默认接口地址**

`defaultEndpoint()` 返回：

```text
http://192.168.115.229:5084/api/MesData/day
```

接口地址允许后续通过 UI 输入覆盖，便于现场 IP 或端口变化时调试。

- [x] **步骤 1.4：实现 HTTP GET**

`testConnectivity()` 和 `fetchDayData()` 都对当前 endpoint 发起 GET 请求：

- `testConnectivity()` 只判断 HTTP 请求是否成功返回，成功时更新连通性状态。
- `fetchDayData()` 在 HTTP 成功后调用 `parseDayReply()`，解析业务字段。
- 请求需要设置合理超时。如当前 Qt 版本可用 `QNetworkRequest::setTransferTimeout()`，设置 3000-5000 ms。
- 错误日志需包含操作类型、URL、HTTP 状态码或网络错误信息。

- [x] **步骤 1.5：实现 JSON 解析**

`parseDayReply()` 必须满足：

- 返回内容必须是 JSON 数组。
- 数组不能为空。
- 首条记录必须是对象。
- `actualQty` 必须存在且为数字。
- 可选解析 `id`、`statDate`、`lineId`、`lineName`、`planQty`、`okQty`、`ngQty`。
- 出错时返回明确中文错误，例如“返回不是 JSON 数组”“返回数组为空”“缺少 actualQty 字段”。

**阶段一验证：** 使用示例 JSON 调用 `parseDayReply()`，结果 `ok=true` 且 `record.actualQty == 1151`。

---

## 阶段二：DeviceManager 接入客户系统调度

**文件：**
- 修改：`src/devicemanager.h`
- 修改：`src/devicemanager.cpp`

- [x] **步骤 2.1：声明和持有调度器**

在 `DeviceManager` 中前置声明并持有：

```cpp
class CustomSysScheduler;
CustomSysScheduler *m_customSysScheduler = nullptr;
```

提供只读访问：

```cpp
CustomSysScheduler *customSysScheduler() const;
```

- [x] **步骤 2.2：扩展 Config**

在 `DeviceManager::Config` 中新增：

```cpp
QUrl customSysEndpoint;
```

或使用 `QString customSysEndpoint`。默认值来自 `CustomSysScheduler::defaultEndpoint()`。如果为了减少头文件依赖，也可以使用 `QString`，在调用调度器时再转为 `QUrl`。

- [x] **步骤 2.3：声明 UI 调用入口**

新增槽函数：

```cpp
void testCustomSystem();
void fetchCustomSystemDayData();
```

入口职责：

- 从当前配置更新 `CustomSysScheduler` 的 endpoint。
- 发起连通性测试或读取数据。
- 不在 `MainWindow` 中直接调用 scheduler 的网络方法。

- [x] **步骤 2.4：声明状态和数据转发信号**

建议新增信号：

```cpp
void customSystemStatusChanged(bool ok, const QString &statusText);
void customSystemDayDataReady(const CustomSysScheduler::DayRecord &record,
                              const QString &rawJson);
void customSystemRequestStarted(const QString &operation);
void customSystemRequestFailed(const QString &operation,
                               const QString &errorMessage,
                               const QString &rawJson);
```

如果 `DayRecord` 跨 queued connection，需 `Q_DECLARE_METATYPE` 和 `qRegisterMetaType`。

- [x] **步骤 2.5：构造函数中连接信号**

`DeviceManager` 创建 `CustomSysScheduler`，并连接：

- scheduler `logMessage` → manager `logMessage`
- scheduler `connectivityChecked` → manager `customSystemStatusChanged`
- scheduler `dayDataReady` → manager `customSystemDayDataReady`
- scheduler `requestStarted` → manager `customSystemRequestStarted`
- scheduler `requestFailed` → manager `customSystemRequestFailed`

日志前缀建议统一为 `[客户系统]`。

**阶段二验证：** 在不打开 UI 的情况下，代码层可通过 `DeviceManager` 调用入口发起请求；所有结果均可由 DeviceManager 信号观察到。

---

## 阶段三：MainWindow 新增客户系统通信测试面板

**文件：**
- 修改：`src/mainwindow.h`
- 修改：`src/mainwindow.cpp`

- [x] **步骤 3.1：声明 UI 控件**

在 `MainWindow` 中新增控件成员：

```cpp
QLineEdit       *m_customSysEndpointEdit = nullptr;
QPushButton     *m_customSysConnectBtn = nullptr;
QPushButton     *m_customSysFetchBtn = nullptr;
DeviceIndicator *m_customSysIndicator = nullptr;
QLineEdit       *m_customSysActualQtyEdit = nullptr;
QLabel          *m_customSysInfoLabel = nullptr;
QLabel          *m_customSysRawLabel = nullptr;
```

`actualQty` 显示框只读。原始 JSON 不放在狭窄面板内，完整内容打印到底部日志区，面板只保留解析摘要。

- [x] **步骤 3.2：新增初始化函数**

新增：

```cpp
void initCustomSystemPanel(QVBoxLayout *leftPanel);
```

面板建议布局：

- 分组标题：`客户系统通信测试`
- 第一行：`测试连接`、`读取数据`、状态指示灯
- 第二行：接口 URL 输入框
- 第三行：`actualQty` 只读显示框
- 第四行：辅助字段摘要，例如 `line=DRM8S plan=10000 ok=1140 ng=11`
- 第五行：提示“原始 JSON 已打印到日志区”

按钮高度、分隔线和间距参考 `initHuayanPanel()`。

- [x] **步骤 3.3：挂载面板**

在 `MainWindow::initUI()` 左侧面板区域挂载：

```cpp
initCustomSystemPanel(leftPanel);
```

建议放在 `initNScanPanel(leftPanel)` 之后、`initAgvPanel(leftPanel)` 之前，作为现场通信类调试项。

- [x] **步骤 3.4：声明并实现槽函数**

新增槽函数：

```cpp
void onCustomSystemTest();
void onCustomSystemFetch();
void onCustomSystemRequestStarted(const QString &operation);
void onCustomSystemDayDataReady(const CustomSysScheduler::DayRecord &record,
                                const QString &rawJson);
void onCustomSystemRequestFailed(const QString &operation,
                                 const QString &errorMessage,
                                 const QString &rawJson);
```

点击按钮时：

- 读取 URL 输入框。
- 写入 `DeviceManager::Config`。
- 调用 `m_devMgr->testCustomSystem()` 或 `m_devMgr->fetchCustomSystemDayData()`。

- [x] **步骤 3.5：连接 DeviceManager 信号**

在 `MainWindow` 构造函数中连接：

- `customSystemStatusChanged` → 更新状态指示灯。
- `customSystemRequestStarted` → 禁用按钮、显示“测试中/读取中”。
- `customSystemDayDataReady` → 显示 `actualQty` 和辅助字段，完整原始 JSON 打印到日志区，恢复按钮。
- `customSystemRequestFailed` → 显示错误、恢复按钮、日志。

- [x] **步骤 3.6：主题适配**

如 `m_customSysActualQtyEdit`、`m_customSysRawLabel` 使用强调色，需要在 `applyTheme(bool dark)` 中统一维护深浅色样式。

**阶段三验证：** 启动程序可看到新面板；点击按钮时 UI 状态变化清晰；无客户网络时显示明确失败，不阻塞界面。

---

## 阶段四：UI 测试路径和异常场景验证

**文件：**
- 修改：`src/mainwindow.cpp`
- 修改：`src/devicemanager.cpp`

- [x] **步骤 4.1：确认不新增独立测试目标**

本需求的测试直接在 UI 新增的“客户系统通信测试”面板完成，不创建：

- `tests/test_customSysScheduler.cpp`
- Qt Test 测试目标
- `enable_testing()` / `add_test()` 相关配置

- [ ] **步骤 4.2：UI 正常路径验证**

在客户现场或可访问客户接口的网络环境中，通过 UI 面板验证：

- 默认接口地址为 `http://192.168.115.229:5084/api/MesData/day`
- 点击“测试连接”后状态指示灯显示连接正常
- 点击“读取数据”后 `actualQty` 显示接口返回值
- 辅助字段可显示 `lineId/lineName/planQty/okQty/ngQty/statDate`
- 日志中记录 HTTP 成功、解析结果和完整原始 JSON

- [ ] **步骤 4.3：UI 异常路径验证**

通过 UI 修改接口地址或断开网络验证失败路径：

- IP 不可达时，状态显示连接失败
- 端口不可达时，状态显示连接失败
- URL 格式不合法时，直接提示配置错误
- HTTP 返回失败时，日志包含状态码或网络错误
- JSON 解析失败时，UI 显示明确错误，不更新旧的 `actualQty` 为误导性成功值

- [x] **步骤 4.4：保留可测试解析边界**

虽然不新增测试文件，`CustomSysScheduler::parseDayReply()` 仍建议实现为独立静态函数，便于后续如需自动化测试时直接复用，也让网络请求和 JSON 解析边界清晰。

**阶段四验证：** 通过 UI 面板完成正常路径和失败路径验证；不要求运行 `ctest`。

---

## 阶段五：集成验证和验收

- [x] **步骤 5.1：编译验证**

执行 CMake configure/build，确认新增文件、MOC 和 Qt Network 均能通过；不新增 Qt Test 目标。

- [ ] **步骤 5.2：离线 UI 验证**

在未连接客户 WiFi 时启动程序：

- 面板默认显示客户接口 URL。
- 点击“连通性测试”后显示不可达或连接失败。
- 日志中包含 `[客户系统]` 前缀和失败原因。
- 现有华沿 SDK、AGV、视觉、扫码器面板不受影响。

- [ ] **步骤 5.3：现场网络验证**

现场步骤：

1. 电脑连接 WiFi `WDAS_PA01`。
2. 打开上位机程序。
3. 保持默认接口 URL。
4. 点击“测试连接”，状态显示连接正常。
5. 点击“读取数据”，`actualQty` 显示客户接口返回值。
6. 日志记录 HTTP 成功和解析结果。

- [ ] **步骤 5.4：回归检查**

确认以下既有功能未被改动或破坏：

- 机械臂 SDK 连接/断开按钮
- AGV 调试面板
- 视觉相机调试面板
- 扫码器调试面板
- N-ScanHub SDK 调试面板
- 整线开始/停止按钮

- [x] **步骤 5.5：文档同步**

如实现期间调整了 UI 字段、默认 URL、错误处理、测试范围或接入方式，必须回写本文档：

- 更新“需求变更记录”
- 更新“文件清单”
- 更新对应阶段任务状态
- 更新最终验收说明

**阶段五完成标准：** 编译通过、UI 可见且可操作、离线失败路径明确、现场成功路径能显示 `actualQty`，并且本文档反映最终实现状态。


---

## 本地实施验证记录

| 阶段 | 结果 | 说明 |
|------|------|------|
| 阶段一 | 通过 | `cmake --build build --target wh-robot-visual -j2` 成功，`customSysScheduler.cpp` 参与编译和链接 |
| 阶段二 | 通过 | `DeviceManager` 接入后重新构建成功，新增信号/槽和元类型注册通过 MOC 编译 |
| 阶段三 | 通过 | 新增 UI 面板、槽函数、主题适配后重新构建成功 |
| 阶段四 | 部分通过 | 本地源码检查确认 UI 正常/异常路径已接入；`QT_QPA_PLATFORM=offscreen timeout 5s ./build/wh-robot-visual` 启动 5 秒未崩溃后被 timeout 正常结束。客户 WiFi 下的真实点击验证需现场完成 |
| 阶段五 | 部分通过 | 本地编译通过；现场网络验证和人工回归检查需在有显示器与 `WDAS_PA01` 网络环境下完成 |
