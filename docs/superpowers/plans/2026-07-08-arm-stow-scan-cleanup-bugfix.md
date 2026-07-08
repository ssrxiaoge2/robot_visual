# 机械臂收姿态、扫码搜索与 Cleanup 修复实施计划

> **给执行代理：** 必须使用 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans` 按任务逐项实施。本计划使用 checkbox（`- [ ]`）跟踪进度。

**目标：** 修复三类现场 Bug：倒料后收姿态按工位配置、扫码搜索成功后任务继续推进、Cleanup 收姿态按华沿 SDK RunFunc/FSM 语义等待并记录诊断。

**架构：** 保持现有 `TaskExecutor` 负责任务编排、`HuayanScheduler` 负责机械臂动作、`lineconfig.h` 负责工位差异配置的边界。工位差异进入配置表；状态机只增加缺失分支和 RunFunc 完成判定，不改变已有正常流程语义。

**技术栈：** C++17、Qt、CMake/CTest、华沿 HRIF SDK、现有 `test_station_pickup_config` C++ 回归测试。

## 全局约束

- 不改动原有正常流程逻辑：首轮扫码成功、取料后收姿态、倒料、码垛失败转任务失败等既有语义保持不变。
- 不使用 Python 作为验证测试；新增或修改的自动化验证使用 C++/CMake 或现有 CTest。
- 一个 Bug 修复对应一个代码 commit；文档 commit 单独提交；不 push。
- 不能硬编码工位 3 特例到状态机分支里，工位差异必须进入 `lineconfig.h` 的工位配置表。
- 问题 3 不能写成“工位 5 专属根因已完全证明”。应按 SDK demo 证据补强 RunFunc/FSM 诊断和等待。

---

## 文件结构

- `src/lineconfig.h`：新增 `StationTaskConfig::stowAfterUnloadFunc`，集中配置 12 工位倒料后收姿态函数。
- `src/huayanScheduler.h`：扩展 `StationArmFunctions`；新增收姿态函数选择接口；为 RunFunc/FSM 诊断增加小型结构体和 helper。
- `src/huayanScheduler.cpp`：按场景选择收姿态函数；RunFunc 命令完成时等待 FSM 离开 `34 ScriptRunning`；输出 Cleanup 诊断日志。
- `src/taskexecutor.h`：新增 `isPickupCompletionState(ExecState state)` 和收姿态场景调用声明需要的成员。
- `src/taskexecutor.cpp`：注入配置；在 `StowAfterUnload` 前选择倒料后收姿态函数；让扫码搜索成功路径进入取料完成分支。
- `tests/test_station_pickup_config.cpp`：使用 C++ 静态/纯函数回归检查覆盖三个 Bug。
- `tests/CMakeLists.txt`：保留或登记 C++ 测试目标；不新增 Python 测试。
- `changelog/CHANGELOG.md`：记录本次三个 Bug 修复和现场验证要点。

## 全局命名与注释要求

- 新增变量名必须表达业务场景，不使用泛化名称。倒料后收姿态函数统一命名为 `stowAfterUnloadFunc`，表示“倒料完成后从当前倒料点回运行安全位的示教器函数”。
- 新增一次性收姿态函数接口统一命名为 `setNextStowFunction(const QString &funcName)`，表示“只影响下一次 `startStow()` 调用”，不能被误解为永久修改默认收姿态函数。
- 新增扫码完成状态 helper 统一命名为 `isPickupCompletionState(ExecState state) const`，表示“阶段一完成后是否应按取料完成推进”。
- 新增机器人状态诊断结构体统一命名为 `RobotStateSnapshot`，字段必须保留 `movingState`、`pauseState`、`errorState`、`errorCode`、`nCurFSM`、`strCurFSM`、`valid`。
- 所有新增注释使用中文，注释说明业务原因和安全边界，不逐行复述代码。
- 修改 `TaskExecutor` 状态推进时，注释必须说明“为什么该状态允许进入取料完成分支”；修改 `HuayanScheduler` RunFunc 等待时，注释必须说明“华沿 SDK demo 中 34 表示 ScriptRunning，RunFunc 未结束前不能下发下一条 RunFunc”。

## 现场分析摘要

问题 1 根因明确：当前 `HuayanScheduler::startStow()` 固定调用 `Func_yun_xing_zhong`，无法表达工位 3 从倒料点经示教器过渡点回安全点的路径。应新增 12 工位配置项，默认仍为 `Func_yun_xing_zhong`。

问题 2 根因明确：扫码第 2/3 轮成功后，任务状态经过 `PreGripScanSearchReturn` 回到原夹取位并继续夹紧，但阶段一完成回调没有把 `PreGripScanSearchReturn` 当作取料完成状态处理，导致后续不进入收姿态或倒料。

问题 3 直接故障位置明确，最终根因需靠诊断确认：工位 5 正常取料、倒料和倒料后收姿态均成功，异常发生在码垛函数缺失后的 Cleanup 收姿态。华沿 SDK demo 显示 `RunFunc()` 后应等待 FSM 离开 `34 ScriptRunning`，因此本次按 RunFunc/FSM 时序补强，不做工位 5 专属修复。

### 任务 1：Bug 1 - 倒料后收姿态函数按工位配置

**文件：**
- 修改：`robot_visual20260625/robot_visual/src/lineconfig.h`
- 修改：`robot_visual20260625/robot_visual/src/huayanScheduler.h`
- 修改：`robot_visual20260625/robot_visual/src/huayanScheduler.cpp`
- 修改：`robot_visual20260625/robot_visual/src/taskexecutor.cpp`
- 修改：`robot_visual20260625/robot_visual/tests/test_station_pickup_config.cpp`

**接口：**
- 使用现有接口：`StationTaskConfig`、`HuayanScheduler::StationArmFunctions`、`HuayanScheduler::startStow()`。
- 新增接口：`StationTaskConfig::stowAfterUnloadFunc`、`StationArmFunctions::stowAfterUnloadFunc`、`HuayanScheduler::setNextStowFunction(const QString &funcName)`。

**新增/修改的函数、接口、变量：**
- `StationTaskConfig::stowAfterUnloadFunc`：新增 `QString` 字段，配置当前工位倒料后回运行安全位的示教器函数。
- `lineconfig_detail::kStationTaskConfigs`：每个工位新增一个配置值；工位 3 使用 `Func_yun_xing_zhong_s3`，其他工位使用 `Func_yun_xing_zhong`。
- `HuayanScheduler::StationArmFunctions::stowAfterUnloadFunc`：新增 `QString` 字段，用于从 `TaskExecutor` 注入工位配置。
- `HuayanScheduler::setNextStowFunction(const QString &funcName)`：新增 public 方法，只设置下一次 `startStow()` 要调用的函数。
- `HuayanScheduler::m_nextStowFuncName`：新增 private 成员，一次性保存下一次收姿态函数名，使用后必须清空。
- `TaskExecutor::start()`：给 `stationFuncs.stowAfterUnloadFunc` 赋值。
- `TaskExecutor::enterState(ExecState::StowAfterUnload, ...)`：调用 `setNextStowFunction(m_stationCfg->stowAfterUnloadFunc)` 后再 `startStow()`。

**必须增加的中文注释：**
- 在 `StationTaskConfig::stowAfterUnloadFunc` 字段旁注释：这是“倒料后”从当前倒料点回运行安全位的函数，不是夹紧后回安全高度函数。
- 在工位 3 配置行附近注释：工位 3 的示教器函数内部包含“倒料点 -> 过渡点 -> 安全点”的路径。
- 在 `setNextStowFunction()` 声明旁注释：该接口只影响下一次收姿态，不修改 UI/手动路径默认函数。
- 在 `TaskExecutor::enterState()` 的 `StowAfterUnload` 分支注释：只有倒料后收姿态走工位配置；取料后、码垛后、Cleanup 保持原默认函数。

- [ ] **步骤 1：编写失败的 C++ 配置测试**

编辑 `tests/test_station_pickup_config.cpp`，在现有 `s12` 配置检查之后加入：

```cpp
    const StationTaskConfig *s3 = stationConfig(3);
    requireTrue(s3 != nullptr, "工位3配置必须存在");
    requireTrue(s3->stowAfterUnloadFunc == QStringLiteral("Func_yun_xing_zhong_s3"),
                "工位3倒料后收姿态必须使用带过渡点的新函数");

    for (int station = 1; station <= 12; ++station) {
        const StationTaskConfig *cfg = stationConfig(station);
        requireTrue(cfg != nullptr, "12工位配置必须完整");
        requireTrue(!cfg->stowAfterUnloadFunc.isEmpty(),
                    "每个工位都必须显式配置倒料后收姿态函数");
        if (station != 3) {
            requireTrue(cfg->stowAfterUnloadFunc == QStringLiteral("Func_yun_xing_zhong"),
                        "非工位3默认使用原全局收姿态函数");
        }
    }
```

在已有 `schedulerSource` 检查附近再加入静态源码检查：

```cpp
    requireTrue(schedulerSource.contains(QStringLiteral("setNextStowFunction")),
                "HuayanScheduler 必须提供下一次收姿态函数选择接口");
    requireTrue(taskExecutorSource.contains(QStringLiteral("setNextStowFunction(m_stationCfg->stowAfterUnloadFunc)")),
                "TaskExecutor 必须在倒料后收姿态前注入当前工位函数");
```

- [ ] **步骤 2：运行 C++ 测试并确认失败**

在 `robot_visual20260625/robot_visual` 目录运行：

```bash
cmake --build build --target test_station_pickup_config
./build/tests/test_station_pickup_config
```

预期：编译失败并提示 `StationTaskConfig` 没有 `stowAfterUnloadFunc` 成员，或测试程序失败并输出“工位3倒料后收姿态必须使用带过渡点的新函数”。

- [ ] **步骤 3：扩展工位配置**

修改 `src/lineconfig.h`。

在 `StationTaskConfig` 结构体中，于 `unloadFunc` 后增加 `stowAfterUnloadFunc` 字段，结构体应保持如下字段顺序：

```cpp
struct StationTaskConfig {
    int stationId = 0;
    int pickupLm = 0;
    int unloadLm = 0;
    PalletArea palletArea = PalletArea::LargeBox;
    QString captureFunc;
    AfterGripMode afterGripMode = AfterGripMode::CaptureFunc;
    QString afterGripFunc;
    QString unloadPointFunc;
    QString unloadFunc;
    QString stowAfterUnloadFunc;
    double grabZClearance = 425.0;
};
```

更新 `kStationTaskConfigs` 的 12 行配置，使新字段位于 `unloadFunc` 和 `grabZClearance` 之间。工位 3 使用：

```cpp
QStringLiteral("Func_yun_xing_zhong_s3")
```

其他工位使用：

```cpp
QStringLiteral("Func_yun_xing_zhong")
```

覆盖工位 1、2、4-12。

- [ ] **步骤 4：增加 HuayanScheduler 接口**

修改 `src/huayanScheduler.h`。

在 `StationArmFunctions` 中增加：

```cpp
        QString stowAfterUnloadFunc; ///< 当前工位倒料后从倒料点回运行安全位的函数；不是夹紧后回安全高度函数。
```

在 public 方法区靠近 `startStow()` 的位置增加：

```cpp
    /// 只覆盖下一次 startStow() 调用使用的函数；执行后自动恢复默认收姿态函数。
    void setNextStowFunction(const QString &funcName);
```

在 private 成员区靠近 `m_stowFuncName` 的位置增加：

```cpp
    QString m_nextStowFuncName;
```

保持现有 `m_stowFuncName = QStringLiteral("Func_yun_xing_zhong")` 不变，作为所有现有 UI/手动路径的默认函数。

- [ ] **步骤 5：实现收姿态函数选择**

修改 `src/huayanScheduler.cpp`。

增加函数实现：

```cpp
void HuayanScheduler::setNextStowFunction(const QString &funcName)
{
    // 只影响下一次收姿态；倒料后的工位定制路径不能污染其他收姿态场景。
    m_nextStowFuncName = funcName;
}
```

在 `executeCurrentStep()` 的 `Stage::Stow`、`StageStep::StowArm` 分支中，将固定函数调用替换为：

```cpp
            const QString stowFunc = m_nextStowFuncName.isEmpty()
                ? m_stowFuncName
                : m_nextStowFuncName;
            // 一次性覆盖使用后立即清空，避免取料后、码垛后或 Cleanup 误用倒料后路径。
            m_nextStowFuncName.clear();
            emit logMessage(QStringLiteral("[收姿态] 调用 %1").arg(stowFunc));
            executeRunFunc(stowFunc);
            break;
```

这保证所有现有调用仍使用 `m_stowFuncName`，只有 `TaskExecutor` 显式设置一次性函数时才改用工位配置。

- [ ] **步骤 6：在 TaskExecutor 注入并使用工位函数**

修改 `src/taskexecutor.cpp`。

在 `TaskExecutor::start()` 中，设置 `stationFuncs.unloadFunc` 后增加：

```cpp
    stationFuncs.stowAfterUnloadFunc = m_stationCfg->stowAfterUnloadFunc;
```

在 `TaskExecutor::enterState()` 中，将 `StowAfterUnload` 从通用收姿态分支拆出：

```cpp
    case ExecState::StowAfterPickup:
    case ExecState::StowAfterPallet:
        emit logMessage(prefix(QStringLiteral("ARM")) + QStringLiteral(" 启动收姿态"));
        m_arm->startStow();
        break;
    case ExecState::StowAfterUnload:
        // 倒料点回安全位可能需要工位专属过渡点，只在倒料后收姿态使用配置函数。
        emit logMessage(prefix(QStringLiteral("ARM")) + QStringLiteral(" 启动倒料后收姿态"));
        m_arm->setNextStowFunction(m_stationCfg->stowAfterUnloadFunc);
        m_arm->startStow();
        break;
```

不要在 `CleanupStow`、`StowAfterPickup`、`StowAfterPallet` 调用 `setNextStowFunction()`。

- [ ] **步骤 7：运行 C++ 验证**

运行：

```bash
cmake --build build --target test_station_pickup_config
./build/tests/test_station_pickup_config
ctest --test-dir build --output-on-failure
```

预期：`test_station_pickup_config` 通过，CTest 报告所有已配置测试通过。

- [ ] **步骤 8：提交 Bug 1**

运行：

```bash
git add src/lineconfig.h src/huayanScheduler.h src/huayanScheduler.cpp src/taskexecutor.cpp tests/test_station_pickup_config.cpp
git commit -m "fix: configure post-unload stow per station"
```

预期：创建一个本地 commit；不要 push。

### 任务 2：Bug 2 - 扫码搜索成功后继续推进任务

**文件：**
- 修改：`robot_visual20260625/robot_visual/src/taskexecutor.h`
- 修改：`robot_visual20260625/robot_visual/src/taskexecutor.cpp`
- 修改：`robot_visual20260625/robot_visual/tests/test_station_pickup_config.cpp`

**接口：**
- 使用现有接口：`TaskExecutor::ExecState`、`TaskExecutor::onArmStageCompleted()`。
- 新增接口：`bool TaskExecutor::isPickupCompletionState(ExecState state) const`。

**新增/修改的函数、接口、变量：**
- `TaskExecutor::isPickupCompletionState(ExecState state) const`：新增 private helper，集中判断阶段一完成信号是否应按“取料完成”推进。
- `TaskExecutor::onArmStageCompleted(const QString &stageName)`：把取料完成分支改为调用 `isPickupCompletionState(m_state)`；保留原同 LM 跳过 AGV、不同 LM 进入 `StowAfterPickup` 的逻辑。
- `ExecState::PreGripScanSearchReturn`：不新增枚举，但必须纳入取料完成 helper。

**必须增加的中文注释：**
- 在 `isPickupCompletionState()` 定义前注释：扫码搜索成功后状态会停在 `PreGripScanSearchReturn`，但机械臂阶段一完成仍代表取料已完成。
- 在 `onArmStageCompleted()` 的 helper 分支注释：统一处理阶段一完成，避免不同扫码路径漏推进。
- 不要在注释中写“临时兼容”或“特殊处理工位”，因为该修复与工位无关。

- [ ] **步骤 1：编写失败的 C++ 静态回归检查**

编辑 `tests/test_station_pickup_config.cpp` 并加入：

```cpp
    requireTrue(taskExecutorSource.contains(QStringLiteral("isPickupCompletionState")),
                "TaskExecutor 必须集中判断阶段一完成状态");
    requireTrue(taskExecutorSource.contains(QStringLiteral("case ExecState::PreGripScanSearchReturn:")),
                "扫码搜索成功回原夹取位后，阶段一完成必须继续推进");
    requireTrue(taskExecutorSource.contains(QStringLiteral("if (isPickupCompletionState(m_state))")),
                "onArmStageCompleted 必须使用取料完成状态 helper");
```

- [ ] **步骤 2：运行 C++ 测试并确认失败**

运行：

```bash
cmake --build build --target test_station_pickup_config
./build/tests/test_station_pickup_config
```

预期：测试失败并输出“TaskExecutor 必须集中判断阶段一完成状态”。

- [ ] **步骤 3：声明 helper**

修改 `src/taskexecutor.h`。

在 private 方法区靠近 `isAgvNavigationState()` 的位置增加：

```cpp
    bool isPickupCompletionState(ExecState state) const;
```

- [ ] **步骤 4：实现 helper**

修改 `src/taskexecutor.cpp`，在 `isAgvNavigationState()` 附近增加：

```cpp
// 阶段一可能从首轮扫码或 Y 轴搜码返回路径完成；这些状态完成后都应按取料完成推进。
bool TaskExecutor::isPickupCompletionState(ExecState state) const
{
    switch (state) {
    case ExecState::ArmPickup:
    case ExecState::PreGripScan:
    case ExecState::PreGripScanSearchReturn:
    case ExecState::RotateForScan:
        return true;
    default:
        return false;
    }
}
```

- [ ] **步骤 5：在阶段完成回调中使用 helper**

修改 `TaskExecutor::onArmStageCompleted()`。

在函数开头、完成日志输出之后插入：

```cpp
    // 统一处理阶段一完成，避免扫码搜索成功路径停在 PreGripScanSearchReturn 后漏推进。
    if (isPickupCompletionState(m_state)) {
        if (m_stationCfg->pickupLm == m_stationCfg->unloadLm) {
            const bool returnedToCaptureSafety = m_stationCfg->afterGripMode != AfterGripMode::None;
            emit logMessage(prefix(QStringLiteral("AGV"))
                            + (returnedToCaptureSafety
                                   ? QStringLiteral(" 取料已回拍照安全高度，取料位与倒料位同一 LM%1，跳过 AGV 导航，直接进入倒料准备点")
                                   : QStringLiteral(" 取料完成后无需回拍照安全高度，取料位与倒料位同一 LM%1，跳过 AGV 导航，直接进入倒料准备点"))
                                  .arg(m_stationCfg->unloadLm));
            enterState(ExecState::ArmUnload,
                       returnedToCaptureSafety
                           ? QStringLiteral("取料已回拍照安全高度，同站工位直接进入倒料准备点")
                           : QStringLiteral("取料完成后无需回拍照安全高度，同站工位直接进入倒料准备点"));
            return;
        }

        enterState(ExecState::StowAfterPickup, QStringLiteral("取料完成，机械臂收姿态"));
        return;
    }
```

然后从 `switch (m_state)` 删除旧的 `case ExecState::ArmPickup: case ExecState::PreGripScan: case ExecState::RotateForScan:` 取料完成分支，避免重复推进。

- [ ] **步骤 6：运行 C++ 验证**

运行：

```bash
cmake --build build --target test_station_pickup_config
./build/tests/test_station_pickup_config
ctest --test-dir build --output-on-failure
```

预期：所有测试通过。

- [ ] **步骤 7：提交 Bug 2**

运行：

```bash
git add src/taskexecutor.h src/taskexecutor.cpp tests/test_station_pickup_config.cpp
git commit -m "fix: continue after scan-search pickup completion"
```

预期：创建一个本地 commit；不要 push。

### 任务 3：Bug 3 - RunFunc 后按 SDK FSM 等待并补充 Cleanup 诊断

**文件：**
- 修改：`robot_visual20260625/robot_visual/src/huayanScheduler.h`
- 修改：`robot_visual20260625/robot_visual/src/huayanScheduler.cpp`
- 修改：`robot_visual20260625/robot_visual/tests/test_station_pickup_config.cpp`
- 修改：`robot_visual20260625/robot_visual/changelog/CHANGELOG.md`

**接口：**
- 使用现有接口：`PendingCommand`、`pollCommandReady()`、`dispatchReadyCommand()`、`onPollTick()`。
- 新增接口：`RobotStateSnapshot`、`readRobotStateSnapshot()`、`formatRobotStateSnapshot(const RobotStateSnapshot &snapshot)`、RunFunc 专用 FSM 等待逻辑。

**新增/修改的函数、接口、变量：**
- `HuayanScheduler::RobotStateSnapshot`：新增 private 结构体，保存机器人 flags 和 FSM 快照。
- `HuayanScheduler::readRobotStateSnapshot() const`：新增 private 方法，一次读取 `HRIF_ReadRobotFlags()` 和 `HRIF_ReadCurFSM()`。
- `HuayanScheduler::formatRobotStateSnapshot(const RobotStateSnapshot &snapshot) const`：新增 private 方法，把快照格式化成日志字符串。
- `HuayanScheduler::m_activeCommandKind`：新增 private 成员，记录当前已下发且正在等待完成的命令类型。
- `HuayanScheduler::m_activeCommandLabel`：新增 private 成员，记录当前命令日志标签。
- `HuayanScheduler::m_loggedRunFuncScriptRunning`：新增 private 成员，避免 FSM=34 等待日志刷屏。
- `HuayanScheduler::dispatchReadyCommand(const PendingCommand &cmd)`：SDK 调用成功后记录 active command 信息。
- `HuayanScheduler::onPollTick()`：当 active command 是 `PendingCommandKind::RunFunc` 时，运动停止后继续读取 FSM；`nCurFSM == 34` 时继续等待。
- `HuayanScheduler::startStow()`：启动收姿态前输出机器人状态快照。
- `HuayanScheduler::stopPollingAndTimers()`：清空 active command 信息，避免停止后旧命令影响新阶段。

**必须增加的中文注释：**
- 在 `RobotStateSnapshot` 定义旁注释：这是现场诊断用快照，不参与运动决策本身。
- 在 `readRobotStateSnapshot()` 前注释：同时读取 flags 和 FSM，是为了定位 Cleanup 20561 前控制器是否仍在脚本运行态。
- 在 `onPollTick()` 的 `nCurFSM == 34` 分支注释：华沿 SDK demo 中 34 表示 `ScriptRunning`，RunFunc 未结束前不能把阶段视为完成。
- 在 `startStow()` 诊断日志旁注释：所有收姿态都会记录状态，但不改变原有动作流程。

- [ ] **步骤 1：编写失败的 C++ 静态回归检查**

编辑 `tests/test_station_pickup_config.cpp` 并加入：

```cpp
    requireTrue(schedulerHeaderSource.contains(QStringLiteral("struct RobotStateSnapshot")),
                "HuayanScheduler 必须定义机器人状态快照用于 Cleanup 诊断");
    requireTrue(schedulerSource.contains(QStringLiteral("readRobotStateSnapshot")),
                "HuayanScheduler 必须读取 flags 和 FSM 形成诊断快照");
    requireTrue(schedulerSource.contains(QStringLiteral("nCurFSM == 34")),
                "RunFunc 完成判定必须等待 SDK demo 中的 34 ScriptRunning 结束");
    requireTrue(schedulerSource.contains(QStringLiteral("RunFunc 仍处于 ScriptRunning")),
                "RunFunc 等待 FSM=34 时必须输出可追踪日志");
    requireTrue(schedulerSource.contains(QStringLiteral("Cleanup 收姿态前机器人状态")),
                "Cleanup 收姿态前必须记录机器人状态诊断");
```

- [ ] **步骤 2：运行 C++ 测试并确认失败**

运行：

```bash
cmake --build build --target test_station_pickup_config
./build/tests/test_station_pickup_config
```

预期：测试失败并输出“HuayanScheduler 必须定义机器人状态快照用于 Cleanup 诊断”。

- [ ] **步骤 3：增加 active RunFunc 命令跟踪**

修改 `src/huayanScheduler.h`。

在 `PendingCommand` 附近增加 private 结构体：

```cpp
    struct RobotStateSnapshot {
        // 现场诊断快照：记录 Cleanup 前控制器状态，不直接改变运动决策。
        int movingState = 0;
        int pauseState = 0;
        int errorState = 0;
        int errorCode = 0;
        int nCurFSM = 0;
        QString strCurFSM;
        bool valid = false;
    };
```

增加 private 成员：

```cpp
    PendingCommandKind m_activeCommandKind = PendingCommandKind::None;
    QString m_activeCommandLabel;
    bool m_loggedRunFuncScriptRunning = false;
```

增加 private 方法：

```cpp
    RobotStateSnapshot readRobotStateSnapshot() const;
    QString formatRobotStateSnapshot(const RobotStateSnapshot &snapshot) const;
```

- [ ] **步骤 4：实现机器人状态快照 helper**

修改 `src/huayanScheduler.cpp`。

在 `hasActiveRobotCommand()` 附近增加：

```cpp
// 同时读取 flags 和 FSM，用于定位 Cleanup 20561 前控制器是否仍处于脚本运行态。
HuayanScheduler::RobotStateSnapshot HuayanScheduler::readRobotStateSnapshot() const
{
    RobotStateSnapshot snapshot;
    int nEnableState = 0;
    int nErrorAxis = 0;
    int nBreaking = 0;
    int nBlendingDone = 0;
    const int flagsRet = HRIF_ReadRobotFlags(m_boxID, m_rbtID,
                                             snapshot.movingState,
                                             nEnableState,
                                             snapshot.errorState,
                                             snapshot.errorCode,
                                             nErrorAxis,
                                             nBreaking,
                                             snapshot.pauseState,
                                             nBlendingDone);
    string fsmText;
    const int fsmRet = HRIF_ReadCurFSM(m_boxID, m_rbtID, snapshot.nCurFSM, fsmText);
    snapshot.strCurFSM = fsmRet == 0 ? QString::fromStdString(fsmText) : QStringLiteral("unknown");
    snapshot.valid = flagsRet == 0 && fsmRet == 0;
    return snapshot;
}

QString HuayanScheduler::formatRobotStateSnapshot(const RobotStateSnapshot &snapshot) const
{
    return QStringLiteral("moving=%1 pause=%2 error=%3 errorCode=%4 fsm=%5/%6 valid=%7")
        .arg(snapshot.movingState)
        .arg(snapshot.pauseState)
        .arg(snapshot.errorState)
        .arg(snapshot.errorCode)
        .arg(snapshot.nCurFSM)
        .arg(snapshot.strCurFSM)
        .arg(snapshot.valid ? QStringLiteral("true") : QStringLiteral("false"));
}
```

- [ ] **步骤 5：收姿态前记录机器人状态**

修改 `HuayanScheduler::startStow()`。

在 `ensureConnected()` 成功之后、`stopPollingAndTimers()` 之前增加：

```cpp
    const RobotStateSnapshot snapshot = readRobotStateSnapshot();
    // 诊断日志不改变动作流程，用于现场复现 20561 时判断控制器状态。
    emit logMessage(QStringLiteral("[收姿态] Cleanup 收姿态前机器人状态：%1")
                        .arg(formatRobotStateSnapshot(snapshot)));
```

这会在每次 `startStow()` 启动时记录状态。日志文案保留 `Cleanup` 关键字是为了现场按错误恢复路径搜索；该日志不改变运动行为。

- [ ] **步骤 6：跟踪当前已下发命令类型**

修改 `dispatchReadyCommand()`。

在每个成功调用 `startWaitForIdle(cmd.timeoutMs);` 之前设置：

```cpp
        m_activeCommandKind = cmd.kind;
        m_activeCommandLabel = cmd.label;
        m_loggedRunFuncScriptRunning = false;
```

`RunFunc`、`MoveRelTool/MoveRelBase`、`MoveJ` 三类分支都要这样处理：SDK 调用成功后、`startWaitForIdle()` 前记录 active command 信息。

修改 `stopPollingAndTimers()`，清空 active command 跟踪状态：

```cpp
    m_activeCommandKind = PendingCommandKind::None;
    m_activeCommandLabel.clear();
    m_loggedRunFuncScriptRunning = false;
```

- [ ] **步骤 7：等待 RunFunc 的 FSM 离开 ScriptRunning**

修改 `onPollTick()` 中“运动已完成”的判定块：

```cpp
    if ((m_hasSeenMoving || m_pollCount >= 30) && nMovingState == 0) {
        if (m_activeCommandKind == PendingCommandKind::RunFunc) {
            int nCurFSM = 0;
            string strCurFSM;
            const int fsmRet = HRIF_ReadCurFSM(m_boxID, m_rbtID, nCurFSM, strCurFSM);
            if (fsmRet != 0) {
                emitOperationError(QStringLiteral("RunFunc 完成前读取 FSM 失败：ret=%1 label=%2")
                                       .arg(fsmRet)
                                       .arg(m_activeCommandLabel));
                return;
            }
            if (nCurFSM == 34) {
                // 华沿 SDK demo 中 34 表示 ScriptRunning；RunFunc 未结束前不能推进下一阶段。
                if (!m_loggedRunFuncScriptRunning) {
                    emit logMessage(QStringLiteral("[华沿] RunFunc 仍处于 ScriptRunning，等待函数结束：label=%1 fsm=%2/%3")
                                        .arg(m_activeCommandLabel)
                                        .arg(nCurFSM)
                                        .arg(QString::fromStdString(strCurFSM)));
                    m_loggedRunFuncScriptRunning = true;
                }
                return;
            }
        }
        m_activeCommandKind = PendingCommandKind::None;
        m_activeCommandLabel.clear();
        m_loggedRunFuncScriptRunning = false;
```

保留该完成块后续已有代码不变，以保持现有 Stage/Action 推进规则。

- [ ] **步骤 8：在 changelog 记录 SDK 依据**

追加到 `changelog/CHANGELOG.md`：

```markdown
## 2026-07-08

- 修复倒料后收姿态函数固定的问题：新增按工位配置的倒料后收姿态函数，工位 3 可使用示教器内含过渡点的新函数，其他工位默认保持 `Func_yun_xing_zhong`。
- 修复扫码搜索成功后任务不继续推进的问题：第 2/3 轮扫码成功并回原夹取位后，阶段一完成会继续进入取料后收姿态或同 LM 倒料。
- 增强 Cleanup 收姿态诊断和 RunFunc 完成判定：参考华沿 SDK demo，RunFunc 后等待 FSM 离开 `34 ScriptRunning`，并记录收姿态前机器人状态，辅助定位 `20561`。
```

- [ ] **步骤 9：运行 C++ 验证**

运行：

```bash
cmake --build build --target test_station_pickup_config
./build/tests/test_station_pickup_config
ctest --test-dir build --output-on-failure
```

预期：所有测试通过。不使用 Python 命令。

- [ ] **步骤 10：提交 Bug 3**

运行：

```bash
git add src/huayanScheduler.h src/huayanScheduler.cpp tests/test_station_pickup_config.cpp changelog/CHANGELOG.md
git commit -m "fix: wait for runfunc fsm before cleanup stow"
```

预期：创建一个本地 commit；不要 push。

## 最终验证

- [ ] **步骤 1：构建项目**

运行：

```bash
cmake --build build
```

预期：构建成功，没有编译错误。

- [ ] **步骤 2：运行所有已配置 CTest 测试**

运行：

```bash
ctest --test-dir build --output-on-failure
```

预期：所有已配置测试通过。

- [ ] **步骤 3：检查 commit 形状**

运行：

```bash
git log --oneline -4
git status --short
```

预期：最新提交包含一个文档 commit，以及每个 Bug 一个修复 commit。`git status --short` 不显示非预期源码改动。不要执行 `git push`。

## 自检

- 规格覆盖：任务 1 覆盖按工位配置倒料后收姿态；任务 2 覆盖扫码搜索成功后的阶段完成推进；任务 3 覆盖 RunFunc/FSM 诊断和 Cleanup 等待。
- 占位扫描：计划不包含未完成占位标记。
- 类型一致性：`stowAfterUnloadFunc`、`setNextStowFunction`、`isPickupCompletionState`、`RobotStateSnapshot`、`readRobotStateSnapshot` 都在使用前定义。
