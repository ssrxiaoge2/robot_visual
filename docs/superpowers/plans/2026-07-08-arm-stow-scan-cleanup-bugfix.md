# 机械臂收姿态、扫码搜索与 Cleanup 修复实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复三类现场 Bug：倒料后收姿态按工位配置、扫码搜索成功后任务继续推进、Cleanup 收姿态按华沿 SDK RunFunc/FSM 语义等待并记录诊断。

**Architecture:** 保持现有 `TaskExecutor` 负责任务编排、`HuayanScheduler` 负责机械臂动作、`lineconfig.h` 负责工位差异配置的边界。工位差异进入配置表；状态机只增加缺失分支和 RunFunc 完成判定，不改变已有正常流程语义。

**Tech Stack:** C++17、Qt、CMake/CTest、华沿 HRIF SDK、现有 `test_station_pickup_config` C++ 回归测试。

## Global Constraints

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

## 现场分析摘要

问题 1 根因明确：当前 `HuayanScheduler::startStow()` 固定调用 `Func_yun_xing_zhong`，无法表达工位 3 从倒料点经示教器过渡点回安全点的路径。应新增 12 工位配置项，默认仍为 `Func_yun_xing_zhong`。

问题 2 根因明确：扫码第 2/3 轮成功后，任务状态经过 `PreGripScanSearchReturn` 回到原夹取位并继续夹紧，但阶段一完成回调没有把 `PreGripScanSearchReturn` 当作取料完成状态处理，导致后续不进入收姿态或倒料。

问题 3 直接故障位置明确，最终根因需靠诊断确认：工位 5 正常取料、倒料和倒料后收姿态均成功，异常发生在码垛函数缺失后的 Cleanup 收姿态。华沿 SDK demo 显示 `RunFunc()` 后应等待 FSM 离开 `34 ScriptRunning`，因此本次按 RunFunc/FSM 时序补强，不做工位 5 专属修复。

### Task 1: Bug 1 - 倒料后收姿态函数按工位配置

**Files:**
- Modify: `robot_visual20260625/robot_visual/src/lineconfig.h`
- Modify: `robot_visual20260625/robot_visual/src/huayanScheduler.h`
- Modify: `robot_visual20260625/robot_visual/src/huayanScheduler.cpp`
- Modify: `robot_visual20260625/robot_visual/src/taskexecutor.cpp`
- Modify: `robot_visual20260625/robot_visual/tests/test_station_pickup_config.cpp`

**Interfaces:**
- Consumes: existing `StationTaskConfig`, `HuayanScheduler::StationArmFunctions`, `HuayanScheduler::startStow()`.
- Produces: `StationTaskConfig::stowAfterUnloadFunc`, `StationArmFunctions::stowAfterUnloadFunc`, `HuayanScheduler::setNextStowFunction(const QString &funcName)`.

- [ ] **Step 1: Write the failing C++ config test**

Edit `tests/test_station_pickup_config.cpp` and add these checks after the existing `s12` config checks:

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

Also add static source checks near existing `schedulerSource` checks:

```cpp
    requireTrue(schedulerSource.contains(QStringLiteral("setNextStowFunction")),
                "HuayanScheduler 必须提供下一次收姿态函数选择接口");
    requireTrue(taskExecutorSource.contains(QStringLiteral("setNextStowFunction(m_stationCfg->stowAfterUnloadFunc)")),
                "TaskExecutor 必须在倒料后收姿态前注入当前工位函数");
```

- [ ] **Step 2: Run the C++ test and verify it fails**

Run from `robot_visual20260625/robot_visual`:

```bash
cmake --build build --target test_station_pickup_config
./build/tests/test_station_pickup_config
```

Expected: build fails because `StationTaskConfig` has no member named `stowAfterUnloadFunc`, or the test binary fails with “工位3倒料后收姿态必须使用带过渡点的新函数”.

- [ ] **Step 3: Extend the station config**

Modify `src/lineconfig.h`.

Replace the `StationTaskConfig` struct with the same fields plus `stowAfterUnloadFunc` after `unloadFunc`:

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

Update all 12 rows in `kStationTaskConfigs` so the new field appears between `unloadFunc` and `grabZClearance`. Use:

```cpp
QStringLiteral("Func_yun_xing_zhong_s3")
```

only for station 3. Use:

```cpp
QStringLiteral("Func_yun_xing_zhong")
```

for stations 1, 2, and 4-12.

- [ ] **Step 4: Add the HuayanScheduler interface**

Modify `src/huayanScheduler.h`.

In `StationArmFunctions`, add:

```cpp
        QString stowAfterUnloadFunc; ///< 当前工位倒料后回运行安全位函数；默认 Func_yun_xing_zhong。
```

In public methods near `startStow()`, add:

```cpp
    void setNextStowFunction(const QString &funcName);
```

In private members near `m_stowFuncName`, add:

```cpp
    QString m_nextStowFuncName;
```

Keep existing `m_stowFuncName = QStringLiteral("Func_yun_xing_zhong")` unchanged as the fallback for all existing UI/manual paths.

- [ ] **Step 5: Implement stow function selection**

Modify `src/huayanScheduler.cpp`.

Add:

```cpp
void HuayanScheduler::setNextStowFunction(const QString &funcName)
{
    m_nextStowFuncName = funcName;
}
```

In `executeCurrentStep()` under `Stage::Stow` and `StageStep::StowArm`, replace the fixed function usage with:

```cpp
            const QString stowFunc = m_nextStowFuncName.isEmpty()
                ? m_stowFuncName
                : m_nextStowFuncName;
            m_nextStowFuncName.clear();
            emit logMessage(QStringLiteral("[收姿态] 调用 %1").arg(stowFunc));
            executeRunFunc(stowFunc);
            break;
```

This keeps all existing callers on `m_stowFuncName` unless `TaskExecutor` explicitly sets a one-shot function.

- [ ] **Step 6: Inject and use the station function from TaskExecutor**

Modify `src/taskexecutor.cpp`.

In `TaskExecutor::start()`, after setting `stationFuncs.unloadFunc`, add:

```cpp
    stationFuncs.stowAfterUnloadFunc = m_stationCfg->stowAfterUnloadFunc;
```

In `TaskExecutor::enterState()`, split `StowAfterUnload` from the generic stow group:

```cpp
    case ExecState::StowAfterPickup:
    case ExecState::StowAfterPallet:
        emit logMessage(prefix(QStringLiteral("ARM")) + QStringLiteral(" 启动收姿态"));
        m_arm->startStow();
        break;
    case ExecState::StowAfterUnload:
        emit logMessage(prefix(QStringLiteral("ARM")) + QStringLiteral(" 启动倒料后收姿态"));
        m_arm->setNextStowFunction(m_stationCfg->stowAfterUnloadFunc);
        m_arm->startStow();
        break;
```

Do not call `setNextStowFunction()` for `CleanupStow`, `StowAfterPickup`, or `StowAfterPallet`.

- [ ] **Step 7: Run C++ verification**

Run:

```bash
cmake --build build --target test_station_pickup_config
./build/tests/test_station_pickup_config
ctest --test-dir build --output-on-failure
```

Expected: `test_station_pickup_config` passes, and CTest reports all configured tests passing.

- [ ] **Step 8: Commit Bug 1**

Run:

```bash
git add src/lineconfig.h src/huayanScheduler.h src/huayanScheduler.cpp src/taskexecutor.cpp tests/test_station_pickup_config.cpp
git commit -m "fix: configure post-unload stow per station"
```

Expected: one local commit is created; do not push.

### Task 2: Bug 2 - 扫码搜索成功后继续推进任务

**Files:**
- Modify: `robot_visual20260625/robot_visual/src/taskexecutor.h`
- Modify: `robot_visual20260625/robot_visual/src/taskexecutor.cpp`
- Modify: `robot_visual20260625/robot_visual/tests/test_station_pickup_config.cpp`

**Interfaces:**
- Consumes: `TaskExecutor::ExecState`, existing `onArmStageCompleted()`.
- Produces: `bool TaskExecutor::isPickupCompletionState(ExecState state) const`.

- [ ] **Step 1: Write the failing C++ static regression check**

Edit `tests/test_station_pickup_config.cpp` and add:

```cpp
    requireTrue(taskExecutorSource.contains(QStringLiteral("isPickupCompletionState")),
                "TaskExecutor 必须集中判断阶段一完成状态");
    requireTrue(taskExecutorSource.contains(QStringLiteral("case ExecState::PreGripScanSearchReturn:")),
                "扫码搜索成功回原夹取位后，阶段一完成必须继续推进");
    requireTrue(taskExecutorSource.contains(QStringLiteral("if (isPickupCompletionState(m_state))")),
                "onArmStageCompleted 必须使用取料完成状态 helper");
```

- [ ] **Step 2: Run the C++ test and verify it fails**

Run:

```bash
cmake --build build --target test_station_pickup_config
./build/tests/test_station_pickup_config
```

Expected: test fails with “TaskExecutor 必须集中判断阶段一完成状态”.

- [ ] **Step 3: Declare the helper**

Modify `src/taskexecutor.h`.

In private methods near `isAgvNavigationState()`, add:

```cpp
    bool isPickupCompletionState(ExecState state) const;
```

- [ ] **Step 4: Implement the helper**

Modify `src/taskexecutor.cpp` near `isAgvNavigationState()`:

```cpp
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

- [ ] **Step 5: Use the helper in stage completion**

Modify `TaskExecutor::onArmStageCompleted()`.

At the top of the function after the log emit, insert:

```cpp
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

Then remove the old `case ExecState::ArmPickup: case ExecState::PreGripScan: case ExecState::RotateForScan:` block from the `switch (m_state)` to avoid duplicate logic.

- [ ] **Step 6: Run C++ verification**

Run:

```bash
cmake --build build --target test_station_pickup_config
./build/tests/test_station_pickup_config
ctest --test-dir build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 7: Commit Bug 2**

Run:

```bash
git add src/taskexecutor.h src/taskexecutor.cpp tests/test_station_pickup_config.cpp
git commit -m "fix: continue after scan-search pickup completion"
```

Expected: one local commit is created; do not push.

### Task 3: Bug 3 - RunFunc 后按 SDK FSM 等待并补充 Cleanup 诊断

**Files:**
- Modify: `robot_visual20260625/robot_visual/src/huayanScheduler.h`
- Modify: `robot_visual20260625/robot_visual/src/huayanScheduler.cpp`
- Modify: `robot_visual20260625/robot_visual/tests/test_station_pickup_config.cpp`
- Modify: `robot_visual20260625/robot_visual/changelog/CHANGELOG.md`

**Interfaces:**
- Consumes: existing `PendingCommand`, `pollCommandReady()`, `dispatchReadyCommand()`, `onPollTick()`.
- Produces: `RobotStateSnapshot`, `readRobotStateSnapshot()`, RunFunc-specific FSM wait that treats `nCurFSM == 34` as still running.

- [ ] **Step 1: Write the failing C++ static regression check**

Edit `tests/test_station_pickup_config.cpp` and add:

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

- [ ] **Step 2: Run the C++ test and verify it fails**

Run:

```bash
cmake --build build --target test_station_pickup_config
./build/tests/test_station_pickup_config
```

Expected: test fails with “HuayanScheduler 必须定义机器人状态快照用于 Cleanup 诊断”.

- [ ] **Step 3: Add command kind tracking for active RunFunc**

Modify `src/huayanScheduler.h`.

Add a private struct near `PendingCommand`:

```cpp
    struct RobotStateSnapshot {
        int movingState = 0;
        int pauseState = 0;
        int errorState = 0;
        int errorCode = 0;
        int nCurFSM = 0;
        QString strCurFSM;
        bool valid = false;
    };
```

Add private members:

```cpp
    PendingCommandKind m_activeCommandKind = PendingCommandKind::None;
    QString m_activeCommandLabel;
    bool m_loggedRunFuncScriptRunning = false;
```

Add private methods:

```cpp
    RobotStateSnapshot readRobotStateSnapshot() const;
    QString formatRobotStateSnapshot(const RobotStateSnapshot &snapshot) const;
```

- [ ] **Step 4: Implement robot state snapshot helpers**

Modify `src/huayanScheduler.cpp`.

Add near `hasActiveRobotCommand()`:

```cpp
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

- [ ] **Step 5: Log Cleanup state before stow**

Modify `HuayanScheduler::startStow()`.

After `ensureConnected()` succeeds and before `stopPollingAndTimers()`, add:

```cpp
    const RobotStateSnapshot snapshot = readRobotStateSnapshot();
    emit logMessage(QStringLiteral("[收姿态] Cleanup 收姿态前机器人状态：%1")
                        .arg(formatRobotStateSnapshot(snapshot)));
```

This logs for every stow start. It is acceptable because the message is diagnostic and does not change motion behavior.

- [ ] **Step 6: Track active command kind**

Modify `dispatchReadyCommand()`.

Before each successful `startWaitForIdle(cmd.timeoutMs);`, set:

```cpp
        m_activeCommandKind = cmd.kind;
        m_activeCommandLabel = cmd.label;
        m_loggedRunFuncScriptRunning = false;
```

Do this for `RunFunc`, `MoveRelTool/MoveRelBase`, and `MoveJ` branches after the SDK call succeeds and before `startWaitForIdle()`.

Modify `stopPollingAndTimers()` to clear active command tracking:

```cpp
    m_activeCommandKind = PendingCommandKind::None;
    m_activeCommandLabel.clear();
    m_loggedRunFuncScriptRunning = false;
```

- [ ] **Step 7: Wait for RunFunc FSM to leave ScriptRunning**

Modify `onPollTick()` in the block where motion is considered complete:

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

Keep the existing remainder of the completion block unchanged after these lines. This preserves all existing Stage/Action advancement rules.

- [ ] **Step 8: Document the SDK reason in changelog**

Append to `changelog/CHANGELOG.md`:

```markdown
## 2026-07-08

- 修复倒料后收姿态函数固定的问题：新增按工位配置的倒料后收姿态函数，工位 3 可使用示教器内含过渡点的新函数，其他工位默认保持 `Func_yun_xing_zhong`。
- 修复扫码搜索成功后任务不继续推进的问题：第 2/3 轮扫码成功并回原夹取位后，阶段一完成会继续进入取料后收姿态或同 LM 倒料。
- 增强 Cleanup 收姿态诊断和 RunFunc 完成判定：参考华沿 SDK demo，RunFunc 后等待 FSM 离开 `34 ScriptRunning`，并记录收姿态前机器人状态，辅助定位 `20561`。
```

- [ ] **Step 9: Run C++ verification**

Run:

```bash
cmake --build build --target test_station_pickup_config
./build/tests/test_station_pickup_config
ctest --test-dir build --output-on-failure
```

Expected: all tests pass. No Python command is used.

- [ ] **Step 10: Commit Bug 3**

Run:

```bash
git add src/huayanScheduler.h src/huayanScheduler.cpp tests/test_station_pickup_config.cpp changelog/CHANGELOG.md
git commit -m "fix: wait for runfunc fsm before cleanup stow"
```

Expected: one local commit is created; do not push.

## Final Verification

- [ ] **Step 1: Build the project**

Run:

```bash
cmake --build build
```

Expected: build succeeds with no compile errors.

- [ ] **Step 2: Run all configured CTest tests**

Run:

```bash
ctest --test-dir build --output-on-failure
```

Expected: all configured tests pass.

- [ ] **Step 3: Inspect commit shape**

Run:

```bash
git log --oneline -4
git status --short
```

Expected: latest commits include one docs commit and one commit per Bug fix. `git status --short` shows no unintended source changes. Do not run `git push`.

## Self-Review

- Spec coverage: Task 1 covers per-station post-unload stow config; Task 2 covers scan-search completion; Task 3 covers RunFunc/FSM diagnosis and Cleanup wait.
- Placeholder scan: this plan contains no unfinished placeholder markers.
- Type consistency: `stowAfterUnloadFunc`, `setNextStowFunction`, `isPickupCompletionState`, `RobotStateSnapshot`, and `readRobotStateSnapshot` are introduced before use.
