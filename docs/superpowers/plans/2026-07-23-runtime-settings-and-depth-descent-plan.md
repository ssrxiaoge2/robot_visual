# 运行参数设置与视觉深度自动下探实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 增加可持久化、分类显示且运行中只读的现场参数设置，并在视觉深度超过 1200 mm 时按 200 mm 步长自动下探、最多累计 400 mm 后安全失败。

**架构：** 使用强类型 `RuntimeSettings` 保存运行参数，`SettingsManager` 负责 INI 校验与原子持久化，`DeviceManager` 以完整快照向业务模块提交配置。深度下探决策提取为纯策略，`HuayanScheduler` 仅把决策接入现有命令队列和状态机。

**技术栈：** C++17、Qt 6 Core/Widgets/Test、CMake、CTest、QSettings、QSaveFile。

---

## 文件结构与职责

- 创建 `src/runtimesettings.h/.cpp`：强类型配置、元数据、校验、分类恢复、差异和深度下探纯策略。
- 创建 `src/settingsmanager.h/.cpp`：INI 加载、无效值回退、原子保存和当前快照。
- 创建 `src/settingsdialog.h/.cpp`：左侧分类导航、参数编辑、变更预览和风险确认。
- 创建 `tests/test_runtime_settings.cpp`、`tests/test_settings_manager.cpp`、`tests/test_settings_dialog.cpp`、`tests/test_depth_descent_contract.cpp`。
- 修改 `src/lineconfig.h`：大篮筐默认 417 mm 和运行时余量覆盖。
- 修改 `src/huayanScheduler.h/.cpp`：配置快照和深度自动下探状态机。
- 修改 `src/devicemanager.h/.cpp`：拥有设置管理器并事务式应用。
- 修改 `src/mainwindow.h/.cpp`：复位右侧设置入口。
- 修改根目录和 `tests/CMakeLists.txt`：注册源文件与测试。

## 任务 1：强类型配置、校验与深度策略

**文件：**
- 创建：`src/runtimesettings.h`
- 创建：`src/runtimesettings.cpp`
- 创建：`tests/test_runtime_settings.cpp`
- 修改：`tests/CMakeLists.txt`

- [ ] **步骤 1：编写失败的默认值和校验测试**

```cpp
void RuntimeSettingsTest::defaultsMatchApprovedSpec()
{
    const RuntimeSettings s = RuntimeSettings::defaults();
    QCOMPARE(s.pickup.largeBasketGrabZClearanceMm, 417.0);
    QCOMPARE(s.pickup.purpleBasketGrabZClearanceMm, 380.0);
    QVERIFY(s.depthDescent.enabled);
    QCOMPARE(s.depthDescent.triggerDepthMm, 1200.0);
    QCOMPARE(s.depthDescent.stepMm, 200.0);
    QCOMPARE(s.depthDescent.maxAccumulatedMm, 400.0);
}

void RuntimeSettingsTest::rejectsStepGreaterThanMaximum()
{
    RuntimeSettings s = RuntimeSettings::defaults();
    s.depthDescent.stepMm = 500.0;
    QVERIFY(!validateRuntimeSettings(s).ok);
}
```

另写分类恢复、非正超时、NaN、深度等于阈值不移动、剩余距离截断和预算耗尽失败测试。

- [ ] **步骤 2：注册并运行测试，确认失败**

```powershell
cmake --build build --target runtime_settings_tests
```

预期：FAIL，找不到 `RuntimeSettings`。

- [ ] **步骤 3：实现稳定接口**

```cpp
enum class SettingsCategory { Pickup, VisionClosedLoop, DepthDescent,
                              Search, Motion, SafetyAndTimeouts };

struct RuntimeSettings {
    struct Pickup {
        double largeBasketGrabZClearanceMm = 417.0;
        double purpleBasketGrabZClearanceMm = 380.0;
        double grabXCompensationMm = 30.0;
        double grabYCompensationMm = -17.0;
        bool zDescendInvert = false;
    } pickup;
    struct DepthDescent {
        bool enabled = true;
        double triggerDepthMm = 1200.0;
        double stepMm = 200.0;
        double maxAccumulatedMm = 400.0;
    } depthDescent;
    struct VisionClosedLoop {
        double xyToleranceMm = 2.0;
        double rzToleranceDeg = 1.0;
        int maxGrabIterations = 15;
        int settleMs = 2000;
        double largeRzJumpThresholdDeg = 80.0;
        double largeRzDeltaToleranceDeg = 15.0;
        int maxLargeRzExecutions = 1;
        double stationRoiHalfXmm = 500.0;
        double stationRoiHalfYmm = 500.0;
        double anchorMaxTrustXmm = 300.0;
        double anchorMaxTrustYmm = 450.0;
        double anchorSameLayerToleranceMm = 20.0;
        double anchorSwitchMaxXyMm = 220.0;
        int lockMaxMissingFrames = 3;
        double lockTrackRadiusMm = 260.0;
        double lockSameLayerToleranceMm = 80.0;
    } vision;
    struct Search {
        double descendStepMm = 20.0;
        double maxAccumulatedMm = 80.0;
        int settleMs = 2000;
    } search;
    struct Motion {
        int speedPercent = 100;
        double velocity = 50.0;
        double acceleration = 100.0;
        double radius = 0.0;
        double scanRecoveryRotationDeg = 180.0;
    } motion;
    struct Safety {
        double maxSingleXyAdjustMm = 250.0;
        double maxZDescendMm = 1078.0;
        int normalMotionTimeoutMs = 30000;
        int longZMotionTimeoutMs = 120000;
        int commandReadyTimeoutMs = 8000;
        int resetSettleMs = 1000;
        int pollIntervalMs = 100;
        int shortMotionFallbackMs = 3000;
    } safety;
    static RuntimeSettings defaults();
};

struct SettingsValidation { bool ok; QStringList errors; };
struct DepthDescentDecision {
    enum class Action { ContinuePickup, MoveDown, FailLimitReached } action;
    double moveMm = 0.0;
};

SettingsValidation validateRuntimeSettings(const RuntimeSettings &);
RuntimeSettings restoreCategoryDefaults(const RuntimeSettings &, SettingsCategory);
DepthDescentDecision decideDepthDescent(double depthMm, double accumulatedMm,
    const RuntimeSettings::DepthDescent &settings);
```

- [ ] **步骤 4：实现最小策略**

```cpp
if (!settings.enabled || depthMm <= settings.triggerDepthMm)
    return {DepthDescentDecision::Action::ContinuePickup, 0.0};
const double remaining = settings.maxAccumulatedMm - accumulatedMm;
if (remaining <= 0.0)
    return {DepthDescentDecision::Action::FailLimitReached, 0.0};
return {DepthDescentDecision::Action::MoveDown,
        qMin(settings.stepMm, remaining)};
```

- [ ] **步骤 5：运行测试并提交**

```powershell
cmake --build build --target runtime_settings_tests
ctest --test-dir build -R runtime_settings_tests --output-on-failure
git add src/runtimesettings.h src/runtimesettings.cpp tests/test_runtime_settings.cpp tests/CMakeLists.txt
git commit -m "feat: add typed runtime settings"
```

预期：测试全部通过。

## 任务 2：INI 加载和原子持久化

**文件：**
- 创建：`src/settingsmanager.h`
- 创建：`src/settingsmanager.cpp`
- 创建：`tests/test_settings_manager.cpp`
- 修改：`tests/CMakeLists.txt`

- [ ] **步骤 1：使用 `QTemporaryDir` 编写失败测试**

覆盖：文件缺失返回默认；合法值重载；单项类型错误只回退该项；深度字段组约束失败时整组回退；保存失败不改变旧文件。

```cpp
QSettings ini(path, QSettings::IniFormat);
ini.setValue("depthDescent/stepMm", 500.0);
ini.setValue("depthDescent/maxAccumulatedMm", 400.0);
ini.sync();
SettingsManager manager(path);
const SettingsLoadResult result = manager.load();
QCOMPARE(result.settings.depthDescent.stepMm, 200.0);
QCOMPARE(result.settings.depthDescent.maxAccumulatedMm, 400.0);
QVERIFY(result.warnings.join('\n').contains(QStringLiteral("深度自动下探")));
```

- [ ] **步骤 2：运行并确认缺少类型而失败**

```powershell
cmake --build build --target settings_manager_tests
```

- [ ] **步骤 3：实现接口和稳定键名**

```cpp
struct SettingsLoadResult { RuntimeSettings settings; QStringList warnings; };
class SettingsManager : public QObject {
    Q_OBJECT
public:
    explicit SettingsManager(QString iniPath, QObject *parent = nullptr);
    SettingsLoadResult load();
    const RuntimeSettings &current() const;
    bool stageCandidate(const RuntimeSettings &, QString *error);
    bool commitStaged(const RuntimeSettings &, QString *error);
    void discardStaged();
};
```

键名使用 `pickup/largeBasketGrabZClearanceMm`、`depthDescent/triggerDepthMm` 等。检查 QVariant 转换和 `std::isfinite()`。`stageCandidate()` 把完整配置写入同目录暂存 INI 并检查 `QSettings::status()`；`commitStaged()` 用 `QSaveFile` 原子替换正式文件并更新当前快照；`discardStaged()` 删除暂存文件。任何失败不得更新当前快照。

- [ ] **步骤 4：运行测试并提交**

```powershell
cmake --build build --target settings_manager_tests
ctest --test-dir build -R settings_manager_tests --output-on-failure
git add src/settingsmanager.h src/settingsmanager.cpp tests/test_settings_manager.cpp tests/CMakeLists.txt
git commit -m "feat: persist runtime settings atomically"
```

## 任务 3：工位余量默认值和运行时覆盖

**文件：**
- 修改：`src/lineconfig.h`
- 修改：`tests/test_station_pickup_config.cpp`

- [ ] **步骤 1：先写 417/380 和覆盖测试**

```cpp
QCOMPARE(lineconfig_detail::kLargeBasketGrabZClearance, 417.0);
QCOMPARE(lineconfig_detail::kPurpleBasketGrabZClearance, 380.0);
for (int station = 1; station <= 11; ++station)
    QCOMPARE(stationTaskConfig(station)->grabZClearance, 417.0);
QCOMPARE(stationTaskConfig(12)->grabZClearance, 380.0);
```

另测 `stationTaskConfig(station, settings)` 仅覆盖副本，不修改静态配置表。

- [ ] **步骤 2：运行测试确认旧值 412 导致失败**

```powershell
cmake --build build --target station_pickup_config_tests
ctest --test-dir build -R station_pickup_config_tests --output-on-failure
```

- [ ] **步骤 3：实现返回值式覆盖查询**

```cpp
std::optional<StationTaskConfig> stationTaskConfig(
    int stationId, const RuntimeSettings &settings);
```

工位 1–11 覆盖为大篮筐值，工位 12 覆盖为紫筐值。

- [ ] **步骤 4：运行测试并提交**

```powershell
ctest --test-dir build -R station_pickup_config_tests --output-on-failure
git add src/lineconfig.h tests/test_station_pickup_config.cpp
git commit -m "feat: make basket grab clearance configurable"
```

## 任务 4：HuayanScheduler 配置快照

**文件：**
- 修改：`src/huayanScheduler.h`
- 修改：`src/huayanScheduler.cpp`
- 修改：`src/visionclient.h`
- 修改：`src/visionclient.cpp`
- 修改：`tests/test_huayan_scheduler_contract.cpp`
- 修改：`tests/test_locked_target_selection.cpp`

- [ ] **步骤 1：先写契约测试**

验证存在 `applyRuntimeSettings(const RuntimeSettings &)` 和 `runtimeSettings() const`，并验证 `kGrabTolerance`、`kSearchDescendStep`、`HUAYAN_MAX_SINGLE_XY_ADJUST_MM` 等迁移目标不再直接驱动运行逻辑。

- [ ] **步骤 2：运行契约测试确认失败**

```powershell
cmake --build build --target huayan_scheduler_contract_tests
ctest --test-dir build -R huayan_scheduler_contract_tests --output-on-failure
```

- [ ] **步骤 3：实现配置应用**

```cpp
void HuayanScheduler::applyRuntimeSettings(const RuntimeSettings &settings)
{
    Q_ASSERT(validateRuntimeSettings(settings).ok);
    Q_ASSERT(!isBusy());
    m_runtimeSettings = settings;
    m_pollTimer->setInterval(settings.safety.pollIntervalMs);
    m_commandReadyTimer->setInterval(settings.safety.pollIntervalMs);
}
```

逐项替换调度器内编译期常量引用；保留 SDK 状态码、协议常量和内部计数器。为 `VisionHttpClient` 增加 `applyRuntimeSettings(const RuntimeSettings &)`，把 ROI、锚点可信边界、同层容差、锁定半径和丢帧阈值从宏迁移到配置快照；扩展 `test_locked_target_selection.cpp` 验证自定义配置实际影响候选选择。

- [ ] **步骤 4：运行回归测试并提交**

```powershell
cmake --build build --target huayan_scheduler_contract_tests vision_target_selection_tests anchor_target_selection_tests locked_target_selection_tests
ctest --test-dir build -R "huayan_scheduler_contract_tests|vision_target_selection_tests|anchor_target_selection_tests|locked_target_selection_tests" --output-on-failure
git add src/huayanScheduler.h src/huayanScheduler.cpp src/visionclient.h src/visionclient.cpp tests/test_huayan_scheduler_contract.cpp tests/test_locked_target_selection.cpp
git commit -m "refactor: apply runtime settings to huayan scheduler"
```

## 任务 5：视觉深度自动下探状态机

**文件：**
- 修改：`src/huayanScheduler.h`
- 修改：`src/huayanScheduler.cpp`
- 创建：`tests/test_depth_descent_contract.cpp`
- 修改：`tests/CMakeLists.txt`

- [ ] **步骤 1：编写失败的接入契约测试**

验证视觉成功回调先调用 `decideDepthDescent()`；独立使用 `m_depthDescentAccumulatedMm`；移动进入 `PendingCommand`；完成后等待稳定并发出 `surveyReady()`；所有终止路径调用 `resetDepthDescentState()`。

- [ ] **步骤 2：运行测试确认失败**

```powershell
cmake --build build --target depth_descent_contract_tests
```

- [ ] **步骤 3：实现状态和入口**

```cpp
double m_depthDescentAccumulatedMm = 0.0;
void resetDepthDescentState();
bool handleExcessiveVisionDepth(double depthMm);
void executeDepthDescent(double moveMm);
```

`handleExcessiveVisionDepth()` 返回 true 表示本帧已被消费，调用者不得继续 X/Y/Rz。MoveDown 使用 `poseId=2`、配置的 Z 方向和策略截断距离，并复用全局 Z 安全检查。

- [ ] **步骤 4：实现回调、清零和日志**

移动完成后累加计划距离，等待配置的视觉稳定时间后发出 `surveyReady()`。开始、完成、停止、复位、错误均清零。预算耗尽发出阶段错误。日志格式包含：

```text
[深度下探] depth=...mm > threshold=...mm，本次=...mm，累计=.../...mm
```

- [ ] **步骤 5：运行测试并提交**

```powershell
cmake --build build --target runtime_settings_tests depth_descent_contract_tests huayan_scheduler_contract_tests
ctest --test-dir build -R "runtime_settings_tests|depth_descent_contract_tests|huayan_scheduler_contract_tests" --output-on-failure
git add src/huayanScheduler.h src/huayanScheduler.cpp tests/test_depth_descent_contract.cpp tests/CMakeLists.txt
git commit -m "feat: add vision depth auto descent"
```

## 任务 6：分类设置对话框

**文件：**
- 创建：`src/settingsdialog.h`
- 创建：`src/settingsdialog.cpp`
- 创建：`tests/test_settings_dialog.cpp`
- 修改：`tests/CMakeLists.txt`

- [ ] **步骤 1：编写失败的 UI 测试**

使用 QSignalSpy 和对象名验证六个分类、运行锁定、分类恢复、取消不发信号、保存发候选配置、安全变更必须确认。稳定对象名：`settingsCategoryList`、`settingsStack`、`restoreCategoryDefaultsButton`、`saveAndApplyButton`、`runtimeLockedBanner`、`safetyAcknowledgementCheckBox`。

- [ ] **步骤 2：运行测试确认失败**

```powershell
cmake --build build --target settings_dialog_tests
```

- [ ] **步骤 3：实现公开接口和布局**

```cpp
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(const RuntimeSettings &current,
                            bool runtimeLocked, QWidget *parent = nullptr);
    RuntimeSettings candidate() const;
signals:
    void saveRequested(const RuntimeSettings &candidate);
};
```

左侧 QListWidget、右侧 QStackedWidget；数值控件显示单位、范围、说明；安全字段警告色；保存前用强类型差异函数显示“旧值 → 新值”，包含安全变更时强制风险勾选。

- [ ] **步骤 4：运行测试并提交**

```powershell
cmake --build build --target settings_dialog_tests
ctest --test-dir build -R settings_dialog_tests --output-on-failure
git add src/settingsdialog.h src/settingsdialog.cpp tests/test_settings_dialog.cpp tests/CMakeLists.txt
git commit -m "feat: add categorized runtime settings dialog"
```

## 任务 7：DeviceManager 事务应用和主窗口入口

**文件：**
- 修改：`src/devicemanager.h`
- 修改：`src/devicemanager.cpp`
- 修改：`src/mainwindow.h`
- 修改：`src/mainwindow.cpp`
- 修改：`CMakeLists.txt`
- 修改：`tests/test_standalone_pickup_contract.cpp`

- [ ] **步骤 1：先写集成契约测试**

验证 DeviceManager 唯一拥有 SettingsManager；启动加载并应用；锁定同时检查 `HuayanScheduler::isBusy()`、`LineManager::state() != Idle` 和兼容流程；候选配置全有或全无；`m_btnSettings` 紧跟 `m_btnReset`。

- [ ] **步骤 2：运行测试确认失败**

```powershell
cmake --build build --target standalone_pickup_contract_tests
ctest --test-dir build -R standalone_pickup_contract_tests --output-on-failure
```

- [ ] **步骤 3：实现 DeviceManager 接口**

```cpp
const RuntimeSettings &runtimeSettings() const;
bool runtimeSettingsLocked() const;
bool applyRuntimeSettingsCandidate(const RuntimeSettings &, QString *error);
```

顺序为：完整校验、确认未运行、各模块无副作用预检、调用 `stageCandidate()` 写暂存文件、保存旧模块快照、提交 HuayanScheduler/VisionHttpClient/工位配置覆盖、调用 `commitStaged()` 原子替换正式文件。模块提交失败时恢复旧模块快照并 `discardStaged()`；正式文件提交失败时同样恢复模块快照，保证全有或全无。

- [ ] **步骤 4：实现设置按钮**

```cpp
m_btnSettings = new QPushButton(QStringLiteral("⚙  设置"));
m_btnSettings->setFixedHeight(32);
toolbar->addWidget(m_btnReset);
toolbar->addWidget(m_btnSettings);
```

点击时传入当前快照和锁定状态。保存成功写变更日志并关闭；失败显示错误并保留编辑内容。

- [ ] **步骤 5：更新 CMake、构建并提交**

```powershell
cmake --build build --target wh-robot-visual
ctest --test-dir build -R "standalone_pickup_contract_tests|settings_dialog_tests|settings_manager_tests" --output-on-failure
git add src/devicemanager.h src/devicemanager.cpp src/mainwindow.h src/mainwindow.cpp CMakeLists.txt tests/test_standalone_pickup_contract.cpp
git commit -m "feat: integrate runtime settings into main window"
```

## 任务 8：全量验证与现场验收准备

**文件：**
- 仅在验证发现事实差异时修改本功能文件；不得改动用户原有未跟踪文件。

- [ ] **步骤 1：检查差异和空白错误**

```powershell
git diff --check
git status --short
```

预期：无空白错误；`.superpowers/`、`.codegraph/`、`scripts/__pycache__/` 和用户原有文档未被暂存。

- [ ] **步骤 2：构建全部目标**

```powershell
cmake --build build
```

预期：构建成功。

- [ ] **步骤 3：运行全部测试**

```powershell
ctest --test-dir build --output-on-failure
```

预期：`100% tests passed, 0 tests failed`。

- [ ] **步骤 4：执行无机械臂 UI 验收**

核对设置按钮位置、六个分类、417/380/1200/200/400 默认值、分类恢复、安全确认、运行锁定、取消不保存和重启加载。

- [ ] **步骤 5：准备现场机械臂验收**

在安全区域验证：深度 ≤1200 不下探；深度 >1200 下探 200 后重拍；一次后达标继续抓取；两次累计 400 仍超限则报警且不发第三次命令；停止和新任务清零。无设备时不得声称现场验收通过。

- [ ] **步骤 6：仅在有实际修正时提交**

```powershell
git add src tests CMakeLists.txt
git commit -m "test: verify runtime settings workflow"
```

没有修正时不创建空提交。
