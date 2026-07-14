Task 3 实施报告：新增独立缺料验证控制台和十五项验证向导

完成内容
- 新增 `src/shortagevalidationdialog.h/.cpp`，实现独立 `ShortageValidationDialog`，窗口使用非模态 `show()` 语义所需的 `Qt::Window`、最小化、最大化和关闭按钮，最小尺寸 1280×760。
- 新增 `ShortageValidationStatus`、`ShortageValidationAction`、`ShortageValidationStep`、`ShortageValidationCase`，接口、字段、所有权、状态分支和信号连接点均带中文注释。
- `validationCases()` 固定提供 VT-01～VT-15，包含中文步骤、固定通过标准、自动/人工边界；涉及真实通信、窗口切换、现场设备或正式隔离的项目只进入“等待人工确认”，不自动标记为实机验收通过。
- `executeNextStep()` 只通过 `ShortageTestController` 公开 slots 分发动作；控制器为空时拒绝执行并记录“验证步骤未执行：测试控制器不可用”。
- 右侧实际状态区显示产品/模式、actualQty/基线、活动工位、等待顺序、当前补料单、严重锁定、最近增量和摘要。
- 底部日志每条包含 UTC 时间、VT 编号、步骤号、输入、预期、实际摘要和中文消息。
- 新增 `tests/test_shortage_validation_dialog.cpp`，覆盖窗口非模态/按钮、十五项中文定义、步骤和通过标准、公开控制器动作、自动证据、人工确认边界、源码隔离契约。
- 在主 `CMakeLists.txt` 和 `tests/CMakeLists.txt` 注册独立验证窗口源码和 `shortage_validation_dialog_tests`。

TDD 记录
- RED：当前接手时前序草稿已包含 `src/shortagevalidationdialog.h/.cpp`，`cmake --build build-shortage --target shortage_validation_dialog_tests --parallel` 已可编译，因此无法在当前工作树复现“缺少 header/source”的原始 RED。按 brief 预期，前序 RED 应为测试目标引用 `ShortageValidationDialog` 或 `../src/shortagevalidationdialog.cpp` 时缺文件/缺类型导致编译失败。
- 可验证失败：首次运行 `QT_QPA_PLATFORM=offscreen ctest --test-dir build-shortage -R '^shortage_validation_dialog_tests$' --output-on-failure` 失败；原因是测试使用的中文正则在 Qt 6.8 下为无效 `QRegularExpression`，`exposesExactlyFifteenChineseValidationCases()` 未通过。
- GREEN：改为 Unicode 码段中文检查，补强固定动作序列测试；实现侧补齐 VT-02、VT-07～VT-11 显式手工源步骤，并将 VT-12/VT-13 从纯说明强化为可执行公开动作步骤后，指定目标和测试通过。

验证结果
- `cmake --build build-shortage --target shortage_validation_dialog_tests --parallel`：通过。
- `QT_QPA_PLATFORM=offscreen ctest --test-dir build-shortage -R '^shortage_validation_dialog_tests$' --output-on-failure`：通过，1/1。
- `git diff --check`：通过，无输出。

隔离和范围说明
- 新验证控制台未接入 `ShortageConfigDialog`、`MainWindow`、`LineManager`、主 FIFO 或硬件控制器；Task 4 再做入口集成。
- 独立测试仍使用 `ShortageStateNamespace::StandaloneTest`；测试用例使用 `QTemporaryDir`，未写正式账本、未进入主 FIFO、未控制 AGV/机械臂/扫码硬件。
- 未修改 `actualQty` 清零算法、补料非抢占算法、主 FIFO 追加规则、真实 MES/PLC 协议地址或寄存器映射。
- 工作区根目录已有未跟踪 `test-state.json`、`test-state.backup.json`、`test-events.jsonl`；本任务未暂存、删除或覆盖这些文件。

注意事项
- 真实通信、窗口人工切换和现场设备未动作仍需人工确认；自动测试只证明本地可判定结果和向导边界。

修正评审发现：自动判定和空控制器门禁

修正内容
- 移除自动判定的兜底“未严重锁定即通过”逻辑；VT-04、VT-06、VT-07、VT-08、VT-09、VT-10、VT-11 均按逐步快照执行本地可证明条件检查，并在验证日志中输出中文通过/失败证据。
- 自动判定保留最近一次 `snapshotChanged` 携带的增量证据，避免用无增量的普通快照误判 VT-06。
- VT-15 在进入人工确认前先核对本地门禁证据：现场采样运行、手工动作禁用、停止后可切回手工源；直接调用拒绝原因仍作为人工复核证据，不报告纯自动通过。
- 控制器为空时，任何“执行下一步”都记录“验证步骤未执行：测试控制器不可用”，不推进步骤、不进入等待人工确认，也不能形成误导性人工通过。
- 测试补充覆盖固定自动判定证据日志、空控制器拒绝执行、VT-15 本地门禁后仍等待人工确认。

修正验证命令输出

`cmake --build build-shortage --target shortage_validation_dialog_tests --parallel`

```text
[  0%] Built target shortage_validation_dialog_tests_autogen_timestamp_deps
[ 12%] Built target shortage_validation_dialog_tests_autogen
[100%] Built target shortage_validation_dialog_tests
```

`QT_QPA_PLATFORM=offscreen ctest --test-dir build-shortage -R '^shortage_validation_dialog_tests$' --output-on-failure`

```text
Internal ctest changing into directory: /home/dh/project/robot/robot_visual20260625_0630_xianchangceshi/robot_visual20260625/robot_visual/build-shortage
Test project /home/dh/project/robot/robot_visual20260625_0630_xianchangceshi/robot_visual20260625/robot_visual/build-shortage
    Start 12: shortage_validation_dialog_tests
1/1 Test #12: shortage_validation_dialog_tests ...   Passed    1.03 sec

100% tests passed, 0 tests failed out of 1

Total Test time (real) =   1.04 sec
```

`git diff --check`

```text
无输出。
```
