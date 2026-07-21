# 视觉锁定连续跟踪与 Rz 大角度保护实施计划

> **给智能体执行者：** 必需子技能：使用 `superpowers:subagent-driven-development`（推荐，且本仓库要求串行、同一时间最多一个子代理）或 `superpowers:executing-plans` 按任务执行本计划；步骤使用复选框语法记录进度。

**目标：** 修复 2026-07-19 现场发现的同层目标闭环摇摆、`Rz=90°` 重复累计旋转、Z 下探 30 秒默认超时、旧圆形锚点可信范围放过斜向旁站高箱，以及闭环阶段 `lockdist` 压过高层优先导致抓低层的问题。

**实施状态：** 任务 1-6 已完成；2026-07-20 根据 `2026-07-19+log (2).txt` 追加任务 7，恢复闭环阶段高层优先。

**架构：** `VisionHttpClient` 继续负责纯视觉候选解析、锚点/锁定距离计算和目标选择；初始锁定帧保留“最高层 + 固定侧 Y”，闭环跟踪帧改为选择 `lockdist` 最小候选。`HuayanScheduler` 负责阶段一 Rz 大角度确认与累计执行次数保护，并在 Z 下探命令上单独设置更长到位等待超时。

**技术栈：** C++17、Qt 6.8.3 Core/Gui/Network/Test、CMake/CTest、华研 Robot SDK V1.0.15.0。

## 全局约束

- Qt 路径固定为 `Qt6_DIR=/opt/Qt/qt6.8/6.8.3/gcc_64/lib/cmake/Qt6`，`qt-cmake=/opt/Qt/qt6.8/6.8.3/gcc_64/bin/qt-cmake`。
- 面向人的计划、规格、审查说明必须使用中文；代码标识符、宏名、函数名、命令和 SDK 原文保持原样。
- 不修改视觉服务接口、视觉模型、推理脚本或返回 JSON 结构。
- 不重新标定手眼矩阵。
- 不改变 `Func_captureN`、`Func_jiajin` 等示教器函数。
- 不改变 7.18 已验证的初始目标选择策略：最高层优先，同层按固定侧 Y 定边。
- 锚点可信范围从旧圆形半径改为 X/Y 独立矩形范围，只过滤明显偏出当前工位外圈的候选，不引入固定槽位。
- 锁定后禁止使用固定侧 Y 抢目标，闭环只按上一帧锁定目标的 `lockdist` 连续跟踪。
- 闭环阶段仍必须高层优先；`lockdist` 只用于最高层同层候选之间的连续跟踪。
- `Rz` 大角度保护只统计 `abs(Rz) >= kLargeRzJumpThreshold` 且实际允许执行的旋转；普通小角度 Rz 不计数。
- Z 下探只调整命令到位等待超时，不放大全局默认超时。
- 新增或修改的宏、`enum class`、接口、参数、成员变量和非显然业务分支必须写明语义、单位、生命周期或拒绝执行规则。
- 不执行 `git push`，除非用户后续明确要求。
- 保留用户现有未跟踪文件 `test-events.jsonl`、`test-state.backup.json`、`test-state.json`，不得加入提交。

---

## 文件结构

### 修改

- `src/visionclient.h`
  - 保留目标选择枚举和锁定上下文。
  - 更新 `LockTrackingFixedSide` 注释，说明该枚举仅为历史兼容或删除该枚举；闭环实现不再产出该原因。
  - 新增 `VISION_ANCHOR_MAX_TRUST_X_MM` 与 `VISION_ANCHOR_MAX_TRUST_Y_MM`，替代旧圆形 `VISION_ANCHOR_MAX_TRUST_XY_MM`。

- `src/visionclient.cpp`
  - 修改锁定闭环分支：`context.hasPreviousAnchorTarget == true` 时，候选通过可信范围和 `lockTrackRadius` 后，直接选择 `lockDistance` 最小者。
  - 修改锚点可信判断：从 `sqrt(anchorX² + anchorY²) <= 半径` 改为 `abs(anchorX) <= X半宽 && abs(anchorY) <= Y半宽`。
  - 追加闭环高层优先修正：锁定闭环分支先筛选最高层候选，再在同层候选中按 `lockDistance` 最小选择。
  - 初始锁定分支继续使用 `chooseFixedSideCandidate()`。
  - 更新选择原因：闭环选中目标统一使用 `LockTrackingTarget`。

- `tests/test_locked_target_selection.cpp`
  - 增加 2026-07-19 现场复现用例，证明闭环不会再因固定侧 Y 切到 `lockdist` 更远的同层目标。
  - 增加初始固定侧仍生效的回归测试，防止误删开局定边逻辑。
  - 增加圆形范围泄漏回归测试，证明斜向偏出但旧圆形距离仍可信的旁站高箱会被矩形锚点范围过滤。
  - 增加闭环高层优先回归测试，证明较低层候选即使 `lockdist` 更近，也不能抢过更高层候选。

- `src/huayanScheduler.h`
  - 增加阶段一大角度 Rz 已执行次数成员变量。
  - 成员变量注释写明单位、生命周期和计数条件。

- `src/huayanScheduler.cpp`
  - 增加 `HUAYAN_STAGE_ONE_MAX_LARGE_RZ_EXECUTIONS` 与 `HUAYAN_STAGE_ONE_Z_DESCEND_TIMEOUT_MS`。
  - 阶段一启动、停止、复位和锚点重置时清零 Rz 大角度次数。
  - `setGrabOffset()` 中保留两帧确认，再增加同一锁定目标周期内大角度执行次数限制。
  - `StageStep::DescendZ` 创建 `PendingCommand` 时设置 `cmd.timeoutMs = HUAYAN_STAGE_ONE_Z_DESCEND_TIMEOUT_MS`。

- `tests/test_huayan_scheduler_contract.cpp`
  - 增加源码契约断言：Z 下探设置 120 秒超时。
  - 增加源码契约断言：大角度 Rz 有累计次数保护和重复跳过日志。

- `docs/superpowers/specs/2026-07-19-vision-lock-tracking-and-rz-guard-design.md`
  - 实施后同步实际接口、宏名和验证结果。

- `docs/superpowers/plans/2026-07-19-vision-lock-tracking-and-rz-guard.md`
  - 执行任务时勾选步骤，保持计划与代码同步。

- `docs/superpowers/specs/2026-07-18-anchor-vision-target-trust-design.md`
  - 增加 2026-07-19 后续修正链接，不覆盖 7.18 历史。

- `docs/superpowers/plans/2026-07-18-anchor-vision-target-trust.md`
  - 增加 2026-07-19 后续修正链接，不覆盖 7.18 历史。

- `README.md`
  - 更新“视觉目标选择策略”说明：初始固定侧、锁定后 `lockdist` 连续跟踪、Rz 大角度次数保护、Z 下探超时。

- `changelog/CHANGELOG.md`
  - 按项目 changelog 方法追加 2026-07-19 现场修正记录。

---

## 任务 1：修复锁定后同层目标摇摆

**文件：**
- 修改：`tests/test_locked_target_selection.cpp`
- 修改：`src/visionclient.cpp`
- 修改：`src/visionclient.h`

**接口：**
- 依赖：
  - `VisionHttpClient::TargetSelectionContext::lockEnabled`
  - `VisionHttpClient::TargetSelectionContext::hasPreviousAnchorTarget`
  - `VisionHttpClient::TargetCandidate::lockDistance`
  - `VisionHttpClient::TargetSelectionReason::LockTrackingTarget`
- 产出：
  - 锁定后闭环目标选择规则：`bestIndex = min(lockDistance)`
  - 初始锁定规则不变：最高层同层多目标仍调用 `chooseFixedSideCandidate()`

- [x] **步骤 1：增加现场摇摆复现测试**

在 `tests/test_locked_target_selection.cpp` 的 `main()` 中，放在现有 `trackingKeepsLockedTarget` 用例后增加：

```cpp
    const auto trackingPrefersNearestLockDistanceOverFixedSide = VisionHttpClient::selectTarget(QJsonArray{
        target(60.2, -216.7, 1363.0),
        target(20.5, 41.0, 1284.0),
        target(-201.8, -191.4, 1472.0)
    }, trackingContext(20.0, -260.0), kIdentityHandEye);
    requireTrue(trackingPrefersNearestLockDistanceOverFixedSide.hasTarget(),
                "闭环锁定帧必须在锁定范围内选出连续目标");
    requireTrue(selected(trackingPrefersNearestLockDistanceOverFixedSide).sourceIndex == 1,
                "闭环锁定后必须选择 lockdist 最小的候选，不能再被固定侧 Y 抢到另一个同层目标");
    requireTrue(trackingPrefersNearestLockDistanceOverFixedSide.reason == Reason::LockTrackingTarget,
                "闭环锁定后即使同层多目标，也必须记录 LockTrackingTarget");
```

该用例复现 2026-07-19 `10:58:22` 的现场问题：`#1` 离锁定目标更近，旧逻辑会因固定侧选择到 `#0`。

- [x] **步骤 2：运行测试确认失败**

运行：

```bash
cmake --build build-field-fixes --target locked_target_selection_tests -j2
./build-field-fixes/tests/locked_target_selection_tests
```

预期：测试失败，失败信息包含：

```text
闭环锁定后必须选择 lockdist 最小的候选，不能再被固定侧 Y 抢到另一个同层目标
```

- [x] **步骤 3：修改闭环选择实现**

在 `src/visionclient.cpp` 的锁定闭环分支中，替换当前逻辑：

```cpp
            const QList<int> sameLayerIndexes =
                sameLayerIndexesFor(selection.candidates, lockedIndexes, context.lockSameLayerZTol);
            const int bestIndex = chooseFixedSideCandidate(selection.candidates, sameLayerIndexes, context);
            selection.selectedCandidateIndex = bestIndex;
            selection.lockMissingFrames = 0;
            selection.lockAnchorX = selection.candidates.at(bestIndex).anchorX;
            selection.lockAnchorY = selection.candidates.at(bestIndex).anchorY;
            selection.reason = sameLayerIndexes.size() > 1
                ? TargetSelectionReason::LockTrackingFixedSide
                : TargetSelectionReason::LockTrackingTarget;
            return selection;
```

替换为：

```cpp
            int bestIndex = lockedIndexes.first();
            for (int candidateIndex : lockedIndexes) {
                const TargetCandidate &candidate = selection.candidates.at(candidateIndex);
                const TargetCandidate &best = selection.candidates.at(bestIndex);
                if (candidate.lockDistance < best.lockDistance) {
                    bestIndex = candidateIndex;
                } else if (qFuzzyCompare(candidate.lockDistance + 1.0, best.lockDistance + 1.0)
                           && candidate.sourceIndex < best.sourceIndex) {
                    // 极少数距离完全相等时按原始下标稳定兜底，避免同一输入在不同平台上选择不稳定。
                    bestIndex = candidateIndex;
                }
            }
            selection.selectedCandidateIndex = bestIndex;
            selection.lockMissingFrames = 0;
            selection.lockAnchorX = selection.candidates.at(bestIndex).anchorX;
            selection.lockAnchorY = selection.candidates.at(bestIndex).anchorY;
            selection.reason = TargetSelectionReason::LockTrackingTarget;
            return selection;
```

- [x] **步骤 4：更新枚举注释**

在 `src/visionclient.h` 中找到：

```cpp
        LockTrackingFixedSide,   ///< 目标锁定：闭环帧锁定范围内同层多目标按固定侧选择。
```

替换为：

```cpp
        LockTrackingFixedSide,   ///< 历史兼容原因；2026-07-19 起闭环帧不再按固定侧选择，改按 lockDistance 最近。
```

- [x] **步骤 5：运行目标测试确认通过**

运行：

```bash
cmake --build build-field-fixes --target locked_target_selection_tests -j2
./build-field-fixes/tests/locked_target_selection_tests
```

预期输出：命令退出码为 `0`，无失败信息。

---

## 任务 2：增加 Rz 大角度累计执行次数保护

**文件：**
- 修改：`tests/test_huayan_scheduler_contract.cpp`
- 修改：`src/huayanScheduler.h`
- 修改：`src/huayanScheduler.cpp`

**接口：**
- 依赖：
  - `kLargeRzJumpThreshold`
  - `kLargeRzJumpDeltaTolerance`
  - `HuayanScheduler::setGrabOffset(double x, double y, double z, double rz)`
- 产出：
  - `HUAYAN_STAGE_ONE_MAX_LARGE_RZ_EXECUTIONS`
  - `m_stageOneLargeRzExecutionCount`
  - 同一阶段一锁定目标周期内大角度 Rz 最多执行一次

- [x] **步骤 1：增加源码契约测试**

在 `tests/test_huayan_scheduler_contract.cpp` 中增加以下检查函数：

```cpp
void requireStageOneLargeRzGuard(const QString &source)
{
    requireContains(source,
                    QStringLiteral("HUAYAN_STAGE_ONE_MAX_LARGE_RZ_EXECUTIONS"),
                    "必须定义阶段一 Rz 大角度最大执行次数宏，避免 90 度重复旋转");
    requireContains(source,
                    QStringLiteral("m_stageOneLargeRzExecutionCount"),
                    "必须记录同一阶段一目标锁定周期内已执行的大角度 Rz 次数");
    requireContains(source,
                    QStringLiteral("已执行过 Rz 大角度修正"),
                    "重复出现 Rz 大角度时必须输出现场可读日志");
    requireContains(source,
                    QStringLiteral("疑似视觉旧帧或角度歧义"),
                    "重复 Rz 大角度日志必须说明可能是视觉旧帧或角度歧义");
}
```

在 `main()` 中读取 `src/huayanScheduler.cpp` 后调用：

```cpp
    requireStageOneLargeRzGuard(huayanSchedulerSource);
```

如果当前测试文件已经有类似 `requireContains()` 辅助函数，复用现有函数，不重复定义。

- [x] **步骤 2：运行契约测试确认失败**

运行：

```bash
cmake --build build-field-fixes --target huayan_scheduler_contract_tests -j2
./build-field-fixes/tests/huayan_scheduler_contract_tests
```

预期：测试失败，提示缺少 `HUAYAN_STAGE_ONE_MAX_LARGE_RZ_EXECUTIONS` 或 `m_stageOneLargeRzExecutionCount`。

- [x] **步骤 3：新增 Rz 大角度次数宏**

在 `src/huayanScheduler.cpp` 中，放在 `kLargeRzJumpDeltaTolerance` 后：

```cpp
static constexpr int HUAYAN_STAGE_ONE_MAX_LARGE_RZ_EXECUTIONS = 1; // 同一阶段一锁定目标周期内允许实际执行的 Rz 大角度旋转次数；只统计 abs(Rz)>=kLargeRzJumpThreshold 的旋转，防止视觉旧帧导致 90° 重复累计。
```

- [x] **步骤 4：新增成员变量**

在 `src/huayanScheduler.h` 中，放在 `m_pendingLargeRz` 后：

```cpp
    int m_stageOneLargeRzExecutionCount = 0; ///< 阶段一当前锁定目标已实际执行的 Rz 大角度次数；阶段启动/停止/锁定重置时清零，普通小角度 Rz 不计数。
```

- [x] **步骤 5：在阶段一状态重置处清零计数**

在 `src/huayanScheduler.cpp` 中所有已清理以下变量的位置：

```cpp
    m_pendingLargeRzConfirmation = false;
    m_pendingLargeRz = 0.0;
```

紧跟增加：

```cpp
    m_stageOneLargeRzExecutionCount = 0;
```

至少应覆盖：

- 阶段一启动初始化。
- 调度停止或复位的阶段状态清理。
- 视觉锚点锁定重置函数。

- [x] **步骤 6：在 `setGrabOffset()` 中增加重复大角度保护**

在 `src/huayanScheduler.cpp` 的 `setGrabOffset()` 中，保留现有两帧确认逻辑，并把确认成功分支：

```cpp
            emit logMessage(QStringLiteral("[阶段一] Rz 大角度跳变已连续确认 Rz=%1，允许执行旋转")
                                .arg(rz, 0, 'f', 1));
            m_pendingLargeRzConfirmation = false;
            m_pendingLargeRz = 0.0;
```

替换为：

```cpp
            if (m_stageOneLargeRzExecutionCount >= HUAYAN_STAGE_ONE_MAX_LARGE_RZ_EXECUTIONS) {
                effectiveRz = 0.0;
                emit logMessage(QStringLiteral("[阶段一] 已执行过 Rz 大角度修正 %1/%2，当前 Rz=%3 疑似视觉旧帧或角度歧义，本轮跳过 Rz 旋转")
                                    .arg(m_stageOneLargeRzExecutionCount)
                                    .arg(HUAYAN_STAGE_ONE_MAX_LARGE_RZ_EXECUTIONS)
                                    .arg(rz, 0, 'f', 1));
            } else {
                ++m_stageOneLargeRzExecutionCount;
                emit logMessage(QStringLiteral("[阶段一] Rz 大角度跳变已连续确认 Rz=%1，允许执行旋转（本目标大角度次数 %2/%3）")
                                    .arg(rz, 0, 'f', 1)
                                    .arg(m_stageOneLargeRzExecutionCount)
                                    .arg(HUAYAN_STAGE_ONE_MAX_LARGE_RZ_EXECUTIONS));
            }
            m_pendingLargeRzConfirmation = false;
            m_pendingLargeRz = 0.0;
```

说明：

- 第一次确认成功时计数从 `0` 变成 `1`，允许执行。
- 第二次及后续确认成功时不再执行，`effectiveRz = 0.0`。
- 小角度 Rz 不经过该分支，不计入次数。

- [x] **步骤 7：运行契约测试确认通过**

运行：

```bash
cmake --build build-field-fixes --target huayan_scheduler_contract_tests -j2
./build-field-fixes/tests/huayan_scheduler_contract_tests
```

预期输出：命令退出码为 `0`，无失败信息。

---

## 任务 3：Z 下探单独增加到位等待超时

**文件：**
- 修改：`tests/test_huayan_scheduler_contract.cpp`
- 修改：`src/huayanScheduler.cpp`

**接口：**
- 依赖：
  - `PendingCommand::timeoutMs`
  - `StageStep::DescendZ`
- 产出：
  - `HUAYAN_STAGE_ONE_Z_DESCEND_TIMEOUT_MS = 120000`
  - Z 下探命令 `cmd.timeoutMs = HUAYAN_STAGE_ONE_Z_DESCEND_TIMEOUT_MS`

- [x] **步骤 1：增加源码契约测试**

在 `tests/test_huayan_scheduler_contract.cpp` 中增加以下检查函数：

```cpp
void requireStageOneZDescendTimeout(const QString &source)
{
    requireContains(source,
                    QStringLiteral("HUAYAN_STAGE_ONE_Z_DESCEND_TIMEOUT_MS"),
                    "必须定义阶段一 Z 下探专用到位等待超时");
    requireContains(source,
                    QStringLiteral("cmd.timeoutMs = HUAYAN_STAGE_ONE_Z_DESCEND_TIMEOUT_MS"),
                    "Z 下探 PendingCommand 必须使用专用超时，不能继续使用 30000ms 默认值");
}
```

在 `main()` 中读取 `src/huayanScheduler.cpp` 后调用：

```cpp
    requireStageOneZDescendTimeout(huayanSchedulerSource);
```

- [x] **步骤 2：运行契约测试确认失败**

运行：

```bash
cmake --build build-field-fixes --target huayan_scheduler_contract_tests -j2
./build-field-fixes/tests/huayan_scheduler_contract_tests
```

预期：测试失败，提示缺少 Z 下探专用超时。

- [x] **步骤 3：新增 Z 下探超时宏**

在 `src/huayanScheduler.cpp` 中，放在 `HUAYAN_MAX_Z_DESCEND_MM` 后：

```cpp
static constexpr int HUAYAN_STAGE_ONE_Z_DESCEND_TIMEOUT_MS = 120000; // 阶段一 Z 下探专用到位等待超时(ms)；1 米级下探可能超过 30s，普通 X/Y/Rz 微调仍使用默认超时。
```

- [x] **步骤 4：设置 Z 下探命令超时**

在 `StageStep::DescendZ` 创建 `PendingCommand cmd;` 的代码块中，当前内容类似：

```cpp
            cmd.kind = PendingCommandKind::MoveRelTool;
            cmd.label = QStringLiteral("Z 下探");
            cmd.poseId = 2;
            cmd.direction = kZDescendInvert ? 0 : 1;
            cmd.distance = descend;
            beginCommandWhenReady(cmd);
```

修改为：

```cpp
            cmd.kind = PendingCommandKind::MoveRelTool;
            cmd.label = QStringLiteral("Z 下探");
            cmd.poseId = 2;
            cmd.direction = kZDescendInvert ? 0 : 1;
            cmd.distance = descend;
            cmd.timeoutMs = HUAYAN_STAGE_ONE_Z_DESCEND_TIMEOUT_MS;
            beginCommandWhenReady(cmd);
```

- [x] **步骤 5：补充 Z 下探日志中的超时说明**

把 Z 下探日志从：

```cpp
            emit logMessage(QStringLiteral("[阶段一] Z 下探 %1mm（未截断计划 %2mm = 视觉深度 %3mm - 余量 %4mm，下发上限 %5mm，硬上限 %6mm）")
```

改为：

```cpp
            emit logMessage(QStringLiteral("[阶段一] Z 下探 %1mm（未截断计划 %2mm = 视觉深度 %3mm - 余量 %4mm，下发上限 %5mm，硬上限 %6mm，到位超时 %7ms）")
```

并在 `.arg(HUAYAN_MAX_Z_DESCEND_MM, 0, 'f', 1)` 后追加：

```cpp
                                .arg(HUAYAN_STAGE_ONE_Z_DESCEND_TIMEOUT_MS));
```

- [x] **步骤 6：运行契约测试确认通过**

运行：

```bash
cmake --build build-field-fixes --target huayan_scheduler_contract_tests -j2
./build-field-fixes/tests/huayan_scheduler_contract_tests
```

预期输出：命令退出码为 `0`，无失败信息。

---

## 任务 4：同步 README、changelog 和追溯文档

**文件：**
- 修改：`README.md`
- 修改：`changelog/CHANGELOG.md`
- 修改：`docs/superpowers/specs/2026-07-18-anchor-vision-target-trust-design.md`
- 修改：`docs/superpowers/plans/2026-07-18-anchor-vision-target-trust.md`
- 修改：`docs/superpowers/specs/2026-07-19-vision-lock-tracking-and-rz-guard-design.md`
- 修改：`docs/superpowers/plans/2026-07-19-vision-lock-tracking-and-rz-guard.md`

**接口：**
- 依赖：任务 1-3 的实际宏名、日志文本和测试结果。
- 产出：文档与代码一致，能追溯 7.18 方案为什么在 7.19 被修正。

- [x] **步骤 1：更新 7.18 设计文档追溯说明**

在 `docs/superpowers/specs/2026-07-18-anchor-vision-target-trust-design.md` 标题后的元信息区域增加：

```markdown
**2026-07-19 后续修正：** 现场复测发现闭环跟踪阶段继续使用固定侧规则会导致同层两个目标之间摇摆；后续修正见 `docs/superpowers/specs/2026-07-19-vision-lock-tracking-and-rz-guard-design.md`。
```

- [x] **步骤 2：更新 7.18 实施计划追溯说明**

在 `docs/superpowers/plans/2026-07-18-anchor-vision-target-trust.md` 标题后的元信息区域增加：

```markdown
**2026-07-19 后续修正：** 本计划记录 7.18 第一版锚点锁定实现；闭环跟踪规则、Rz 大角度累计保护和 Z 下探超时修正见 `docs/superpowers/plans/2026-07-19-vision-lock-tracking-and-rz-guard.md`。
```

- [x] **步骤 3：更新 README**

在 `README.md` 的视觉目标选择或华研阶段一说明区域增加：

```markdown
### 阶段一视觉目标锁定策略（2026-07-19）

- 初始锁定：在固定拍照锚点可信范围内优先选择最高层；同层多个目标时按固定侧 Y 定边，默认选择 `raw Y` 最小目标。
- 闭环跟踪：目标锁定后不再按固定侧 Y 重新选择，只在锁定目标附近候选中选择 `lockdist` 最小者，避免同层两个箱子之间摇摆。
- Rz 大角度：`abs(Rz) >= 80°` 仍需连续两帧确认；同一目标锁定周期内大角度 Rz 最多实际执行一次，后续重复 `90°` 视为疑似视觉旧帧或角度歧义并跳过。
- Z 下探：阶段一 Z 下探使用 `120000ms` 专用到位等待超时，普通 X/Y/Rz 微调不受影响。
```

- [x] **步骤 4：更新 changelog**

在 `changelog/CHANGELOG.md` 顶部按现有格式追加一条 2026-07-19 记录；若文件使用日期标题，新增：

```markdown
## 2026-07-19

- 修复阶段一视觉锁定后的同层目标摇摆：初始帧继续使用“最高层 + 固定侧 Y”定边，闭环帧改为按上一帧锁定目标的 `lockdist` 最近候选连续跟踪。
- 增加阶段一 Rz 大角度累计保护：`abs(Rz) >= 80°` 仍需连续两帧确认，但同一锁定目标周期内最多实际执行一次，避免视觉旧帧或角度歧义导致 90° 重复累计旋转。
- 将阶段一 Z 下探命令到位等待超时单独调整为 `120000ms`，避免 1 米级下探被 30 秒默认超时误杀。
- 补充 2026-07-19 现场复测追溯文档，记录 7.18 锚点锁定方案的后续修正原因。
```

- [x] **步骤 5：更新 7.19 文档实施状态**

任务 1-3 验证通过后，把 `docs/superpowers/specs/2026-07-19-vision-lock-tracking-and-rz-guard-design.md` 的状态改为：

```markdown
**状态：** 已实施并完成自动化验证，等待现场复测
```

在本计划顶部增加：

```markdown
**实施状态：** 任务 1-4 已完成；自动化测试通过，等待现场复测。
```

---

## 任务 5：完整验证与本地提交

**文件：**
- 修改：所有任务涉及文件

**接口：**
- 依赖：任务 1-4 已完成。
- 产出：本地提交，不推送。

- [x] **步骤 1：配置构建目录**

如果 `build-field-fixes` 不存在，运行：

```bash
/opt/Qt/qt6.8/6.8.3/gcc_64/bin/qt-cmake -S . -B build-field-fixes -DCMAKE_BUILD_TYPE=Debug
```

预期：CMake 配置成功，最后输出包含：

```text
Build files have been written to:
```

- [x] **步骤 2：运行重点测试**

运行：

```bash
cmake --build build-field-fixes --target locked_target_selection_tests huayan_scheduler_contract_tests -j2
./build-field-fixes/tests/locked_target_selection_tests
./build-field-fixes/tests/huayan_scheduler_contract_tests
```

预期：两个测试命令退出码均为 `0`。

- [x] **步骤 3：运行全部 CTest**

运行：

```bash
ctest --test-dir build-field-fixes --output-on-failure
```

预期：所有测试通过，例如：

```text
100% tests passed
```

- [x] **步骤 4：检查文档语言和占位符**

运行：

```bash
pattern='TO''DO|TB''D|fill'' in|implement'' later|Similar'' to Task'
rg -n "$pattern" docs/superpowers/specs/2026-07-19-vision-lock-tracking-and-rz-guard-design.md docs/superpowers/plans/2026-07-19-vision-lock-tracking-and-rz-guard.md
```

预期：无输出。

- [x] **步骤 5：检查变更范围**

运行：

```bash
git status --short
git diff -- src/visionclient.h src/visionclient.cpp src/huayanScheduler.h src/huayanScheduler.cpp tests/test_locked_target_selection.cpp tests/test_huayan_scheduler_contract.cpp README.md changelog/CHANGELOG.md docs/superpowers/specs/2026-07-18-anchor-vision-target-trust-design.md docs/superpowers/plans/2026-07-18-anchor-vision-target-trust.md docs/superpowers/specs/2026-07-19-vision-lock-tracking-and-rz-guard-design.md docs/superpowers/plans/2026-07-19-vision-lock-tracking-and-rz-guard.md
```

预期：

- 只包含本计划列出的文件变更。
- `test-events.jsonl`、`test-state.backup.json`、`test-state.json` 仍为未跟踪且未加入提交。

- [x] **步骤 6：本地提交**

运行：

```bash
git add src/visionclient.h src/visionclient.cpp src/huayanScheduler.h src/huayanScheduler.cpp tests/test_locked_target_selection.cpp tests/test_huayan_scheduler_contract.cpp README.md changelog/CHANGELOG.md docs/superpowers/specs/2026-07-18-anchor-vision-target-trust-design.md docs/superpowers/plans/2026-07-18-anchor-vision-target-trust.md docs/superpowers/specs/2026-07-19-vision-lock-tracking-and-rz-guard-design.md docs/superpowers/plans/2026-07-19-vision-lock-tracking-and-rz-guard.md
git commit -m "fix: stabilize locked vision tracking and rz guard"
```

预期：生成一个本地提交，不执行 `git push`。

---

## 任务 6：追加锚点可信范围从圆形改为矩形

**文件：**
- 修改：`tests/test_locked_target_selection.cpp`
- 修改：`src/visionclient.h`
- 修改：`src/visionclient.cpp`
- 修改：`src/huayanScheduler.cpp`
- 修改：`README.md`
- 修改：`docs/superpowers/specs/2026-07-19-vision-lock-tracking-and-rz-guard-design.md`
- 修改：`docs/superpowers/plans/2026-07-19-vision-lock-tracking-and-rz-guard.md`
- 修改：`changelog/CHANGELOG.md`

**接口：**
- 产出：
  - `VISION_ANCHOR_MAX_TRUST_X_MM`
  - `VISION_ANCHOR_MAX_TRUST_Y_MM`
  - `TargetSelectionContext::maxTrustX`
  - `TargetSelectionContext::maxTrustY`
- 行为：
  - 候选可信判断从圆形 `sqrt(anchorX² + anchorY²) <= 半径` 改为矩形 `abs(anchorX) <= maxTrustX && abs(anchorY) <= maxTrustY`。
  - `anchorDistance` 继续保留为日志诊断值，不再作为可信条件。

- [x] **步骤 1：增加圆形范围泄漏回归测试**

在 `tests/test_locked_target_selection.cpp` 中增加用例：一个本工位低箱与一个斜向偏出的旁站高箱同时出现时，旁站高箱虽然满足旧圆形半径，但必须因 X/Y 矩形范围超界而标记为不可信，最终选择本工位候选。

- [x] **步骤 2：确认旧实现测试失败**

运行：

```bash
cmake --build build-field-fixes --target locked_target_selection_tests -j2
./build-field-fixes/tests/locked_target_selection_tests
```

预期旧实现失败，失败信息包含：

```text
斜向偏出但圆形距离仍小于旧阈值的旁站高箱，必须被 X/Y 矩形锚点可信范围过滤
```

- [x] **步骤 3：修改锚点可信接口和实现**

在 `src/visionclient.h` 中删除旧圆形阈值宏：

```cpp
VISION_ANCHOR_MAX_TRUST_XY_MM
```

改为：

```cpp
VISION_ANCHOR_MAX_TRUST_X_MM
VISION_ANCHOR_MAX_TRUST_Y_MM
```

在 `TargetSelectionContext` 中将 `maxTrustDistance` 改为 `maxTrustX` 与 `maxTrustY`。

在 `src/visionclient.cpp` 中新增矩形判断辅助函数，注释写明：该规则按初始拍照点位的物理 X/Y 外圈过滤旁站目标，不再使用圆形距离作为可信条件。

- [x] **步骤 4：同步日志和文档文案**

将“最高目标离拍照锚点过远”调整为“最高目标超出拍照锚点矩形可信范围”，并在 README 与本设计中记录：

- 旧圆形半径会放过斜向进入的旁站高箱。
- 新规则使用 X/Y 独立阈值。
- 默认值为 `X=300mm`、`Y=450mm`，现场可按轴向和正常目标日志微调。

- [x] **步骤 5：运行验证**

运行：

```bash
cmake --build build-field-fixes --target locked_target_selection_tests anchor_target_selection_tests -j2
./build-field-fixes/tests/locked_target_selection_tests
./build-field-fixes/tests/anchor_target_selection_tests
ctest --test-dir build-field-fixes --output-on-failure
```

预期：全部通过。

---

## 任务 7：恢复闭环阶段高层优先

**文件：**
- 修改：`tests/test_locked_target_selection.cpp`
- 修改：`src/visionclient.cpp`
- 修改：`README.md`
- 修改：`docs/superpowers/specs/2026-07-19-vision-lock-tracking-and-rz-guard-design.md`
- 修改：`docs/superpowers/plans/2026-07-19-vision-lock-tracking-and-rz-guard.md`
- 修改：`changelog/CHANGELOG.md`

**接口：**
- 依赖：
  - `sameLayerIndexesFor(const QList<TargetCandidate> &, const QList<int> &, double)`
  - `TargetSelectionContext::lockSameLayerZTol`
  - `TargetCandidate::depth`
  - `TargetCandidate::lockDistance`
- 行为：
  - 锁定闭环候选先通过矩形可信范围和 `lockTrackRadius`。
  - 在通过候选中先按最高层筛选；`depth_compensated` 越小表示越高。
  - 只有最高层同层候选之间才按 `lockDistance` 最近选择。

- [x] **步骤 1：增加闭环低层误抓回归测试**

在 `tests/test_locked_target_selection.cpp` 中增加用例：低层候选 `lockdist=5mm`，高层候选 `lockdist=200mm`，两者都可信且都在锁定半径内，必须选择高层候选。

- [x] **步骤 2：确认旧实现测试失败**

运行：

```bash
cmake --build build-field-fixes --target locked_target_selection_tests -j2
./build-field-fixes/tests/locked_target_selection_tests
```

旧实现预期失败，失败信息包含：

```text
闭环帧仍必须最高层优先，不能因为较低层目标 lockdist 更近就抓低层箱子
```

- [x] **步骤 3：修改闭环选择实现**

在 `src/visionclient.cpp` 的锁定闭环分支中，将 `lockedIndexes` 直接按 `lockDistance` 选择，改为：

```cpp
const QList<int> sameLayerIndexes =
    sameLayerIndexesFor(selection.candidates, lockedIndexes, context.lockSameLayerZTol);
int bestIndex = sameLayerIndexes.first();
for (int candidateIndex : sameLayerIndexes) {
    const TargetCandidate &candidate = selection.candidates.at(candidateIndex);
    const TargetCandidate &best = selection.candidates.at(bestIndex);
    if (candidate.lockDistance < best.lockDistance) {
        bestIndex = candidateIndex;
    } else if (qFuzzyCompare(candidate.lockDistance + 1.0, best.lockDistance + 1.0)
               && candidate.sourceIndex < best.sourceIndex) {
        // 闭环帧先按最高层过滤，再在同层内按 lockDistance 最近跟踪；
        // 极少数距离完全相等时按原始下标稳定兜底，避免同一输入在不同平台上选择不稳定。
        bestIndex = candidateIndex;
    }
}
```

- [x] **步骤 4：同步文档**

更新 README、7.19 设计、7.19 计划和 changelog，明确：

- `log/2026-07-19+log (2).txt` 中 `17:15` 附近暴露了闭环低层误抓风险。
- `lockdist` 连续性不能压过高层优先。
- 修正后闭环帧优先级为：矩形可信范围和锁定半径过滤 → 最高层 → 同层 `lockdist` 最近。

- [x] **步骤 5：运行验证**

运行：

```bash
cmake --build build-field-fixes --target locked_target_selection_tests -j2
./build-field-fixes/tests/locked_target_selection_tests
ctest --test-dir build-field-fixes --output-on-failure
cmake --build build-field-fixes -j2
```

预期：目标测试、全量测试和完整构建均通过。

---

## 自查清单

- 规格覆盖：任务 1 覆盖目标锁定连续跟踪；任务 2 覆盖 Rz 大角度累计保护；任务 3 覆盖 Z 下探专用超时；任务 4 覆盖 README、changelog 和 7.18/7.19 追溯；任务 5 覆盖验证和本地提交；任务 6 覆盖圆形锚点范围改为 X/Y 矩形范围；任务 7 覆盖闭环阶段高层优先。
- 占位符检查：计划中没有常见占位符关键字或“类似任务 N”的占位描述。
- 类型一致性：计划中使用的宏名、成员变量名、枚举值和文件路径在任务内均有定义或来自现有代码。
- 中文规则：正文说明、步骤、预期结果和自查说明使用中文；代码标识符、命令、路径和 SDK 原文保持原样。
