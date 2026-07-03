# Main Dispatch Live Shortage Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在保护 `e1ffb3f` 既有主调度和已验证缺料测试面板的前提下，把真实缺料库存闭环接入现有严格 FIFO。

**Architecture:** 冻结 `ShortageTestSession/ShortageTestPanel`；重写 `ShortageMonitor` 为生产专用会话，复用 `ShortageCalculator` 的纯库存计算。`TaskExecutor` 只上报既有倒料成功节点，`LineManager/DeviceManager` 只传递任务事实，所有派单仍追加到现有 FIFO 队尾。

**Tech Stack:** C++17、Qt 6 Widgets/Network/Test、CMake、CTest。

---

## 0. 实施纪律

- 唯一设计依据：`docs/superpowers/specs/2026-07-03-main-dispatch-live-shortage-design.md`。
- 不修改旧 `2026-06-25-*`、`2026-07-03-live-shortage-dispatch-*` 或测试面板 spec/plan。
- 不修改 `src/shortagetestpanel.*`、`src/shortagetestsession.*` 及其测试。
- 不使用、新增或运行 Python 验证脚本。
- 本计划中的任务不得执行 `git commit`；所有修改最后由用户人工统一提交。
- 每个任务开始前运行 `git status --short`，不得覆盖用户已有修改。
- 每个任务结束运行 `git diff --check`，并检查修改文件白名单。
- 所有代码修改必须同时补充详尽中文注释；不仅注释新增声明，也要在修改既有逻辑的位置解释修改原因、修改前后语义、边界条件和不影响旧主流程的依据。
- 每项任务结束必须执行一次独立注释审查；注释缺失、含糊或与实现不一致时，该任务不得勾选完成，也不得进入下一任务。
- 任何需要改变 `e1ffb3f` 原状态跳转、FIFO顺序、Stop/Error行为或硬件动作的情况，立即停止并请用户确认。

### 全任务注释验收模板

Tasks 2–7 每项都必须逐项确认：

- [ ] 新增/修改的类、枚举、结构体及字段均有中文业务说明。
- [ ] 新增/修改的成员变量说明所有权、有效期、初始化和清理条件。
- [ ] 新增/修改的接口说明参数、返回值、失败语义和调用边界。
- [ ] 新增/修改的信号槽说明唯一触发时点、接收方和重复连接约束。
- [ ] 条件分支和状态转换说明业务原因，不能只复述代码。
- [ ] 修改既有函数的位置说明原行为、新行为及为何不影响 `e1ffb3f`。
- [ ] UI修改说明控件职责、模式切换影响和明确不允许发生的副作用。
- [ ] 注释与测试、spec及实际实现逐项一致，无含糊或过期描述。

## 1. 文件职责映射

| 文件 | 本项目职责 |
|---|---|
| `src/shortagecalculator.h/.cpp` | 纯库存运算；新增一次倒料加一箱，不接 FIFO |
| `src/shortagemonitor.h/.cpp` | 生产轮询、稳定确认、任务占用、派单请求和任务对账 |
| `src/lineconfig.h` | 保留 `TaskSource`；任务快照仍是跨层事件载体 |
| `src/taskexecutor.h/.cpp` | 在既有 `ArmUnload` 成功转换点发出一次倒料事实 |
| `src/linemanager.h/.cpp` | 保持 FIFO；返回新任务号并转发任务生命周期 |
| `src/devicemanager.h/.cpp` | 连接生产会话、LineManager和测试/生产互斥 |
| `src/mainwindow.h/.cpp` | 调度监控来源选择和三列生产状态表 |
| `tests/test_shortagecalculator.cpp` | 倒料加箱和计算回归 |
| `tests/test_shortagemonitor.cpp` | 生产会话核心状态机和轮询测试 |
| `tests/test_live_shortage_dispatch.cpp` | FIFO、倒料、失败和来源过滤集成测试 |
| `tests/test_live_shortage_ui.cpp` | 调度监控控件和模式切换测试 |
| `CMakeLists.txt` | 注册新增 Qt Test 目标 |

## Task 1: 固化基准与修改边界

**Files:**
- Create: `tests/test_live_shortage_baseline.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 记录开始状态**

Run:

```bash
git status --short
git diff --name-only e1ffb3f -- src/linemanager.cpp src/taskexecutor.cpp src/taskqueue.h
```

Expected: 第一条只显示用户现有未提交文件；第二条用于人工记录当前分支相对基准的主流程差异，不做自动回退。

- [ ] **Step 2: 写严格 FIFO 的 RED 测试**

新增 `tests/test_live_shortage_baseline.cpp`，直接测试不依赖硬件的 `TaskQueue`：

```cpp
#include <QtTest>
#include "taskqueue.h"

class LiveShortageBaselineTest : public QObject
{
    Q_OBJECT
private slots:
    void queue_keeps_global_fifo_across_sources();
};

void LiveShortageBaselineTest::queue_keeps_global_fifo_across_sources()
{
    TaskQueue queue;
    const Task first = queue.enqueue(8, TaskSource::UiMock);
    const Task second = queue.enqueue(3, TaskSource::CustomerSystem);
    const Task third = queue.enqueue(3, TaskSource::CustomerSystem);

    QCOMPARE(queue.takeNext().taskId, first.taskId);
    QCOMPARE(queue.takeNext().taskId, second.taskId);
    QCOMPARE(queue.takeNext().taskId, third.taskId);
    QVERIFY(!queue.hasPending());
}

QTEST_APPLESS_MAIN(LiveShortageBaselineTest)
#include "test_live_shortage_baseline.moc"
```

- [ ] **Step 3: 注册并运行基准测试**

在 `CMakeLists.txt` 增加 `live-shortage-baseline-test`，源文件仅包含测试文件和头文件，链接 `Qt6::Test Qt6::Core`。

Run:

```bash
cmake -S . -B build
cmake --build build --target live-shortage-baseline-test -j$(nproc)
ctest --test-dir build -R live-shortage-baseline --output-on-failure
```

Expected: PASS，证明现有 `TaskQueue` 已满足跨来源严格 FIFO，不需要修改队列算法。

- [ ] **Step 4: 白名单与注释验收**

Run:

```bash
git diff --name-only -- tests/test_live_shortage_baseline.cpp CMakeLists.txt
git diff --check
```

Expected: 本任务只涉及上述两个文件；测试名称明确说明跨来源仍保持全局 FIFO。

## Task 2: 为纯计算器增加倒料加一箱

**Files:**
- Modify: `src/shortagecalculator.h`
- Modify: `src/shortagecalculator.cpp`
- Modify: `tests/test_shortagecalculator.cpp`

- [ ] **Step 1: 写倒料闭环 RED 测试**

在 `ShortageCalculatorTest` 增加以下槽和测试：

```cpp
void replenishment_adds_exactly_one_box();
void replenishment_rejects_wrong_product_or_bad_station();

void ShortageCalculatorTest::replenishment_adds_exactly_one_box()
{
    ShortageCalculator calculator(buildSyntheticConfigs());
    calculator.initializeForProduct(ProductModel::Model88);
    QVERIFY(calculator.ingest({1000, ProductModel::Model88, ProductionMode::L68}).ok);
    QVERIFY(calculator.ingest({1110, ProductModel::Model88, ProductionMode::L68}).ok);
    QCOMPARE(calculator.snapshot().at(0).estimatedAvailable, 30);
    QVERIFY(calculator.snapshot().at(0).shortage);

    QVERIFY(calculator.recordReplenishment(1, ProductModel::Model88));
    QCOMPARE(calculator.snapshot().at(0).estimatedAvailable, 130);
    QVERIFY(!calculator.snapshot().at(0).shortage);
}

void ShortageCalculatorTest::replenishment_rejects_wrong_product_or_bad_station()
{
    ShortageCalculator calculator(buildSyntheticConfigs());
    calculator.initializeForProduct(ProductModel::Model88);
    const qint64 before = calculator.snapshot().at(0).estimatedAvailable;
    QVERIFY(!calculator.recordReplenishment(0, ProductModel::Model88));
    QVERIFY(!calculator.recordReplenishment(1, ProductModel::Model92));
    QCOMPARE(calculator.snapshot().at(0).estimatedAvailable, before);
}
```

- [ ] **Step 2: 运行测试确认 RED**

Run:

```bash
cmake --build build --target shortage-calculator-test -j$(nproc)
```

Expected: 编译失败，提示 `recordReplenishment` 未定义。

- [ ] **Step 3: 增加最小接口**

在 `shortagecalculator.h` 公共区增加：

```cpp
/**
 * @brief 记录一个真实任务已经把一整箱物料实际倒入指定工位。
 * @return 仅在工位、当前产品和箱量配置均有效且加法不溢出时返回 true。
 *
 * 本接口只修改预计库存，不创建任务；UiMock 调用必须在上层被过滤。
 */
bool recordReplenishment(int stationId, ProductModel product);
```

在 `shortagecalculator.cpp` 实现：校验 `1..12`、`m_initialized`、产品与 `m_currentProduct` 一致、配置完整、加法不溢出；成功时只执行 `estimatedAvailable += boxQuantity`，同步刷新 `awaitingAcceptance` 和 `reason`。

- [ ] **Step 4: 运行计算器全量测试**

Run:

```bash
cmake --build build --target shortage-calculator-test -j$(nproc)
ctest --test-dir build -R shortage-calculator --output-on-failure
```

Expected: 新增测试及原有首轮基线、负库存、配置表测试全部 PASS。

- [ ] **Step 5: 冻结测试面板范围**

Run:

```bash
git diff --name-only | rg 'shortagetest(panel|session)' && exit 1 || true
git diff --check
```

Expected: 无测试面板/测试会话文件输出；新增接口的中文注释明确“只计算、不派单”。

- [ ] **Step 6: 按全任务注释模板验收 Task 2**

逐项检查 `recordReplenishment()` 的参数、失败返回、溢出边界、产品校验、只计算不派单，以及既有测试路径不受影响；任一说明缺失则不得开始 Task 3。

## Task 3: 重写生产 ShortageMonitor 核心状态机

**Files:**
- Modify: `src/shortagemonitor.h`
- Modify: `src/shortagemonitor.cpp`
- Modify: `tests/test_shortagemonitor.cpp`

- [ ] **Step 1: 定义生产快照和任务记录 RED 编译测试**

在测试中使用预期公共接口：

```cpp
Q_DECLARE_METATYPE(Task)

void shortage_emits_once_until_unload();
void unload_adds_box_and_may_request_one_followup();
void ui_mock_never_changes_inventory();
void product_and_mode_require_two_rounds();
void restart_after_stop_preserves_runtime_inventory();
```

测试通过 Fake scheduler 发送完整四响应；用 `QSignalSpy dispatchSpy(&monitor, &ShortageMonitor::dispatchRequested)` 验证缺料后只发一次。调用：

```cpp
monitor.confirmDispatch(stationId, 101, true);
monitor.onTaskStarted(Task{101, stationId, TaskSource::CustomerSystem});
monitor.onMaterialUnloaded(Task{101, stationId, TaskSource::CustomerSystem});
monitor.onTaskFinished(Task{101, stationId, TaskSource::CustomerSystem});
```

并验证 `UiMock` 事件前后 `estimatedAvailable` 不变。

- [ ] **Step 2: 运行确认 RED**

Run:

```bash
cmake --build build --target shortage-monitor-test -j$(nproc)
```

Expected: 编译失败，提示生产快照、派单确认和任务生命周期接口不存在。

- [ ] **Step 3: 在头文件锁定接口**

`shortagemonitor.h` 使用以下生产接口；每个字段写中文业务注释：

```cpp
enum class LiveShortageTaskState {
    Normal,
    WaitingForQueue,
    Queued,
    Running,
    UnloadedFinishing,
    CommunicationPaused,
    ConfigurationError,
    WaitingOldProductTasks
};

struct LiveShortageStationSnapshot {
    int stationId = 0;
    qint64 estimatedAvailable = 0;
    int safetyStock = 0;
    LiveShortageTaskState state = LiveShortageTaskState::Normal;
};

class ShortageMonitor : public QObject {
    Q_OBJECT
public:
    explicit ShortageMonitor(CustomSysScheduler *client, QObject *parent = nullptr);
    bool isRunning() const;
    QList<LiveShortageStationSnapshot> snapshot() const;
public slots:
    void start();
    void stop();
    void confirmDispatch(int stationId, quint64 taskId, bool accepted);
    void onTaskStarted(const Task &task);
    void onMaterialUnloaded(const Task &task);
    void onTaskFinished(const Task &task);
signals:
    void dispatchRequested(int stationId);
    void statusChanged(QString text, bool healthy);
    void sampleUpdated(qint64 actualQty, ProductModel product, ProductionMode mode);
    void inventoryUpdated(QList<LiveShortageStationSnapshot> stations);
    void logMessage(QString message);
};
```

私有 `LiveTaskRecord` 至少保存 `taskId/stationId/product/unloaded/started`；使用 `QHash<quint64, LiveTaskRecord>`，不能用单个布尔值，因为“旧任务已倒料待收尾 + 下一箱已排队”允许同时存在。

- [ ] **Step 4: 实现会话与稳定确认**

保留已验证的 5秒轮询、3秒超时、会话号/轮次号和四请求聚合。产品与方式都必须恰好一个位为真；候选组合连续两轮一致才确认。实现以下边界：

```cpp
if (confirmedProductChanged) {
    m_calculator.initializeForProduct(product);
    m_waitingForOldProductTasks = hasTasksForOtherProduct(product);
}
if (onlyModeChanged) {
    m_rebaselineNextSample = true;
}
```

`stop()` 只停止定时器和未完成轮次，不调用 `m_calculator.reset()`，不清 `m_tasks`；再次 `start()` 必须保留运行期库存。

- [ ] **Step 5: 实现派单占用与倒料闭环**

每次快照刷新按以下顺序处理：

```cpp
if (m_running
    && station.shortage
    && !hasNotYetUnloadedTask(station.stationId)
    && !m_waitingForOldProductTasks) {
    markWaitingForQueue(station.stationId);
    emit dispatchRequested(station.stationId);
}
```

`confirmDispatch(... accepted=false)` 保持待入队；`accepted=true` 必须要求非零任务号并建立任务记录。`onMaterialUnloaded` 必须同时校验来源、任务记录和 `unloaded == false`，调用 `recordReplenishment` 后设置 `unloaded=true`。只有 `m_running == true` 才能立即判断并追加下一箱；模拟模式下只完成库存对账，再次 start 后重新评估。`onTaskFinished` 移除记录并解除换型门禁。

- [ ] **Step 6: 运行生产监控全量测试**

Run:

```bash
cmake --build build --target shortage-monitor-test -j$(nproc)
ctest --test-dir build -R shortage-monitor --output-on-failure
```

Expected: 轮询聚合、两轮确认、单工位去重、倒料加箱、连续补料、来源过滤、stop/start保留状态全部 PASS。

- [ ] **Step 7: 注释与范围验收**

按全任务注释模板逐项验收。确认注释解释会话停止不等于清库存、一个尚未倒料任务限制、已倒料收尾与下一箱可并存、幂等只是最后保护、换型旧任务门禁。运行 `git diff --check`；任一说明缺失则不得开始 Task 4。

## Task 4: 增加不改变状态流的任务事实通知

**Files:**
- Modify: `src/taskexecutor.h`
- Modify: `src/taskexecutor.cpp`
- Modify: `src/linemanager.h`
- Modify: `src/linemanager.cpp`
- Create: `tests/test_live_shortage_dispatch.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写 TaskExecutor 倒料事件 RED 测试**

测试夹具驱动现有 TaskExecutor 到 `ArmUnload`，用 `QSignalSpy` 断言：倒料完成前为0，完成并进入 `StowAfterUnload` 时为1，后续收姿态/码垛完成后仍为1；任务来源为 `UiMock` 时事件可以上报事实，但生产层测试必须证明其被忽略。

- [ ] **Step 2: 在唯一成功节点发出事件**

`taskexecutor.h` 增加：

```cpp
/// ArmUnload 的完整机械臂阶段成功后发出一次；它早于整任务成功，不表示码垛已完成。
void materialUnloaded(const Task &task);
```

`taskexecutor.cpp` 只在现有分支中插入一行，不移动原状态转换：

```cpp
case ExecState::ArmUnload:
    emit materialUnloaded(m_task);
    enterState(ExecState::StowAfterUnload, QStringLiteral("倒料完成，机械臂收姿态"));
    break;
```

- [ ] **Step 3: 为 LineManager 写生命周期 RED 测试**

测试要求：真实任务入队返回非零任务号；模拟任务旧 `reportShortage()` 行为不变；任务开始、倒料、成功/失败/取消均转发完整 `Task`；不同来源仍严格 FIFO。

- [ ] **Step 4: 增加最小 LineManager 接口**

保持旧调用可用，新增：

```cpp
quint64 reportShortageWithId(int stationId, TaskSource source);

signals:
    void taskEnqueued(Task task);
    void taskStarted(Task task);
    void materialUnloaded(Task task);
    void taskFinished(Task task);
```

`reportShortage()` 只包装 `return reportShortageWithId(...) != 0;`。新方法必须复用原校验、`m_queue.enqueue()` 和 `tryStartNext()` 顺序，不新增去重、不改变 Idle/Running/ReturningHome分支。`tryStartNext()` 取出任务后发 `taskStarted`；成功、失败和取消终态发 `taskFinished`。

- [ ] **Step 5: Stop/Error 清理通知但不改变清理行为**

清 Pending 前先复制 `pendingSnapshot()`，逐个将副本标为 `Canceled/Done` 并发 `taskFinished`，再调用原 `clearPendingAsCanceled()`。当前任务同样只增加通知；原停止设备、Error和报警顺序保持不变。

- [ ] **Step 6: 构建并运行集成测试**

Run:

```bash
cmake --build build --target live-shortage-dispatch-test wh-robot-visual -j$(nproc)
ctest --test-dir build -R 'live-shortage-(baseline|dispatch)' --output-on-failure
```

Expected: 全部 PASS；主程序完整链接成功。

- [ ] **Step 7: 基准差异人工审查**

Run:

```bash
git diff -- src/taskexecutor.cpp src/linemanager.cpp src/taskqueue.h
git diff --check
```

Expected: `taskqueue.h` 无修改；TaskExecutor只有事件；LineManager只有返回任务号和事实通知，原分支条件与设备动作不变。

- [ ] **Step 8: 按全任务注释模板验收 Task 4**

重点检查倒料事件唯一触发点、任务号返回语义、生命周期通知、Stop/Error只增加通知不改变原清理顺序；任一说明缺失则不得开始 Task 5。

## Task 5: DeviceManager 生产接线与会话互斥

**Files:**
- Modify: `src/devicemanager.h`
- Modify: `src/devicemanager.cpp`
- Create: `tests/test_live_shortage_wiring.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写接线 RED 测试**

验证 `DeviceManager` 同时持有独立的 `shortageTestSession()` 和 `shortageMonitor()`；启动真实模式会停止已运行测试会话；停止真实模式不会清生产快照；`dispatchRequested` 同步调用 `reportShortageWithId(CustomerSystem)` 并回传任务号；LineManager生命周期事件全部连接到 monitor。

- [ ] **Step 2: 增加公共控制与转发接口**

`devicemanager.h` 增加：

```cpp
ShortageMonitor *shortageMonitor() const;
void startLiveShortage();
void stopLiveShortage();

signals:
    void liveShortageStatusChanged(QString text, bool healthy);
    void liveShortageSampleUpdated(qint64 actualQty, ProductModel product, ProductionMode mode);
    void liveShortageInventoryUpdated(QList<LiveShortageStationSnapshot> stations);
```

成员注释必须明确测试会话和生产会话状态独立，二者只共享网络客户端和纯计算规则。

- [ ] **Step 3: 实现唯一生产接线**

构造时创建一个 `ShortageMonitor`。使用普通唯一连接；不在 start 中重复 connect。派单连接实现为：

```cpp
connect(m_shortageMonitor, &ShortageMonitor::dispatchRequested,
        this, [this](int stationId) {
    const quint64 taskId = m_lineManager
        ? m_lineManager->reportShortageWithId(stationId, TaskSource::CustomerSystem)
        : 0;
    m_shortageMonitor->confirmDispatch(stationId, taskId, taskId != 0);
});
```

连接 `taskStarted/materialUnloaded/taskFinished` 到 monitor 对应槽。`startLiveShortage()` 先调用现有 `stopShortageTest()`，设置 `.228` endpoint，再启动 monitor；`stopLiveShortage()` 只调用 monitor stop。

- [ ] **Step 4: 运行接线和冻结回归测试**

Run:

```bash
cmake --build build --target live-shortage-wiring-test shortage-test-wiring-test -j$(nproc)
ctest --test-dir build -R '(live-shortage-wiring|shortage-test-wiring)' --output-on-failure
```

Expected: 新接线 PASS；既有测试面板接线 PASS。

- [ ] **Step 5: 范围验收**

Run:

```bash
git diff --name-only | rg 'shortagetest(panel|session)' && exit 1 || true
git diff --check
```

Expected: 冻结文件无修改；所有连接只在构造期建立一次。

- [ ] **Step 6: 按全任务注释模板验收 Task 5**

重点检查对象所有权、测试/生产互斥、构造期唯一连接、同步派单确认和停止生产不清库存；任一说明缺失则不得开始 Task 6。

## Task 6: 调度监控来源切换和三列状态表

**Files:**
- Modify: `src/mainwindow.h`
- Modify: `src/mainwindow.cpp`
- Create: `tests/test_live_shortage_ui.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写 UI RED 测试**

使用 `QT_QPA_PLATFORM=offscreen` 构造主窗口或独立查找控件，断言：默认模拟；两个来源互斥；模拟模式12按钮可用；真实模式禁用12按钮；真实状态表恰为三列；切换不调用 `LineManager::stop/resetError`；测试面板控件树不发生变化。

- [ ] **Step 2: 声明 UI 成员**

`mainwindow.h` 增加并写中文展示职责注释：

```cpp
QRadioButton *m_mockShortageRadio = nullptr;
QRadioButton *m_liveShortageRadio = nullptr;
QLabel *m_liveShortageSummaryLabel = nullptr;
QTableWidget *m_liveShortageTable = nullptr;
```

增加私有方法：

```cpp
void setLiveShortageMode(bool enabled);
void updateLiveShortageTable(const QList<LiveShortageStationSnapshot> &stations);
```

- [ ] **Step 3: 在调度监控构建来源和三列表**

在现有模拟按钮标题前增加互斥单选，默认模拟。表头严格为：

```cpp
QStringLiteral("工位"),
QStringLiteral("库存/安全线"),
QStringLiteral("状态")
```

真实模式调用 `startLiveShortage()` 并禁用 `m_stationButtons`；模拟模式调用 `stopLiveShortage()` 并恢复按钮。任何分支都不得调用 `LineManager::stop()`、`resetError()` 或清队列。

- [ ] **Step 4: 连接生产状态**

连接 `liveShortageStatusChanged/sampleUpdated/inventoryUpdated`。状态表只显示设计规定的八种文案；顶部明确显示“库存为估算初值”。不得复制六个 PLC 位或测试按钮。

- [ ] **Step 5: 构建运行 UI 测试**

Run:

```bash
cmake --build build --target live-shortage-ui-test shortage-test-ui-test wh-robot-visual -j$(nproc)
QT_QPA_PLATFORM=offscreen ctest --test-dir build -R '(live-shortage-ui|shortage-test-ui)' --output-on-failure
```

Expected: 新生产 UI 和冻结测试 UI 全部 PASS，主程序完整构建。

- [ ] **Step 6: 人工 UI 检查**

确认默认模拟、真实切换、十二按钮启停、三列表、估算提示和现有 FIFO来源列。确认测试面板无任何视觉变化。

- [ ] **Step 7: 按全任务注释模板验收 Task 6**

重点检查每个新增控件职责、二选一切换副作用、三列表数据来源，以及为何模式切换不会清 FIFO、停止主调度或重置库存；任一说明缺失则不得开始 Task 7。

## Task 7: 补齐产品、通信、失败和连续补料矩阵

**Files:**
- Modify: `tests/test_shortagemonitor.cpp`
- Modify: `tests/test_live_shortage_dispatch.cpp`
- Modify: `tests/test_live_shortage_wiring.cpp`

- [ ] **Step 1: 增加通信矩阵测试**

逐项增加 Qt Test：四响应不齐不计算、超时后迟到响应丢弃、stop后迟到响应丢弃、产品位不唯一、方式位不唯一、通信恢复首轮只重建基线且不追算。

- [ ] **Step 2: 增加换型/方式矩阵测试**

验证单轮候选不切换、第二轮才确认；换产品重初始化；旧产品任务存在时新产品不派单；旧任务全部终态后恢复；仅方式变化保留预计库存并重建基线。

- [ ] **Step 3: 增加任务失败矩阵测试**

覆盖：排队取消、执行中倒料前失败、倒料后失败、系统Error清Pending、模拟任务终态；断言库存和占用符合 spec。

- [ ] **Step 4: 增加连续补料与全局 FIFO 测试**

构造工位1深度负库存：第一次任务倒料后仍缺料，只产生一个后续任务；在其前已有工位8模拟任务时，执行顺序必须是工位8后才到工位1续补；当前任务完整结束前续补任务只保持 Pending。

- [ ] **Step 5: 运行全部缺料 C++ 测试**

Run:

```bash
cmake --build build --target \
  shortage-calculator-test shortage-monitor-test shortage-test-session-test \
  shortage-test-wiring-test shortage-test-ui-test live-shortage-baseline-test \
  live-shortage-dispatch-test live-shortage-wiring-test live-shortage-ui-test \
  -j$(nproc)
ctest --test-dir build --output-on-failure
```

Expected: 0 failures；既有测试面板测试保持 PASS。

- [ ] **Step 6: 按全任务注释模板验收 Task 7**

逐项比对测试名称、测试意图注释与 spec 边界，确保失败矩阵、换型矩阵和通信矩阵无需阅读实现即可理解预期；任一说明缺失则不得开始 Task 8。

## Task 8: 注释、文档一致性和完整离线验收

**Files:**
- Modify: Tasks 2–7 中经测试证明需要修正的对应实现或测试文件；不得扩大文件范围
- Do not modify: historical specs/plans and frozen test panel/session files

- [ ] **Step 1: 中文注释审查**

逐个检查新增类、枚举、结构体、成员、信号、槽和状态转换。注释必须解释：对象所有权、测试/生产隔离、估算初值、负库存、尚未倒料占用、倒料事实、来源过滤、幂等保护、FIFO队尾、失败对账、stop/start保留、两轮确认、换型门禁和通信恢复基线。

- [ ] **Step 2: 禁止项扫描**

Run:

```bash
git diff --name-only | rg 'src/shortagetest(panel|session)|tests/test_shortagetest(ui|session)' && exit 1 || true
git diff --name-only | rg 'docs/superpowers/(specs|plans)/(2026-06-25|2026-07-03-live-shortage-dispatch)' && exit 1 || true
git diff --check
```

Expected: 无冻结文件或旧文档输出；diff check无输出。

- [ ] **Step 3: 完整构建和CTest**

Run:

```bash
cmake -S . -B build
cmake --build build --target wh-robot-visual -j$(nproc)
ctest --test-dir build --output-on-failure
```

Expected: 主程序构建成功，CTest 0 failures。

- [ ] **Step 4: 基准主流程人工差异审查**

Run:

```bash
git diff e1ffb3f -- src/taskqueue.h src/taskexecutor.cpp src/linemanager.cpp src/agvcontroller.cpp src/lineorchestrator.cpp
```

Expected: `agvcontroller.cpp`、`lineorchestrator.cpp` 无本项目修改；`taskqueue.h` 无算法修改；TaskExecutor和LineManager只有设计批准的事件与返回值增量。发现其他行为差异时停止验收。

- [ ] **Step 5: 输出人工现场验收清单**

现场逐项记录：首次基线、一个工位触发、等待期间不重复、倒料加箱、仍缺料续补到FIFO末尾、当前任务结束后才执行续补、倒料前失败、倒料后失败、来源切换、通信恢复、换型和模拟模式回归。

- [ ] **Step 6: 保持未提交状态交付**

Run:

```bash
git status --short
git diff --stat
```

Expected: 展示全部待人工审查文件；不执行 `git add` 或 `git commit`。

## Task 9: 客户确认与后续优化登记

**Files:**
- No production code changes
- Review: `docs/superpowers/specs/2026-07-03-main-dispatch-live-shortage-design.md`

- [ ] **Step 1: 客户确认记录**

逐项记录 spec 第15节的11个问题。未确认项保持“本阶段临时决策”，不得改写为客户最终规则。

- [ ] **Step 2: 后续版本拆分**

客户反馈后，分别为以下独立项目重新 brainstorming/spec/plan：真实初始库存、状态持久化与异常重启、多产品独立库存、可信断线补算、连续补料保护、FIFO优先级和硬件倒料反馈。不得在本计划实施中顺带加入。

- [ ] **Step 3: 最终人工提交前检查**

由用户确认现场结果、客户确认状态、diff范围和全部测试结果后，再由用户人工决定统一提交内容与提交说明。
