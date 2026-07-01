# 同站工位安全倒料与日志去重实现计划

> **面向 AI 代理的工作者：** 必须使用子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 修复 1、2、11、12 同站工位在取料回拍照安全高度后未直接进入倒料流程的问题，并去掉机械臂日志三重输出。

**架构：** `HuayanScheduler` 继续负责机械臂阶段内动作，`StageOne` 的 `LiftLoad` 仍调用当前工位 `Func_captureN` 回到拍照/安全高度。`TaskExecutor` 只在收到 `StageOne` 完成信号后判断是否同站：同站则跳过 AGV 导航与收姿态，直接启动 `startUnload()`，由 `HuayanScheduler` 执行 `Func_daoliaoN -> Func_fanzhuan`；非同站保持原有“取料后收姿态，再 AGV 到倒料位”的流程。日志入口统一收敛到 `DeviceManager::logMessage -> MainWindow::log`，避免原始机械臂日志和任务前缀日志重复进入 UI。

**技术栈：** Qt 6、C++17、华沿 SDK 阶段机、`LineManager + TaskExecutor` 调度状态机、Python 静态回归脚本、CMake/MinGW 构建。

---

## 文件结构

- 新建：`scripts/test_same_lm_unload_and_log_dedup.py`
  - 静态检查同站工位取料完成后的状态推进、日志连接去重、正常阶段完成静默停止。
- 修改：`src/taskexecutor.cpp`
  - 调整 `TaskExecutor::onArmStageCompleted()` 中 `ArmPickup` 完成后的分支。
  - 移除 `TaskExecutor` 对 `HuayanScheduler::logMessage` 的二次转发。
- 修改：`src/huayanScheduler.h`
  - 将内部停止/清理接口扩展为可静默，用于正常阶段完成。
- 修改：`src/huayanScheduler.cpp`
  - `completeStage()` 使用静默清理，避免正常阶段完成时输出“调度已停止”。
  - 人工 Stop 和错误停止仍输出必要停止日志。
- 修改：`src/mainwindow.cpp`
  - 移除 `MainWindow` 对 `HuayanScheduler::logMessage` 的直接连接，保留 `DeviceManager::logMessage` 统一入口。
- 修改：`changelog/CHANGELOG.md`
  - 追加 `v0.2.3`，说明同站工位安全倒料和日志去重。

---

### 任务 1：添加失败回归脚本

**文件：**
- 创建：`scripts/test_same_lm_unload_and_log_dedup.py`

- [ ] **步骤 1：编写当前应失败的静态回归脚本**

创建脚本，读取关键文件并检查以下行为标记：

```python
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]


def read_text(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def main() -> int:
    task_cpp = read_text("src/taskexecutor.cpp")
    huayan_h = read_text("src/huayanScheduler.h")
    huayan_cpp = read_text("src/huayanScheduler.cpp")
    mainwindow_cpp = read_text("src/mainwindow.cpp")
    changelog = read_text("changelog/CHANGELOG.md")

    arm_pickup_block = re.search(
        r"case ExecState::ArmPickup:(.*?)case ExecState::StowAfterPickup:",
        task_cpp,
        re.S,
    )
    arm_pickup_text = arm_pickup_block.group(1) if arm_pickup_block else ""

    checks = [
        (
            "same-LM pickup completion must start unload after capture safety height",
            "pickupLm == m_stationCfg->unloadLm" in arm_pickup_text
            and "enterState(ExecState::ArmUnload" in arm_pickup_text
            and "StowAfterPickup" not in arm_pickup_text,
        ),
        (
            "TaskExecutor must not relay raw arm logs with task prefix",
            "HuayanScheduler::logMessage" not in task_cpp
            and "prefix(QStringLiteral(\"ARM\")) + QStringLiteral(\" \") + message" not in task_cpp,
        ),
        (
            "MainWindow must not directly connect HuayanScheduler logMessage",
            "connect(hs, &HuayanScheduler::logMessage" not in mainwindow_cpp,
        ),
        (
            "normal stage completion must stop silently",
            "void stop(bool emitStoppedLog = true)" in huayan_h
            and "stop(false)" in huayan_cpp
            and "emitStoppedLog" in huayan_cpp,
        ),
        (
            "changelog records v0.2.3 same-LM unload and log dedup",
            "v0.2.3" in changelog
            and "同站工位" in changelog
            and "日志去重" in changelog,
        ),
    ]

    failed = [message for message, ok in checks if not ok]
    if failed:
        print("FAILED same-lm/log-dedup checks:")
        for item in failed:
            print(item)
        return 1

    print("same-lm/log-dedup checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **步骤 2：运行脚本确认失败**

运行：`python scripts\test_same_lm_unload_and_log_dedup.py`

预期：失败，并至少报告同站工位推进、日志去重、静默停止和 changelog 未接线。

- [ ] **步骤 3：Commit**

```bash
git add scripts/test_same_lm_unload_and_log_dedup.py
git commit -m "test: add same lm unload and log dedup checks"
```

---

### 任务 2：修复同站工位取料完成后的安全倒料推进

**文件：**
- 修改：`src/taskexecutor.cpp:253-280`
- 测试：`scripts/test_same_lm_unload_and_log_dedup.py`

- [ ] **步骤 1：调整 `ArmPickup` 完成分支**

在 `TaskExecutor::onArmStageCompleted()` 中，将 `ArmPickup` 独立处理。注意：此时 `HuayanScheduler::StageOne` 已完成 `CloseGripper -> LiftLoad`，其中 `LiftLoad` 已调用当前工位 `Func_captureN` 回到拍照/安全高度，所以这里进入倒料准备点是安全前提成立后的动作。

目标结构：

```cpp
    switch (m_state) {
    case ExecState::ArmPickup:
    case ExecState::PreGripScan:
    case ExecState::RotateForScan:
        if (m_stationCfg->pickupLm == m_stationCfg->unloadLm) {
            emit logMessage(prefix(QStringLiteral("AGV"))
                            + QStringLiteral(" 取料已回拍照安全高度，取料位与倒料位同一 LM%1，跳过 AGV 导航，直接进入倒料准备点")
                                  .arg(m_stationCfg->unloadLm));
            enterState(ExecState::ArmUnload,
                       QStringLiteral("取料已回拍照安全高度，同站工位直接进入倒料准备点"));
            break;
        }

        enterState(ExecState::StowAfterPickup,
                   QStringLiteral("取料完成，机械臂收姿态"));
        break;

    case ExecState::StowAfterPickup:
        startAgvStep(ExecState::AgvToUnload,
                     m_stationCfg->unloadLm,
                     QStringLiteral("AGV 前往倒料位 LM%1").arg(m_stationCfg->unloadLm));
        break;
```

这里必须删除旧的 `StowAfterPickup` 内部同站判断，避免同站工位已经收姿态后才进入倒料。

- [ ] **步骤 2：运行静态脚本确认同站推进项通过**

运行：`python scripts\test_same_lm_unload_and_log_dedup.py`

预期：脚本仍可能因日志去重和 changelog 失败，但不再报告 `same-LM pickup completion must start unload after capture safety height`。

- [ ] **步骤 3：Commit**

```bash
git add src/taskexecutor.cpp scripts/test_same_lm_unload_and_log_dedup.py
git commit -m "fix: start unload after capture height for same lm stations"
```

---

### 任务 3：去掉机械臂日志重复转发

**文件：**
- 修改：`src/taskexecutor.cpp:36-65`
- 修改：`src/mainwindow.cpp:349-365`
- 测试：`scripts/test_same_lm_unload_and_log_dedup.py`

- [ ] **步骤 1：移除 `TaskExecutor` 对机械臂原始日志的二次转发**

删除 `TaskExecutor` 构造函数中这段连接：

```cpp
        connect(m_arm, &HuayanScheduler::logMessage, this, [this](const QString &message) {
            if (m_task.taskId == 0 || !isBusy()) {
                return;
            }
            emit logMessage(prefix(QStringLiteral("ARM")) + QStringLiteral(" ") + message);
        });
```

保留 `stageCompleted/stageError/preGripScanRequested/...` 等状态推进信号连接。

- [ ] **步骤 2：移除 `MainWindow` 对机械臂日志的直接连接**

删除 `MainWindow` 华沿 SDK 信号连接区中的这段：

```cpp
        connect(hs, &HuayanScheduler::logMessage,
                this, &MainWindow::onHuayanLog);
```

保留 `DeviceManager::logMessage -> MainWindow::log` 作为唯一日志入口。因为 `DeviceManager` 已经在 `src/devicemanager.cpp` 中转发 `HuayanScheduler::logMessage`，这样 UI 只会收到一份原始机械臂日志。

- [ ] **步骤 3：运行静态脚本确认日志去重项通过**

运行：`python scripts\test_same_lm_unload_and_log_dedup.py`

预期：脚本仍可能因静默停止和 changelog 失败，但不再报告两条日志去重检查。

- [ ] **步骤 4：Commit**

```bash
git add src/taskexecutor.cpp src/mainwindow.cpp scripts/test_same_lm_unload_and_log_dedup.py
git commit -m "fix: deduplicate arm logs in scheduler UI"
```

---

### 任务 4：正常阶段完成时静默清理调度

**文件：**
- 修改：`src/huayanScheduler.h:210-225`
- 修改：`src/huayanScheduler.cpp:528-552`
- 测试：`scripts/test_same_lm_unload_and_log_dedup.py`

- [ ] **步骤 1：扩展 `stop()` 声明**

在 `src/huayanScheduler.h` 中将公开停止接口改为：

```cpp
    void stop(bool emitStoppedLog = true);
```

保持默认参数为 `true`，确保人工 Stop、调试面板停止、系统错误路径仍输出“调度已停止”。

- [ ] **步骤 2：实现静默停止参数**

在 `src/huayanScheduler.cpp` 中将实现改为：

```cpp
void HuayanScheduler::stop(bool emitStoppedLog)
{
    stopPollingAndTimers();
    requestRobotStop();
    clearActionState();
    m_searchDescendCount = 0;
    m_searchDescendedMm = 0.0;
    m_pendingLargeRzConfirmation = false;
    m_pendingLargeRz = 0.0;
    m_waitingPreGripScan = false;
    m_preGripScanSearchCurrentY = 0.0;
    m_preGripScanSearchTargetY = 0.0;
    m_stage = Stage::None;
    m_stageStep = StageStep::None;
    if (emitStoppedLog)
        emit logMessage(QStringLiteral("调度已停止"));
}
```

- [ ] **步骤 3：让正常阶段完成静默清理**

在 `HuayanScheduler::completeStage()` 中将：

```cpp
    stop();
```

改为：

```cpp
    stop(false);
```

保留 `requestRobotStop()` 当前行为；如果 SDK 在正常完成阶段返回 20018，仍会输出一条警告，但不会再紧跟“调度已停止”。

- [ ] **步骤 4：运行静态脚本确认静默停止项通过**

运行：`python scripts\test_same_lm_unload_and_log_dedup.py`

预期：脚本只剩 changelog 项失败。

- [ ] **步骤 5：Commit**

```bash
git add src/huayanScheduler.h src/huayanScheduler.cpp scripts/test_same_lm_unload_and_log_dedup.py
git commit -m "fix: silence normal arm stage cleanup log"
```

---

### 任务 5：更新 changelog

**文件：**
- 修改：`changelog/CHANGELOG.md:1-40`
- 测试：`scripts/test_same_lm_unload_and_log_dedup.py`

- [ ] **步骤 1：追加 v0.2.3 记录**

在 `changelog/CHANGELOG.md` 顶部追加：

```markdown
## 2026-07-01 | v0.2.3 | 同站工位安全倒料与日志去重

### 修复
- 修复 1、2、11、12 同站工位在取料回到拍照安全高度后未直接进入倒料准备点的问题；现在 `StageOne` 完成后跳过 AGV 同站导航与收姿态，直接执行 `Func_daoliaoN -> Func_fanzhuan`。
- 修复机械臂日志被 `HuayanScheduler`、`TaskExecutor` 和 `MainWindow` 多路径转发导致同一条日志输出三遍的问题。

### 变更
- 正常阶段完成时静默清理内部调度状态，不再额外输出“调度已停止”；人工 Stop 和错误停止仍保留停止日志。

### 文件
- `src/taskexecutor.cpp`
- `src/huayanScheduler.h`
- `src/huayanScheduler.cpp`
- `src/mainwindow.cpp`
- `scripts/test_same_lm_unload_and_log_dedup.py`
- `changelog/CHANGELOG.md`
```

- [ ] **步骤 2：运行静态脚本确认通过**

运行：`python scripts\test_same_lm_unload_and_log_dedup.py`

预期：`same-lm/log-dedup checks passed`。

- [ ] **步骤 3：Commit**

```bash
git add changelog/CHANGELOG.md scripts/test_same_lm_unload_and_log_dedup.py
git commit -m "docs: record same lm unload and log dedup fix"
```

---

### 任务 6：整体验证

**文件：**
- 读取：`scripts/*.py`
- 读取：`build/Desktop_Qt_6_8_3_MinGW_64_bit-Debug`

- [ ] **步骤 1：运行新增回归脚本**

运行：`python scripts\test_same_lm_unload_and_log_dedup.py`

预期：`same-lm/log-dedup checks passed`。

- [ ] **步骤 2：运行既有回归脚本**

运行：

```powershell
python scripts\test_rz_confirm_and_scan_search.py
python scripts\test_version_metadata.py
python scripts\test_unload_function_name.py
```

预期：

```text
rz-confirm/scan-search checks passed
version metadata checks passed
unload function checks passed
```

- [ ] **步骤 3：重新编译 Qt 工程**

运行：

```powershell
$env:PATH='D:\study\qt\Tools\mingw1310_64\bin;' + $env:PATH
cmake --build 'D:\study\qtpro\robot-deploy\wh-robot-visual\build\Desktop_Qt_6_8_3_MinGW_64_bit-Debug' --target wh-robot-visual -j 4
```

预期：`[100%] Built target wh-robot-visual`。

- [ ] **步骤 4：检查 diff 格式**

运行：`git diff --check`

预期：退出码 `0`。允许现有 CRLF warning；不得出现 trailing whitespace 或 conflict marker。

- [ ] **步骤 5：现场验证建议**

在工位 2 模拟缺料时观察日志顺序应为：

```text
[阶段一] 抬升（调用拍照位函数 Func_capture2 回到安全高度）
[警告] 停止指令未成功下发：20018
[倒料] 调用倒料准备点 Func_daoliao2
[倒料] 执行倒料 Func_fanzhuan
```

同一条机械臂日志应只出现一次，不再同时出现原始日志、`[T#1 S02][ARM]` 前缀日志和另一条原始日志。

---

## 自检

- 规格覆盖：计划覆盖同站工位安全倒料、日志去重、正常阶段完成静默停止、changelog 记录和完整验证。
- 安全约束：同站工位不是夹取后直接翻转，而是在 `StageOne::LiftLoad` 已调用 `Func_captureN` 回到拍照/安全高度后，才进入 `Func_daoliaoN -> Func_fanzhuan`。
- 范围控制：不改视觉、扫码、AGV 到达判定、码垛流程；只修改同站取料完成后的调度分支和日志连接。
- 类型一致性：使用现有 `ExecState::ArmPickup`、`ExecState::ArmUnload`、`startAgvStep()`、`enterState()`、`HuayanScheduler::stop()`，不引入新状态。
