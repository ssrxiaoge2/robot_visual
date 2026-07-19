# 固定拍照锚点视觉目标可信选择设计

**日期：** 2026-07-18
**状态：** 已实施并完成自动化验证；2026-07-18 现场验证确认“目标锁定”策略有效，40961 待向华研厂家确认
**关联日志：** `log/2026-07-17+log.txt`、`log/2026-07-18+log.txt`
**关联上一版设计：** `docs/superpowers/specs/2026-07-16-field-fault-diagnostics-and-workflow-isolation-design.md`
**2026-07-19 后续修正：** 现场复测发现闭环跟踪阶段继续使用固定侧规则会导致同层两个目标之间摇摆；后续修正见 `docs/superpowers/specs/2026-07-19-vision-lock-tracking-and-rz-guard-design.md`。

## 1. 背景

上一版修复后，现场日志证明 `MoveRelL` 失败诊断有效，49601 的触发点已经可以定位到具体命令：

- `16:23:29`：`相对运动 Y`，负向 `253.413mm`，SDK 返回 `49601（Target orientation exceeded cartesian safety space）`。
- `16:31:52`：`Z 下探`，正向 `1806.004mm`，SDK 返回 `49601`。

这些日志说明问题不只是控制器内部的奇异点或安全空间判断，也和上位机目标选择、闭环追踪和异常运动保护有关。尤其是多目标场景下，当前视觉选择使用“当前相机中心”的 ROI；机械臂每次微调后相机中心也移动，ROI 随之漂移，可能导致闭环过程中切换到旁边工位或另一摞箱子。

现场目标已经明确：

- 取料必须优先抓最高层箱子，低层箱子可能碰到夹爪。
- “最高层”必须限定在当前工位可信目标内，不应抓视野里旁边工位的全局最高箱。
- 阈值目前不确定，需要第一版保守、可调，并通过日志指导后续现场微调。

## 2. 目标

本次设计要实现：

1. 以阶段一最初到达的 `Func_captureN` 拍照位作为本轮固定视觉锚点。
2. 视觉候选不再只按当前相机中心判断，而是换算到“相对初始拍照锚点”的工具系 XY。
3. 在锚点可信范围内按 Z 最高优先；最高层候选有多个时，再按锚点 XY 最近选择。
4. 如果最高层目标离初始拍照锚点过远，判定为目标不可信，不下发大幅追踪运动。
5. 增加单次 XY 微调和 Z 下探的上位机硬保护，避免异常选择继续放大为 49601。
6. 输出紧凑但足够诊断的视觉选择日志，便于现场后续调参。

## 3. 非目标

- 不修改视觉服务接口和识别模型。
- 不重新标定手眼矩阵。
- 不改变 `Func_captureN`、`Func_jiajin` 等示教器函数。
- 不把每个工位的阈值一次性全部标定完成；第一版使用全局保守阈值，后续根据日志再决定是否拆成工位级配置。
- 不移除上一版 `MoveRelL` 失败四行诊断日志。

## 4. 现状问题分析

### 4.1 ROI 随相机漂移

当前 `VisionHttpClient::selectTarget(const QJsonArray &objects)` 使用原始 `offset_mm.x/y` 做 `±500mm` 范围判断。这个坐标是相对当前相机中心的。阶段一闭环中：

1. 机械臂到 `Func_captureN` 拍照位。
2. 视觉返回候选。
3. 机械臂按选中目标做 XY 微调。
4. 相机随工具一起移动。
5. 下一帧视觉的 `offset_mm.x/y` 已经相对新的相机中心。

因此“当前工位范围”实际被相机拖着走。多个箱子在视野中时，算法可能在不同帧之间切换目标。

### 4.2 Z 优先不能跨工位全局生效

现场要求 Z 优先是正确的，因为低层箱子会碰夹爪。但 Z 优先必须在“当前工位可信目标”内成立。否则旁边工位更高的箱子会胜出，机械臂会向旁边工位大幅移动。

### 4.3 异常目标会变成异常运动

`2026-07-17+log.txt` 中存在两种异常运动：

- 初始选择后 XY 微调超过 `250mm`。
- Z 下探出现 `1806mm`。

这类运动不应依赖控制器最后兜底拒绝。上位机应先识别为目标不可信或运动超限，直接失败并记录原因。

## 5. 方案选择

讨论过三类方案：

### 方案 A：固定矩形 ROI 硬过滤

使用初始拍照位中心的固定矩形范围，范围外候选直接排除。

优点：实现简单。
缺点：箱子高度不同、每个工位拍照点不同，固定矩形可能误杀正确目标。

### 方案 B：固定锚点 + Z 优先 + XY 可信度保护

建立初始拍照锚点。每个候选先经过手眼矩阵转换为工具系对准偏移，再叠加当前已执行的工具系 XY 累计微调，得到相对初始锚点的候选 XY。选择时：

1. 先找最高层候选。
2. 如果最高层候选离锚点超过可信阈值，判定目标不可信。
3. 可信最高层候选有多个时，选锚点 XY 最近者。
4. 闭环过程中目标跳变过大时判定不可信。

优点：符合“拍照位是业务锚点”的现场逻辑，同时保留 Z 优先。
缺点：需要维护本轮累计 XY，并新增上下文接口。

### 方案 C：每个工位独立阈值

为 1～12 工位配置各自锚点可信距离、层高容差和跳变阈值。

优点：最终最精细。
缺点：当前缺少足够日志样本，一次性做会增加现场调参成本。

**选择：** 第一版采用方案 B。阈值使用全局宏，先保守偏大；日志记录每次正常和异常的锚点距离，后续再决定是否演进到方案 C。

## 6. 详细设计

### 6.1 新增参数

参数先集中放在 `visionclient.h` 和 `huayanScheduler.cpp`，使用宏或 `constexpr`，并写明单位和现场调参方式。

- `VISION_ANCHOR_MAX_TRUST_XY_MM`：最高层候选离初始拍照锚点的最大可信距离，单位 mm。
- `VISION_ANCHOR_SAME_LAYER_Z_TOL_MM`：最高层同层容差，单位 mm。
- `VISION_ANCHOR_SWITCH_MAX_XY_MM`：闭环过程中选中目标相对上一帧锚点位置的最大跳变距离，单位 mm。
- `HUAYAN_MAX_SINGLE_XY_ADJUST_MM`：单次阶段一 XY 微调最大允许距离，单位 mm。
- `HUAYAN_MAX_Z_DESCEND_MM`：阶段一 Z 下探硬安全上限，单位 mm。

初始建议值：

- `VISION_ANCHOR_MAX_TRUST_XY_MM = 450.0`
- `VISION_ANCHOR_SAME_LAYER_Z_TOL_MM = 20.0`
- `VISION_ANCHOR_SWITCH_MAX_XY_MM = 220.0`
- `HUAYAN_MAX_SINGLE_XY_ADJUST_MM = 250.0`
- `HUAYAN_MAX_Z_DESCEND_MM = 1078.0`

这些值不是最终现场标定值，只是第一版保守保护。后续根据日志收紧。

### 6.2 锚点选择上下文

`VisionHttpClient` 增加 `TargetSelectionContext`：

- `anchorEnabled`：是否启用固定锚点选择。
- `accumulatedToolX`：本轮从初始拍照位到当前相机位置已经执行的工具系 X 位移，单位 mm。
- `accumulatedToolY`：本轮从初始拍照位到当前相机位置已经执行的工具系 Y 位移，单位 mm。
- `hasPreviousAnchorTarget`：是否已有上一帧选中目标。
- `previousAnchorX` / `previousAnchorY`：上一帧选中目标相对初始锚点的 XY，单位 mm。

`VisionHttpClient` 在发起推理前使用当前上下文。`HuayanScheduler` 负责维护上下文，因为只有调度器知道机械臂本轮实际完成了哪些工具系微调。

### 6.3 候选坐标换算

每个视觉候选继续从 JSON 解析：

- 原始 `offset_mm.x/y`
- 原始 `depth_compensated`
- `angle`
- `confidence`

然后对每个候选执行现有 `transformToMm()`，得到工具系偏移：

- `toolX = raw.x`
- `toolY = raw.y`
- `toolZ = raw.z`
- `toolRz = raw.rz`

注意：现有阶段一移动方向是：

- X：`addMove(0, x)`
- Y：`addMove(1, -y)`

因此用于锚点距离判断的“对准运动坐标”应为：

- `alignmentX = toolX`
- `alignmentY = -toolY`

候选相对初始拍照锚点的 XY 为：

- `anchorX = accumulatedToolX + alignmentX`
- `anchorY = accumulatedToolY + alignmentY`

这样得到的是“如果从最初拍照位直接对准这个候选，工具大约需要移动到哪里”的业务坐标。

### 6.4 选择规则

启用锚点上下文时，使用新规则：

1. 解析全部合法候选，并计算每个候选的 `anchorX/anchorY/anchorDistance`。
2. 找到深度最小的一组最高层候选，深度差小于等于 `VISION_ANCHOR_SAME_LAYER_Z_TOL_MM` 视为同层。
3. 从最高层候选中选择 `anchorDistance` 最小者。
4. 如果该候选 `anchorDistance > VISION_ANCHOR_MAX_TRUST_XY_MM`，返回无目标，原因是 `AnchorDistanceTooFar`。
5. 如果已有上一帧目标，且本次候选与上一帧锚点位置距离超过 `VISION_ANCHOR_SWITCH_MAX_XY_MM`，返回无目标，原因是 `AnchorTargetJumpTooFar`。
6. 通过可信检查后，返回选中候选。

未启用锚点上下文时，保留现有选择逻辑，用于兼容测试或非取料场景。

### 6.5 阶段一上下文维护

`HuayanScheduler` 在阶段一启动和移动中维护锚点状态：

- 启动阶段一时清零：
  - `m_anchorAccumulatedToolX = 0`
  - `m_anchorAccumulatedToolY = 0`
  - `m_anchorHasPreviousTarget = false`
- 每次进入 `WaitForVision` 前，把当前上下文注入 `VisionHttpClient`。
- 每个 `MoveToGrab` 单轴相对运动完成后，按实际完成的 `RelMove` 更新累计位移：
  - X 正向加，X 负向减。
  - Y 正向加，Y 负向减。
  - Rz 不参与锚点 XY 累计。
- 视觉选中目标后，记录本帧选中候选的 `anchorX/anchorY` 作为下一帧跳变保护基准。

### 6.6 运动保护

即使视觉选择通过，也增加上位机运动保护：

1. 阶段一单次 X 或 Y 微调距离超过 `HUAYAN_MAX_SINGLE_XY_ADJUST_MM` 时，不下发 `MoveRelL`，直接阶段失败。
2. 阶段一 Z 下探先按未截断计划值 `plannedDescend = 视觉深度 - 工位余量` 判断；若 `plannedDescend > HUAYAN_MAX_Z_DESCEND_MM`，不调用 `calculateGrabDescend()` 截断、不下发 `MoveRelL`，直接阶段失败。
3. 阶段失败日志必须包含：
   - 轴向
   - 计划运动距离
   - 安全上限
   - 当前候选选择摘要

这层保护用于避免异常目标继续放大为 SDK 49601。

### 6.7 日志

视觉选择日志保持单行，新增字段：

- 原始候选下标。
- 当前工具系对准偏移 `toolX/toolY/toolZ`。
- 锚点坐标 `anchorX/anchorY`。
- 锚点距离。
- 是否可信。
- 选择原因。

目标不可信时日志示例：

```text
[视觉选择] 候选数=2 最高层=#0 锚点距离=512.4mm 上限=450.0mm 选中=无 原因=最高目标离拍照锚点过远，目标不可信
```

单次运动保护日志示例：

```text
[阶段一] 目标不可信：计划 Y 微调 253.4mm 超过单次上限 250.0mm，拒绝下发 MoveRelL
```

### 6.8 现场调参方式

第一版上线后，现场重点观察日志中的：

- 正常成功抓取的 `anchorDistance` 分布。
- 误选旁边工位时的 `anchorDistance`。
- 闭环过程中正常目标跳变距离。
- 失败时计划单次 XY 距离和 Z 下探距离。

调参原则：

1. `VISION_ANCHOR_MAX_TRUST_XY_MM` 设置为正常最大锚点距离加安全冗余。
2. `VISION_ANCHOR_SWITCH_MAX_XY_MM` 设置为正常闭环帧间跳变最大值加冗余。
3. `HUAYAN_MAX_SINGLE_XY_ADJUST_MM` 应小于控制器容易触发 49601 的大幅运动距离。
4. `HUAYAN_MAX_Z_DESCEND_MM` 保持硬安全上限，不因临时调试被放大。

### 6.9 最终实现接口

本轮最终落地接口名如下：

- `VisionHttpClient::TargetSelectionContext`：阶段一固定拍照锚点选择上下文。
- `VisionHttpClient::setTargetSelectionContext(const TargetSelectionContext &context)`：每次 `/inference` 前由调度器注入上下文。
- `VisionHttpClient::selectTarget(const QJsonArray &objects, const TargetSelectionContext &context, const float handEyeMatrix[4][4])`：锚点选择纯逻辑入口；无锚点时保留 `selectTarget(const QJsonArray &objects)` 兼容旧逻辑。
- `VisionHttpClient::formatTargetSelectionLog(const TargetSelection &selection)`：输出单行候选/选择摘要。
- `VisionHttpClient::rawCoordinatesReady(double x, double y, double z, double rz, double anchorX, double anchorY)`：视觉结果携带选中目标相对初始拍照锚点的 XY。
- `VisionHttpClient::targetRejectedByTrustRule(TargetSelectionReason reason, QString message)`：仅在 `AnchorDistanceTooFar` / `AnchorTargetJumpTooFar` 时发出，表示“检测到候选但被锚点可信规则拒绝”，不能复用普通 `noObjectDetected()`。
- `HuayanScheduler::setVisionClient(VisionHttpClient *client)`：注入非拥有视觉客户端指针。
- `HuayanScheduler::makeVisionTargetSelectionContext() const`、`resetVisionAnchorTracking()`、`recordCompletedGrabMove(const RelMove &move)`：维护阶段一锚点累计和上一帧目标。
- `HuayanScheduler::validateStageOneRelMoveBeforeDispatch(const RelMove &move)`：阶段一 XY 微调下发前硬保护。
- `HuayanScheduler::onVisionTargetRejectedForPickup(VisionHttpClient::TargetSelectionReason reason, const QString &msg)`：锚点距离过远或闭环跳变过大时直接 `emitOperationError` 使阶段失败，不进入 `onVisionNoObject()` 的搜索下移。

最终状态补充：

- 普通“没有检测到目标”继续走 `noObjectDetected()`，保留原搜索下移兼容行为。
- 锚点可信规则拒绝走独立拒绝信号，阶段一直接 fail-closed，避免旁边工位/跳变目标导致搜索下移或继续追踪。
- Z 下探硬上限按未截断计划值（单位 mm）先判定；只有计划值未超过硬上限时，才用 `calculateGrabDescend(..., qMin(kMaxDescend, HUAYAN_MAX_Z_DESCEND_MM))` 计算实际下发距离。

## 7. 测试策略

### 7.1 视觉纯逻辑测试

新增或扩展 `tests/test_vision_target_selection.cpp`：

- 未启用锚点时，旧逻辑仍可通过。
- 启用锚点时，最高层候选优先。
- 最高层多个候选时，选锚点距离最近。
- 最高目标锚点距离过远时返回无目标。
- 上一帧目标与本帧目标跳变过大时返回无目标。
- 候选顺序变化不影响物理选择。

### 7.2 华研调度契约测试

扩展 `tests/test_huayan_scheduler_contract.cpp`：

- 阶段一启动时必须清零锚点累计量。
- 进入等待视觉前必须注入 `TargetSelectionContext`。
- `MoveToGrab` 完成后必须更新锚点累计位移。
- X/Y 单次微调超过上限时必须拒绝下发。
- Z 下探必须先按未截断计划值做硬安全上限 fail-closed，再计算实际下发值。
- `AnchorDistanceTooFar` / `AnchorTargetJumpTooFar` 必须发锚点拒绝信号，由调度器直接阶段失败，不能走普通无目标搜索下移。

### 7.3 集成验证

运行：

```bash
cmake --build build-field-fixes --target vision_target_selection_tests huayan_scheduler_contract_tests wh-robot-visual -j2
ctest --test-dir build-field-fixes -R '^(vision_target_selection_tests|huayan_scheduler_contract_tests)$' --output-on-failure
cmake --build build-field-fixes -j2
ctest --test-dir build-field-fixes --output-on-failure
```

## 8. 验收标准

- 多目标场景下，不再因为机械臂移动导致 ROI 漂移后切换目标。
- 最高层目标若离初始拍照锚点过远，系统判定目标不可信，不下发大幅 XY 追踪。
- 最高层目标离锚点过远或闭环目标跳变过大时，系统直接阶段失败，不进入“未检测到目标”的搜索下移。
- 阶段一单次 XY 微调超限时，不下发 `MoveRelL`。
- Z 下探按未截断计划值先判断，计划值超过 `HUAYAN_MAX_Z_DESCEND_MM` 时直接失败，不会通过截断后继续下发。
- 视觉日志能显示锚点距离和选择原因，现场能据此微调阈值。
- 49601 若仍发生，日志能区分是 SDK 拒绝正常范围内运动，还是上位机保护之前未覆盖的新异常。

## 9. 风险与缓解

- **风险：阈值过小误杀正确目标。** 第一版阈值保守偏大，并输出日志指导收紧。
- **风险：锚点累计方向错误。** 测试必须覆盖 X 正负、Y 正负，并按现有 `addMove(0, x)`、`addMove(1, -y)` 方向约定验证。
- **风险：视觉选择和运动保护重复报错。** 视觉不可信优先报视觉选择原因；运动超限只在视觉已选中但计划运动超限时触发。
- **风险：调试时有人把 Z 上限放大。** 把 `HUAYAN_MAX_Z_DESCEND_MM` 写成独立硬保护，并在日志中打印上限。

## 10. 后续扩展

如果全局阈值仍不足以覆盖 1～12 工位差异，再增加工位级配置：

- `anchorMaxTrustXY`
- `anchorSwitchMaxXY`
- `sameLayerZTol`

第一版不实现工位级配置，避免在样本不足时过度设计。

## 11. 自动化验证结果

2026-07-18 本地 `build-field-fixes` 验证结果：

- `git diff --check`：通过。
- `anchor_target_selection_tests`：构建通过，CTest 通过。
- `vision_target_selection_tests`：构建通过，CTest 通过，兼容旧选择逻辑。
- `huayan_scheduler_contract_tests`：构建通过，CTest 通过。
- `station_pickup_config_tests`：因 `HuayanScheduler.h` 新增 `VisionHttpClient` 依赖补齐 Qt Gui/Network 链接，并同步旧静态契约到 Z 硬上限实现后通过。
- `wh-robot-visual`：构建通过。
- 全量构建：`cmake --build build-field-fixes -j2` 通过。
- 全量 CTest：`ctest --test-dir build-field-fixes --output-on-failure` 11/11 通过。

## 12. 现场复测重点

- 多目标时日志中最高层目标的 `anchorDistance`。
- 被拒绝目标是否确实来自旁边工位或明显远离拍照位。
- 正常抓取的 `anchorDistance` 最大值，用于后续收紧 `VISION_ANCHOR_MAX_TRUST_XY_MM`。
- 是否还出现 49601；若出现，检查对应命令是否已在上位机保护范围之外。

## 13. 2026-07-18 现场复测后的修订结论

现场使用 `log/2026-07-18+log.txt` 复测后，第一版固定拍照锚点策略能暴露旁边工位目标和异常大位移，但仍不能稳定选中业务目标。主要表现为：

- 同层两个箱子高度接近时，视觉返回的 Z 抖动会让目标在左右两个箱子之间切换。
- 旁边工位目标偶发识别到时，如果它的 Z 被判断为更高，会让正常目标被拒绝或被抢走。
- 机械臂开始闭环微调后，如果本帧没有识别到原目标，而只识别到旁边工位目标，不能把旁边工位目标当成新的候选继续追踪。
- 49601 不能由视觉策略彻底根治，但错误目标导致的 380mm 以上大幅 X/Y 微调可以通过更稳定的目标选择显著减少。

因此，后续策略从“每帧重新按锚点和 Z 选择”升级为“初始锁定目标，闭环只跟踪锁定目标”。

## 14. 目标锁定策略

### 14.1 初始锁定

阶段一第一次到达 `Func_captureN` 拍照位后，视觉选择按以下顺序执行：

1. 先过滤离初始拍照锚点过远的候选，明显属于旁边工位的目标不能参与最高层竞争。
2. 在可信候选中选择最高层，满足低层箱子不能被夹爪碰撞的业务要求。
3. 最高层有多个同层候选时，不再按锚点最近或图像中心最近选择，而是按固定侧规则选择。
4. 选中后记录为本轮锁定目标，保存其锚点坐标、层高参考、固定侧配置和丢帧计数。

### 14.2 闭环跟踪

机械臂开始向锁定目标微调后，后续帧进入闭环跟踪模式：

1. 只允许在锁定目标附近查找候选。
2. 如果有候选落入锁定目标允许范围，则继续更新锁定目标位置并下发正常微调。
3. 如果本帧没有找到锁定目标，则本帧不下发 `MoveRelL`，记录一次“锁定目标暂时丢失”。
4. 连续丢失达到上限后，阶段一失败并停止调度。
5. 禁止因为原目标本帧丢失，就退回全局最高 Z 选择。
6. 禁止把旁边工位偶发识别到的目标当作锁定目标的替代目标。

这条规则的核心原则是：视觉可以短暂不稳定，但机械臂不能跟着视觉漂移。宁可等待或停止，也不能自动换到其他工位目标。

### 14.3 固定侧同层选择

同层多个候选时，使用现场可调宏固定选择一侧，避免两个箱子之间左右摇摆。

第一版建议使用 Y 轴固定侧：

```cpp
#define VISION_LOCK_FIXED_SIDE_AXIS_Y 1
#define VISION_LOCK_FIXED_SIDE_PICK_MIN 1
```

含义：

- `VISION_LOCK_FIXED_SIDE_AXIS_Y`：同层候选按 Y 轴做固定侧选择。
- `VISION_LOCK_FIXED_SIDE_PICK_MIN`：值为 `1` 时选择 Y 最小，值为 `0` 时选择 Y 最大。

如果现场确认应该按 X 轴区分左右，可增加对应宏并保持同一个选择接口。

### 14.4 新增参数

目标锁定参数集中定义在 `visionclient.h`，并在注释中写明单位和现场调参方式：

```cpp
#define VISION_LOCK_MAX_MISSING_FRAMES 3
#define VISION_LOCK_TRACK_RADIUS_MM 260.0
#define VISION_LOCK_SAME_LAYER_Z_TOL_MM 80.0
#define VISION_LOCK_FIXED_SIDE_AXIS_Y 1
#define VISION_LOCK_FIXED_SIDE_PICK_MIN 1
```

参数说明：

- `VISION_LOCK_MAX_MISSING_FRAMES`：锁定目标连续丢失多少帧后阶段失败，第一版使用 3 帧。
- `VISION_LOCK_TRACK_RADIUS_MM`：闭环候选离锁定目标锚点位置的最大允许距离，单位 mm。
- `VISION_LOCK_SAME_LAYER_Z_TOL_MM`：目标锁定模式下的同层 Z 容差，单位 mm。
- `VISION_LOCK_FIXED_SIDE_AXIS_Y`：同层固定侧选择是否使用 Y 轴。
- `VISION_LOCK_FIXED_SIDE_PICK_MIN`：固定侧选择最小值还是最大值。

这些参数仍是现场可调宏，不是最终标定值。

### 14.5 拒绝执行规则

目标锁定后发生以下情况时，不允许自动切换目标：

- 本帧只识别到旁边工位目标。
- 本帧候选全部超出锁定目标跟踪半径。
- 本帧最高 Z 候选与锁定目标不连续。
- 原目标短暂丢失但丢失帧数未达到上限。

处理方式：

- 丢失帧数未达到上限：记录日志，本帧不下发 `MoveRelL`，等待下一帧。
- 丢失帧数达到上限：阶段一失败，日志写明锁定目标连续丢失次数、最后一次锁定目标锚点坐标、当前候选摘要。

### 14.6 日志要求

新增日志应保持单行摘要，避免刷屏。每次目标锁定或丢失时记录：

- 当前模式：初始锁定 / 闭环跟踪。
- 锁定目标锚点坐标。
- 当前候选到锁定目标的距离。
- 连续丢失帧数。
- 是否因固定侧规则选中。
- 是否拒绝自动切换到旁边工位目标。

示例：

```text
[视觉锁定] 模式=闭环跟踪 锁定=(23.0,211.3) 候选数=2 最近锁定距离=18.5mm 丢失=0 选中=#0 原因=锁定目标连续
[视觉锁定] 模式=闭环跟踪 锁定=(23.0,211.3) 候选数=1 最近锁定距离=547.6mm 丢失=1/3 选中=无 原因=锁定目标暂时丢失，拒绝切换旁站目标
```

## 15. 修订后验收标准

- 初始拍照位第一次选中目标后，闭环过程中不会自动切换到其他工位目标。
- 同层两个箱子高度接近时，目标按固定侧规则稳定选择，不再左右摇摆。
- 旁边工位目标即使 Z 更高，也不能抢走锁定目标。
- 锁定目标短暂丢失时，本帧不下发运动；连续丢失 3 帧后停止阶段一。
- 40961/49601 若仍出现，应主要来自控制器对正常范围运动的安全空间判断，而不是视觉错误目标导致的大幅跨工位微调。

## 16. 目标锁定实现落地说明

本轮实现保持视觉服务接口不变，只在上位机目标选择和阶段一调度上下文中增加锁定状态：

- `VisionHttpClient::TargetSelectionContext::lockEnabled`：阶段一启用目标锁定策略。
- `VisionHttpClient::TargetSelectionContext::lockMissingFrames`：调度器传入的当前连续丢失帧数。
- `VisionHttpClient::TargetSelectionReason::LockInitialHighestLayer`：初始帧在可信候选中锁定唯一最高层目标。
- `VisionHttpClient::TargetSelectionReason::LockInitialFixedSide`：初始帧最高同层多目标按固定侧锁定。
- `VisionHttpClient::TargetSelectionReason::LockTrackingTarget`：闭环帧继续跟踪锁定目标附近候选。
- `VisionHttpClient::TargetSelectionReason::LockTrackingFixedSide`：闭环帧锁定范围内同层多目标按固定侧选择。
- `VisionHttpClient::TargetSelectionReason::LockTargetMissing`：锁定目标暂时丢失，本帧拒绝下发 `MoveRelL`。
- `VisionHttpClient::TargetSelectionReason::LockTargetLost`：锁定目标连续丢失达到上限，阶段一失败。

`HuayanScheduler` 维护 `m_anchorMissingFrames`。阶段一第一次选中目标后清零丢失计数；后续如果视觉只识别到旁边工位目标或候选全部超出锁定半径，则不进入普通无目标搜索下移，而是等待下一帧。连续丢失达到 `VISION_LOCK_MAX_MISSING_FRAMES` 后，阶段一失败并记录锁定目标锚点。

本地验证：

```bash
cmake --build build-field-fixes --target locked_target_selection_tests anchor_target_selection_tests vision_target_selection_tests huayan_scheduler_contract_tests wh-robot-visual -j2
ctest --test-dir build-field-fixes -R '^(locked_target_selection_tests|anchor_target_selection_tests|vision_target_selection_tests|huayan_scheduler_contract_tests)$' --output-on-failure
```

## 17. 现场验证结论与华研待确认问题

现场复测结论：

- 目标锁定策略选择效果良好，不再选择到旁边工位目标。
- 底层目标也可以稳定选择，说明“可信范围内 Z 优先 + 同层固定侧 + 闭环锁定”符合当前业务场景。
- 该方法依赖现场工位料箱摆放准确性，以及 `Func_captureN` 拍照点位尽量位于当前工位正中间附近；若两者偏差过大，锚点锁定范围会被带偏。
- 阶段一单次 X/Y 微调硬上限按现场验证同步为 `HUAYAN_MAX_SINGLE_XY_ADJUST_MM = 250.0`。

仍需向华研厂家确认的问题：

- 现场仍出现 `40961`，需要厂家确认错误码含义、触发条件，以及是否与此前日志中的 `49601（Target orientation exceeded cartesian safety space）` 属于同类笛卡尔安全空间限制。
- 当前上位机只能通过目标锁定和单次 X/Y 微调硬上限减少错误目标导致的大幅运动；控制器对正常范围运动的安全空间拒绝不在上位机侧宣称根治。
