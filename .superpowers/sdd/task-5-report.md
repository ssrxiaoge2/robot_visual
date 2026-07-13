# Task 5 Report

## Changes

- 新增 `ReplenishmentPlanner`，实现低位触发、高位停止、首次低位时间 + 工位号稳定排序、活动工位非抢占、拒收重试同一补料单号、倒料前失败暂停和任务事实校验。
- 新增 `ShortageEngine` 统一事务门面，Engine 唯一持有运行态；Planner/Ledger 只操作事务副本，通过 Engine 发布。
- 新增 Task 5 领域类型：`ShortageDispatchRequest`、`TaskFactKind`、`TaskFact`、`ShortageEngineResult`、`ShortageUiSnapshot`、`PlannerApplyResult`。
- Engine 恢复安装增加补料单、等待表排序、等待工位低位状态、序列号等校验；`Queued`/`Running`/`Unloaded` 恢复进入维护锁定。
- Engine 对建账、人工建单、派单结果、任务开始、倒料、终态、维护修正等运行态变化写入 `ShortageStateStore`；关键持久化失败时保留内存事实并设置 `criticalLock`。
- 实现旧任务未排空时的换型保护：采样上下文变化且存在 FIFO 相关未终态任务时只保存 `pendingActualQty`，`activatePendingContextIfDrained(true)` 后再按新上下文扣减并重评估。
- 新增 `replenishment_planner_tests`，覆盖 PL-01～PL-06、TK-04～TK-08、PS-11、PS-15、ER-03 相关行为。

## TDD / Review Notes

- RED 1：新增测试后目标构建失败，缺少 `replenishmentplanner.h`。
- GREEN 1：实现 Planner/Engine 后 focused test 通过。
- Review 后新增 RED：派单接受未持久化、旧任务未排空时换型被提前应用。
- GREEN 2：补齐关键状态持久化、恢复校验和 pending context 事务后 focused test 通过。

## Verification

- `cmake --build build-shortage --target replenishment_planner_tests -j2`：通过。
- `ctest --test-dir build-shortage -R '^replenishment_planner_tests$' --output-on-failure`：1/1 通过。
- `ctest --test-dir build-shortage --output-on-failure`：12/12 通过。
- `git diff --check`：无输出。

## Files Changed

- `CMakeLists.txt`
- `src/shortagetypes.h`
- `src/replenishmentplanner.h`
- `src/replenishmentplanner.cpp`
- `src/shortageengine.h`
- `src/shortageengine.cpp`
- `tests/CMakeLists.txt`
- `tests/test_replenishment_planner.cpp`
- `.superpowers/sdd/task-5-report.md`

## Concerns

- `recordDispatchResult()` 接口没有事件时间参数，目前持久化审计时间使用 `QDateTime::currentDateTimeUtc()`；后续接入 LineManager 时可考虑传入调度接受时间以便全链路时间一致。
- 生产协调器接入时仍需在调用 `applyMaintenanceCorrection()` 前复核全部门禁；本任务仅提供 Engine 侧事务入口。
