# 缺料完整逻辑测试修复与独立验证控制台 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复现有缺料测试页的手工源、现场源、首样本补料展示和状态刷新问题，并新增可隐藏入口、独立文件实现、支持最小化和最大化的缺料验证控制台。

**Architecture:** `ShortageTestController` 继续作为独立测试 Engine 的唯一门面，新增测试输入源门禁、动作可用性和模拟重启接口；`ShortageConfigDialog` 只修复自由操作页并提供第三 Tab 入口；新的 `ShortageValidationDialog` 在独立 `.h/.cpp` 中编排验证步骤，只调用控制器公开接口并观察只读快照。主窗口和配置窗口分别使用 `QPointer` 管理唯一非模态窗口，重复入口只恢复和激活现有窗口。

**Tech Stack:** C++17、Qt 6.8.3、Qt Widgets、Qt Test、CMake、Linux Debug。

## Global Constraints

- 所有新增和修改的界面文案、日志、验证步骤、失败原因和通过标准使用中文。
- plan 代码片段和实际代码中，新增或修改的枚举及每个枚举值、结构体及字段、函数接口及参数/返回值/失败语义、成员变量所有权和生命周期、关键状态分支、信号唯一连接点必须增加中文注释。
- 修改既有代码的关键分支必须说明修改前行为、现场问题、修改后行为以及不影响的既有逻辑。
- 独立测试只使用 `ShortageStateNamespace::StandaloneTest`；不得写正式账本、不得进入主 FIFO、不得控制 AGV/机械臂/扫码硬件。
- 不修改 `actualQty` 清零算法、补料非抢占算法、主 FIFO 追加规则、真实 MES/PLC 协议地址或寄存器映射。
- 现有测试窗口和新验证控制台必须使用非模态 `show()`，提供最小化、最大化和关闭按钮；禁止使用阻塞主页面的 `exec()`。
- 新验证控制台固定使用 `src/shortagevalidationdialog.h/.cpp`，验证业务不得混入既有 `ShortageConfigDialog`。
- 第三个 Tab 由单一编译期开关控制；隐藏后不得影响参数配置页、完整逻辑测试页和缺料业务核心。
- 自动测试只证明本地可判定结果；真实通信、窗口人工切换和现场设备未动作仍需人工确认，不得自动标记为实机验收通过。
- 测试仅使用 C++17、Qt Test/CMake，不使用 Python。
- 用户现场生成且当前未跟踪的 `test-state.json`、`test-state.backup.json`、`test-events.jsonl` 不得暂存、修改或删除。

## 文件结构

| 文件 | 单一职责 |
| --- | --- |
| `src/shortagetypes.h` | 增加测试输入源、测试动作可用性值类型及中文语义 |
| `src/shortagetestcontroller.h/.cpp` | 测试源门禁、手工/现场输入、模拟任务事实、快照和动作可用性的唯一业务门面 |
| `src/shortageconfigdialog.h/.cpp` | 修复既有自由操作测试页；只提供独立验证窗口入口和唯一实例持有 |
| `src/shortagevalidationdialog.h/.cpp` | 独立验证项定义、步骤编排、自动/人工结果和验证日志 |
| `src/mainwindow.h/.cpp` | 只管理唯一缺料配置窗口及窗口恢复/置顶 |
| `tests/test_shortage_test_controller.cpp` | 测试输入源门禁、首样本计划、动作状态和模拟重启 |
| `tests/test_shortage_dialog.cpp` | 既有测试页输入、动态快照、按钮门禁、窗口按钮和第三 Tab 入口 |
| `tests/test_shortage_validation_dialog.cpp` | 独立验证窗口、15 项定义、步骤执行、人工边界、窗口按钮和隐藏开关 |
| `tests/test_live_shortage_ui.cpp` | 主窗口唯一配置窗口和非模态显示源码契约 |
| `CMakeLists.txt`、`tests/CMakeLists.txt` | 编译新 Dialog 并注册独立 Qt Widgets 测试 |

---

### Task 1: 建立测试输入源门禁、动作可用性和首样本补料契约

**Files:**

- Modify: `src/shortagetypes.h`
- Modify: `src/shortagetestcontroller.h`
- Modify: `src/shortagetestcontroller.cpp`
- Modify: `tests/test_shortage_test_controller.cpp`

**Interfaces:**

- Consumes: 现有 `ShortageEngine::applyStableSample(...)`、`nextDispatchRequest()` 和 `ShortageSampleCoordinator::stableSampleReady`。
- Produces: `ShortageTestInputSource`、`ShortageTestActionAvailability`、`ShortageTestController::selectInputSource(...)`、`currentSnapshot()`、`actionAvailability()`、`simulateRestart()`、`actionAvailabilityChanged(...)`。
- 保持: `applyManualSample(...)`、`startFieldSampling()`、任务事实接口名称不变，供既有测试和后续两个 Dialog 复用。

- [ ] **Step 1: 写输入源和首样本失败测试**

在 `ShortageTestControllerTest` 增加以下声明和完整测试。测试必须先证明手工源不启动协调器、现场源拒绝手工样本，以及 0 建账后首个稳定样本已经生成待派补料单：

```cpp
private slots:
    void manualSourceNeverStartsFieldCoordinator();       ///< 手工源不能访问真实 MES/PLC 采样路径。
    void fieldSourceRejectsManualSample();                ///< 现场源禁止混入手工 actualQty。
    void firstStableSampleCreatesVisibleDispatchOrder();  ///< 0 建账后的首样本必须产生待派补料单。
    void actionAvailabilityFollowsOrderLifecycle();       ///< 按钮能力必须随待派、运行、倒料和终态变化。
    void simulatedRestartUsesOnlyTestNamespace();         ///< 模拟重启不得改变 production-* 文件。
```

`firstStableSampleCreatesVisibleDispatchOrder()` 使用确定输入，不依赖真实网络：

```cpp
void ShortageTestControllerTest::firstStableSampleCreatesVisibleDispatchOrder()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ShortageTestController controller(
        testConfiguration(), directory.path(), nullptr,
        [] { return ShortageOperationResult{true, QStringLiteral("允许")}; });

    controller.initializeZeroAfterConfirmation();
    controller.selectInputSource(ShortageTestInputSource::Manual);
    controller.applyManualSample(ProductModel::Model88,
                                 ProductionMode::LeftRight,
                                 qint64{100});

    // 首个样本只建立 actualQty 基线，但 Planner 必须同时重评估 0 库存并创建一箱意图。
    const ShortageUiSnapshot snapshot = controller.currentSnapshot();
    QCOMPARE(snapshot.runtime.actualQty.baseline, qint64{100});
    QCOMPARE(snapshot.runtime.activeStationId, 1);
    QCOMPARE(snapshot.runtime.orders.size(), 1);
    QCOMPARE(snapshot.runtime.orders.first().stationId, 1);
    QCOMPARE(snapshot.runtime.orders.first().state,
             ReplenishmentOrderState::AwaitingDispatch);
    QVERIFY(controller.actionAvailability().canAcceptDispatch);
}
```

`fieldSourceRejectsManualSample()` 必须断言 `operationRejected` 增加一次、基线仍不存在；`manualSourceNeverStartsFieldCoordinator()` 必须断言选择手工源后调用 `startFieldSampling()` 被拒绝且 `fieldSamplingActive()==false`。

- [ ] **Step 2: 运行测试并确认 RED**

Run:

```bash
cmake --build build-shortage --target shortage_test_controller_tests --parallel
build-shortage/tests/shortage_test_controller_tests \
  firstStableSampleCreatesVisibleDispatchOrder
```

Expected：编译失败，明确缺少 `ShortageTestInputSource`、`selectInputSource()`、`currentSnapshot()` 或 `actionAvailability()`；旧测试仍能编译到新增接口引用处。

- [ ] **Step 3: 增加带中文注释的测试值类型**

在 `src/shortagetypes.h` 增加以下完整定义，并在文件末尾注册元类型：

```cpp
/// 独立完整逻辑测试的数据来源；与正式主界面的 Mock/Live 开关分离，避免手工源误启真实采样。
enum class ShortageTestInputSource {
    Manual, ///< 手工输入产品、模式和非负 qint64 actualQty，不访问真实 MES/PLC。
    Field   ///< 复用唯一现场采样协调器，只把稳定样本送入 StandaloneTest Engine。
};

/// 测试控制器根据权威运行状态计算的动作门禁；UI 只能展示，业务接口仍执行二次校验。
struct ShortageTestActionAvailability {
    bool canSubmitManualSample = true;   ///< 手工源且现场采样停止时允许提交样本。
    bool canStartFieldSampling = false;  ///< 现场源且采样停止时允许启动。
    bool canStopFieldSampling = false;   ///< 现场采样运行时允许停止。
    bool canAcceptDispatch = false;      ///< 存在 AwaitingDispatch 补料单时允许模拟接受。
    bool canRejectDispatch = false;      ///< 存在 AwaitingDispatch 补料单时允许模拟拒收。
    bool canFailBeforeUnload = false;    ///< 当前测试任务处于 Running 且尚未倒料时允许失败。
    bool canRecordUnload = false;        ///< 当前测试任务处于 Running 且尚未倒料时允许倒料。
    bool canFailAfterUnload = false;     ///< 当前测试任务已倒料但未终态时允许倒料后失败。
    bool canSucceed = false;             ///< 当前测试任务 Running 或 Unloaded 时允许成功终态。
    bool canResendUnload = false;        ///< 已保存最近倒料事实时允许验证重复倒料锁定。
};

Q_DECLARE_METATYPE(ShortageTestInputSource)
Q_DECLARE_METATYPE(ShortageTestActionAvailability)
```

- [ ] **Step 4: 实现控制器输入源和只读接口**

在 `ShortageTestController` 增加下列接口和成员。实际实现必须保留注释中的失败语义：

```cpp
public:
    /// 返回当前测试输入源；该值只决定测试入口，不改变正式 ShortageInputSource。
    ShortageTestInputSource inputSource() const { return m_inputSource; }
    /// 复制当前测试 Engine 快照；调用方不得持有或修改 Engine 内部状态引用。
    ShortageUiSnapshot currentSnapshot() const;
    /// 根据当前输入源、采样和补料单生命周期计算动作可用性。
    ShortageTestActionAvailability actionAvailability() const;

public slots:
    /// 切到 Manual 前先停止现场采样；切到 Field 只改变门禁，不自动访问真实系统。
    void selectInputSource(ShortageTestInputSource source);
    /// 模拟程序重启：保存当前测试事实，重建 StandaloneTest Engine，再走原恢复校验。
    void simulateRestart();

signals:
    /// 每次权威快照变化后发一次，两个测试窗口据此统一刷新按钮门禁。
    void actionAvailabilityChanged(ShortageTestActionAvailability availability);

private:
    /// 构造无副作用的测试快照；result 只携带本次产量增量，不修改 Engine。
    ShortageUiSnapshot buildSnapshot(const ShortageEngineResult &result = {}) const;

    /// 当前测试输入源；默认 Manual，避免打开窗口即访问真实 MES/PLC。
    ShortageTestInputSource m_inputSource = ShortageTestInputSource::Manual;
```

输入源实现固定为：

```cpp
void ShortageTestController::selectInputSource(ShortageTestInputSource source)
{
    if (source == m_inputSource)
        return;

    // 修改前现场采样可能继续运行；切换到手工源必须先停采样，避免手工与真实样本混入同一测试账本。
    if (source == ShortageTestInputSource::Manual && m_fieldSamplingActive)
        stop();
    m_inputSource = source;
    emit eventLogged(source == ShortageTestInputSource::Manual
                         ? QStringLiteral("测试输入源已切换为手工源：不会访问真实 MES/PLC")
                         : QStringLiteral("测试输入源已切换为现场源：尚未启动采样"));
    emitSnapshot();
}
```

`applyManualSample(...)` 开头增加 `m_inputSource != Manual` 或 `m_fieldSamplingActive` 拒绝分支；`startFieldSampling()` 开头增加 `m_inputSource != Field` 拒绝分支。两条拒绝都不得调用 Engine 或采样协调器。

`currentSnapshot()` 复用 `emitSnapshot()` 的构造逻辑，抽取无副作用的 `buildSnapshot(...) const`；`emitSnapshot()` 只执行：

```cpp
void ShortageTestController::emitSnapshot(const ShortageEngineResult &result)
{
    const ShortageUiSnapshot snapshot = buildSnapshot(result);
    emit snapshotChanged(snapshot);
    emit actionAvailabilityChanged(actionAvailability());
}
```

`actionAvailability()` 必须遍历 `m_engine->state().orders`，只根据 `m_currentOrderNo/m_currentTaskId` 对应补料单状态设置动作，不用按钮状态反推业务真值。

`simulateRestart()` 固定执行 `stop()`、清空仅运行期 task 绑定缓存、`rebuildEngine()`、`reloadTestState()`；不得删除测试文件或访问正式命名空间。

- [ ] **Step 5: 运行控制器测试并确认 GREEN**

Run:

```bash
cmake --build build-shortage --target shortage_test_controller_tests --parallel
ctest --test-dir build-shortage -R '^shortage_test_controller_tests$' --output-on-failure
```

Expected：1/1 通过；新增五个测试和全部既有控制器测试通过，输出中没有真实网络访问。

- [ ] **Step 6: 检查并提交 Task 1**

Run: `git diff --check`

Expected：无输出。

```bash
git add src/shortagetypes.h src/shortagetestcontroller.h \
  src/shortagetestcontroller.cpp tests/test_shortage_test_controller.cpp
git commit -m "修复缺料测试输入源与首样本计划"
```

---

### Task 2: 修复现有完整逻辑测试页并实时显示补料状态

**Files:**

- Modify: `src/shortageconfigdialog.h`
- Modify: `src/shortageconfigdialog.cpp`
- Modify: `tests/test_shortage_dialog.cpp`

**Interfaces:**

- Consumes: Task 1 的 `selectInputSource(...)`、`currentSnapshot()`、`actionAvailabilityChanged(...)` 和全部测试动作。
- Produces: 手工产品/模式/`actualQty` 输入、动态工位表、补料单摘要、完整事件按钮和窗口最小化/最大化契约。
- 不产生: 验证项、验证步骤或通过规则；这些只属于 Task 3 的独立 Dialog。

- [ ] **Step 1: 写现有页面结构和行为失败测试**

在 `ShortageDialogTest` 增加：

```cpp
private slots:
    void configDialogSupportsMinimizeMaximizeAndClose();     ///< 旧测试窗口可与主页面切换。
    void manualSourceProvidesProductModeAndActualQty();      ///< 手工源具备完整输入。
    void sourceSelectionCallsControllerAndGatesControls();   ///< 来源切换不会误启现场采样。
    void snapshotRefreshesStationAndOrderViews();            ///< 首样本补料必须立即可见。
    void testPageProvidesAllDesignedOperations();             ///< 状态、任务和异常动作完整。
    void actionButtonsFollowControllerAvailability();         ///< 禁用只做提示，控制器仍二次校验。
```

`manualSourceProvidesProductModeAndActualQty()` 必须按固定 `objectName` 查找：

```cpp
auto *product = requiredChild<QComboBox>(&dialog, "manualProductCombo");
auto *mode = requiredChild<QComboBox>(&dialog, "manualModeCombo");
auto *actualQty = requiredChild<QLineEdit>(&dialog, "manualActualQtyEdit");
auto *submit = requiredChild<QPushButton>(&dialog, "manualSampleSubmitButton");
QCOMPARE(product->count(), 3);
QCOMPARE(mode->count(), 3);
QVERIFY(actualQty->validator() != nullptr);
QVERIFY(submit->isEnabled());
```

`snapshotRefreshesStationAndOrderViews()` 使用真实 `ShortageTestController`：点击 0 建账、设置 `actualQty=100` 并提交后，断言第 1 行库存为 0、活动工位为 1、补料单状态文本包含“等待派单”。

- [ ] **Step 2: 运行测试并确认 RED**

Run:

```bash
cmake --build build-shortage --target shortage_dialog_tests --parallel
QT_QPA_PLATFORM=offscreen build-shortage/tests/shortage_dialog_tests \
  manualSourceProvidesProductModeAndActualQty
```

Expected：FAIL，缺少 `manualProductCombo`；现有八按钮契约仍通过。

- [ ] **Step 3: 增加现有页面成员和刷新接口**

在 `shortageconfigdialog.h` 增加前置声明、函数和成员；每个成员保持以下中文所有权说明：

```cpp
class QComboBox;
class QLabel;
class QRadioButton;

private:
    /// 按当前选择把手工产品、模式和 qint64 actualQty 一次提交给测试控制器。
    void submitManualSample();
    /// 只更新输入源控件门禁；业务层仍在 ShortageTestController 二次校验。
    void refreshTestSourceControls();
    /// 用控制器权威快照刷新 12 工位、基线、等待顺序和当前补料单。
    void refreshTestSnapshot(const ShortageUiSnapshot &snapshot);
    /// 用控制器计算的动作能力刷新事件按钮，不在 UI 复制状态机规则。
    void refreshTestActionAvailability(const ShortageTestActionAvailability &availability);

    QRadioButton *m_manualSourceRadio = nullptr; ///< Dialog 拥有；默认选中且不访问真实系统。
    QRadioButton *m_fieldSourceRadio = nullptr;  ///< Dialog 拥有；选中后仍需明确点击启动采样。
    QComboBox *m_manualProductCombo = nullptr;   ///< Dialog 拥有；data 保存 ProductModel。
    QComboBox *m_manualModeCombo = nullptr;      ///< Dialog 拥有；data 保存 ProductionMode。
    QLineEdit *m_manualActualQtyEdit = nullptr;  ///< Dialog 拥有；验证完整非负 qint64 文本。
    QPushButton *m_manualSampleSubmitButton = nullptr; ///< Dialog 拥有；只在手工源可用。
    QTableWidget *m_testRuntimeTable = nullptr;  ///< Dialog 拥有；只读显示 12 工位权威快照。
    QLabel *m_testPlanSummaryLabel = nullptr;    ///< Dialog 拥有；显示基线、活动工位和等待顺序。
    QLabel *m_testOrderSummaryLabel = nullptr;   ///< Dialog 拥有；显示当前测试补料单状态。
    QMap<QString, QPushButton *> m_testActionButtons; ///< objectName 到按钮，仅用于门禁刷新。
```

- [ ] **Step 4: 实现手工输入、来源门禁和动态快照**

手工 `actualQty` 使用 `QRegularExpressionValidator` 接受 `0` 到 `9223372036854775807` 的十进制文本，并在 `toLongLong(&ok)` 失败时记录“手工样本提交失败：actualQty 不是非负 qint64”。

来源接线固定为：

```cpp
connect(m_manualSourceRadio, &QRadioButton::clicked, this, [this] {
    // 修改前单选框只改变外观；现在先通知控制器停止现场采样，再刷新输入门禁。
    if (m_testController != nullptr)
        m_testController->selectInputSource(ShortageTestInputSource::Manual);
    refreshTestSourceControls();
});
connect(m_fieldSourceRadio, &QRadioButton::clicked, this, [this] {
    // 选择现场源不自动启动 MES/PLC，仍需操作员明确点击“启动现场采样”。
    if (m_testController != nullptr)
        m_testController->selectInputSource(ShortageTestInputSource::Field);
    refreshTestSourceControls();
});
```

12 工位表改为七列：工位、库存、最低、最高、当前用量、状态、连续失败。`refreshTestSnapshot(...)` 从 `m_configuration` 查找当前产品配置和模式用量；该计算只用于显示，不写回 Engine。

补料单摘要选择最近一个非终态补料单，按完整中文映射显示 `AwaitingDispatch/Queued/Running/Unloaded`；没有补料单显示“当前测试补料单：无”。所有枚举分支必须列全，不使用整数强转作为界面文案。

- [ ] **Step 5: 补齐始终可见的自由操作按钮**

保留原按钮并新增以下固定 `objectName`：

```text
testSaveStateButton             保存测试状态
testReloadStateButton           重新加载测试状态
testClearStateButton            清空测试状态
testFailureAfterUnloadButton    倒料后失败
testResendUnloadButton          重复发送倒料事件
testSimulateRestartButton       模拟程序重启
manualSampleSubmitButton        提交手工样本
```

清空测试状态必须弹出中文二次确认；取消不调用控制器。事件按钮始终显示，`refreshTestActionAvailability(...)` 只根据 Task 1 值类型设置 enabled 和 tooltip。

- [ ] **Step 6: 增加旧窗口最小化和最大化按钮**

在构造完成 UI 前设置：

```cpp
// 修改前 QDialog 默认标题栏缺少最小化/最大化；增加窗口按钮只改变窗口管理，不影响测试状态。
setWindowFlag(Qt::Window, true);
setWindowFlags(windowFlags()
               | Qt::WindowMinimizeButtonHint
               | Qt::WindowMaximizeButtonHint
               | Qt::WindowCloseButtonHint);
```

测试断言三个 Hint 均存在，并断言窗口保持非模态 `isModal()==false`。

- [ ] **Step 7: 运行现有页面测试并确认 GREEN**

Run:

```bash
cmake --build build-shortage --target shortage_dialog_tests --parallel
QT_QPA_PLATFORM=offscreen ctest --test-dir build-shortage \
  -R '^shortage_dialog_tests$' --output-on-failure
```

Expected：1/1 通过；手工源提交后不启动现场采样，首样本补料单显示“等待派单”。

- [ ] **Step 8: 检查并提交 Task 2**

Run: `git diff --check`

Expected：无输出。

```bash
git add src/shortageconfigdialog.h src/shortageconfigdialog.cpp \
  tests/test_shortage_dialog.cpp
git commit -m "修复缺料完整逻辑测试页面"
```

---

### Task 3: 新增独立缺料验证控制台和十五项验证向导

**Files:**

- Create: `src/shortagevalidationdialog.h`
- Create: `src/shortagevalidationdialog.cpp`
- Create: `tests/test_shortage_validation_dialog.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

- Consumes: Task 1 `ShortageTestController` 公开 slots、`snapshotChanged` 和 `actionAvailabilityChanged`。
- Produces: 独立 `ShortageValidationDialog`、15 项固定定义、步骤执行和自动/人工结果。
- 不依赖: `ShortageConfigDialog`、`MainWindow`、`LineManager`、`TaskQueue` 或任何硬件类。

- [ ] **Step 1: 写独立 Dialog 失败测试和源码隔离契约**

创建 `tests/test_shortage_validation_dialog.cpp`，测试类固定包含：

```cpp
class ShortageValidationDialogTest final : public QObject
{
    Q_OBJECT
private slots:
    void windowSupportsMinimizeMaximizeCloseAndIsNonModal(); ///< 新窗口可与主页面自由切换。
    void exposesExactlyFifteenChineseValidationCases();      ///< VT-01～VT-15 不缺项。
    void eachCaseHasStepsAndPassCriteria();                  ///< 每项均有操作和通过标准。
    void nextStepUsesOnlyTestControllerPublicActions();      ///< 向导不直接修改 Engine。
    void automaticCaseShowsActualSnapshotEvidence();         ///< 可判定项目显示预期、实际和结果。
    void fieldEvidenceWaitsForManualConfirmation();          ///< 实机项不能被本地测试冒充通过。
    void sourceFilesContainNoProductionOrHardwareDependency();///< 新 Dialog 与正式/FIFO/硬件隔离。
};
```

源码隔离测试读取新 `.h/.cpp`，断言不包含 `ShortageEngine`、`LineManager`、`TaskQueue`、`AgvController`、`HuayanScheduler` 和 `CustomSysScheduler`。

- [ ] **Step 2: 注册测试 target 并确认 RED**

在 `tests/CMakeLists.txt` 增加：

```cmake
# 独立验证窗口测试只链接测试控制器和缺料核心，不链接 MainWindow、FIFO 或硬件控制器。
add_shortage_cpp_test(shortage_validation_dialog_tests
    test_shortage_validation_dialog.cpp
    ../src/shortagevalidationdialog.cpp
    ../src/customSysScheduler.cpp
    ../src/shortagesamplecoordinator.cpp
    ../src/shortagestatestore.cpp
    ../src/shortageledger.cpp
    ../src/replenishmentplanner.cpp
    ../src/shortageengine.cpp
    ../src/shortagetestcontroller.cpp
)
target_link_libraries(shortage_validation_dialog_tests PRIVATE Qt6::Widgets)
target_compile_definitions(shortage_validation_dialog_tests PRIVATE
    ROBOT_VISUAL_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
set_tests_properties(shortage_validation_dialog_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)
```

Run: `cmake --build build-shortage --target shortage_validation_dialog_tests --parallel`

Expected：FAIL，缺少 `src/shortagevalidationdialog.cpp` 或 `ShortageValidationDialog`。

- [ ] **Step 3: 定义验证状态、动作和步骤值类型**

创建 `src/shortagevalidationdialog.h`，公共值类型固定为：

```cpp
/// 单项验证的进度和结论；自动结果与人工现场确认严格分开。
enum class ShortageValidationStatus {
    NotStarted,             ///< 尚未执行任何步骤。
    InProgress,             ///< 已开始且仍有步骤待执行。
    PassedAutomatically,    ///< 只读快照足以证明结果通过。
    FailedAutomatically,    ///< 只读快照与通过条件不一致。
    WaitingManualEvidence,  ///< 涉及真实通信、窗口或硬件隔离，等待人工核对。
    PassedByOperator        ///< 操作员已经根据现场证据确认通过。
};

/// 向导允许调用的测试控制器公开动作；不包含正式 Engine、FIFO 或硬件命令。
enum class ShortageValidationAction {
    ShowInstruction,     ///< 只显示人工操作说明，不调用业务接口。
    ClearTestState,      ///< 清空 StandaloneTest 文件并重建测试 Engine。
    InitializeZero,      ///< 明确确认测试现场从 0 建账。
    SelectManualSource,  ///< 切换手工源，不访问真实系统。
    SelectFieldSource,   ///< 切换现场源，但不自动启动采样。
    ApplyManualSample,   ///< 使用步骤携带的产品、模式和 actualQty 提交样本。
    StartFieldSampling,  ///< 经正式 Live 停止门禁后启动测试现场采样。
    StopFieldSampling,   ///< 停止测试现场采样并屏蔽迟到样本。
    DispatchRejected,    ///< 模拟主调度拒收当前补料单。
    DispatchAccepted,    ///< 模拟接受并生成测试专用 taskId。
    FailureBeforeUnload, ///< 模拟倒料前失败，不增加库存。
    MaterialUnloaded,    ///< 模拟唯一倒料事实并增加一箱。
    FailureAfterUnload,  ///< 模拟倒料后失败，保留已入账库存。
    TaskSucceeded,       ///< 模拟任务成功终态。
    ResendUnload,        ///< 重发最近倒料事实，验证幂等和严重锁定。
    SaveState,           ///< 记录测试状态保存证据。
    ReloadState,         ///< 从 StandaloneTest 文件加载并走恢复校验。
    SimulateRestart      ///< 重建测试 Engine 并恢复，不接触 production-* 文件。
};

/// 一个可执行验证步骤；输入只在 ApplyManualSample 时生效。
struct ShortageValidationStep {
    QString instructionZh; ///< 面向现场人员的当前操作。
    QString expectedZh;    ///< 执行后应观察到的状态。
    ShortageValidationAction action = ShortageValidationAction::ShowInstruction;
    ProductModel product = ProductModel::Model88;       ///< 手工样本产品。
    ProductionMode mode = ProductionMode::LeftRight;   ///< 手工样本模式。
    qint64 actualQty = 0;                               ///< 非负累计产量。
};

/// 固定验证项定义；步骤和通过条件来自已确认的中文设计文档。
struct ShortageValidationCase {
    QString id;                         ///< VT-01～VT-15 稳定编号。
    QString nameZh;                     ///< 列表显示名称。
    QList<ShortageValidationStep> steps;///< 按顺序执行的全部步骤。
    QString passCriteriaZh;             ///< 完整通过条件，不使用省略描述。
    bool requiresManualEvidence = false;///< true 时自动执行结束后仍等待人工确认。
};
```

`ShortageValidationDialog` 构造函数和成员所有权固定为：

```cpp
/// 独立缺料验证控制台；只编排 StandaloneTest 公开动作，不持有正式 Engine/FIFO/硬件引用。
class ShortageValidationDialog final : public QDialog
{
    Q_OBJECT
public:
    /// testController 为非拥有指针，DeviceManager 保证其生命周期覆盖所有缺料窗口。
    explicit ShortageValidationDialog(ShortageTestController *testController,
                                      QWidget *parent = nullptr);
    /// 返回固定 VT-01～VT-15 定义的只读副本，供 UI 和 Qt Test 核对完整性。
    QList<ShortageValidationCase> validationCases() const { return m_cases; }

private:
    void buildUi();                              ///< 创建三栏和底部日志，不修改测试状态。
    void selectCase(int row);                    ///< 重置当前步骤显示，不自动执行业务动作。
    void executeNextStep();                      ///< 只分发 ShortageValidationAction 到控制器公开接口。
    void evaluateCurrentCase(const ShortageUiSnapshot &snapshot); ///< 依据只读证据判定可自动项目。
    void confirmManualEvidence();                ///< 只允许 WaitingManualEvidence 转人工通过。
    void appendValidationLog(const QString &messageZh); ///< 追加带时间、编号和步骤的中文证据。
    /// 创建固定 VT-01～VT-15 定义；只生成编排数据，不读取或修改测试状态。
    static QList<ShortageValidationCase> createValidationCases();

    ShortageTestController *m_testController = nullptr; ///< 非拥有；为空时只允许浏览步骤。
    QList<ShortageValidationCase> m_cases;              ///< Dialog 拥有的固定验证定义。
    int m_currentCaseIndex = -1;                        ///< 当前验证项下标，-1 表示未选择。
    int m_currentStepIndex = 0;                         ///< 下一条待执行步骤下标。
    ShortageValidationStatus m_status = ShortageValidationStatus::NotStarted; ///< 当前项结论。
};
```

- [ ] **Step 4: 建立十五项完整步骤定义**

`validationCases()` 必须恰好按以下编号和动作构建，不得缺项：

```text
VT-01 窗口切换能力：5 条 ShowInstruction，等待人工确认。
VT-02 测试与正式环境隔离：记录正式哈希/队列/硬件计数、执行测试动作、复核证据，等待人工确认。
VT-03 手工源不访问真实系统：SelectManualSource、ApplyManualSample(88,L/R,100)、复核真实请求计数，等待人工确认。
VT-04 0 建账和首样本补料计划：ClearTestState、InitializeZero、SelectManualSource、ApplyManualSample(88,L/R,100)，自动判定。
VT-05 现场源首样本补料计划：ClearTestState、InitializeZero、SelectFieldSource、StartFieldSampling、等待两轮、StopFieldSampling，等待人工确认。
VT-06 正常产量扣减：ClearTestState、InitializeZero、SelectManualSource、ApplyManualSample(88,L/R,100)、ApplyManualSample(88,L/R,105)，自动判定增量 5。
VT-07 派单拒绝和原单重试：建立 VT-04 状态、DispatchRejected、DispatchAccepted，自动判定原单号和 Running。
VT-08 倒料入账与连续补料：建立 VT-04 状态、DispatchAccepted、MaterialUnloaded、TaskSucceeded，自动判定一箱和下一意图。
VT-09 倒料前失败保护：建立 VT-04 状态，按 preUnloadFailureLimit 次循环 DispatchAccepted、FailureBeforeUnload，自动判定目标工位暂停。
VT-10 倒料后失败：建立 VT-04 状态、DispatchAccepted、MaterialUnloaded、FailureAfterUnload，自动判定库存不回滚。
VT-11 重复倒料严重锁定：建立 VT-04 状态、DispatchAccepted、MaterialUnloaded、ResendUnload，自动判定库存只加一次和 criticalLock。
VT-12 actualQty 清零与毛刺：分别执行 1000→2→5、1000→900→500→0→2→5、1000→2→1005，展示每组基线/候选/库存证据，等待人工复核三组。
VT-13 换型和九种组合：手工源遍历 3 产品×3 模式核对 Engine 用量；现场源制造一轮抖动和连续两轮新上下文，并在旧任务未终态时确认一次上下文切换，等待人工复核现场稳定证据。
VT-14 保存、重载、清空和重启恢复：SaveState、SimulateRestart、ReloadState、ClearTestState，等待人工复核正式文件哈希。
VT-15 来源切换和误操作门禁：SelectFieldSource、StartFieldSampling、SelectManualSource、ApplyManualSample，并检查无前置状态按钮，自动与人工证据联合确认。
```

每项的 `passCriteriaZh` 必须使用下表固定内容；步骤生成辅助函数只允许减少 C++ 重复，不得删减输入值、事件或通过条件：

| 编号 | `passCriteriaZh` 固定内容 |
| --- | --- |
| VT-01 | 两个窗口均可最小化、最大化和恢复；主窗口始终可以激活；重复点击只激活唯一窗口，不出现多个相同窗口；窗口操作不改变测试状态。 |
| VT-02 | 正式状态文件哈希和主 FIFO 数量不变；AGV、机械臂和扫码硬件命令计数不增加；测试状态只写入 `test-*` 文件。 |
| VT-03 | 页面接收手工 `actualQty=100`；真实请求计数不变；启动现场采样不可用；日志明确记录手工样本。 |
| VT-04 | 建账后 12 工位均为 0；首样本只建基线；低位工位按同时间工位号排序；活动工位为 1；只生成一个工位 1 的 `AwaitingDispatch` 补料单并立即显示。 |
| VT-05 | 真实采样只在现场源启动；两轮稳定前不产生稳定样本；首次稳定样本只建基线并立即显示一个 `AwaitingDispatch` 补料单；正式 Engine 和 FIFO 不变化。 |
| VT-06 | 每个启用工位库存等于旧库存减 `5×当前模式用量`；用量 0 工位不变；基线为 105；最近增量为 5。 |
| VT-07 | 拒收后补料单保持 `AwaitingDispatch` 且单号不变，不生成重复单；接受后绑定测试 taskId 并进入 `Running`。 |
| VT-08 | 倒料完成时增加准确一箱；任务终态不重复加箱；未达最高位生成下一箱；达到最高位后释放活动工位并切换下一等待工位。 |
| VT-09 | 每次倒料前失败不增加库存；达到阈值时只暂停目标工位；其他工位继续计划；日志包含工位、原库存、失败次数和处理动作。 |
| VT-10 | 已倒料的一箱不回滚；倒料前失败计数不增加；补料单进入倒料后失败终态。 |
| VT-11 | 库存不第二次增加；系统进入严重锁定；停止新自动意图；日志包含补料单号、taskId、工位和重复倒料处理动作。 |
| VT-12 | `1000→2→5` 按新周期累计 5 扣减；连续下降序列最终合计扣 5；`1000→2→1005` 只按旧周期增量 5 扣减；页面显示基线和候选变化。 |
| VT-13 | 手工源九种组合使用正确配置用量；现场源一轮抖动不切换、两轮相同才确认；旧任务未终态时保存待切换上下文；排空后切换；库存沿用并使用新组合用量。 |
| VT-14 | 安全状态逐字段恢复；不安全状态进入维护锁定；清空只删除 `test-*` 文件；正式文件哈希始终不变。 |
| VT-15 | 现场采样运行中不能混用输入源；停止后可切换；非法动作按钮禁用；直接调用控制器仍被拒绝并输出中文原因。 |

- [ ] **Step 5: 实现动作分发、证据显示和人工边界**

`executeNextStep()` 使用下列完整 `switch` 分发全部动作：

```cpp
switch (step.action) {
case ShortageValidationAction::ShowInstruction:
    break;
case ShortageValidationAction::ClearTestState:
    m_testController->clearTestStateAfterConfirmation();
    break;
case ShortageValidationAction::InitializeZero:
    m_testController->initializeZeroAfterConfirmation();
    break;
case ShortageValidationAction::SelectManualSource:
    m_testController->selectInputSource(ShortageTestInputSource::Manual);
    break;
case ShortageValidationAction::SelectFieldSource:
    m_testController->selectInputSource(ShortageTestInputSource::Field);
    break;
case ShortageValidationAction::ApplyManualSample:
    m_testController->applyManualSample(step.product, step.mode, step.actualQty);
    break;
case ShortageValidationAction::StartFieldSampling:
    m_testController->startFieldSampling();
    break;
case ShortageValidationAction::StopFieldSampling:
    m_testController->stop();
    break;
case ShortageValidationAction::DispatchRejected:
    m_testController->simulateDispatchRejected();
    break;
case ShortageValidationAction::DispatchAccepted:
    m_testController->simulateDispatchAccepted();
    break;
case ShortageValidationAction::FailureBeforeUnload:
    m_testController->simulateFailureBeforeUnload();
    break;
case ShortageValidationAction::MaterialUnloaded:
    m_testController->simulateMaterialUnloaded();
    break;
case ShortageValidationAction::FailureAfterUnload:
    m_testController->simulateFailureAfterUnload();
    break;
case ShortageValidationAction::TaskSucceeded:
    m_testController->simulateTaskSucceeded();
    break;
case ShortageValidationAction::ResendUnload:
    m_testController->resendLastUnloadFact();
    break;
case ShortageValidationAction::SaveState:
    m_testController->saveTestState();
    break;
case ShortageValidationAction::ReloadState:
    m_testController->reloadTestState();
    break;
case ShortageValidationAction::SimulateRestart:
    m_testController->simulateRestart();
    break;
}
```

控制器为空时拒绝执行并记录“验证步骤未执行：测试控制器不可用”。步骤全部完成后：`requiresManualEvidence=true` 转 `WaitingManualEvidence`；否则调用 `evaluateCurrentCase(...)` 设置自动通过或失败。人工确认按钮只在 `WaitingManualEvidence` 可用。

- [ ] **Step 6: 实现窗口按钮和实际状态区**

窗口构造时设置 `Qt::Window`、最小化、最大化和关闭 Hint，`setModal(false)`，最小尺寸 1280×760。右侧必须显示：产品/模式、actualQty/基线、活动工位、等待顺序、当前补料单、严重锁定、最近增量。底部日志每条包含 UTC 时间、VT 编号、步骤号、输入、预期和实际摘要。

在主 `CMakeLists.txt` 的 `PROJECT_SOURCES` 增加：

```cmake
        # 独立现场验证窗口；入口可隐藏，但不把验证编排混入既有配置 Dialog。
        src/shortagevalidationdialog.cpp
        src/shortagevalidationdialog.h
```

- [ ] **Step 7: 运行独立验证窗口测试并确认 GREEN**

Run:

```bash
cmake --build build-shortage --target shortage_validation_dialog_tests --parallel
QT_QPA_PLATFORM=offscreen ctest --test-dir build-shortage \
  -R '^shortage_validation_dialog_tests$' --output-on-failure
```

Expected：1/1 通过；恰好 15 项，每项步骤和通过条件非空，实机项停在“等待人工确认”。

- [ ] **Step 8: 检查并提交 Task 3**

Run: `git diff --check`

Expected：无输出。

```bash
git add src/shortagevalidationdialog.h src/shortagevalidationdialog.cpp \
  tests/test_shortage_validation_dialog.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "新增独立缺料验证控制台"
```

---

### Task 4: 接入可隐藏第三 Tab 并管理两个窗口唯一实例

**Files:**

- Modify: `src/shortagevalidationdialog.h`
- Modify: `src/shortageconfigdialog.h`
- Modify: `src/shortageconfigdialog.cpp`
- Modify: `src/mainwindow.h`
- Modify: `src/mainwindow.cpp`
- Modify: `tests/test_shortage_dialog.cpp`
- Modify: `tests/test_live_shortage_ui.cpp`

**Interfaces:**

- Consumes: Task 3 `ShortageValidationDialog`。
- Produces: `kShowShortageValidationTab`、第三 Tab 入口、`QPointer` 唯一验证窗口、`QPointer` 唯一配置测试窗口。
- 保持: 关闭任一窗口不启动/停止采样、不修改测试状态。

- [ ] **Step 1: 写第三 Tab 和唯一窗口失败测试**

`ShortageDialogTest` 增加：

```cpp
void validationTabOnlyContainsIndependentDialogEntry(); ///< 第三 Tab 不混入验证业务控件。
void repeatedValidationOpenReusesSingleDialog();         ///< 重复入口只恢复现有验证窗口。
void hiddenValidationEntryLeavesFirstTwoTabsUntouched(); ///< 隐藏开关不影响既有两页。
```

`test_live_shortage_ui.cpp` 增加源码契约：

```cpp
requireContains(header, QStringLiteral("QPointer<ShortageConfigDialog> m_shortageConfigDialog"));
requireContains(cpp, QStringLiteral("m_shortageConfigDialog->showNormal()"));
requireContains(cpp, QStringLiteral("m_shortageConfigDialog->raise()"));
requireContains(cpp, QStringLiteral("m_shortageConfigDialog->activateWindow()"));
QVERIFY2(cpp.contains(QStringLiteral("ShortageConfigDialog")),
         "主窗口必须保留缺料配置测试入口");
```

- [ ] **Step 2: 运行测试并确认 RED**

Run:

```bash
cmake --build build-shortage --target shortage_dialog_tests live_shortage_ui_tests --parallel
QT_QPA_PLATFORM=offscreen ctest --test-dir build-shortage \
  -R '^(shortage_dialog_tests|live_shortage_ui_tests)$' --output-on-failure
```

Expected：FAIL，缺少第三 Tab、`m_shortageValidationDialog` 或 `m_shortageConfigDialog`。

- [ ] **Step 3: 增加单一入口开关和验证窗口唯一实例**

在 `shortagevalidationdialog.h` 增加：

```cpp
/// 现场验证阶段显示第三个 Tab；全部现场验收完成后只改为 false，不删除测试核心或既有页面。
inline constexpr bool kShowShortageValidationTab = true;
```

在 `ShortageConfigDialog` 增加：

```cpp
#include <QPointer>

class ShortageValidationDialog;

private:
    /// 创建只含边界说明和“打开验证控制台”按钮的第三 Tab，不承载验证业务。
    QWidget *buildValidationEntryPage();
    /// 打开唯一非模态验证窗口；最小化时恢复，重复点击不创建第二个会话。
    void openValidationDialog();

    /// 自动失效的非拥有 Qt 窗口指针；实际对象由父子关系和 WA_DeleteOnClose 管理。
    QPointer<ShortageValidationDialog> m_shortageValidationDialog;
```

`buildUi()` 只在开关为 true 时执行：

```cpp
if (kShowShortageValidationTab) {
    // 验证业务位于独立 Dialog；第三 Tab 只保留可后续隐藏的入口。
    m_mainTabs->addTab(buildValidationEntryPage(), QStringLiteral("验证向导"));
}
```

`openValidationDialog()` 固定行为：

```cpp
void ShortageConfigDialog::openValidationDialog()
{
    if (m_shortageValidationDialog != nullptr) {
        if (m_shortageValidationDialog->isMinimized())
            m_shortageValidationDialog->showNormal();
        m_shortageValidationDialog->show();
        m_shortageValidationDialog->raise();
        m_shortageValidationDialog->activateWindow();
        return;
    }

    auto *dialog = new ShortageValidationDialog(m_testController, this);
    m_shortageValidationDialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}
```

- [ ] **Step 4: 让主窗口复用唯一配置测试窗口**

在 `mainwindow.h` 前置声明 `ShortageConfigDialog` 并增加：

```cpp
/// 自动失效的缺料配置测试窗口指针；重复点击入口只恢复和置顶现有窗口。
QPointer<ShortageConfigDialog> m_shortageConfigDialog;
```

替换 `onOpenShortageConfigDialog()`：

```cpp
void MainWindow::onOpenShortageConfigDialog()
{
    if (m_shortageConfigDialog != nullptr) {
        // 修改前重复点击会创建多个窗口；现在恢复唯一窗口，不改变采样或测试账本。
        if (m_shortageConfigDialog->isMinimized())
            m_shortageConfigDialog->showNormal();
        m_shortageConfigDialog->show();
        m_shortageConfigDialog->raise();
        m_shortageConfigDialog->activateWindow();
        return;
    }

    auto *dialog = new ShortageConfigDialog(ShortageConfigStore::sheet3Defaults(),
                                            [] { return ShortageEditConditions {}; },
                                            m_devMgr->shortageTestController(),
                                            m_shortageSnapshot,
                                            this);
    m_shortageConfigDialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}
```

- [ ] **Step 5: 运行入口和唯一窗口测试并确认 GREEN**

Run:

```bash
cmake --build build-shortage --target shortage_dialog_tests live_shortage_ui_tests wh-robot-visual --parallel
QT_QPA_PLATFORM=offscreen ctest --test-dir build-shortage \
  -R '^(shortage_dialog_tests|live_shortage_ui_tests)$' --output-on-failure
```

Expected：2/2 通过，主程序链接成功；重复入口只存在一个配置窗口和一个验证窗口。

- [ ] **Step 6: 检查并提交 Task 4**

Run: `git diff --check`

Expected：无输出。

```bash
git add src/shortagevalidationdialog.h src/shortageconfigdialog.h \
  src/shortageconfigdialog.cpp src/mainwindow.h src/mainwindow.cpp \
  tests/test_shortage_dialog.cpp tests/test_live_shortage_ui.cpp
git commit -m "接入可隐藏缺料验证入口与唯一窗口"
```

---

### Task 5: 全量回归、现场验证清单和用户文档同步

**Files:**

- Modify: `docs/superpowers/specs/2026-07-14-shortage-test-bugfix.md`
- Modify: `docs/superpowers/specs/2026-07-14-shortage-validation-console-design.md`（仅在实际实现与设计存在已确认差异时）
- Modify: `README.md`
- Modify: `changelog/CHANGELOG.md`

**Interfaces:**

- Consumes: Tasks 1～4 的最终行为和测试结果。
- Produces: 可重复执行的本地验证证据、现场 VT-01～VT-15 空白结果表和用户可见说明。

- [ ] **Step 1: 在干净构建目录配置 Linux Debug**

Run:

```bash
cmake -E remove_directory build-shortage-validation
/opt/Qt/qt6.8/6.8.3/gcc_64/bin/qt-cmake \
  -S . \
  -B build-shortage-validation \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Debug
```

Expected：配置成功，Qt 版本为 6.8.3，新 `shortage_validation_dialog_tests` target 可见。

- [ ] **Step 2: 构建主程序和全部测试**

Run:

```bash
cmake --build build-shortage-validation --parallel
```

Expected：`wh-robot-visual` 和全部测试 target 构建成功，无编译或链接错误。

- [ ] **Step 3: 运行全量 CTest**

Run:

```bash
QT_QPA_PLATFORM=offscreen ctest \
  --test-dir build-shortage-validation \
  --output-on-failure
```

Expected：100% tests passed，0 tests failed；既有 AGV、视觉、扫码、机械臂、码垛、FIFO 和缺料测试全部不回归。

- [ ] **Step 4: 运行边界和注释契约扫描**

Run:

```bash
rg -n 'ShortageEngine|LineManager|TaskQueue|AgvController|HuayanScheduler|CustomSysScheduler' \
  src/shortagevalidationdialog.h src/shortagevalidationdialog.cpp
rg -n 'exec\(\)' src/shortageconfigdialog.cpp src/shortagevalidationdialog.cpp \
  src/mainwindow.cpp
rg -n 'enum class|struct |ShortageValidation|ShortageTestInputSource|m_shortageValidationDialog|m_shortageConfigDialog' \
  src/shortagetypes.h src/shortagetestcontroller.h src/shortagevalidationdialog.h \
  src/shortageconfigdialog.h src/mainwindow.h
git diff --check
```

Expected：第一条无输出；第二条不在两个缺料窗口打开路径发现 `exec()`；第三条命中项均有相邻中文注释；`git diff --check` 无输出。

- [ ] **Step 5: 同步中文用户说明和现场结果表**

在 `README.md` 增加以下用户可见事实：

```text
缺料配置窗口中的“完整逻辑测试”支持手工 actualQty 和隔离的现场采样；测试状态不进入正式 FIFO、不控制硬件。现场验证阶段可通过第三个“验证向导”入口打开独立控制台，两个窗口均可最小化、最大化并与主页面切换。
```

在 `changelog/CHANGELOG.md` 增加：

```text
- 修复缺料手工源缺少 actualQty、选择手工源仍启动真实采样、首次补料单不可见和测试状态不刷新的问题。
- 新增独立缺料验证控制台，提供 VT-01～VT-15 中文步骤和通过条件；入口可在现场验收后隐藏。
- 缺料测试窗口和验证控制台支持最小化、最大化及唯一窗口复用。
```

在 `2026-07-14-shortage-test-bugfix.md` 末尾增加现场结果表，固定列为：`编号 | 执行人 | 执行时间 | 输入/现场条件 | 实际结果 | 证据文件 | 结论`。VT-01～VT-15 每项一行，初始结论写“未执行”，不得预填“通过”。

- [ ] **Step 6: 复核运行时未跟踪文件未被修改**

Run:

```bash
git status --short
git diff -- test-state.json test-state.backup.json test-events.jsonl
```

Expected：三个现场文件仍为用户原有未跟踪状态，`git diff` 无输出，任何提交暂存区不包含它们。

- [ ] **Step 7: 提交验证和文档同步**

```bash
git add README.md changelog/CHANGELOG.md \
  docs/superpowers/specs/2026-07-14-shortage-test-bugfix.md
git add docs/superpowers/specs/2026-07-14-shortage-validation-console-design.md
git commit -m "同步缺料验证说明与现场清单"
```

提交前如果设计文档没有实际差异，第二条 `git add` 不会暂存内容，提交仍只包含真实修改。

## 需求覆盖自检

| 设计要求 | 实施任务 |
| --- | --- |
| 手工源产品/模式/actualQty，且不访问真实系统 | Task 1、Task 2、VT-03 |
| 现场源才允许启动采样 | Task 1、Task 2、VT-05、VT-15 |
| 首次稳定样本显示待派补料单 | Task 1、Task 2、VT-04、VT-05 |
| 原页面实时 12 工位、计划和补料单 | Task 2 |
| 原页面全部任务/故障/状态动作 | Task 2 |
| 新 Dialog 单独文件、不混入旧 UI | Task 3、Task 4 |
| 第三个 Tab 后续可隐藏 | Task 4 |
| 两个窗口最小化、最大化、关闭和主页面切换 | Task 2、Task 3、Task 4、VT-01 |
| 两个窗口唯一实例 | Task 4、VT-01 |
| 15 项步骤、预期、实际和通过条件 | Task 3、Task 5 |
| 自动结果与现场人工确认分离 | Task 3 |
| 测试不改正式账本/FIFO/硬件 | Tasks 1～5、VT-02 |
| plan 和实际代码中文注释 | Global Constraints、Tasks 1～5 |
| 本地全量构建和 CTest | Task 5 |
