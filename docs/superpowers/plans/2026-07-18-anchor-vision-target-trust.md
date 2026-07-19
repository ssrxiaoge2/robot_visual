# 固定拍照锚点视觉目标可信选择实施计划

> **给智能体执行者：** 必需子技能：使用 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans` 按任务执行本计划；步骤使用复选框语法记录进度。

**目标：** 以阶段一最初拍照位作为固定锚点，在当前工位可信目标内按 Z 最高优先选择箱子，并在目标不可信、XY 微调过大或 Z 下探过大时提前拒绝执行，减少 40961/49601 风险。

**实施状态（2026-07-18）：** 任务 1-4 已在本地实现；最终审查补丁要求的锚点拒绝执行通道与 Z 未截断计划值硬上限检查已同步到代码/契约/文档。本计划按“一个大功能一次提交”收口为单个本地提交，未推送，等待人工推送。7 月 18 日现场复测后确认第一版仍会在视觉不稳定时重新选择旁边工位目标，因此追加任务 5：目标锁定闭环跟踪。

**2026-07-19 后续修正：** 本计划记录 7.18 第一版锚点锁定实现；闭环跟踪规则、Rz 大角度累计保护和 Z 下探超时修正见 `docs/superpowers/plans/2026-07-19-vision-lock-tracking-and-rz-guard.md`。

**架构：** `VisionHttpClient` 负责纯视觉候选解析、工具系换算、锚点选择和单行日志；普通无目标继续发 `noObjectDetected()`，`AnchorDistanceTooFar` / `AnchorTargetJumpTooFar` 通过独立拒绝信号交给调度器直接阶段失败。`HuayanScheduler` 负责维护本轮拍照锚点上下文、累计已完成工具系 XY 位移，并在下发机械臂相对运动前做硬保护；Z 下探先按未截断 `plannedDescend = 视觉深度 - 工位余量` 与硬上限(mm)比较，未超限时才计算实际下发距离。测试先锁住纯选择规则和调度契约，再改生产代码。

**技术栈：** C++17、Qt 6.8.3 Core/Gui/Network/Test、CMake/CTest、华研 Robot SDK V1.0.15.0。

## 全局约束

- Qt 路径固定为 `Qt6_DIR=/opt/Qt/qt6.8/6.8.3/gcc_64/lib/cmake/Qt6`，`qt-cmake=/opt/Qt/qt6.8/6.8.3/gcc_64/bin/qt-cmake`。
- 不修改视觉服务接口和识别模型。
- 不重新标定手眼矩阵。
- 不改变 `Func_captureN`、`Func_jiajin` 等示教器函数。
- 不移除上一版 `[华沿][MoveRelL诊断][命令=N]` 四行失败诊断日志。
- Z 最高优先只在当前拍照锚点可信目标内成立；旁边工位的全局最高箱不应被抓取。
- 阈值第一版使用全局保守值，并通过日志指导后续现场手动微调。
- 新增或修改的宏、`enum class`、接口、参数、成员变量和非显然业务分支必须写明语义、单位、生命周期或拒绝执行规则。
- 实际代码、测试、设计文档和本计划最终作为一个完整功能提交；任务之间不创建零散提交。
- 不执行 `git push`；由现场人员人工推送。
- 保留用户现有未跟踪文件 `test-events.jsonl`、`test-state.backup.json`、`test-state.json`，不得加入提交。

---

## 文件结构

### 新增

- `tests/test_anchor_target_selection.cpp`
  纯逻辑测试：锚点坐标换算、Z 优先、同层锚点最近、目标距离过远、目标跳变过大、顺序稳定。

- `tests/test_locked_target_selection.cpp`
  目标锁定测试：初始锁定、同层固定侧选择、闭环只跟踪锁定目标、锁定目标短暂丢失不切换、连续丢失达到上限后失败。

### 修改

- `src/visionclient.h`
  增加锚点选择宏、上下文类型、候选工具系/锚点字段、目标不可信原因、`setTargetSelectionContext()`、新选择接口，以及区分锚点可信拒绝和普通无目标的拒绝信号。

- `src/visionclient.cpp`
  把候选先转换到工具系，再根据 `TargetSelectionContext` 计算锚点 XY；实现锚点可信选择和增强日志；锚点距离过远/跳变过大时发拒绝信号，不发 `noObjectDetected()`。

- `src/huayanScheduler.h`
  增加锚点累计状态、上一帧选中目标状态、选择摘要缓存、硬保护辅助函数声明和锚点可信拒绝槽。

- `src/huayanScheduler.cpp`
  阶段一启动清零锚点；每次等待视觉前注入上下文；MoveToGrab 每个单轴完成后累计已完成工具系 XY；下发 XY/Z 相对运动前执行拒绝下发保护；锚点可信拒绝直接阶段失败，不进入搜索下移；Z 下探硬上限在 `calculateGrabDescend()` 截断前用未截断计划值判断。

- `src/devicemanager.cpp`
  将 `VisionHttpClient` 非拥有指针注入 `HuayanScheduler`，把携带锚点坐标的视觉结果信号连接到新的阶段一槽函数，并连接锚点可信拒绝信号。

- `tests/CMakeLists.txt`
  注册 `anchor_target_selection_tests`。

- `tests/test_huayan_scheduler_contract.cpp`
  增加源码契约断言，锁住锚点上下文注入、累计更新和硬保护。

- `docs/superpowers/specs/2026-07-18-anchor-vision-target-trust-design.md`
  实施完成后同步最终接口名、日志格式和验证结果。

- `docs/superpowers/plans/2026-07-18-anchor-vision-target-trust.md`
  追加现场复测后的目标锁定任务，保持计划和代码后续修改一致。

---

### 任务 1： 纯视觉锚点目标选择接口和测试

**文件：**
- 新增： `tests/test_anchor_target_selection.cpp`
- 修改： `tests/CMakeLists.txt`
- 修改： `src/visionclient.h`
- 修改： `src/visionclient.cpp`

**接口：**
- 依赖： 现有 `VisionHttpClient::transformToMm(float cx, float cy, float cz, float angleDeg)` 和现有 JSON 字段 `offset_mm.x/y`、`depth_compensated`、`angle`、`confidence`。
- 产出：
  - `struct VisionHttpClient::TargetSelectionContext`
  - `void VisionHttpClient::setTargetSelectionContext(const TargetSelectionContext &context)`
  - `static TargetSelection selectTarget(const QJsonArray &objects, const TargetSelectionContext &context, const float handEyeMatrix[4][4])`
  - 扩展后的 `TargetCandidate` 字段： `toolX/toolY/toolZ/toolRz/alignmentX/alignmentY/anchorX/anchorY/anchorDistance/trusted`

- [x] **步骤 1：注册预期失败的测试目标**

编辑 `tests/CMakeLists.txt`，在 `vision_target_selection_tests` 后增加：

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

- [x] **步骤 2：编写预期失败的锚点选择测试**

创建 `tests/test_anchor_target_selection.cpp`：

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
                "最高层目标离初始锚点过远时必须 失败关闭，不允许改抓低层近目标");
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
                "闭环目标相对上一帧锚点位置跳变过大时必须 失败关闭");
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

- [x] **步骤 3：运行新测试并确认红灯失败**

运行：

```bash
cmake --build build-field-fixes --target anchor_target_selection_tests -j2
```

预期：构建失败，因为 `TargetSelectionContext` 和重载的 `selectTarget()` 尚不存在。

- [x] **步骤 4：增加锚点常量、枚举值、上下文和候选字段**

修改 `src/visionclient.h`，位置靠近现有 ROI 宏：

```cpp
// 最高层候选离初始拍照锚点的最大可信距离，单位 mm；现场根据正常 锚点距离 日志微调。
#define VISION_ANCHOR_MAX_TRUST_XY_MM 450.0

// 锚点选择的同层 Z 容差，单位 mm；必须远小于料箱层高，避免低层被当作同层。
#define VISION_ANCHOR_SAME_LAYER_Z_TOL_MM 20.0

// 闭环过程中本帧目标相对上一帧锚点位置的最大可信跳变，单位 mm。
#define VISION_ANCHOR_SWITCH_MAX_XY_MM 220.0
```

修改 `TargetSelectionReason`：

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

在 `TargetCandidate` 前增加：

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

扩展 `TargetCandidate`：

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

增加公开方法：

```cpp
    /// 纯逻辑：按固定拍照锚点上下文选择目标；handEyeMatrix 为 4x4 行主序矩阵。
    static TargetSelection selectTarget(const QJsonArray &objects,
                                        const TargetSelectionContext &context,
                                        const float handEyeMatrix[4][4]);

    /// 设置下一次 /inference 使用的目标选择上下文；由机械臂阶段一在发起推理前注入。
    void setTargetSelectionContext(const TargetSelectionContext &context);
```

增加私有成员：

```cpp
    TargetSelectionContext m_targetSelectionContext; ///< 最近一次推理使用的选择上下文，生命周期到下一次 set 覆盖。
```

- [x] **步骤 5：实现锚点解析和选择逻辑**

修改 `src/visionclient.cpp`，在现有选择辅助函数附近增加辅助函数：

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

增加设置函数：

```cpp
void VisionHttpClient::setTargetSelectionContext(const TargetSelectionContext &context)
{
    m_targetSelectionContext = context;
}
```

实现重载接口：

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

- [x] **步骤 6：推理解析时使用锚点选择重载接口**

修改 `VisionHttpClient::parseInferenceReply()`：

```cpp
    const QJsonArray objects = doc.object().value(QStringLiteral("objects")).toArray();
    const TargetSelection selection = selectTarget(objects, m_targetSelectionContext, m_T);
    emit selectionLogMessage(formatTargetSelectionLog(selection));
```

选中候选后的其余转换逻辑保持兼容；启用锚点模式时使用已经转换好的工具系值：

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

- [x] **步骤 7：扩展选择日志格式**

修改 `formatTargetSelectionLog()`，让有效候选在有锚点字段时输出锚点信息：

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

最终返回字符串必须保持单行；同时为新增枚举值补充原因文本：

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

- [x] **步骤 8：运行锚点选择测试**

运行：

```bash
cmake --build build-field-fixes --target anchor_target_selection_tests vision_target_selection_tests -j2
ctest --test-dir build-field-fixes -R '^(anchor_target_selection_tests|vision_target_selection_tests)$' --output-on-failure
```

预期：两个测试均通过。

---

### 任务 2： HuayanScheduler 维护拍照锚点上下文

**文件：**
- 修改： `src/huayanScheduler.h`
- 修改： `src/huayanScheduler.cpp`
- 修改： `src/devicemanager.cpp`
- 修改： `src/visionclient.h`
- 修改： `tests/test_huayan_scheduler_contract.cpp`

**接口：**
- 依赖： 任务 1 产出的 `VisionHttpClient::TargetSelectionContext` 和 `VisionHttpClient::setTargetSelectionContext(const TargetSelectionContext &context)`。
- 产出：
  - `resetVisionAnchorTracking()`
  - `makeVisionTargetSelectionContext() const`
  - `recordCompletedGrabMove(const RelMove &move)`

- [x] **步骤 1：增加预期失败的调度器契约断言**

向 `tests/test_huayan_scheduler_contract.cpp` 追加这些检查：

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

- [x] **步骤 2：运行调度器契约测试并确认红灯失败**

运行：

```bash
cmake --build build-field-fixes --target huayan_scheduler_contract_tests -j2
ctest --test-dir build-field-fixes -R '^huayan_scheduler_contract_tests$' --output-on-failure
```

预期：测试失败，因为锚点跟踪函数和源码字符串尚不存在。

- [x] **步骤 3：增加 HuayanScheduler 成员和辅助函数**

修改 `src/huayanScheduler.h`；如果尚未可见，则包含 `visionclient.h`：

```cpp
#include "visionclient.h"
```

增加私有辅助函数声明：

```cpp
    /// 清零阶段一固定拍照锚点状态；每次 startStageOne() 必须调用一次。
    void resetVisionAnchorTracking();
    /// 生成下一次视觉推理使用的固定拍照锚点上下文。
    VisionHttpClient::TargetSelectionContext makeVisionTargetSelectionContext() const;
    /// 记录一条已经完成的阶段一 XY 微调；只能在控制器确认到位后调用。
    void recordCompletedGrabMove(const RelMove &move);
```

增加私有成员：

```cpp
    double m_anchorAccumulatedToolX = 0.0; ///< 初始拍照位到当前相机位置已完成工具系 X 位移(mm)。
    double m_anchorAccumulatedToolY = 0.0; ///< 初始拍照位到当前相机位置已完成工具系 Y 位移(mm)。
    bool m_anchorHasPreviousTarget = false; ///< 是否已有上一帧可信目标用于闭环跳变保护。
    double m_anchorPreviousTargetX = 0.0; ///< 上一帧可信目标相对初始拍照锚点 X(mm)。
    double m_anchorPreviousTargetY = 0.0; ///< 上一帧可信目标相对初始拍照锚点 Y(mm)。
```

- [x] **步骤 4：实现重置、上下文生成和累计辅助函数**

修改 `src/huayanScheduler.cpp`：

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

- [x] **步骤 5：阶段一启动时重置锚点跟踪状态**

修改 `HuayanScheduler::startStageOne()`，放在其他阶段级重置逻辑附近：

```cpp
    resetVisionAnchorTracking();
```

预期上下文：该方法已经重置阶段状态、搜索计数和视觉迭代状态；锚点重置应与这些重置放在一起。

- [x] **步骤 6：推理前注入选择上下文**

修改 `executeCurrentStep()` 中的 `StageStep::WaitForVision` 分支：

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

实现：

```cpp
void HuayanScheduler::setVisionClient(VisionHttpClient *client)
{
    m_visionClient = client;
}
```

在 `DeviceManager` 中，`m_visionClient` 和 `m_huayanScheduler` 构造完成后增加：

```cpp
    m_huayanScheduler->setVisionClient(m_visionClient);
```

- [x] **步骤 7：移动索引递增前累计已完成的 XY 位移**

修改 `onPollTick()` 中 `MoveToGrab` 完成分支：

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

- [x] **步骤 8：视觉结果返回后记录上一帧可信锚点目标**

只有在任务 1 给发出的信号增加锚点坐标时，才修改 `setGrabOffset()` 签名。推荐的最小接口：

在 `VisionHttpClient` 中增加信号：

```cpp
    void rawCoordinatesReady(double x, double y, double z, double rz,
                             double anchorX, double anchorY);
```

保留现有 4 参数信号以兼容其他连接；在 `parseInferenceReply()` 中，启用锚点模式时发出 6 参数信号。

在 `HuayanScheduler` 中增加重载：

```cpp
    void setGrabOffset(double x, double y, double z, double rz,
                       double contextSelectedAnchorX, double contextSelectedAnchorY);
```

实现开始时先记录本次选中的锚点目标：

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

更新 `DeviceManager` 连接，把 6 参数信号连接到 6 参数槽。

- [x] **步骤 9：运行调度器契约测试并构建应用目标**

运行：

```bash
cmake --build build-field-fixes --target huayan_scheduler_contract_tests wh-robot-visual -j2
ctest --test-dir build-field-fixes -R '^huayan_scheduler_contract_tests$' --output-on-failure
```

预期：测试通过，应用目标构建通过。

---

### 任务 3： 阶段一 XY/Z 运动硬保护

**文件：**
- 修改： `src/huayanScheduler.cpp`
- 修改： `src/huayanScheduler.h`
- 修改： `tests/test_huayan_scheduler_contract.cpp`

**接口：**
- 依赖： 现有 `RelMove` 和 `calculateGrabDescend()`。
- 产出：
  - `HUAYAN_MAX_SINGLE_XY_ADJUST_MM`
  - `HUAYAN_MAX_Z_DESCEND_MM`
  - `bool validateStageOneRelMoveBeforeDispatch(const RelMove &move) const`
  - 下发前的硬保护失败日志，位置在 `MoveRelL` 下发前。

- [x] **步骤 1：增加运动保护的预期失败契约断言**

追加到 `tests/test_huayan_scheduler_contract.cpp`：

```cpp
    requireTrue(schedulerSource.contains(QStringLiteral("HUAYAN_MAX_SINGLE_XY_ADJUST_MM"))
                    && schedulerSource.contains(QStringLiteral("HUAYAN_MAX_Z_DESCEND_MM")),
                "阶段一必须有独立的 XY 单步和 Z 下探硬保护常量");

    requireTrue(schedulerSource.contains(QStringLiteral("validateStageOneRelMoveBeforeDispatch"))
                    && schedulerSource.contains(QStringLiteral("目标不可信：计划"))
                    && schedulerSource.contains(QStringLiteral("拒绝下发 MoveRelL")),
                "阶段一相对运动下发前必须执行拒绝下发保护并记录原因");

    requireTrue(schedulerSource.contains(QStringLiteral("qMin(kMaxDescend, HUAYAN_MAX_Z_DESCEND_MM)")),
                "Z 下探计算必须同时受原有 kMaxDescend 和新的硬保护上限约束");
```

- [x] **步骤 2：运行契约测试并确认红灯失败**

运行：

```bash
cmake --build build-field-fixes --target huayan_scheduler_contract_tests -j2
ctest --test-dir build-field-fixes -R '^huayan_scheduler_contract_tests$' --output-on-failure
```

预期：测试失败，因为保护常量和辅助函数尚不存在。

- [x] **步骤 3：增加保护常量**

修改 `src/huayanScheduler.cpp`，位置靠近现有 `kMaxDescend`：

```cpp
static constexpr double HUAYAN_MAX_SINGLE_XY_ADJUST_MM = 250.0; ///< 阶段一单次 XY 微调上限(mm)，现场验证 250mm 可覆盖正常锁定目标微调。
static constexpr double HUAYAN_MAX_Z_DESCEND_MM = 1078.0;       ///< 阶段一 Z 下探硬上限(mm)，不得因临时调试放大。
```

- [x] **步骤 4：增加运动校验辅助函数**

在 `src/huayanScheduler.h` 中声明：

```cpp
    /// 阶段一相对运动下发前的上位机硬保护；返回 false 时必须已经记录错误并停止阶段。
    bool validateStageOneRelMoveBeforeDispatch(const RelMove &move) const;
```

在 `src/huayanScheduler.cpp` 中实现：

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

如果要避免 `const_cast`，则把辅助函数改为非 const：

```cpp
bool validateStageOneRelMoveBeforeDispatch(const RelMove &move);
```

并直接发出错误。

- [x] **步骤 5：每次下发 XY/Rz 抓取微调前调用保护函数**

修改 `executeNextGrabMove()`，在记录日志和创建 `PendingCommand` 前增加：

```cpp
    const RelMove &mv = m_grabMoves.at(m_grabMoveIdx);
    if (!validateStageOneRelMoveBeforeDispatch(mv))
        return false;
```

- [x] **步骤 6：应用 Z 下探硬上限**

修改 `StageStep::DescendZ`：

```cpp
            const double plannedDescend = m_grabOffset.z - m_grabZClearance;
            if (plannedDescend > HUAYAN_MAX_Z_DESCEND_MM) {
                emitOperationError(QStringLiteral("[阶段一] 目标不可信：计划 Z 下探 %1mm 超过硬上限 %2mm，拒绝下发 MoveRelL")
                                       .arg(plannedDescend, 0, 'f', 1)
                                       .arg(HUAYAN_MAX_Z_DESCEND_MM, 0, 'f', 1));
                return;
            }

            const double descend = calculateGrabDescend(
                m_grabOffset.z,
                m_grabZClearance,
                qMin(kMaxDescend, HUAYAN_MAX_Z_DESCEND_MM));
```

先用未截断的计划下探值做硬保护；只有计划值未超过硬上限时，才允许进入 `calculateGrabDescend()` 计算实际下发距离。

- [x] **步骤 7：运行保护测试并构建应用目标**

运行：

```bash
cmake --build build-field-fixes --target huayan_scheduler_contract_tests wh-robot-visual -j2
ctest --test-dir build-field-fixes -R '^huayan_scheduler_contract_tests$' --output-on-failure
```

预期：测试通过，应用目标构建通过。

---

### 任务 4： 文档同步、完整验证和唯一功能提交

**文件：**
- 修改： `docs/superpowers/specs/2026-07-18-anchor-vision-target-trust-design.md`
- 修改： `docs/superpowers/plans/2026-07-18-anchor-vision-target-trust.md`
- 修改： 任务 1-3 修改过的所有文件

**接口：**
- 依赖：任务 1-3 已完成的实现。
- 产出：一个包含代码、测试和文档的本地提交；不推送。

- [x] **步骤 1：更新设计文档实施状态**

编辑 `docs/superpowers/specs/2026-07-18-anchor-vision-target-trust-design.md`：

```markdown
**状态：** 已实施并完成自动化验证；目标锁定策略已现场验证有效，40961 待向华研厂家确认
```

增加验证结果章节：

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
- 是否还出现 40961/49601；若出现，检查对应命令是否已在上位机保护范围之外。
```

- [x] **步骤 2：运行空白字符和占位符检查**

运行：

```bash
git diff --check
rg -n "TB[D]|TO[D]O|implement[ ]later|fill[ ]in[ ]details|适当处[理]|类似[ ]Task" \
  docs/superpowers/specs/2026-07-18-anchor-vision-target-trust-design.md \
  docs/superpowers/plans/2026-07-18-anchor-vision-target-trust.md \
  src/visionclient.h src/visionclient.cpp src/huayanScheduler.h src/huayanScheduler.cpp \
  tests/test_anchor_target_selection.cpp tests/test_huayan_scheduler_contract.cpp
```

预期：

- `git diff --check` 退出码为 0。
- `rg` 退出码为 1，且没有匹配。

- [x] **步骤 3：构建并运行重点测试**

运行：

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

预期：

- 四个目标均构建通过。
- 三个重点测试均通过。

- [x] **步骤 4：运行完整验证**

运行：

```bash
cmake --build build-field-fixes -j2
ctest --test-dir build-field-fixes --output-on-failure
```

预期：所有测试通过。

- [x] **步骤 5：检查最终差异，确认没有禁止修改**

运行：

```bash
git status --short
git diff --stat
git diff -- src/visionclient.h src/visionclient.cpp src/huayanScheduler.h src/huayanScheduler.cpp
```

人工确认：

- 未修改视觉服务 Python 接口。
- 未修改示教器函数名配置，除非测试明确要求。
- 未删除 `[华沿][MoveRelL诊断][命令=%1]` 日志。
- `test-events.jsonl`、`test-state.backup.json`、`test-state.json` 保持未跟踪且未暂存。

- [x] **步骤 6：创建唯一的本地实现提交**

仅暂存任务相关文件：

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
git commit -m "fix: add ancho红灯失败 vision target trust guard"
```

预期：

- 只暂存任务相关文件。
- 三个用户 JSON 文件没有被暂存。
- 创建一个本地提交。

- [x] **步骤 7：确认没有执行推送**

运行：

```bash
git status --short
git log -2 --oneline
```

预期：

- 没有任务相关的未暂存修改。
- 以下用户文件保持未跟踪是允许的：
  - `test-events.jsonl`
  - `test-state.backup.json`
  - `test-state.json`
- 不执行 `git push`。

---

## 自查记录

- 规格覆盖：任务 1-3 覆盖锚点选择、目标不可信、运动保护和日志；任务 4 覆盖文档、验证和单个本地提交。
- 占位符扫描：本计划不包含禁止的占位符标记。
- 类型一致性：`TargetSelectionContext` 在任务 1 定义、任务 2 使用；运动保护辅助函数在任务 3 定义并使用。

---

## 任务 5：现场复测后的目标锁定闭环跟踪

**状态：** 已实施并完成现场验证；选择效果良好，40961 待厂家确认。

**目标：** 解决 2026-07-18 现场复测发现的目标左右摇摆、旁边工位目标偶发抢占、锁定目标短暂丢失后误切换的问题。机械臂一旦开始朝某个箱子闭环微调，后续帧只允许继续跟踪这个锁定目标；如果锁定目标暂时识别不到，本帧不下发 `MoveRelL`，连续丢失达到上限后停止阶段一。

**文件：**

- 新增：`tests/test_locked_target_selection.cpp`
- 修改：`tests/CMakeLists.txt`
- 修改：`src/visionclient.h`
- 修改：`src/visionclient.cpp`
- 修改：`src/huayanScheduler.h`
- 修改：`src/huayanScheduler.cpp`
- 修改：`docs/superpowers/specs/2026-07-18-anchor-vision-target-trust-design.md`
- 修改：`docs/superpowers/plans/2026-07-18-anchor-vision-target-trust.md`

**新增或调整的接口：**

- `VisionHttpClient::TargetSelectionContext`：在原锚点上下文上扩展 `lockEnabled`、`lockMissingFrames`、锁定半径、同层容差和固定侧配置，避免另起一套重复接口。
- `VisionHttpClient::TargetSelection`：扩展锁定日志字段，包括锁定目标锚点、连续丢失帧数、最近锁定距离和丢失上限。
- `VisionHttpClient::selectTarget(const QJsonArray &objects, const TargetSelectionContext &context, const float handEyeMatrix[4][4])`：复用现有纯逻辑入口；`lockEnabled=true` 时执行目标锁定策略，未启用时保持原锚点选择行为。
- `HuayanScheduler::resetVisionAnchorTracking()`：阶段一启动、失败、完成时清空锚点累计和锁定目标丢失状态。
- `HuayanScheduler::setGrabOffset(..., contextSelectedAnchorX, contextSelectedAnchorY)`：视觉选中后更新锁定目标锚点并清零连续丢失帧数。

**新增宏：**

```cpp
// 锁定目标连续丢失帧数上限；达到后阶段一失败，避免视觉飘到旁边工位后继续追踪。
#define VISION_LOCK_MAX_MISSING_FRAMES 3

// 闭环候选离锁定目标锚点位置的最大允许距离，单位 mm；超过则认为不是同一个箱子。
#define VISION_LOCK_TRACK_RADIUS_MM 260.0

// 目标锁定模式下的同层 Z 容差，单位 mm；用于把同一层两个箱子归为同层候选。
#define VISION_LOCK_SAME_LAYER_Z_TOL_MM 80.0

// 同层固定侧选择使用 Y 轴；现场若确认应按 X 轴区分，可新增 X 轴宏并切换。
#define VISION_LOCK_FIXED_SIDE_AXIS_Y 1

// 固定侧选择是否取较小坐标值；1 表示取 Y 最小，0 表示取 Y 最大。
#define VISION_LOCK_FIXED_SIDE_PICK_MIN 1
```

- [x] **步骤 1：增加目标锁定纯逻辑测试**

新增 `tests/test_locked_target_selection.cpp`，覆盖以下场景：

1. 初始帧先过滤锚点距离过远目标，再在可信目标中选最高层。
2. 最高层同层有两个候选时，按固定侧规则选择，不按锚点最近摇摆。
3. 闭环帧存在锁定目标附近候选时，继续选锁定目标附近候选。
4. 闭环帧只识别到旁边工位目标时，返回“锁定目标暂时丢失”，不返回普通目标。
5. 连续丢失未达到 `VISION_LOCK_MAX_MISSING_FRAMES` 时，不下发运动但不立即切换目标。
6. 连续丢失达到 `VISION_LOCK_MAX_MISSING_FRAMES` 时，返回阶段失败原因。
7. 日志必须包含锁定目标锚点、候选到锁定目标距离、丢失帧数和拒绝切换原因。

预期红灯：

```bash
cmake --build build-field-fixes --target locked_target_selection_tests -j2
```

构建失败或测试失败，因为目标锁定接口尚不存在。

- [x] **步骤 2：注册测试目标**

修改 `tests/CMakeLists.txt`，增加 `locked_target_selection_tests`，链接 Qt Core/Gui/Network，并加入 CTest。

预期：

```bash
ctest --test-dir build-field-fixes -R '^locked_target_selection_tests$' --output-on-failure
```

测试目标能被 CTest 发现，且在实现前保持失败。

- [x] **步骤 3：在视觉客户端增加锁定上下文和选择结果**

修改 `src/visionclient.h`：

1. 增加目标锁定宏，并为每个宏写明单位、业务含义和现场调参方式。
2. 扩展 `TargetSelectionContext`，字段注释必须说明生命周期：
   - 是否处于锁定模式。
   - 是否已有锁定目标。
   - 锁定目标锚点 X/Y。
   - 连续丢失帧数。
   - 最大丢失帧数。
   - 跟踪半径。
   - 同层 Z 容差。
   - 固定侧轴和固定侧方向。
3. 扩展 `TargetSelection`，字段注释必须说明调度器如何使用：
   - 是否选中目标。
   - 是否锁定目标暂时丢失。
   - 是否达到丢失上限。
   - 本帧最新丢失帧数。
   - 最近候选到锁定目标的距离。
   - 单行日志摘要。

预期：头文件能清楚表达“锁定后不允许自动切换”的业务规则。

- [x] **步骤 4：实现初始锁定选择**

修改 `src/visionclient.cpp`：

1. 复用现有候选解析、工具系换算和锚点坐标计算。
2. 初始未锁定时，先排除 `anchorDistance > VISION_ANCHOR_MAX_TRUST_XY_MM` 的候选。
3. 在可信候选中选择最高层。
4. 最高层同层多个候选时，使用固定侧规则选择：
   - `VISION_LOCK_FIXED_SIDE_AXIS_Y == 1` 时按候选 raw `y` 比较。
   - `VISION_LOCK_FIXED_SIDE_PICK_MIN == 1` 时取较小值，否则取较大值。
5. 选中后返回锁定目标锚点坐标，供调度器保存。

预期：旁边工位目标即使 Z 更高，只要锚点距离不可信，就不能参与初始锁定。

- [x] **步骤 5：实现闭环锁定跟踪**

修改 `src/visionclient.cpp`：

1. 已有锁定目标时，不执行全局最高 Z 重新选择。
2. 计算每个候选到锁定目标锚点的距离。
3. 只保留距离小于等于 `VISION_LOCK_TRACK_RADIUS_MM` 的候选。
4. 如果存在锁定范围内候选，在这些候选里按最高层和固定侧规则选择。
5. 如果不存在锁定范围内候选，返回“锁定目标暂时丢失”，并给出最新丢失帧数。
6. 丢失达到上限时，返回“锁定目标连续丢失达到上限”。

预期：视觉某帧只识别到旁边工位目标时，程序不切换目标、不下发运动。

- [x] **步骤 6：调度器维护目标锁定状态**

修改 `src/huayanScheduler.h/.cpp`：

1. 阶段一开始、失败、完成时调用 `resetVisionAnchorTracking()`。
2. 第一次视觉选中后记录锁定目标锚点。
3. 闭环视觉选中后更新锁定目标锚点并清零丢失帧数。
4. 锁定目标暂时丢失时增加丢失帧数，本帧不下发 `MoveRelL`。
5. 连续丢失达到上限时，调用阶段失败流程，不进入普通无目标搜索下移。
6. 日志写清楚：
   - 当前工位。
   - 锁定目标锚点。
   - 丢失帧数。
   - 本帧候选摘要。
   - 是否拒绝切换旁边工位目标。

预期：机械臂闭环过程中不会因为视觉不稳定漂到其他工位。

- [x] **步骤 7：保留并强化运动保护**

确认 `HUAYAN_MAX_SINGLE_XY_ADJUST_MM` 继续在下发 `MoveRelL` 前生效。

要求：

1. 目标锁定策略减少 380mm 以上大幅错误微调。
2. 如果仍出现大幅微调，单次 XY 上限继续 fail-closed 拒绝下发。
3. 不继续通过放大 `HUAYAN_MAX_SINGLE_XY_ADJUST_MM` 来绕过 40961/49601；250mm 是当前现场验证值。

预期：40961/49601 风险降低，但不承诺由上位机彻底根治控制器安全空间拒绝。

- [x] **步骤 8：更新文档和日志说明**

更新 `docs/superpowers/specs/2026-07-18-anchor-vision-target-trust-design.md` 和本计划：

1. 写明第一版锚点策略的现场问题。
2. 写明目标锁定策略替代“每帧全局重新选择”的原因。
3. 写明新增宏、接口、日志字段和验收标准。
4. 所有正文说明使用中文。

预期：文档和代码保持一致。

- [x] **步骤 9：验证**

运行：

```bash
cmake --build build-field-fixes --target \
  locked_target_selection_tests \
  anchor_target_selection_tests \
  vision_target_selection_tests \
  huayan_scheduler_contract_tests \
  wh-robot-visual \
  -j2

ctest --test-dir build-field-fixes \
  -R '^(locked_target_selection_tests|anchor_target_selection_tests|vision_target_selection_tests|huayan_scheduler_contract_tests)$' \
  --output-on-failure

cmake --build build-field-fixes -j2
ctest --test-dir build-field-fixes --output-on-failure
```

预期：

- 新增锁定目标测试通过。
- 旧锚点选择测试继续通过。
- 调度器契约测试继续通过。
- 主程序构建通过。
- 全量 CTest 通过。

- [x] **步骤 10：提交前检查**

运行：

```bash
git diff --check
git status --short
git diff --stat
```

确认：

- 未推送。
- 未暂存用户未跟踪 JSON 文件。
- 日志文件不加入提交。
- 目标锁定代码、测试、spec 和 plan 一起作为一个功能提交。

### 任务 5 本地与现场验证记录

2026-07-18 本地 `build-field-fixes` 验证：

- `locked_target_selection_tests`：构建通过，CTest 通过。
- `anchor_target_selection_tests`：构建通过，CTest 通过。
- `vision_target_selection_tests`：构建通过，CTest 通过。
- `huayan_scheduler_contract_tests`：构建通过，CTest 通过。
- `wh-robot-visual`：构建通过。

2026-07-18 现场验证：

- 目标锁定选择效果良好，未再选择到旁边工位目标。
- 底层目标可正常选择。
- 方法依赖工位料箱摆放准确性和拍照点位接近当前工位中心。
- `HUAYAN_MAX_SINGLE_XY_ADJUST_MM` 同步现场验证值 `250.0mm`。
- 华研 `40961` 仍需向厂家确认；此前日志同类问题曾记录为 `49601（Target orientation exceeded cartesian safety space）`。

提交状态：

- 本次按用户要求未提交、未推送。
- `test-events.jsonl`、`test-state.backup.json`、`test-state.json` 保持未跟踪，未暂存。
