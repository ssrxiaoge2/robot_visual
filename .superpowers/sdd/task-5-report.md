# Task 5 Report: 全量回归、现场验证清单和用户文档同步

## Status

- Completed.

## Changes

- 修复 `live_shortage_coordinator_tests` 夹具：独立测试控制器默认手工源后，测试共享现场采样互斥场景必须先显式切到现场源。
- `README.md` 增加缺料完整逻辑测试、验证向导、窗口最小化/最大化和正式 FIFO/硬件隔离说明。
- `changelog/CHANGELOG.md` 增加 2026-07-14 缺料测试修复与独立验证控制台记录。
- `docs/superpowers/specs/2026-07-14-shortage-test-bugfix.md` 增加 VT-01～VT-15 现场验证结果表，所有结论初始为“未执行”。

## Verification

- `/opt/Qt/qt6.8/6.8.3/gcc_64/bin/qt-cmake -S . -B build-shortage-validation -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug`：通过。
- `cmake --build build-shortage-validation -j2`：通过，`wh-robot-visual` 和全部测试目标构建成功。
- 首轮全量 CTest：17/18 通过，`live_shortage_coordinator_tests` 因测试夹具未按新输入源门禁切到现场源失败。
- 修复后聚焦验证：`live_shortage_coordinator_tests` 通过。
- 修复后全量验证：`QT_QPA_PLATFORM=offscreen ctest --test-dir build-shortage-validation --output-on-failure`：18/18 通过。
- 边界扫描：
  - `src/shortagevalidationdialog.h/.cpp` 未命中 `ShortageEngine|LineManager|TaskQueue|AgvController|HuayanScheduler|CustomSysScheduler`。
  - 缺料窗口打开路径未新增 `exec()`；扫描到的 `src/mainwindow.cpp:1835 dlg->exec()` 是既有 `HandEyeDialog` 路径。
  - 关键枚举、结构、成员和验证类型注释命中。
- `git diff --check`：无输出。
- `git diff -- test-state.json test-state.backup.json test-events.jsonl`：无输出。

## Concerns

- 真实通信、窗口人工切换和现场设备未动作仍需现场人工填写 VT-01～VT-15 结果表确认。
- 未跟踪现场文件 `test-events.jsonl`、`test-state.backup.json`、`test-state.json` 保持未暂存、未修改、未删除。
