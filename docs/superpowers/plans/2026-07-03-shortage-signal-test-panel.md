# 缺料信号源测试面板实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将现有“缺料信号源”改造成与主调度完全隔离的 MES/PLC 缺料计算测试面板。

**Architecture:** 保留 `CustomSysScheduler` 作为 HTTP/JSON 客户端，新增测试专用 `ShortageTestSession` 持有独立、非持久化的 `ShortageCalculator`。`MainWindow` 只连接测试状态信号，不存在任何 FIFO/LineManager 接线。

**Tech Stack:** C++17、Qt 6 Widgets/Network/Test、CMake、Qt Test 集成与 UI 行为测试。

---

## 文件职责

- 新增 `src/shortagetestsession.h/.cpp`：测试轮询、两轮稳定确认、换班识别、测试状态生命周期。
- 修改 `src/shortagecalculator.h/.cpp`：提供预计可用物料测试模型，不包含派单职责。
- 修改 `src/shortageconfig.h`：按 `xiancahngxitong/安全库存.xlsx` 固化 12 工位 × 3 产品配置，sheet2 空值按共同主品号回退 sheet1，并精确测试 36 个组合。
- 修改 `src/devicemanager.h/.cpp`：拥有并转发测试会话，不连接 `LineManager`。
- 修改 `src/mainwindow.h/.cpp`：精简测试 UI，移除来源切换。
- 新增 `tests/test_shortagetestsession.cpp`：测试会话行为。
- 修改 `tests/test_shortagecalculator.cpp`：预计可用物料模型单测。
- 新增 `tests/test_shortagetestui.cpp`：使用 Qt Test 验证中文控件、按钮状态和五列表格。
- 修改 `CMakeLists.txt`：加入源文件和 Qt Test 目标。

## 实施契约：中文界面、变量、信号链与注释

本节是所有任务的强制验收契约。实现者不得自行缩写中文文案、复用生产派单变量或省略职责注释。

### 中文界面固定文案

| 控件/字段 | 固定中文文案 | objectName / 成员变量 |
|---|---|---|
| 面板标题 | `缺料信号计算测试` | `shortageTestGroup` |
| 开始按钮 | `开始检测` | `m_shortageTestStartBtn` |
| 停止按钮 | `停止检测` | `m_shortageTestStopBtn` |
| 设备指示器 | `现场系统` | `m_shortageTestIndicator` |
| 产量标签 | `actualQty:` | `m_shortageTestActualQtyEdit` |
| 增量标签 | `产量增量:` | `m_shortageTestDeltaLabel` |
| 产品标签 | `产品:` | `m_shortageTestProductLabel` |
| 方式标签 | `生产方式:` | `m_shortageTestModeLabel` |
| 用量标签 | `单件用量:` | `m_shortageTestUsageLabel` |
| PLC 标签 | `L 位:` | `m_shortageTestBitsLabel` |
| 总体状态 | `状态:` | `m_shortageTestStatusLabel` |
| 工位表 | `工位 / 预计可用 / 安全线 / 箱量 / 状态` | `m_shortageTestTable` |

工位状态只允许使用明确中文：`等待首轮基线`、`等待生产信号稳定`、`正常`、`缺料（仅测试）`、`配置缺失`、`通信中断`、`产品信号不唯一`、`生产方式信号不唯一`、`产量异常回退`。不得显示无解释的 `-` 代替错误原因。

### 类型和变量命名

```cpp
/// 测试会话当前生命周期；只控制测试轮询，不代表主调度授权。
enum class ShortageTestState { Stopped, Polling, WaitingStable, Fault };

/// 一轮 MES + 三片 PLC 请求的临时聚合状态；轮次结束后整体丢弃。
struct ShortageTestRound {
    quint64 roundId = 0;          ///< 测试会话内单调递增轮次号，用于丢弃迟到响应。
    bool mesReceived = false;     ///< 本轮是否收到 MES 响应。
    bool plc68Received = false;   ///< 本轮是否收到 L68/L69 分片。
    bool plc71Received = false;   ///< 本轮是否收到 L71/L72/L73 分片。
    bool plc1998Received = false; ///< 本轮是否收到 L1998 分片。
    qint64 actualQty = 0;         ///< 本轮 MES 累计产量原值。
};
```

`ShortageTestSession` 成员固定采用：

```cpp
CustomSysScheduler *m_client = nullptr; ///< 非拥有指针；只发起和接收客户 HTTP 请求。
QTimer *m_pollTimer = nullptr;          ///< 5 秒测试轮询定时器，由 QObject 父子关系释放。
QTimer *m_roundTimeout = nullptr;       ///< 单轮 3 秒超时定时器。
ShortageCalculator m_calculator;        ///< 测试专用计算实例，不与生产状态共享。
ShortageTestRound m_round;              ///< 当前测试轮次聚合状态。
ShortageTestState m_state = ShortageTestState::Stopped; ///< 当前测试生命周期。
quint64 m_sessionId = 0;                ///< 每次 start 递增，隔离 stop 前的迟到响应。
qint64 m_lastActualQty = 0;             ///< 最近一次已接受样本，用于计算产量增量。
bool m_hasBaseline = false;             ///< false 表示下一有效轮只建立基线。
ProductModel m_confirmedProduct;        ///< 连续两轮确认后的产品。
ProductionMode m_confirmedMode;         ///< 连续两轮确认后的生产方式。
```

局部变量统一使用 `currentActualQty`、`productionDelta`、`usagePerUnit`、`stationInventory`、`statusReason`；禁止继续使用含义错误的 `accumulated`、`awaitingAcceptance` 或 `pendingStations` 表达测试库存。

### 完整信号链

```text
MainWindow::m_shortageTestStartBtn.clicked
  -> DeviceManager::startShortageTest()
  -> ShortageTestSession::start()

MainWindow::m_shortageTestStopBtn.clicked
  -> DeviceManager::stopShortageTest()
  -> ShortageTestSession::stop()

CustomSysScheduler::mesReplyReady / plcReplyReady
  -> ShortageTestSession 聚合同一 sessionId + roundId
  -> ShortageCalculator::ingest()
  -> ShortageTestSession::sampleUpdated / inventoryUpdated / statusChanged
  -> DeviceManager 同名测试信号转发
  -> MainWindow 更新顶部字段和 12 工位表
```

禁止链路：

```text
ShortageTestSession -X-> LineManager::reportShortage
ShortageTestSession -X-> TaskQueue/FIFO
ShortageTestSession -X-> TaskSource::CustomerSystem
ShortageTestSession -X-> 生产状态持久化
```

### 逐文件中文注释清单

- `shortagetestsession.h`：类注释必须写明“只测试、不派单、不持久化”；每个 public slot、signal、枚举和结构字段均写中文业务注释。
- `shortagetestsession.cpp`：注释 start/stop 会话代号、四请求同轮聚合、5 秒不重叠、3 秒超时、两轮稳定确认、20:00 清零和迟到响应丢弃原因。
- `shortagecalculator.h/.cpp`：注释 12 工位逐工位扣减、`安全线 + 箱量` 初值、负库存保留、配置有效性和溢出保护；明确不包含 FIFO 或任务完成语义。
- `shortageconfig.h`：每项注明 Excel sheet、位置、共同主品号、单位；每个 sheet1 回退字段注明原 sheet2 空单元格及回退品号；工位 3/4 参数相同但状态独立。
- `customSysScheduler.h/.cpp`：注释 `.228` 为现场实测 MES 地址、PLC 地址、JSON 契约、请求超时和 `roundId` 原样透传。
- `devicemanager.h/.cpp`：注释测试会话所有权和纯转发职责；明确不存在主调度接线。
- `mainwindow.h/.cpp`：成员注释只描述展示职责；开始/停止按钮注释明确不等于启用/停止主调度。
- 测试文件：每个测试函数用中文注释写明业务场景、输入和不变量，失败信息必须能定位工位、产品与轮次。

### RED/GREEN 验证分工

C++/Qt Test 同时验证接口边界和真实信号行为：编译期接口不向测试会话提供派单能力；运行时使用 fake client 与 QSignalSpy 验证：fake client 发出四个响应后才刷新；`QSignalSpy` 验证 start/stop/status/sample/inventory；旧 `sessionId` 或 `roundId` 不产生刷新；达到安全线时 FIFO spy 始终为 0。全部行为必须由可执行 Qt Test 证明，不把源码字符串搜索列为验收依据。

### 任务 1：锁定 MES 地址契约

**文件：**
- Modify: `tests/test_customsysscheduler.cpp`
- Test: `tests/test_shortagetestui.cpp`
- Modify: `src/customSysScheduler.cpp`
- Modify: `src/devicemanager.h`

- [ ] **步骤 1：写地址和隔离失败测试**

在解析测试中加入：

```cpp
void CustomSysSchedulerTest::defaultMesEndpointUsesVerifiedHost()
{
    QCOMPARE(CustomSysScheduler::mesDayEndpoint().toString(),
             QStringLiteral("http://192.168.115.228:5084/api/MesData/day"));
}
```

- [ ] **步骤 2：运行测试并验证 RED**

运行：`cmake --build build --target custom-system-parser-test -j$(nproc) && ctest --test-dir build -R custom-system-parser --output-on-failure`  
预期：FAIL，原因是尚无公开 `mesDayEndpoint()` 或仍返回旧 `.229` 地址。

- [ ] **步骤 3：集中唯一 MES 常量**

在 `CustomSysScheduler` 增加只读接口并让所有 MES 请求使用它：

```cpp
static QUrl mesDayEndpoint();

QUrl CustomSysScheduler::mesDayEndpoint()
{
    return QUrl(QStringLiteral("http://192.168.115.228:5084/api/MesData/day"));
}
```

删除或替换 `DeviceManager::Config` 中的 `.229` 默认值，避免双重配置。

- [ ] **步骤 4：运行地址测试**

运行：`cmake --build build --target custom-system-parser-test -j$(nproc) && ctest --test-dir build -R custom-system-parser --output-on-failure`  
预期：PASS。

**任务 1 源码注释要求：** `CustomSysScheduler` 的端点接口必须注明 `.228` 来自现场实测。

**任务 1 注释与测试验收清单：**

- [ ] MES 地址只有一个定义，`.229` 不再出现在运行源码。
- [ ] 端点函数、请求函数和 `roundId` 参数均有中文职责注释。
- [ ] parser Qt Test 验证实际返回 URL，完整源码构建确认所有调用点使用同一端点接口。
- [ ] RED 失败原因与 GREEN 通过命令均已实际记录。

### 任务 2：实现预计可用物料纯计算模型

**文件：**
- Modify: `src/shortagecalculator.h`
- Modify: `src/shortagecalculator.cpp`
- Modify: `src/shortageconfig.h`
- Modify: `tests/test_shortagecalculator.cpp`

- [ ] **步骤 1：写纯计算 RED 用例**

先验证 Excel 固化配置而不是继续使用 0 占位：

```cpp
const MaterialConfig *station11Model88 = materialConfig(11, ProductModel::Model88);
QVERIFY(station11Model88);
QCOMPARE(station11Model88->boxQuantity, 114);
QCOMPARE(station11Model88->safetyStock, 600);

const MaterialConfig *station1Model88 = materialConfig(1, ProductModel::Model88);
QVERIFY(station1Model88);
QCOMPARE(station1Model88->boxQuantity, 250);
QCOMPARE(station1Model88->spreadsheetUsage, 1); // sheet2 为空，按 18117-RM8S0 回退 sheet1
QCOMPARE(station1Model88->safetyStock, 1000);  // sheet2 为空，按 18117-RM8S0 回退 sheet1
```

再以 Qt Test 数据列覆盖 12 工位 × 3 产品共 36 个组合，断言每项 `boxQuantity > 0`、`spreadsheetUsage > 0`、`safetyStock > 0`；工位 3 与 4 参数相同但 `stationId` 独立。

增加测试配置并断言 12 工位分别扣减：

```cpp
MaterialConfig config{1, ProductModel::Model88, QStringLiteral("P1"), 100, 2, 40};
ShortageCalculator calculator({config});
calculator.initializeForProduct(ProductModel::Model88);
QCOMPARE(calculator.snapshot().first().estimatedAvailable, 140);

QVERIFY(calculator.ingest({1000, ProductModel::Model88, ProductionMode::L68}).ok);
QVERIFY(calculator.ingest({1020, ProductModel::Model88, ProductionMode::L68}).ok);
const StationConsumption station = calculator.snapshot().first();
QCOMPARE(station.estimatedAvailable, 100);
QVERIFY(!station.shortage);
```

再覆盖刚好安全线、负数、配置缺失、乘法溢出和 12 工位独立状态。

- [ ] **步骤 2：运行并验证 RED**

运行：`cmake --build build --target shortage-calculator-test -j$(nproc)`  
预期：FAIL，缺少 `estimatedAvailable`、`shortage` 和 `initializeForProduct()`。

- [ ] **步骤 3：最小实现库存状态**

将快照改为明确库存语义：

```cpp
struct StationConsumption {
    int stationId = 0;
    qint64 estimatedAvailable = 0;
    int safetyStock = 0;
    int boxQuantity = 0;
    bool configured = false;
    bool shortage = false;
    QString reason;
};
```

初始化与扣减必须使用安全算术：

```cpp
runtime.estimatedAvailable = qint64(config->safetyStock) + config->boxQuantity;
runtime.estimatedAvailable -= productionDelta * usage;
item.shortage = item.configured && item.estimatedAvailable <= item.safetyStock;
```

配置无效时 `configured=false` 且 `reason="配置缺失"`，不得判定缺料。

- [ ] **步骤 4：运行 GREEN**

运行：`cmake --build build --target shortage-calculator-test -j$(nproc) && ctest --test-dir build -R shortage-calculator --output-on-failure`  
预期：PASS。

**任务 2 源码注释要求：** 配置字段必须注明 Excel sheet、位置、品号、单位和回退来源；计算器必须逐项解释初始库存、逐工位扣减、负库存、阈值比较和溢出保护。

**任务 2 注释与测试验收清单：**

- [ ] 36 个工位产品组合均有精确配置数据测试。
- [ ] sheet2 空值的每个回退字段均可追溯到 sheet1 共同主品号。
- [ ] 工位 3/4 参数相同但状态独立。
- [ ] 12 工位分别计算，测试中不存在总消耗分摊。
- [ ] 刚好安全线、负库存、无效配置和 qint64 溢出均有 RED/GREEN 用例。

### 任务 3：新增独立测试会话

**文件：**
- Create: `src/shortagetestsession.h`
- Create: `src/shortagetestsession.cpp`
- Create: `tests/test_shortagetestsession.cpp`
- Modify: `CMakeLists.txt`

- [ ] **步骤 1：写会话 RED 测试**

使用 fake client 覆盖首轮基线、四片聚合、stop 后迟到响应、两轮产品确认和 L 位不唯一：

```cpp
QSignalSpy inventorySpy(&session, &ShortageTestSession::inventoryUpdated);
session.start();
emitCompleteRound(client, 1, 1000, validBits88L68());
QCOMPARE(session.snapshot().first().estimatedAvailable, 140);
emitCompleteRound(client, 2, 1020, validBits88L68());
QCOMPARE(session.snapshot().first().estimatedAvailable, 100);
QCOMPARE(inventorySpy.count(), 2); // 两个完整轮次均产生测试库存快照
```

- [ ] **步骤 2：运行并验证 RED**

运行：`cmake -S . -B build && cmake --build build --target shortage-test-session-test -j$(nproc)`  
预期：FAIL，目标或类不存在。

- [ ] **步骤 3：定义测试专用接口**

```cpp
class ShortageTestSession : public QObject {
    Q_OBJECT
public:
    explicit ShortageTestSession(CustomSysScheduler *client, QObject *parent = nullptr);
    bool isRunning() const;
    QList<StationConsumption> snapshot() const;
public slots:
    void start();
    void stop();
signals:
    void statusChanged(QString text, bool healthy);
    void sampleUpdated(qint64 actualQty, qint64 delta, ProductModel product,
                       ProductionMode mode, QHash<QString, bool> bits);
    void inventoryUpdated(QList<StationConsumption> stations);
};
```

该类不得声明 `shortageRequested`。

- [ ] **步骤 4：实现轮次与稳定确认**

复用 5 秒轮询、3 秒超时；产品/方式必须各恰好一个 true。候选值连续两轮一致后确认，确认期间发出“等待生产信号稳定”且不计算。

- [ ] **步骤 5：运行 GREEN**

运行：`cmake --build build --target shortage-test-session-test -j$(nproc) && ctest --test-dir build -R shortage-test-session --output-on-failure`  
预期：PASS。

**任务 3 源码注释要求：** 类注释写明“只测试、不派单、不持久化”；每个定时器、轮次字段、会话号、稳定候选和信号均有中文注释。

**任务 3 注释与测试验收清单：**

- [ ] start 立即轮询、stop 失效迟到响应。
- [ ] 四请求同轮齐全才刷新，旧 sessionId/roundId 被丢弃。
- [ ] 产品和方式均恰好一个 true，且连续两轮一致才确认。
- [ ] 5 秒轮询不重叠，3 秒超时有明确中文状态。
- [ ] QSignalSpy 验证 status/sample/inventory，不存在派单信号。

### 任务 4：DeviceManager 只转发测试会话

**文件：**

- 修改：`src/devicemanager.h`
- 修改：`src/devicemanager.cpp`
- 新增测试：`tests/test_shortagetestwiring.cpp`
- 修改：`CMakeLists.txt`

- [ ] **步骤 1：写 Qt 接线 RED 测试**

```cpp
void ShortageTestWiringTest::testSessionHasNoDispatchSignal()
{
    FakeCustomSysScheduler client;
    ShortageTestSession session(&client);
    QCOMPARE(session.metaObject()->indexOfSignal("shortageRequested(int)"), -1);
}

void ShortageTestWiringTest::deviceManagerForwardsTestInventoryWithoutQueueMutation()
{
    DeviceManager manager;
    const int queueBefore = manager.lineManager()->queueSnapshot().size();
    QSignalSpy inventorySpy(&manager, &DeviceManager::shortageTestInventoryUpdated);

    QList<StationConsumption> snapshot{makeConfiguredStation(1, 100, 40, 100)};
    QMetaObject::invokeMethod(manager.shortageTestSession(), "inventoryUpdated",
                              Qt::DirectConnection,
                              Q_ARG(QList<StationConsumption>, snapshot));

    QCOMPARE(inventorySpy.count(), 1);
    QCOMPARE(manager.lineManager()->queueSnapshot().size(), queueBefore);
}
```

- [ ] **步骤 2：运行接线测试并验证 RED**

运行：`cmake -S . -B build && cmake --build build --target shortage-test-wiring-test -j$(nproc)`  
预期：FAIL，缺少 `shortageTestSession()` 和测试专用转发信号，或测试会话仍暴露派单连接。

- [ ] **步骤 3：实现测试专用接线**

`DeviceManager` 新增 `shortageTestSession()`、`startShortageTest()`、`stopShortageTest()`，只转发 `statusChanged`、`sampleUpdated`、`inventoryUpdated`。删除测试 UI 路径对旧 `ShortageMonitor::shortageRequested` 的连接。若默认构造会启动不适合单测的设备线程，增加 `DeviceManager::StartupMode::TestOnly`，在该模式只创建本测试所需对象，不连接真实硬件。

- [ ] **步骤 4：运行 GREEN 和完整回归**

运行：`cmake --build build --target shortage-test-wiring-test wh-robot-visual -j$(nproc) && ctest --test-dir build -R shortage-test-wiring --output-on-failure && git diff --check`  
预期：Qt 接线测试 PASS、应用完整构建成功、diff check 无输出。

**任务 4 源码注释要求：** `DeviceManager` 成员、getter、启停接口、`StartupMode::TestOnly` 和转发信号均注明测试专用职责；明确禁止连接 `LineManager` 和真实硬件派单。

**任务 4 注释与测试验收清单：**

- [ ] Qt 元对象测试确认测试会话不存在 `shortageRequested(int)` 信号。
- [ ] QSignalSpy 验证测试库存信号能够经 DeviceManager 转发。
- [ ] 转发前后 FIFO 快照、当前任务和主调度状态完全不变。
- [ ] 启停接口只影响测试轮询，不改变主调度或硬件。
### 任务 5：精简测试面板 UI

**文件：**

- 新增：`src/shortagetestpanel.h`
- 新增：`src/shortagetestpanel.cpp`
- 修改：`src/mainwindow.h`
- 修改：`src/mainwindow.cpp`
- 新增测试：`tests/test_shortagetestui.cpp`
- 修改：`CMakeLists.txt`

- [ ] **步骤 1：写 Qt UI RED 测试**

```cpp
void ShortageTestUiTest::fixedChineseTitlesAndFiveColumns()
{
    ShortageTestPanel panel;
    auto *table = panel.findChild<QTableWidget *>("shortageTestTable");
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 12);
    QCOMPARE(table->columnCount(), 5);
    const QStringList expected{QStringLiteral("工位"), QStringLiteral("预计可用"),
                               QStringLiteral("安全线"), QStringLiteral("箱量"),
                               QStringLiteral("状态")};
    for (int column = 0; column < expected.size(); ++column)
        QCOMPARE(table->horizontalHeaderItem(column)->text(), expected.at(column));
    QVERIFY(panel.findChild<QRadioButton *>("shortageMockRadio") == nullptr);
    QVERIFY(panel.findChild<QRadioButton *>("shortageLiveRadio") == nullptr);
}

void ShortageTestUiTest::buttonsEmitTestOnlyRequests()
{
    ShortageTestPanel panel;
    QSignalSpy startSpy(&panel, &ShortageTestPanel::startTestRequested);
    QTest::mouseClick(panel.findChild<QPushButton *>("shortageTestStartBtn"), Qt::LeftButton);
    QCOMPARE(startSpy.count(), 1);
}
```

- [ ] **步骤 2：运行 UI 测试并验证 RED**

运行：`cmake -S . -B build && cmake --build build --target shortage-test-ui-test -j$(nproc)`  
预期：FAIL，`ShortageTestPanel` 尚不存在，旧 UI 仍含来源选择和旧表头。

- [ ] **步骤 3：实现独立可测试面板并嵌入 MainWindow**

面板只提供 `startTestRequested`、`stopTestRequested` 以及更新状态/样本/库存的 slots。`MainWindow` 将两个请求连接到 `DeviceManager::startShortageTest()` 和 `stopShortageTest()`；删除旧来源单选框。状态表严格使用五列，缺料显示“缺料（仅测试）”，配置和通信错误显示明确中文原因。

- [ ] **步骤 4：运行 GREEN、完整构建和主题人工检查**

运行：`cmake --build build --target shortage-test-ui-test wh-robot-visual -j$(nproc) && ctest --test-dir build -R shortage-test-ui --output-on-failure && git diff --check`  
预期：Qt UI Test PASS、应用构建成功、diff check 无输出；人工切换深浅主题后所有字段可读且横向无溢出。

**任务 5 源码注释要求：** `ShortageTestPanel` 类、成员、signals 和 slots 全部注明只读展示或测试控制职责；开始/停止按钮注释明确不等于启用/停止主调度。

**任务 5 注释与测试验收清单：**

- [ ] 标题、按钮、顶部字段、五列表头和状态原因全部使用固定中文。
- [ ] 来源单选和生产授权控件不出现在测试面板。
- [ ] QTest 验证按钮只发出测试请求，未暴露派单信号。
- [ ] Qt UI Test、完整构建和深浅主题人工检查均通过。

### 任务 6：总体验证和人工验收

**文件：**

- 测试：`tests/test_shortagecalculator.cpp`
- 测试：`tests/test_shortagetestsession.cpp`
- 测试：`tests/test_customsysscheduler.cpp`
- 测试：`tests/test_shortagetestui.cpp`
- 测试：`tests/test_shortagetestwiring.cpp`

- [ ] **步骤 1：运行全部自动测试**

运行：

```bash
cmake -S . -B build
cmake --build build --target wh-robot-visual shortage-calculator-test custom-system-parser-test shortage-test-session-test shortage-test-wiring-test shortage-test-ui-test -j$(nproc)
ctest --test-dir build --output-on-failure
git diff --check
```

预期：应用及全部 Qt Test 目标构建成功、CTest 0 失败、diff check 无输出。

- [ ] **步骤 2：人工验证不派单**

启动主调度并记录 FIFO 行数；运行测试面板直到测试状态出现“缺料（仅测试）”；确认 FIFO 行数、当前任务和硬件动作均未变化。

- [ ] **步骤 3：人工验证错误原因**

依次制造配置缺失、产品位多 true、方式位全 false、请求超时和 stop 后迟到响应；确认 UI 显示具体原因且主调度不受影响。


