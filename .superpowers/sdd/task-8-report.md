# Task 8 Report: 宽屏配置与完整逻辑测试弹窗

## 状态

已实现并验证通过。

## 实现内容

- 新增 `ShortageConfigDialog`
  - 1200×720 最小宽屏弹窗。
  - 顶层 `QTabWidget` 包含“宽屏配置”和“完整逻辑测试”。
  - 配置页内按 88 / 88R / 92 建立三产品页签，每页签 12 行 × 11 列。
  - `liveMesDayEndpointEdit` 独立管理真实缺料 MES 地址，默认 `.228`，未提供 `.229` 旧输入框。
  - 采样间隔、单轮超时、通信报警时间、连续倒料前失败阈值均使用显式范围 `QSpinBox`。
  - 保存分支先调用外部门禁，再调用 `ShortageConfigStore::validate()`；Dialog 不直接访问生产 Engine。
  - 测试页包含 12 行运行表、计划摘要、8 个任务事件按钮、只读事件日志。
  - 手工源/现场源使用 exclusive `QButtonGroup`。
  - 测试按钮仅连接 `ShortageTestController`。

- 新增 `ShortageRecoveryDialog`
  - 仅接受只读 `ShortageUiSnapshot`。
  - 只提交单工位 `ShortageMaintenanceCorrection`。
  - 要求原因非空、复核输入工位号与目标工位一致。
  - 不提供全清零、批量提交、任务决策控件。

- 新增 `shortage_dialog_tests`
  - 按 objectName 检查 UI 契约，不依赖布局下标。
  - 为 Qt Widgets 测试设置 `QT_QPA_PLATFORM=offscreen`，保证全量 `ctest` 可在无显示环境运行。

## TDD 记录

- RED：新增 `tests/test_shortage_dialog.cpp` 和目标后，`cmake -S . -B build-shortage` 失败于缺少 `../src/shortageconfigdialog.cpp`。
- GREEN：补齐 Dialog 实现后，`shortage_dialog_tests` 通过。
- 修正：边界警告最初位于测试页内部，配置页激活时不可见；已提升为弹窗顶层常显控件。

## 验证

通过以下命令：

```bash
cmake -S . -B build-shortage
cmake --build build-shortage --target shortage_dialog_tests -j2
ctest --test-dir build-shortage -R '^shortage_dialog_tests$' --output-on-failure
ctest --test-dir build-shortage --output-on-failure
git diff --check
cmake --build build-shortage --target wh-robot-visual -j2
```

结果：

- `shortage_dialog_tests`: 1/1 passed。
- 全量 `ctest`: 14/14 passed。
- `git diff --check`: 无输出。
- 主程序目标 `wh-robot-visual`: 构建通过。

## 关注点

- 当前保存按钮只发出 `configurationSaved()`，未在 Dialog 内绑定具体正式配置路径；这是为了保持 Dialog 不直接写生产 Engine/外部状态，后续集成层应负责持久化路径与刷新。
- 新增 UI 已具备 objectName 契约和控制器连接点，但尚未接入主窗口入口；Task 8 brief 未要求改主窗口。

## 评审修复

- 新增 `ShortageConfigDialog::validatedConfiguration() const` 安全交接 API：
  - 返回最近一次同时通过运行门禁和 `ShortageConfigStore::validate()` 的配置副本。
  - 保存被门禁或校验拦截时保持上一份成功值不变。
  - Dialog 仍不直接写生产 Engine 或配置文件，由后续集成层读取该副本并负责持久化。
- 新增回归测试：
  - 有效保存后 `configurationSaved()` 发出一次，`validatedConfiguration()` 暴露已编辑的 MES 地址。
  - 运行门禁拦截保存时不发出 `configurationSaved()`，已暴露配置不被污染。
  - 非法 endpoint 拦截保存时不发出 `configurationSaved()`，已暴露配置不被污染。
  - 明确断言完整逻辑测试页提供 8 个任务事件按钮。

## 评审修复验证

```bash
cmake --build build-shortage --target shortage_dialog_tests --parallel
QT_QPA_PLATFORM=offscreen ctest --test-dir build-shortage -R '^shortage_dialog_tests$' --output-on-failure
```

结果：

- `shortage_dialog_tests`: 1/1 passed。
