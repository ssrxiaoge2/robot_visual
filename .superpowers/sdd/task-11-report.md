# Task 11 实施报告：主调度监控 UI 接入真实缺料入口

## 完成内容

- 新增 `live_shortage_ui_tests` C++/Qt Test 源码契约测试，覆盖来源互斥、12 工位按钮、真实人工补料确认、状态/FIFO 页签、FIFO 来源列、自动计划禁用人工按钮、配置/恢复弹窗入口。
- 主界面“调度监控”增加 Mock/Live 二选一来源，默认 Mock；Mock 下 12 个按钮仍调用原 `LineManager::reportShortage(stationId)`。
- Live 下 12 个按钮标题仍保持“工位X”，点击后先读取 `LiveShortageCoordinator::manualBoxConfirmation(stationId)`，确认信息逐项展示产品、模式、工位、位置、品号、每箱、当前库存、最低/最高安全位；高位风险增加第二次确认；最终只调用 `DeviceManager::requestManualShortageBox()`。
- 将 FIFO 表和 12 工位状态表合并进 `QTabWidget`；FIFO 增加“来源”列，页签标题显示当前数量。
- 增加真实缺料两行摘要，订阅 `DeviceManager::shortageSnapshotChanged()` 更新来源、摘要和工位表。
- 增加“缺料配置与完整逻辑测试...”弹窗入口和“异常恢复...”入口；恢复提交经 `DeviceManager::applyShortageMaintenanceCorrection()` 进入协调器复验。

## 验证结果

```bash
cmake --build build-shortage --target live_shortage_ui_tests shortage_dialog_tests --parallel
QT_QPA_PLATFORM=offscreen ctest --test-dir build-shortage -R '^(live_shortage_ui_tests|shortage_dialog_tests)$' --output-on-failure
```

结果：2/2 passed。

```bash
QT_QPA_PLATFORM=offscreen ctest --test-dir build-shortage --output-on-failure
```

结果：17/17 passed。

```bash
git diff --check
```

结果：无输出。

## 注意事项

- 当前 `ShortageUiSnapshot` 不携带生产配置的最低/最高安全位副本，主界面工位表保留“最低/最高”列，但无配置副本时显示 `-`。
- 配置弹窗当前由主界面以 `ShortageConfigStore::sheet3Defaults()` 打开；本任务未新增生产配置保存接口，避免越界改动配置持久化职责。
