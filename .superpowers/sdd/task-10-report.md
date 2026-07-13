# Task 10 实施报告

状态：完成。

实现内容：

- 新增 `LiveShortageCoordinator` 和 `IShortageTaskGateway`，生产路径只通过队尾追加接口派发正式缺料任务。
- 新增 `live_shortage_coordinator_tests`，覆盖 Mock/Live 互斥、恢复确认门禁、拒收重试、人工补料、任务事实入账、错误中文结构化说明、DeviceManager 单通信对象契约。
- 在 `DeviceManager` 构造函数中一次性复用唯一 `m_liveShortageScheduler`，创建采样协调器、测试控制器和生产协调器，并连接 LineManager 正式事实信号。
- `setInputSource(Live)` 不调用 `LineManager::start()`；自动派单只在 Live、恢复已确认、配置有效、无严重锁定、整线 Running 时执行。
- `UiMock` 倒料事实不修改正式账本；`LiveAutomatic/LiveManual` 事实统一转换为 `TaskFact` 交给 `ShortageEngine`。
- 将 `ShortageUiSnapshot` 和 `ShortageMaintenanceCorrection` 的 Qt metatype 声明集中到 `shortagetypes.h`，避免主程序 MOC 聚合编译时晚声明。

验证：

- `cmake --build build-shortage --target live_shortage_coordinator_tests -j2`：通过。
- `ctest --test-dir build-shortage -R '^live_shortage_coordinator_tests$' --output-on-failure`：1/1 通过。
- `ctest --test-dir build-shortage --output-on-failure`：16/16 通过。
- `git diff --check`：通过，无输出。
- 额外集成验证 `cmake --build build-shortage --target wh-robot-visual -j2`：通过。

注意事项：

- 工作区存在任务前已有的未提交 docs 和 `tests/test_station_pickup_config.cpp` 改动；本任务未暂存这些文件。
- 本任务没有创建 brief 之外的新文件；为解决 Qt MOC 声明顺序，修改了既有 metatype 声明位置。

Review 修复追加：

- 修复 ER-04：`criticalLock` 现在只阻止新的自动派单；人工补料不再被 `ShortageEngine::requestManualBox()` 拦截，`ReplenishmentPlanner::nextDispatchRequest()` 在严重锁定时只跳过自动单并允许 `Manual` 单派发，`LiveShortageCoordinator::pumpDispatch()` 改由 Planner 区分来源。这是跨 Task 10/Engine/Planner 的正确性修复。
- 补强 PS-15：新增 `ShortageEngine::restoreLocked()` 只读门禁，恢复失败或需要维护时 `LiveShortageCoordinator::setInputSource(Live)` 保持 Mock、发出拒绝原因，且不进入真实采样/派单路径。
- 补齐 DeviceManager 暴露面：新增生产协调器 getter、缺料输入源/恢复确认/人工补料/维护修正转发槽，以及 `shortageSnapshotChanged` 转发信号；注释明确 DeviceManager 持有所有权，UI 只观察/调用唯一生产协调器。
- 新增/更新回归测试：`criticalLockStopsNewAutomaticOnly` 验证自动被挡、人工 `LiveManual` 被接受；`startupRestoreFailureBlocksLiveAndDispatch` 验证恢复失败时 Live 不激活且不派单；源码契约测试验证 DeviceManager getter/slot/signal 转发存在。

Review 修复验证：

- `cmake --build build-shortage --target live_shortage_coordinator_tests -j2 && ctest --test-dir build-shortage -R '^live_shortage_coordinator_tests$' --output-on-failure`：通过，1/1。
- `cmake --build build-shortage --target replenishment_planner_tests -j2 && ctest --test-dir build-shortage -R '^replenishment_planner_tests$' --output-on-failure`：通过，1/1。
- `ctest --test-dir build-shortage --output-on-failure`：通过，16/16。

Review 二次修复追加：

- 修复正式 Live 与独立测试现场采样互斥的反向缺口：`DeviceManager::setShortageInputSource(Live)` 在转发给生产协调器前检查 `ShortageTestController::fieldSamplingActive()`，若测试现场采样正在运行则拒绝启动正式 Live。
- 新增只读门禁接口 `ShortageTestController::fieldSamplingActive()`，不暴露测试 Engine/FIFO 写入口；拒绝原因使用中文结构化提示“正式 Live 启动失败：独立测试现场采样正在运行，处理动作=先停止测试现场采样”。
- 补强 `live_shortage_coordinator_tests` 源码契约，证明互斥两方向同时存在：测试现场采样启动已有正式 Live 门禁，正式 Live 启动现在也有测试现场采样门禁。

Review 二次修复验证：

- `cmake --build build-shortage --target live_shortage_coordinator_tests --parallel 2 && ctest --test-dir build-shortage -R '^live_shortage_coordinator_tests$' --output-on-failure`：通过，1/1。
- `cmake --build build-shortage --target shortage_test_controller_tests --parallel 2 && ctest --test-dir build-shortage -R '^shortage_test_controller_tests$' --output-on-failure`：通过，1/1。
