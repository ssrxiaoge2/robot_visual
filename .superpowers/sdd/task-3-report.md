Task 3 实施报告：正式/测试隔离的快照、备份和流水

完成内容
- 新增 `ShortageStateNamespace`、`ShortageAuditEvent`、`ShortageRuntimeState`、`ShortageStateLoadResult` 等运行态类型。
- 新增 `ShortageStateStore`，按命名空间固定使用 production/test 快照、备份和 JSONL 流水文件名。
- 快照使用去除 checksum 字段后的紧凑 JSON SHA-256 校验。
- 保存策略：
  - `saveCritical()` 先追加流水，再通过 `QSaveFile` 写主快照，并在覆盖主快照前生成备份。
  - `savePeriodic()` 追加聚合流水，距离上次快照不足 60 秒时不刷新快照。
  - 空事件视为“状态未变化”，不写入快照或流水。
  - `backupBeforeMaintenance()` 写入 `maintenance-backups/` 下带 UTC 时间戳的维护备份。
- 恢复策略：
  - 先尝试主快照 + 其后连续流水，再尝试备份快照 + 连续流水。
  - 流水要求 sequence 严格连续、不可重复；缺口、重复、损坏都不会退化为“裸快照恢复成功”。
  - 无账本时要求现场清空确认。
  - 恢复后未人工确认时账本可读，但 `criticalLock=true`，禁止自动派单。

TDD 记录
- RED：新增 `tests/test_shortage_state_store.cpp` 和目标后，`cmake -S . -B build-shortage && cmake --build build-shortage --target shortage_state_store_tests --parallel` 因缺少 `../src/shortagestatestore.cpp` 失败，符合预期。
- GREEN：实现 `src/shortagestatestore.h/.cpp` 和运行态类型后，`shortage_state_store_tests` 全部通过。

验证结果
- `cmake --build build-shortage --target shortage_state_store_tests -j2`：通过。
- `ctest --test-dir build-shortage -R '^shortage_state_store_tests$' --output-on-failure`：通过，1/1。
- `ctest --test-dir build-shortage --output-on-failure`：未全绿；既有 `shortage_sample_coordinator_tests` 在 `protocolReplyKeepsRoundIdAndAddressRange()` 的 `plcSpy.wait(2000)` 超时失败。该测试单独复跑仍失败，和本任务新增状态仓库代码无直接依赖。
- `git diff --check`：通过，无输出。

注意事项
- 未触碰预先存在的脏文件和未跟踪文档。
- PS-11 的补料单字段仍按任务说明留给 Task 5。

Review 修复记录（2026-07-13）
- `savePeriodic()` 实际刷新快照时不再要求调用方预先更新 `lastSavedAtUtc`；写入快照前会复制状态并设置为本次 `nowUtc`。新增回归断言：60 秒边界刷新后恢复出的 `lastSavedAtUtc` 等于调用 `savePeriodic()` 传入的时间。
- 快照恢复新增语义校验：12 个工位状态必须恰好覆盖 1～12 且不可重复；必需标量字段必须以期望 JSON 类型存在，缺失或类型错误不再被 Qt 默认值静默变成 0/false/空对象。新增 checksum 合法但语义非法的缺失工位、重复工位、越界工位、缺失必需标量、标量类型错误用例。
- 主快照刷新前的备份改为读取现有主快照字节，再用 `QSaveFile` 原子提交到 backup，避免先 `remove()` 再 `QFile::copy()` 造成备份窗口期丢失。

Review 修复 TDD 记录
- RED：`cmake --build build-shortage --target shortage_state_store_tests -j2 && ctest --test-dir build-shortage -R '^shortage_state_store_tests$' --output-on-failure` 失败；新增用例捕获到 `lastSavedAtUtc` 仍为旧值，语义坏快照未按字段错误拒绝。
- GREEN：实现快照时间复制、严格字段/工位校验和原子备份刷新后，同一命令通过，1/1。

Review 修复验证结果
- `cmake --build build-shortage --target shortage_state_store_tests -j2`：通过。
- `ctest --test-dir build-shortage -R '^shortage_state_store_tests$' --output-on-failure`：通过，1/1。
- `git diff --check`：通过，无输出。
- `ctest --test-dir build-shortage --output-on-failure`：通过，10/10；本次未复现此前 Task 1 相关 flaky timeout。

Re-review 修复记录（2026-07-13）
- 快照恢复新增引用和时间语义校验：`activeStationId` 只能为 0 或 1～12；`waitingStationIds` 必须为整数、1～12、唯一且引用已存在工位；补料单 `stationId` 必须为 1～12。
- 时间字段改为严格解析：`lastSavedAtUtc`、补料单 `createdAtUtc`、流水 `occurredAtUtc` 必须非空且为合法 ISO UTC 时间；工位 `firstLowAtUtc` 可为空，但非空时必须为合法 ISO 时间。
- `saveCritical()` 和需要刷快照的 `savePeriodic()` 现在把规范化后的 `lastSavedAtUtc` 同步写入流水 `stateAfter`，避免备份快照加流水重放时遇到空时间字段。
- 语义测试改为移除生产流水后读取篡改快照，避免重复流水副作用掩盖快照语义校验结果。
- 新增 checksum 合法但语义非法的回归用例：`activeStationId=13`、`waitingStationIds=[99]`、重复等待工位、补料单 `stationId=99`、`lastSavedAtUtc/createdAtUtc/firstLowAtUtc/occurredAtUtc` 为 `not-a-date`。

Re-review 修复 TDD 记录
- RED：新增并修正语义测试后，`cmake --build build-shortage --target shortage_state_store_tests -j2 && ctest --test-dir build-shortage -R '^shortage_state_store_tests$' --output-on-failure` 失败，捕获到 checksum 合法的 `activeStationId=13` 快照仍能恢复。
- GREEN：实现整数、引用、时间语义校验并同步规范化流水状态后，同一目标测试通过，1/1。

Re-review 修复验证结果
- `cmake --build build-shortage --target shortage_state_store_tests -j2`：通过。
- `ctest --test-dir build-shortage -R '^shortage_state_store_tests$' --output-on-failure`：通过，1/1。
- `git diff --check`：通过，无输出。
- `ctest --test-dir build-shortage --output-on-failure`：通过，10/10。
