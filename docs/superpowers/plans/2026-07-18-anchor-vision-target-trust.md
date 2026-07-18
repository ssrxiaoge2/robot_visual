# 固定拍照锚点视觉目标可信选择 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 以阶段一最初拍照位作为固定锚点，在当前工位可信目标内按 Z 最高优先选择箱子，并在目标不可信、XY 微调过大或 Z 下探过大时提前 fail-closed，减少 49601 风险。

**Architecture:** `VisionHttpClient` 负责纯视觉候选解析、工具系换算、锚点选择和单行日志；`HuayanScheduler` 负责维护本轮拍照锚点上下文、累计已完成工具系 XY 位移，并在下发机械臂相对运动前做硬保护。测试先锁住纯选择规则和调度契约，再改生产代码。

**Tech Stack:** C++17、Qt 6.8.3 Core/Gui/Network/Test、CMake/CTest、华研 Robot SDK V1.0.15.0。

## Global Constraints

- Qt 路径固定为 `Qt6_DIR=/opt/Qt/qt6.8/6.8.3/gcc_64/lib/cmake/Qt6`，`qt-cmake=/opt/Qt/qt6.8/6.8.3/gcc_64/bin/qt-cmake`。
- 不修改视觉服务接口和识别模型。
- 不重新标定手眼矩阵。
- 不改变 `Func_captureN`、`Func_jiajin` 等示教器函数。
- 不移除上一版 `[华沿][MoveRelL诊断][命令=N]` 四行失败诊断日志。
- Z 最高优先只在当前拍照锚点可信目标内成立；旁边工位的全局最高箱不应被抓取。
- 阈值第一版使用全局保守值，并通过日志指导后续现场手动微调。
- 新增或修改的宏、`enum class`、接口、参数、成员变量和非显然业务分支必须写明语义、单位、生命周期或 fail-closed 规则。
- 实际代码、测试、设计文档和本计划最终作为一个完整功能提交；任务之间不创建零散提交。
- 不执行 `git push`；由现场人员人工推送。
- 保留用户现有未跟踪文件 `test-events.jsonl`、`test-state.backup.json`、`test-state.json`，不得加入提交。

---

## File Structure

### Create

- `tests/test_anchor_target_selection.cpp`
  纯逻辑测试：锚点坐标换算、Z 优先、同层锚点最近、目标距离过远、目标跳变过大、顺序稳定。

### Modify

- `src/visionclient.h`
  增加锚点选择宏、上下文类型、候选工具系/锚点字段、目标不可信原因、`setTargetSelectionContext()` 和新选择接口。

- `src/visionclient.cpp`
  把候选先转换到工具系，再根据 `TargetSelectionContext` 计算锚点 XY；实现锚点可信选择和增强日志。

- `src/huayanScheduler.h`
  增加锚点累计状态、上一帧选中目标状态、选择摘要缓存、硬保护辅助函数声明。

- `src/huayanScheduler.cpp`
  阶段一启动清零锚点；每次等待视觉前注入上下文；MoveToGrab 每个单轴完成后累计已完成工具系 XY；下发 XY/Z 相对运动前执行 fail-closed 保护。

- `src/devicemanager.cpp`
  将 `VisionHttpClient` 非拥有指针注入 `HuayanScheduler`，并把携带锚点坐标的视觉结果信号连接到新的阶段一槽函数。

- `tests/CMakeLists.txt`
  注册 `anchor_target_selection_tests`。

- `tests/test_huayan_scheduler_contract.cpp`
  增加源码契约断言，锁住锚点上下文注入、累计更新和硬保护。

- `docs/superpowers/specs/2026-07-18-anchor-vision-target-trust-design.md`
  实施完成后同步最终接口名、日志格式和验证结果。

---

### Task 1: 纯视觉锚点目标选择接口和测试

**Files:**
- Create: `tests/test_anchor_target_selection.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `src/visionclient.h`
- Modify: `src/visionclient.cpp`

**Interfaces:**
- Consumes: existing `VisionHttpClient::transformToMm(float cx, float cy, float cz, float angleDeg)` and existing JSON fields `offset_mm.x/y`、`depth_compensated`、`angle`、`confidence`。
- Produces:
  - `struct VisionHttpClient::TargetSelectionContext`
  - `void VisionHttpClient::setTargetSelectionContext(const TargetSelectionContext &context)`
  - `static TargetSelection selectTarget(const QJsonArray &objects, const TargetSelectionContext &context, const float handEyeMatrix[4][4])`
  - expanded `TargetCandidate` fields: `toolX/toolY/toolZ/toolRz/alignmentX/alignmentY/anchorX/anchorY/anchorDistance/trusted`

- [ ] **Step 1: Register the failing test target**

Edit `tests/CMakeLists.txt` and add after `vision_target_selection_tests`:

```cmake
add_executable(anchor_target_selection_tests
    test_anchor_target_selection.cpp
    ../src/visionclient.cpp
)

target_include_directories(anchor_target_selection_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(anchor_target_selection_tests PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
)

add_test(NAME anchor_target_selection_tests
         COMMAND anchor_target_selection_tests)
```

- [ ] **Step 2: Write the failing anchored-selection test**

Create `tests/test_anchor_target_selection.cpp`:

```cpp
#include "visionclient.h"

#include <QJsonArray>
#include <QJsonObject>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void requireTrue(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << std::endl;
        std::exit(1);
    }
}

void requireNear(double actual, double expected, double tolerance, const char *message)
{
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << message << " actual=" << actual << " expected=" << expected << std::endl;
        std::exit(1);
    }
}

QJsonObject target(double x, double y, double depth, double angle = 0.0, double confidence = 0.95)
{
    return {
        {QStringLiteral("offset_mm"), QJsonObject{
             {QStringLiteral("x"), x},
             {QStringLiteral("y"), y}}},
        {QStringLiteral("depth_compensated"), depth},
        {QStringLiteral("angle"), angle},
        {QStringLiteral("confidence"), confidence}
    };
}

const VisionHttpClient::TargetCandidate &selected(const VisionHttpClient::TargetSelection &selection)
{
    requireTrue(selection.hasTarget(), "选择结果必须包含目标");
    return selection.candidates.at(selection.selectedCandidateIndex);
}

VisionHttpClient::TargetSelectionContext context(double accumulatedX = 0.0,
                                                 double accumulatedY = 0.0)
{
    VisionHttpClient::TargetSelectionContext ctx;
    ctx.anchorEnabled = true;
    ctx.accumulatedToolX = accumulatedX;
    ctx.accumulatedToolY = accumulatedY;
    return ctx;
}

const float kIdentityHandEye[4][4] = {
    {1.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 1.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 1.0f}
};

} // namespace

int main()
{
    using Reason = VisionHttpClient::TargetSelectionReason;

    const auto highestInsideAnchor = VisionHttpClient::selectTarget(QJsonArray{
        target(20.0, -30.0, 1000.0),
        target(50.0, -40.0, 900.0)
    }, context(), kIdentityHandEye);
    requireTrue(selected(highestInsideAnchor).sourceIndex == 1,
                "锚点可信范围内必须优先选择最高层目标");
    requireTrue(highestInsideAnchor.reason == Reason::AnchorHighestLayer,
                "单个可信最高层必须记录 AnchorHighestLayer");

    const auto sameLayerNearestAnchor = VisionHttpClient::selectTarget(QJsonArray{
        target(250.0, 0.0, 900.0),
        target(80.0, 0.0, 910.0)
    }, context(), kIdentityHandEye);
    requireTrue(selected(sameLayerNearestAnchor).sourceIndex == 1,
                "最高层同层范围内必须选择离初始锚点 XY 最近的目标");
    requireTrue(sameLayerNearestAnchor.reason == Reason::AnchorSameLayerNearest,
                "同层锚点择近必须记录 AnchorSameLayerNearest");

    const auto accumulatedAnchor = VisionHttpClient::selectTarget(QJsonArray{
        target(10.0, 20.0, 900.0)
    }, context(100.0, -50.0), kIdentityHandEye);
    requireNear(selected(accumulatedAnchor).anchorX, 110.0, 0.001,
                "anchorX 必须等于已完成 X 位移 + 对准 X");
    requireNear(selected(accumulatedAnchor).anchorY, -70.0, 0.001,
                "anchorY 必须等于已完成 Y 位移 + 对准 Y，Y 使用现有阶段一取反方向");

    VisionHttpClient::TargetSelectionContext farCtx = context();
    farCtx.maxTrustDistance = 200.0;
    const auto farHighest = VisionHttpClient::selectTarget(QJsonArray{
        target(350.0, 0.0, 800.0),
        target(20.0, 0.0, 1000.0)
    }, farCtx, kIdentityHandEye);
    requireTrue(!farHighest.hasTarget(),
                "最高层目标离初始锚点过远时必须 fail-closed，不允许改抓低层近目标");
    requireTrue(farHighest.reason == Reason::AnchorDistanceTooFar,
                "最高目标过远必须记录 AnchorDistanceTooFar");

    VisionHttpClient::TargetSelectionContext jumpCtx = context();
    jumpCtx.hasPreviousAnchorTarget = true;
    jumpCtx.previousAnchorX = 0.0;
    jumpCtx.previousAnchorY = 0.0;
    jumpCtx.maxSwitchDistance = 100.0;
    const auto jump = VisionHttpClient::selectTarget(QJsonArray{
        target(180.0, 0.0, 800.0)
    }, jumpCtx, kIdentityHandEye);
    requireTrue(!jump.hasTarget(),
                "闭环目标相对上一帧锚点位置跳变过大时必须 fail-closed");
    requireTrue(jump.reason == Reason::AnchorTargetJumpTooFar,
                "目标跳变过大必须记录 AnchorTargetJumpTooFar");

    const QString summary = VisionHttpClient::formatTargetSelectionLog(sameLayerNearestAnchor);
    requireTrue(summary.contains(QStringLiteral("anchor=("))
                    && summary.contains(QStringLiteral("dist="))
                    && summary.contains(QStringLiteral("可信")),
                "锚点选择日志必须包含 anchor 坐标、距离和可信状态");
    requireTrue(!summary.contains(QLatin1Char('\n')),
                "视觉选择摘要必须保持单行，避免现场日志刷屏");

    return 0;
}
```

- [ ] **Step 3: Run the new test and confirm red**

Run:

```bash
cmake --build build-field-fixes --target anchor_target_selection_tests -j2
```

Expected: build fails because `TargetSelectionContext` and the overloaded `selectTarget()` do not exist.

- [ ] **Step 4: Add anchor constants, enum values, context and candidate fields**

Modify `src/visionclient.h` near the existing ROI macros:

```cpp
// 最高层候选离初始拍照锚点的最大可信距离，单位 mm；现场根据正常 anchor distance 日志微调。
#define VISION_ANCHOR_MAX_TRUST_XY_MM 450.0

// 锚点选择的同层 Z 容差，单位 mm；必须远小于料箱层高，避免低层被当作同层。
#define VISION_ANCHOR_SAME_LAYER_Z_TOL_MM 20.0

// 闭环过程中本帧目标相对上一帧锚点位置的最大可信跳变，单位 mm。
#define VISION_ANCHOR_SWITCH_MAX_XY_MM 220.0
```

Modify `TargetSelectionReason`:

```cpp
    enum class TargetSelectionReason {
        None,                   ///< 没有合法且位于当前工位矩形内的目标。
        HighestLayer,           ///< 兼容旧逻辑：最高层分组中只有一个候选。
        SameLayerNearestCenter, ///< 兼容旧逻辑：最高层有多个候选，按当前中心择近。
        StableSourceIndex,      ///< 深度和 XY 距离完全相同时，按服务端原始下标稳定兜底。
        AnchorHighestLayer,     ///< 锚点逻辑：可信范围内只有一个最高层候选。
        AnchorSameLayerNearest, ///< 锚点逻辑：可信最高层有多个候选，按锚点 XY 最近选择。
        AnchorDistanceTooFar,   ///< 最高层候选离初始拍照锚点过远，目标不可信。
        AnchorTargetJumpTooFar  ///< 闭环目标相对上一帧锚点位置跳变过大，目标不可信。
    };
```

Add before `TargetCandidate`:

```cpp
    /// 阶段一固定拍照锚点选择上下文；由 HuayanScheduler 在每次推理前注入。
    struct TargetSelectionContext {
        bool anchorEnabled = false; ///< false 时使用兼容旧选择逻辑。
        double accumulatedToolX = 0.0; ///< 初始拍照位到当前相机位置的已完成工具系 X 位移(mm)。
        double accumulatedToolY = 0.0; ///< 初始拍照位到当前相机位置的已完成工具系 Y 位移(mm)。
        bool hasPreviousAnchorTarget = false; ///< 是否有上一帧可信目标用于跳变保护。
        double previousAnchorX = 0.0; ///< 上一帧可信目标相对初始拍照锚点的 X(mm)。
        double previousAnchorY = 0.0; ///< 上一帧可信目标相对初始拍照锚点的 Y(mm)。
        double maxTrustDistance = VISION_ANCHOR_MAX_TRUST_XY_MM; ///< 目标可信最大锚点距离(mm)。
        double sameLayerZTol = VISION_ANCHOR_SAME_LAYER_Z_TOL_MM; ///< 同层 Z 容差(mm)。
        double maxSwitchDistance = VISION_ANCHOR_SWITCH_MAX_XY_MM; ///< 闭环目标最大跳变(mm)。
    };
```

Expand `TargetCandidate`:

```cpp
        double toolX = 0.0;           ///< 候选经手眼矩阵转换后的工具系 X 偏移(mm)。
        double toolY = 0.0;           ///< 候选经手眼矩阵转换后的工具系 Y 偏移(mm)。
        double toolZ = 0.0;           ///< 候选经手眼矩阵转换后的工具系 Z 深度(mm)。
        double toolRz = 0.0;          ///< 候选经角度规范化后的工具系 Rz(deg)。
        double alignmentX = 0.0;      ///< 为对准候选需要执行的工具系 X 位移(mm)。
        double alignmentY = 0.0;      ///< 为对准候选需要执行的工具系 Y 位移(mm)，按阶段一 Y 取反规则计算。
        double anchorX = 0.0;         ///< 候选相对初始拍照锚点的 X(mm)。
        double anchorY = 0.0;         ///< 候选相对初始拍照锚点的 Y(mm)。
        double anchorDistance = 0.0;  ///< 候选离初始拍照锚点的 XY 距离(mm)。
        bool trusted = true;          ///< 锚点逻辑下候选是否通过最终可信检查。
```

Add public methods:

```cpp
    /// 纯逻辑：按固定拍照锚点上下文选择目标；handEyeMatrix 为 4x4 行主序矩阵。
    static TargetSelection selectTarget(const QJsonArray &objects,
                                        const TargetSelectionContext &context,
                                        const float handEyeMatrix[4][4]);

    /// 设置下一次 /inference 使用的目标选择上下文；由机械臂阶段一在发起推理前注入。
    void setTargetSelectionContext(const TargetSelectionContext &context);
```

Add private member:

```cpp
    TargetSelectionContext m_targetSelectionContext; ///< 最近一次推理使用的选择上下文，生命周期到下一次 set 覆盖。
```

- [ ] **Step 5: Implement anchored parsing and selection**

Modify `src/visionclient.cpp`. Add helper functions near existing selection helpers:

```cpp
double distanceSq(double x, double y)
{
    return x * x + y * y;
}

struct ToolCoords {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double rz = 0.0;
};

ToolCoords transformWithMatrix(const float matrix[4][4],
                               double cx,
                               double cy,
                               double cz,
                               double angleDeg)
{
    const float in[4] = {
        static_cast<float>(cx),
        static_cast<float>(cy),
        static_cast<float>(cz),
        1.0f
    };
    float out[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c)
            out[r] += matrix[r][c] * in[c];
    }

    float normAngle = std::fmod(static_cast<float>(angleDeg), 180.0f);
    if (normAngle < 0.0f)
        normAngle += 180.0f;
    if (normAngle > 90.0f)
        normAngle -= 180.0f;

    return {
        static_cast<double>(out[0]),
        static_cast<double>(out[1]),
        static_cast<double>(out[2]),
        static_cast<double>(normAngle)
    };
}
```

Add setter:

```cpp
void VisionHttpClient::setTargetSelectionContext(const TargetSelectionContext &context)
{
    m_targetSelectionContext = context;
}
```

Implement the overload:

```cpp
VisionHttpClient::TargetSelection VisionHttpClient::selectTarget(
    const QJsonArray &objects,
    const TargetSelectionContext &context,
    const float handEyeMatrix[4][4])
{
    if (!context.anchorEnabled)
        return selectTarget(objects);

    TargetSelection selection;
    QList<int> validIndexes;

    for (int sourceIndex = 0; sourceIndex < objects.size(); ++sourceIndex) {
        TargetCandidate candidate;
        candidate.sourceIndex = sourceIndex;
        const QJsonValue entry = objects.at(sourceIndex);
        if (!entry.isObject()) {
            candidate.rejectionReason = QStringLiteral("目标不是JSON对象");
            selection.candidates.append(candidate);
            continue;
        }

        const QJsonObject object = entry.toObject();
        const QJsonValue offsetValue = object.value(QStringLiteral("offset_mm"));
        if (!offsetValue.isObject()) {
            candidate.rejectionReason = QStringLiteral("offset_mm不是对象");
            selection.candidates.append(candidate);
            continue;
        }

        const QJsonObject offset = offsetValue.toObject();
        if (!readFiniteNumber(offset.value(QStringLiteral("x")), &candidate.x)
            || !readFiniteNumber(offset.value(QStringLiteral("y")), &candidate.y)
            || !readFiniteNumber(object.value(QStringLiteral("depth_compensated")), &candidate.depth)
            || !readFiniteNumber(object.value(QStringLiteral("angle")), &candidate.angle)
            || !readFiniteNumber(object.value(QStringLiteral("confidence")), &candidate.confidence)) {
            candidate.rejectionReason = QStringLiteral("字段缺失或不是有限数值");
            selection.candidates.append(candidate);
            continue;
        }

        const ToolCoords raw = transformWithMatrix(handEyeMatrix,
                                                   candidate.x,
                                                   candidate.y,
                                                   candidate.depth,
                                                   candidate.angle);
        candidate.valid = true;
        candidate.toolX = raw.x;
        candidate.toolY = raw.y;
        candidate.toolZ = raw.z;
        candidate.toolRz = raw.rz;
        candidate.alignmentX = candidate.toolX;
        candidate.alignmentY = -candidate.toolY;
        candidate.anchorX = context.accumulatedToolX + candidate.alignmentX;
        candidate.anchorY = context.accumulatedToolY + candidate.alignmentY;
        candidate.anchorDistance = std::sqrt(distanceSq(candidate.anchorX, candidate.anchorY));
        candidate.insideStationRoi = true;
        selection.candidates.append(candidate);
        validIndexes.append(selection.candidates.size() - 1);
    }

    if (validIndexes.isEmpty())
        return selection;

    double highestDepth = selection.candidates.at(validIndexes.first()).depth;
    for (int candidateIndex : validIndexes)
        highestDepth = qMin(highestDepth, selection.candidates.at(candidateIndex).depth);

    QList<int> sameLayerIndexes;
    for (int candidateIndex : validIndexes) {
        if (qAbs(selection.candidates.at(candidateIndex).depth - highestDepth)
            <= context.sameLayerZTol) {
            sameLayerIndexes.append(candidateIndex);
        }
    }

    int bestIndex = sameLayerIndexes.first();
    for (int candidateIndex : sameLayerIndexes) {
        const TargetCandidate &candidate = selection.candidates.at(candidateIndex);
        const TargetCandidate &best = selection.candidates.at(bestIndex);
        if (candidate.anchorDistance < best.anchorDistance) {
            bestIndex = candidateIndex;
        } else if (qFuzzyCompare(candidate.anchorDistance + 1.0, best.anchorDistance + 1.0)
                   && candidate.sourceIndex < best.sourceIndex) {
            bestIndex = candidateIndex;
        }
    }

    TargetCandidate &best = selection.candidates[bestIndex];
    if (best.anchorDistance > context.maxTrustDistance) {
        best.trusted = false;
        selection.reason = TargetSelectionReason::AnchorDistanceTooFar;
        return selection;
    }

    if (context.hasPreviousAnchorTarget) {
        const double jump = std::sqrt(distanceSq(best.anchorX - context.previousAnchorX,
                                                 best.anchorY - context.previousAnchorY));
        if (jump > context.maxSwitchDistance) {
            best.trusted = false;
            selection.reason = TargetSelectionReason::AnchorTargetJumpTooFar;
            return selection;
        }
    }

    selection.selectedCandidateIndex = bestIndex;
    selection.reason = sameLayerIndexes.size() > 1
        ? TargetSelectionReason::AnchorSameLayerNearest
        : TargetSelectionReason::AnchorHighestLayer;
    return selection;
}
```

- [ ] **Step 6: Use the anchored overload during inference**

Modify `VisionHttpClient::parseInferenceReply()`:

```cpp
    const QJsonArray objects = doc.object().value(QStringLiteral("objects")).toArray();
    const TargetSelection selection = selectTarget(objects, m_targetSelectionContext, m_T);
    emit selectionLogMessage(formatTargetSelectionLog(selection));
```

Keep the rest of the selected-candidate transform unchanged, but use the already transformed values when anchor mode is enabled:

```cpp
    const TargetCandidate &candidate =
        selection.candidates.at(selection.selectedCandidateIndex);
    RawCoords raw;
    if (m_targetSelectionContext.anchorEnabled) {
        raw = {candidate.toolX, candidate.toolY, candidate.toolZ, candidate.toolRz};
    } else {
        raw = transformToMm(static_cast<float>(candidate.x),
                            static_cast<float>(candidate.y),
                            static_cast<float>(candidate.depth),
                            static_cast<float>(candidate.angle));
    }
```

- [ ] **Step 7: Extend selection log formatting**

Modify `formatTargetSelectionLog()` so valid candidates include anchor fields when present:

```cpp
        const QString anchorText = candidate.valid
            ? QStringLiteral(" tool=(%1,%2,%3) anchor=(%4,%5) dist=%6 %7")
                  .arg(candidate.toolX, 0, 'f', 1)
                  .arg(candidate.toolY, 0, 'f', 1)
                  .arg(candidate.toolZ, 0, 'f', 1)
                  .arg(candidate.anchorX, 0, 'f', 1)
                  .arg(candidate.anchorY, 0, 'f', 1)
                  .arg(candidate.anchorDistance, 0, 'f', 1)
                  .arg(candidate.trusted ? QStringLiteral("可信") : QStringLiteral("不可信"))
            : QString();
```

Keep the final returned string single-line. Add reason text for new enum values:

```cpp
    case VisionHttpClient::TargetSelectionReason::AnchorHighestLayer:
        return QStringLiteral("锚点最高层");
    case VisionHttpClient::TargetSelectionReason::AnchorSameLayerNearest:
        return QStringLiteral("锚点同层最近");
    case VisionHttpClient::TargetSelectionReason::AnchorDistanceTooFar:
        return QStringLiteral("最高目标离拍照锚点过远，目标不可信");
    case VisionHttpClient::TargetSelectionReason::AnchorTargetJumpTooFar:
        return QStringLiteral("目标跳变过大，目标不可信");
```

- [ ] **Step 8: Run anchored selection tests**

Run:

```bash
cmake --build build-field-fixes --target anchor_target_selection_tests vision_target_selection_tests -j2
ctest --test-dir build-field-fixes -R '^(anchor_target_selection_tests|vision_target_selection_tests)$' --output-on-failure
```

Expected: both tests pass.

---

### Task 2: HuayanScheduler 维护拍照锚点上下文

**Files:**
- Modify: `src/huayanScheduler.h`
- Modify: `src/huayanScheduler.cpp`
- Modify: `src/devicemanager.cpp`
- Modify: `src/visionclient.h`
- Modify: `tests/test_huayan_scheduler_contract.cpp`

**Interfaces:**
- Consumes: `VisionHttpClient::TargetSelectionContext` and `VisionHttpClient::setTargetSelectionContext(const TargetSelectionContext &context)` from Task 1.
- Produces:
  - `resetVisionAnchorTracking()`
  - `makeVisionTargetSelectionContext() const`
  - `recordCompletedGrabMove(const RelMove &move)`

- [ ] **Step 1: Add failing scheduler contract assertions**

Append these checks to `tests/test_huayan_scheduler_contract.cpp`:

```cpp
    requireTrue(schedulerHeader.contains(QStringLiteral("resetVisionAnchorTracking()"))
                    && schedulerHeader.contains(QStringLiteral("makeVisionTargetSelectionContext() const"))
                    && schedulerHeader.contains(QStringLiteral("recordCompletedGrabMove(const RelMove &move)")),
                "HuayanScheduler 必须声明锚点清零、上下文生成和已完成微调累计接口");

    requireTrue(schedulerSource.contains(QStringLiteral("m_visionClient->setTargetSelectionContext(makeVisionTargetSelectionContext())")),
                "每次进入 WaitForVision 发起推理前必须注入固定拍照锚点上下文");

    requireTrue(schedulerSource.contains(QStringLiteral("resetVisionAnchorTracking();"))
                    && schedulerSource.contains(QStringLiteral("startStageOne()")),
                "阶段一启动时必须清零本轮拍照锚点累计状态");

    requireTrue(schedulerSource.contains(QStringLiteral("recordCompletedGrabMove(m_grabMoves.at(m_grabMoveIdx))")),
                "MoveToGrab 单轴完成后必须在 m_grabMoveIdx++ 之前累计已完成工具系 XY 位移");

    requireTrue(schedulerSource.contains(QStringLiteral("m_anchorHasPreviousTarget = true"))
                    && schedulerSource.contains(QStringLiteral("m_anchorPreviousTargetX = contextSelectedAnchorX")),
                "收到可信视觉结果后必须记录上一帧锚点目标用于跳变保护");
```

- [ ] **Step 2: Run scheduler contract test and confirm red**

Run:

```bash
cmake --build build-field-fixes --target huayan_scheduler_contract_tests -j2
ctest --test-dir build-field-fixes -R '^huayan_scheduler_contract_tests$' --output-on-failure
```

Expected: test fails because the anchor-tracking functions and source strings do not exist.

- [ ] **Step 3: Add HuayanScheduler members and helpers**

Modify `src/huayanScheduler.h`. Include `visionclient.h` if not already visible:

```cpp
#include "visionclient.h"
```

Add private helper declarations:

```cpp
    /// 清零阶段一固定拍照锚点状态；每次 startStageOne() 必须调用一次。
    void resetVisionAnchorTracking();
    /// 生成下一次视觉推理使用的固定拍照锚点上下文。
    VisionHttpClient::TargetSelectionContext makeVisionTargetSelectionContext() const;
    /// 记录一条已经完成的阶段一 XY 微调；只能在控制器确认到位后调用。
    void recordCompletedGrabMove(const RelMove &move);
```

Add private members:

```cpp
    double m_anchorAccumulatedToolX = 0.0; ///< 初始拍照位到当前相机位置已完成工具系 X 位移(mm)。
    double m_anchorAccumulatedToolY = 0.0; ///< 初始拍照位到当前相机位置已完成工具系 Y 位移(mm)。
    bool m_anchorHasPreviousTarget = false; ///< 是否已有上一帧可信目标用于闭环跳变保护。
    double m_anchorPreviousTargetX = 0.0; ///< 上一帧可信目标相对初始拍照锚点 X(mm)。
    double m_anchorPreviousTargetY = 0.0; ///< 上一帧可信目标相对初始拍照锚点 Y(mm)。
```

- [ ] **Step 4: Implement reset/context/accumulation helpers**

Modify `src/huayanScheduler.cpp`:

```cpp
void HuayanScheduler::resetVisionAnchorTracking()
{
    m_anchorAccumulatedToolX = 0.0;
    m_anchorAccumulatedToolY = 0.0;
    m_anchorHasPreviousTarget = false;
    m_anchorPreviousTargetX = 0.0;
    m_anchorPreviousTargetY = 0.0;
}

VisionHttpClient::TargetSelectionContext HuayanScheduler::makeVisionTargetSelectionContext() const
{
    VisionHttpClient::TargetSelectionContext context;
    context.anchorEnabled = m_stage == Stage::StageOne;
    context.accumulatedToolX = m_anchorAccumulatedToolX;
    context.accumulatedToolY = m_anchorAccumulatedToolY;
    context.hasPreviousAnchorTarget = m_anchorHasPreviousTarget;
    context.previousAnchorX = m_anchorPreviousTargetX;
    context.previousAnchorY = m_anchorPreviousTargetY;
    return context;
}

void HuayanScheduler::recordCompletedGrabMove(const RelMove &move)
{
    const double signedDistance = move.direction ? move.distance : -move.distance;
    if (move.poseId == 0)
        m_anchorAccumulatedToolX += signedDistance;
    else if (move.poseId == 1)
        m_anchorAccumulatedToolY += signedDistance;
}
```

- [ ] **Step 5: Reset anchor tracking when stage one starts**

Modify `HuayanScheduler::startStageOne()` near other per-stage resets:

```cpp
    resetVisionAnchorTracking();
```

Expected context: the method already resets stage state, search counters and vision iteration state. Place anchor reset with those resets.

- [ ] **Step 6: Inject context before inference**

Modify `executeCurrentStep()` in `StageStep::WaitForVision`:

```cpp
        case StageStep::WaitForVision:
            emit logMessage(QStringLiteral("[阶段一] 已到拍照位，等待视觉推理结果"));
            if (m_visionClient)
                m_visionClient->setTargetSelectionContext(makeVisionTargetSelectionContext());
            emit surveyReady();
            m_timeoutTimer->start(10000);
            break;
```

当前 `HuayanScheduler` 不持有视觉客户端；新增一个非拥有指针 setter，让调度器只在发起推理前注入选择上下文：

```cpp
public:
    /// 注入视觉客户端，仅用于阶段一发起推理前设置目标选择上下文；不拥有对象。
    void setVisionClient(VisionHttpClient *client);

private:
    VisionHttpClient *m_visionClient = nullptr; ///< 非拥有指针；DeviceManager 创建并注入。
```

Implementation:

```cpp
void HuayanScheduler::setVisionClient(VisionHttpClient *client)
{
    m_visionClient = client;
}
```

In `DeviceManager` after `m_visionClient` and `m_huayanScheduler` are constructed:

```cpp
    m_huayanScheduler->setVisionClient(m_visionClient);
```

- [ ] **Step 7: Accumulate completed XY moves before incrementing index**

Modify `onPollTick()` in the `MoveToGrab` completion branch:

```cpp
        if (m_stage == Stage::StageOne && m_stageStep == StageStep::MoveToGrab) {
            recordCompletedGrabMove(m_grabMoves.at(m_grabMoveIdx));
            m_grabMoveIdx++;
            const quint64 seq = nextCallbackSeq();
            QTimer::singleShot(300, this, [this, seq] {
                if (seq == m_commandSeq
                    && m_stage == Stage::StageOne
                    && m_stageStep == StageStep::MoveToGrab)
                    executeNextGrabMove();
            });
        } else {
            advanceStep();
            proceedStage();
        }
```

- [ ] **Step 8: Record previous trusted anchor target after vision result**

Modify `setGrabOffset()` signature only if Task 1 adds anchor coordinates to the emitted signal. Preferred minimal interface:

In `VisionHttpClient`, add signal:

```cpp
    void rawCoordinatesReady(double x, double y, double z, double rz,
                             double anchorX, double anchorY);
```

Keep the existing 4-argument signal for compatibility if current connections use it elsewhere. In `parseInferenceReply()` emit the 6-argument signal when anchor mode is enabled.

In `HuayanScheduler`, add overload:

```cpp
    void setGrabOffset(double x, double y, double z, double rz,
                       double contextSelectedAnchorX, double contextSelectedAnchorY);
```

Implementation starts by recording the selected anchor target:

```cpp
void HuayanScheduler::setGrabOffset(double x, double y, double z, double rz,
                                    double contextSelectedAnchorX, double contextSelectedAnchorY)
{
    m_anchorHasPreviousTarget = true;
    m_anchorPreviousTargetX = contextSelectedAnchorX;
    m_anchorPreviousTargetY = contextSelectedAnchorY;
    setGrabOffset(x, y, z, rz);
}
```

Update the `DeviceManager` connection to connect the 6-argument signal to the 6-argument slot.

- [ ] **Step 9: Run scheduler contract and build**

Run:

```bash
cmake --build build-field-fixes --target huayan_scheduler_contract_tests wh-robot-visual -j2
ctest --test-dir build-field-fixes -R '^huayan_scheduler_contract_tests$' --output-on-failure
```

Expected: test passes and app target builds.

---

### Task 3: 阶段一 XY/Z 运动硬保护

**Files:**
- Modify: `src/huayanScheduler.cpp`
- Modify: `src/huayanScheduler.h`
- Modify: `tests/test_huayan_scheduler_contract.cpp`

**Interfaces:**
- Consumes: existing `RelMove` and `calculateGrabDescend()`。
- Produces:
  - `HUAYAN_MAX_SINGLE_XY_ADJUST_MM`
  - `HUAYAN_MAX_Z_DESCEND_MM`
  - `bool validateStageOneRelMoveBeforeDispatch(const RelMove &move) const`
  - hard-fail logs before `MoveRelL` dispatch.

- [ ] **Step 1: Add failing contract assertions for motion guards**

Append to `tests/test_huayan_scheduler_contract.cpp`:

```cpp
    requireTrue(schedulerSource.contains(QStringLiteral("HUAYAN_MAX_SINGLE_XY_ADJUST_MM"))
                    && schedulerSource.contains(QStringLiteral("HUAYAN_MAX_Z_DESCEND_MM")),
                "阶段一必须有独立的 XY 单步和 Z 下探硬保护常量");

    requireTrue(schedulerSource.contains(QStringLiteral("validateStageOneRelMoveBeforeDispatch"))
                    && schedulerSource.contains(QStringLiteral("目标不可信：计划"))
                    && schedulerSource.contains(QStringLiteral("拒绝下发 MoveRelL")),
                "阶段一相对运动下发前必须执行 fail-closed 保护并记录原因");

    requireTrue(schedulerSource.contains(QStringLiteral("qMin(kMaxDescend, HUAYAN_MAX_Z_DESCEND_MM)")),
                "Z 下探计算必须同时受原有 kMaxDescend 和新的硬保护上限约束");
```

- [ ] **Step 2: Run contract test and confirm red**

Run:

```bash
cmake --build build-field-fixes --target huayan_scheduler_contract_tests -j2
ctest --test-dir build-field-fixes -R '^huayan_scheduler_contract_tests$' --output-on-failure
```

Expected: fails because the guard constants and helper do not exist.

- [ ] **Step 3: Add guard constants**

Modify `src/huayanScheduler.cpp` near existing `kMaxDescend`:

```cpp
static constexpr double HUAYAN_MAX_SINGLE_XY_ADJUST_MM = 220.0; ///< 阶段一单次 XY 微调上限(mm)，现场按 anchor 日志微调。
static constexpr double HUAYAN_MAX_Z_DESCEND_MM = 1078.0;       ///< 阶段一 Z 下探硬上限(mm)，不得因临时调试放大。
```

- [ ] **Step 4: Add motion validation helper**

Declare in `src/huayanScheduler.h`:

```cpp
    /// 阶段一相对运动下发前的上位机硬保护；返回 false 时必须已经记录错误并停止阶段。
    bool validateStageOneRelMoveBeforeDispatch(const RelMove &move) const;
```

Implement in `src/huayanScheduler.cpp`:

```cpp
bool HuayanScheduler::validateStageOneRelMoveBeforeDispatch(const RelMove &move) const
{
    if (m_stage != Stage::StageOne)
        return true;

    const bool isXY = move.poseId == 0 || move.poseId == 1;
    if (isXY && move.distance > HUAYAN_MAX_SINGLE_XY_ADJUST_MM) {
        const QString axis = move.poseId == 0 ? QStringLiteral("X") : QStringLiteral("Y");
        const_cast<HuayanScheduler *>(this)->emitOperationError(
            QStringLiteral("[阶段一] 目标不可信：计划 %1 微调 %2mm 超过单次上限 %3mm，拒绝下发 MoveRelL")
                .arg(axis)
                .arg(move.distance, 0, 'f', 1)
                .arg(HUAYAN_MAX_SINGLE_XY_ADJUST_MM, 0, 'f', 1));
        return false;
    }
    return true;
}
```

If avoiding `const_cast`, make the helper non-const:

```cpp
bool validateStageOneRelMoveBeforeDispatch(const RelMove &move);
```

and emit directly.

- [ ] **Step 5: Call guard before each XY/Rz grab move dispatch**

Modify `executeNextGrabMove()` before logging and creating `PendingCommand`:

```cpp
    const RelMove &mv = m_grabMoves.at(m_grabMoveIdx);
    if (!validateStageOneRelMoveBeforeDispatch(mv))
        return false;
```

- [ ] **Step 6: Apply hard Z upper bound**

Modify `StageStep::DescendZ`:

```cpp
            const double descend = calculateGrabDescend(
                m_grabOffset.z,
                m_grabZClearance,
                qMin(kMaxDescend, HUAYAN_MAX_Z_DESCEND_MM));
```

Before creating `PendingCommand`, add explicit fail-closed guard:

```cpp
            if (descend > HUAYAN_MAX_Z_DESCEND_MM) {
                emitOperationError(QStringLiteral("[阶段一] 目标不可信：计划 Z 下探 %1mm 超过硬上限 %2mm，拒绝下发 MoveRelL")
                                       .arg(descend, 0, 'f', 1)
                                       .arg(HUAYAN_MAX_Z_DESCEND_MM, 0, 'f', 1));
                break;
            }
```

This branch should not normally trigger because `calculateGrabDescend()` clamps to the same bound; it remains as a readable business guard.

- [ ] **Step 7: Run guard tests and app build**

Run:

```bash
cmake --build build-field-fixes --target huayan_scheduler_contract_tests wh-robot-visual -j2
ctest --test-dir build-field-fixes -R '^huayan_scheduler_contract_tests$' --output-on-failure
```

Expected: test passes and app target builds.

---

### Task 4: 文档同步、完整验证和唯一功能提交

**Files:**
- Modify: `docs/superpowers/specs/2026-07-18-anchor-vision-target-trust-design.md`
- Modify: `docs/superpowers/plans/2026-07-18-anchor-vision-target-trust.md`
- Modify: all files touched by Tasks 1-3

**Interfaces:**
- Consumes: Tasks 1-3 completed implementation.
- Produces: one local commit containing code, tests and docs; no push.

- [ ] **Step 1: Update design document implementation status**

Edit `docs/superpowers/specs/2026-07-18-anchor-vision-target-trust-design.md`:

```markdown
**状态：** 已实施并完成自动化验证；阈值等待现场根据 anchor distance 日志微调
```

Add verification section:

```markdown
## 11. 自动化验证结果

- `anchor_target_selection_tests`：通过。
- `vision_target_selection_tests`：通过，兼容旧选择逻辑。
- `huayan_scheduler_contract_tests`：通过。
- `wh-robot-visual`：构建通过。
- 全量 CTest：通过。

## 12. 现场复测重点

- 多目标时日志中最高层目标的 `anchorDistance`。
- 被拒绝目标是否确实来自旁边工位或明显远离拍照位。
- 正常抓取的 `anchorDistance` 最大值，用于后续收紧 `VISION_ANCHOR_MAX_TRUST_XY_MM`。
- 是否还出现 49601；若出现，检查对应命令是否已在上位机保护范围之外。
```

- [ ] **Step 2: Run whitespace and placeholder checks**

Run:

```bash
git diff --check
rg -n "TB[D]|TO[D]O|implement[ ]later|fill[ ]in[ ]details|适当处[理]|类似[ ]Task" \
  docs/superpowers/specs/2026-07-18-anchor-vision-target-trust-design.md \
  docs/superpowers/plans/2026-07-18-anchor-vision-target-trust.md \
  src/visionclient.h src/visionclient.cpp src/huayanScheduler.h src/huayanScheduler.cpp \
  tests/test_anchor_target_selection.cpp tests/test_huayan_scheduler_contract.cpp
```

Expected:

- `git diff --check` exits 0.
- `rg` exits 1 with no matches.

- [ ] **Step 3: Build and run focused tests**

Run:

```bash
cmake --build build-field-fixes --target \
  anchor_target_selection_tests \
  vision_target_selection_tests \
  huayan_scheduler_contract_tests \
  wh-robot-visual \
  -j2

ctest --test-dir build-field-fixes \
  -R '^(anchor_target_selection_tests|vision_target_selection_tests|huayan_scheduler_contract_tests)$' \
  --output-on-failure
```

Expected:

- All four targets build.
- Three focused tests pass.

- [ ] **Step 4: Run full verification**

Run:

```bash
cmake --build build-field-fixes -j2
ctest --test-dir build-field-fixes --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 5: Review final diff for forbidden changes**

Run:

```bash
git status --short
git diff --stat
git diff -- src/visionclient.h src/visionclient.cpp src/huayanScheduler.h src/huayanScheduler.cpp
```

Verify manually:

- No changes to visual service Python API.
- No changes to示教器函数名配置 unless explicitly required by tests.
- No deletion of `[华沿][MoveRelL诊断][命令=%1]` logs.
- `test-events.jsonl`、`test-state.backup.json`、`test-state.json` remain untracked and unstaged.

- [ ] **Step 6: Create the single local implementation commit**

Stage only task files:

```bash
git add \
  src/visionclient.h \
  src/visionclient.cpp \
  src/huayanScheduler.h \
  src/huayanScheduler.cpp \
  src/devicemanager.cpp \
  tests/CMakeLists.txt \
  tests/test_anchor_target_selection.cpp \
  tests/test_huayan_scheduler_contract.cpp \
  docs/superpowers/specs/2026-07-18-anchor-vision-target-trust-design.md \
  docs/superpowers/plans/2026-07-18-anchor-vision-target-trust.md

git diff --cached --check
git status --short
git commit -m "fix: add anchored vision target trust guard"
```

Expected:

- Only task files are staged.
- The three user JSON files are not staged.
- One local commit is created.

- [ ] **Step 7: Confirm no push was performed**

Run:

```bash
git status --short
git log -2 --oneline
```

Expected:

- No task-related unstaged changes remain.
- It is acceptable for these user files to remain untracked:
  - `test-events.jsonl`
  - `test-state.backup.json`
  - `test-state.json`
- Do not run `git push`.

---

## Self-Review Notes

- Spec coverage: Tasks 1-3 cover anchored selection, target distrust, motion guards and logging; Task 4 covers docs, verification and single local commit.
- Placeholder scan: this plan intentionally contains no forbidden placeholder markers.
- Type consistency: `TargetSelectionContext` is defined in Task 1, consumed by Task 2; motion guard helpers are defined and consumed inside Task 3.
