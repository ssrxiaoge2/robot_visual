# Task 9 Report

## 变更范围

- `src/lineconfig.h`
- `src/taskqueue.h`
- `src/taskexecutor.h`
- `src/taskexecutor.cpp`
- `src/linemanager.h`
- `src/linemanager.cpp`
- `tests/CMakeLists.txt`
- `tests/test_shortage_task_lifecycle.cpp`

## 已实现内容

1. 增加任务来源与补料单字段：
   - `TaskSource::{UiMock, LiveAutomatic, LiveManual}`。
   - `taskSourceText()` 统一中文来源文案。
   - `Task::replenishmentOrderNo`，模拟任务默认保持 `0`。

2. 保持 FIFO 默认兼容：
   - `TaskQueue::enqueue()` 增加默认参数 `replenishmentOrderNo = 0`。
   - `m_pending.append(task)` 和 `takeFirst()` 顺序保持不变。
   - 测试覆盖连续同工位/不同来源入队后按 taskId 原顺序出队。

3. 增加唯一倒料事实：
   - `TaskExecutor::materialUnloaded(const Task &task)`。
   - 只在 `ExecState::ArmUnload` 成功后、进入 `StowAfterUnload` 前发出一次。
   - 不改变机械臂倒料、倒料后收姿态、码垛和任务成功判定。

4. 增加 LineManager 真实队尾接口与事实信号：
   - `TaskEnqueueResult LineManager::enqueueShortageTask(...)`。
   - `reportShortage(int)` 改为调用 `enqueueShortageTask(stationId, TaskSource::UiMock, 0)`，保留原 UI 模拟日志和 Idle/Running/ReturningHome 行为。
   - 增加 `shortageTaskAccepted`、`shortageTaskStarted`、`shortageMaterialUnloaded`、`shortageTaskTerminal`。
   - 成功、失败、系统错误、Stop 当前任务取消、清 Pending 取消都会发终态事实。

5. 增加契约测试：
   - `shortage_task_lifecycle_tests` 覆盖 TK-01～TK-03、TK-06、RG-01～RG-05。
   - 对无法无硬件完整驱动的执行器/整线行为使用 C++ 源码文本契约测试。

## 验证结果

已执行并通过：

```bash
cmake --build build-shortage --target shortage_task_lifecycle_tests -j2
ctest --test-dir build-shortage -R '^shortage_task_lifecycle_tests$' --output-on-failure
ctest --test-dir build-shortage --output-on-failure
git diff --check
```

额外执行并通过：

```bash
cmake --build build-shortage -j2
cmake --build build-shortage --target shortage_task_lifecycle_tests task_executor_pallet_commit_semantics_tests --parallel
ctest --test-dir build-shortage -R '^(shortage_task_lifecycle_tests|task_executor_pallet_commit_semantics_tests)$' --output-on-failure
```

结果：

- `shortage_task_lifecycle_tests`：通过。
- `task_executor_pallet_commit_semantics_tests`：通过。
- 完整 `ctest`：15/15 通过。
- `git diff --check`：无输出。

## 注意事项

- 本次未改动 AGV、机械臂、视觉、扫码动作调用。
- 本次未改动倒料/收姿态动作、码垛算法或 FIFO 顺序。
- 现有 UI 模拟缺料默认仍使用 `UiMock + replenishmentOrderNo=0`。
