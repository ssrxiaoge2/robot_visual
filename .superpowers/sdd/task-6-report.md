# Task 6 Report: 同轮采样、稳定确认和换型上下文

## Summary

已按 Task 6 实现 `ShortageSampleCoordinator`，覆盖同一 roundId 四响应聚合、迟到响应隔离、三选一校验、两轮稳定、换型确认、短断线恢复、长断线报警和恢复回退维护锁定。

采样协调器只消费 `CustomSysScheduler::mesReplyReady/plcReplyReady`，输出 `stableSampleReady/sampleRejected/communicationStateChanged/contextChangeConfirmed`，未调用 `ShortageEngine`，未修改库存或任务状态。

## TDD Evidence

### RED

先在 `tests/test_shortage_sample_coordinator.cpp` 追加 CM-01～CM-12、CH-01、CH-02、CH-05 测试，并把现有 target 扩展为包含尚不存在的 `../src/shortagesamplecoordinator.cpp`。

运行：

```bash
cmake --build build-shortage --target shortage_sample_coordinator_tests -j2
```

预期失败：

```text
Cannot find source file:
  ../src/shortagesamplecoordinator.cpp
```

### GREEN

新增：

- `src/shortagesamplecoordinator.h`
- `src/shortagesamplecoordinator.cpp`

修改：

- `tests/test_shortage_sample_coordinator.cpp`
- `tests/CMakeLists.txt`
- `CMakeLists.txt`

## Behavior Covered

- CM-01：同一 roundId 的 MES + PLC L68-L69 + PLC L71-L73 + PLC L1998 全部到齐后才输出。
- CM-02：超时拒绝整轮，迟到响应不补齐。
- CM-03：旧 roundId 响应不能完成新轮。
- CM-04/CM-05：产品/模式必须恰好一个 true，错误信息列出 L68/L69/L71/L72/L73/L1998。
- CM-06：第一轮只记录候选，连续第二轮相同才确认稳定。
- CM-07：新定时参数只应用到下一轮，不影响进行中轮次。
- CM-08：中断后恢复且 actualQty 不小于基线时输出 catch-up 样本并标记 `recoveredAfterInterruption`。
- CM-09：连续失败达到配置分钟数进入 `Alarm`。
- CM-10：中断恢复后 actualQty 小于旧基线进入 `RecoveryNeedsReview`。
- CM-11：HTTP/JSON 错误拒绝整轮，不写入部分候选状态。
- CM-12：actualQty 使用 qint64 边界并拒绝负值。
- CH-01：单轮上下文抖动不切换。
- CH-02：两轮稳定新上下文只发 `contextChangeConfirmed`，后续由上层决定何时激活。
- CH-05：3 产品 × 3 模式全部识别。

同时将 Task 1 的 `protocolReplyKeepsRoundIdAndAddressRange()` 改为 `QTRY_VERIFY_WITH_TIMEOUT(...)` 等待已有信号计数，保留 roundId 和地址范围断言，避免单次 `QSignalSpy::wait()` 在信号已到达时偶发超时。

## Validation

运行：

```bash
cmake --build build-shortage --target shortage_sample_coordinator_tests -j2
ctest --test-dir build-shortage -R '^shortage_sample_coordinator_tests$' --output-on-failure
ctest --test-dir build-shortage --output-on-failure
git diff --check
```

结果：

- `shortage_sample_coordinator_tests`: passed
- Full `ctest`: 12/12 passed
- `git diff --check`: no output

额外 sanity check：

```bash
cmake --build build-shortage --target wh-robot-visual -j2
```

结果：passed。

## Concerns

- 工作区原有未提交的 docs 和 `tests/test_station_pickup_config.cpp` 变更未触碰、未暂存。

## Review Fix 2026-07-14

修复评审阻塞项：

- 新上下文两轮稳定后仅发 `contextChangeConfirmed`，保存为 pending，不再覆盖当前 stable context；新增 `activateConfirmedContextForTestOrCaller(...)`，只有上层确认旧任务排空后才显式激活。
- `communicationAlarmMinutes` 按 1～60 分钟 clamp，补充 120 分钟输入按 60 分钟报警的边界覆盖。
- `RecoveryNeedsReview` 进入维护停机：停止当前轮/下一轮定时并关闭自动采样，调用方必须显式 `start()`/重置后才能继续。

新增/更新测试：

- `twoStableRoundsCreatePendingContext()`：确认新上下文后继续同新上下文采样不发 `stableSampleReady`；显式激活后才发新上下文稳定样本。
- `alarmTurnsRedAtConfiguredDuration()`：覆盖通信报警上限 60 分钟。
- `reconnectBelowBaselineRequiresMaintenance()`：覆盖维护状态后 `triggerNextRoundForTest()` 不再产生新请求。

验证：

```bash
cmake --build build-shortage --target shortage_sample_coordinator_tests --parallel
ctest --test-dir build-shortage -R '^shortage_sample_coordinator_tests$' --output-on-failure
```

结果：`shortage_sample_coordinator_tests` passed。

## Review Fix 2026-07-14 Round 2

修复复审阻塞项：

- `contextChangeConfirmed` 发出前先写入 `m_hasPendingConfirmedContext/m_pendingProduct/m_pendingMode`，保证 Qt 同线程直接连接槽可以在信号回调内立即调用 `activateConfirmedContextForTestOrCaller(...)` 并成功激活。
- 初次稳定上下文仍保持原语义：无旧任务需要排空，发出确认后立即建立 stable context、输出首个稳定样本，并清除 pending 标记。

新增测试：

- `directContextConfirmationSlotCanActivateImmediately()`：使用 `Qt::DirectConnection` 连接 `contextChangeConfirmed`，槽内立即调用 `activateConfirmedContextForTestOrCaller(...)`；下一轮同上下文必须输出 `stableSampleReady`。

验证：

```bash
cmake --build build-shortage --target shortage_sample_coordinator_tests --parallel
ctest --test-dir build-shortage -R '^shortage_sample_coordinator_tests$' --output-on-failure
```

结果：`shortage_sample_coordinator_tests` passed。
