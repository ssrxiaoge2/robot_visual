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
