# 现场故障诊断与流程隔离实施计划

> **供智能体执行人员使用：** 必须使用子技能 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans`，按任务逐项实施本计划。各步骤使用复选框（`- [ ]`）跟踪进度。

**目标：** 在不重写已验证生产状态机的前提下，补齐华研 49601 失败诊断、把 AGV 等待统一为 5 分钟、隔离阶段一手工测试配置，并按“当前工位 XY → 最高层 Z → 同层最近中心”选择视觉目标。

**实施状态：** 已按本计划完成代码、测试和设计文档同步；自动化验证 10/10 通过，49601 根因仍等待现场复现日志确认。

**架构：** 保留唯一的 `HuayanScheduler`，由生产入口和手工测试入口各自在启动前注入完整配置，并用业务层忙碌检查实现互斥。视觉选择提取为 `VisionHttpClient` 的纯静态逻辑进行单元测试；AGV、UI/调度隔离和厂家 SDK 诊断沿用仓库现有源码契约测试，再用真实 Qt 构建与现场日志验收闭环。

**技术栈：** C++17、Qt 6.8.3（Core/Widgets/Network/Test）、CMake/CTest、华研 Robot SDK V1.0.15.0。

## 全局约束

- Qt 路径固定为 `Qt6_DIR=/opt/Qt/qt6.8/6.8.3/gcc_64/lib/cmake/Qt6`，`qt-cmake=/opt/Qt/qt6.8/6.8.3/gcc_64/bin/qt-cmake`。
- 机械臂错误的实际现场码是 49601；本次只增加失败诊断，不宣称修复控制器安全空间问题。
- 工具坐标系正常运动 381～404 mm 不得被视觉 ROI 或新增运动阈值拒绝。
- AGV 上位机导航和返航等待统一为 300000 ms；明确的 Failed/Canceled/Timeout 仍立即失败。
- 手工“启动取料（阶段一）”必须选择 1～12 号工位、关闭扫码、复用现有阶段一状态机，并与总调度双向互斥。
- 视觉当前工位矩形由共用宏 `VISION_STATION_ROI_HALF_X_MM=500.0`、`VISION_STATION_ROI_HALF_Y_MM=500.0` 控制；边界有效。
- 新增或修改的 `enum class`、接口、参数、配置宏、成员变量和非显然业务分支必须写明语义、单位、生命周期或互斥约束。
- 实际代码、测试、设计文档与本计划最终作为一个完整功能提交；任务之间不创建零散提交。
- 不执行 `git push`；由现场人员人工推送。
- 保留用户现有未跟踪文件 `test-events.jsonl`、`test-state.backup.json`、`test-state.json`，不得加入提交。

---

## 文件结构

### 新建文件

- `tests/test_vision_target_selection.cpp`：纯 JSON 候选筛选单元测试，覆盖 ROI、层高、中心距离、顺序无关和非法字段。
- `tests/test_standalone_pickup_contract.cpp`：锁住手工测试配置注入、关闭扫码、1～12 工位 UI 和双向互斥。
- `tests/test_navigation_timeout_contract.cpp`：锁住三个 300000 ms 超时、耗时日志和明确失败立即处理。

### 修改文件

- `tests/CMakeLists.txt`：注册三个新增测试目标。
- `tests/test_huayan_scheduler_contract.cpp`：增加 MoveRelL 失败快照只在 SDK 非零返回分支输出的契约检查。
- `src/visionclient.h`：声明共用 ROI 宏、候选/结果类型、选择与日志格式化接口、单条选择日志信号。
- `src/visionclient.cpp`：实现合法性校验、两阶段确定性选择和单条候选摘要；解析成功后复用选择结果。
- `src/devicemanager.h/.cpp`：提供手工阶段一业务入口，显式装载所选工位配置并关闭扫码；转发视觉选择日志。
- `src/huayanScheduler.h/.cpp`：公开只读忙碌状态；给相对运动增加命令序号、失败前后快照和失败专用日志。
- `src/linemanager.h/.cpp`：拒绝机械臂被手工测试或兼容旧整线占用时启动总调度；返航超时改为 5 分钟并记录实际等待。
- `src/taskexecutor.h/.cpp`：任务导航超时改为 5 分钟并记录实际等待；保留明确失败立即处理。
- `src/lineorchestrator.h/.cpp`：旧流程入口同样执行机械臂忙碌和新总调度运行互斥，导航超时统一为 5 分钟。
- `src/mainwindow.h/.cpp`：增加 1～12 工位下拉框，调用业务层测试入口，并统一刷新手工/总调度按钮互斥状态。
- `docs/superpowers/specs/2026-07-16-field-fault-diagnostics-and-workflow-isolation-design.md`：实施后同步最终接口名、日志格式、测试和验证结果。

---

### Task 1：实现确定性的当前工位视觉目标选择

**文件：**
- 新建：`tests/test_vision_target_selection.cpp`
- 修改：`tests/CMakeLists.txt`
- 修改：`src/visionclient.h:29-131`
- 修改：`src/visionclient.cpp:168-270`
- 修改：`src/devicemanager.cpp:105-135`

**接口：**
- 输入：视觉服务 `objects[]` 中的 `offset_mm.x/y`、`depth_compensated`、`angle`、`confidence`。
- 输出：`VisionHttpClient::TargetSelection selectTarget(const QJsonArray &objects)`、`QString formatTargetSelectionLog(const TargetSelection &selection)`、`selectionLogMessage(QString)`。

- [ ] **步骤1：添加会失败的视觉选择测试目标**

在 `tests/CMakeLists.txt` 末尾加入：

```cmake
add_executable(vision_target_selection_tests
    test_vision_target_selection.cpp
    ../src/visionclient.cpp
)

target_include_directories(vision_target_selection_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(vision_target_selection_tests PRIVATE
    Qt6::Core
    Qt6::Network
)

add_test(NAME vision_target_selection_tests
         COMMAND vision_target_selection_tests)
```

- [ ] **步骤2：编写会失败的目标选择测试**

创建 `tests/test_vision_target_selection.cpp`：

```cpp
#include "visionclient.h"

#include <QJsonArray>
#include <QJsonObject>

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

QJsonObject target(double x,
                   double y,
                   double depth,
                   double angle = 0.0,
                   double confidence = 0.95)
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

const VisionHttpClient::TargetCandidate &selected(
    const VisionHttpClient::TargetSelection &selection)
{
    requireTrue(selection.hasTarget(), "选择结果必须包含目标");
    return selection.candidates.at(selection.selectedCandidateIndex);
}

} // namespace

int main()
{
    using Reason = VisionHttpClient::TargetSelectionReason;

    const auto currentStationWins = VisionHttpClient::selectTarget(QJsonArray{
        target(501.0, 0.0, 100.0),
        target(120.0, 80.0, 300.0)
    });
    requireTrue(selected(currentStationWins).sourceIndex == 1,
                "矩形外目标即使更高也必须被排除");

    const auto highestLayerWins = VisionHttpClient::selectTarget(QJsonArray{
        target(5.0, 5.0, 400.0),
        target(300.0, 300.0, 300.0)
    });
    requireTrue(selected(highestLayerWins).sourceIndex == 1,
                "当前工位内必须先选择最高层，而不是最近中心");
    requireTrue(highestLayerWins.reason == Reason::HighestLayer,
                "单个最高层目标必须记录 HighestLayer 原因");

    const auto sameLayerNearest = VisionHttpClient::selectTarget(QJsonArray{
        target(250.0, 250.0, 300.0),
        target(40.0, 30.0, 315.0)
    });
    requireTrue(selected(sameLayerNearest).sourceIndex == 1,
                "20mm 同层范围内必须选择 XY 最近中心目标");
    requireTrue(sameLayerNearest.reason == Reason::SameLayerNearestCenter,
                "同层择近必须记录 SameLayerNearestCenter 原因");

    const auto boundary = VisionHttpClient::selectTarget(QJsonArray{
        target(-500.0, 500.0, 200.0)
    });
    requireTrue(boundary.hasTarget(), "±500mm 边界必须有效");
    requireTrue(selected(boundary).insideStationRoi, "边界目标必须标记为 ROI 内");

    const auto orderA = VisionHttpClient::selectTarget(QJsonArray{
        target(200.0, 0.0, 350.0),
        target(20.0, 0.0, 200.0),
        target(100.0, 0.0, 210.0)
    });
    const auto orderB = VisionHttpClient::selectTarget(QJsonArray{
        target(100.0, 0.0, 210.0),
        target(200.0, 0.0, 350.0),
        target(20.0, 0.0, 200.0)
    });
    requireTrue(selected(orderA).x == 20.0 && selected(orderB).x == 20.0,
                "调换 JSON 顺序后必须选择同一物理目标");

    QJsonObject missingDepth = target(0.0, 0.0, 100.0);
    missingDepth.remove(QStringLiteral("depth_compensated"));
    const auto noValidTarget = VisionHttpClient::selectTarget(QJsonArray{
        target(600.0, 0.0, 100.0),
        missingDepth,
        QJsonObject{{QStringLiteral("offset_mm"), QStringLiteral("非法")}}
    });
    requireTrue(!noValidTarget.hasTarget(),
                "矩形外或关键字段非法时必须返回无目标");
    requireTrue(noValidTarget.reason == Reason::None,
                "无目标必须记录 None 原因");

    const auto stableTie = VisionHttpClient::selectTarget(QJsonArray{
        target(10.0, 0.0, 200.0),
        target(-10.0, 0.0, 200.0)
    });
    requireTrue(selected(stableTie).sourceIndex == 0,
                "深度和中心距离完全相同时必须按原始下标稳定兜底");
    requireTrue(stableTie.reason == Reason::StableSourceIndex,
                "稳定兜底必须记录 StableSourceIndex 原因");

    const QString summary = VisionHttpClient::formatTargetSelectionLog(currentStationWins);
    requireTrue(summary.contains(QStringLiteral("#0"))
                    && summary.contains(QStringLiteral("范围外"))
                    && summary.contains(QStringLiteral("选中=#1")),
                "候选摘要必须包含下标、ROI 判定和最终选择");
    requireTrue(!summary.contains(QLatin1Char('\n')),
                "每次推理候选摘要必须保持单行");

    return 0;
}
```

- [ ] **步骤3：配置并运行新测试，确认处于红灯状态**

运行：

```bash
/opt/Qt/qt6.8/6.8.3/gcc_64/bin/qt-cmake -S . -B build-field-fixes \
  -DQt6_DIR=/opt/Qt/qt6.8/6.8.3/gcc_64/lib/cmake/Qt6 \
  -DBUILD_TESTING=ON
cmake --build build-field-fixes --target vision_target_selection_tests -j2
```

预期：编译失败，因为 `TargetCandidate`、`TargetSelectionReason`、`selectTarget()` 和 `formatTargetSelectionLog()` 尚不存在。

- [ ] **步骤4：增加带完整注释的 ROI 与目标选择接口**

在 `src/visionclient.h` 的头文件防重复包含保护之后、类定义之前加入：

```cpp
#include <QJsonArray>
#include <QList>

// 当前工位视觉候选矩形的 X 半宽，单位 mm；只过滤原始 offset_mm.x，不参与机械臂运动限位。
#define VISION_STATION_ROI_HALF_X_MM 500.0

// 当前工位视觉候选矩形的 Y 半宽，单位 mm；只过滤原始 offset_mm.y，不参与机械臂运动限位。
#define VISION_STATION_ROI_HALF_Y_MM 500.0
```

在 `VisionHttpClient` 的 `public:` 公有区加入：

```cpp
    /// 视觉目标最终入选原因；用于测试和现场候选摘要，不改变坐标转换语义。
    enum class TargetSelectionReason {
        None,                   ///< 没有合法且位于当前工位矩形内的目标。
        HighestLayer,           ///< 最高层分组中只有一个候选，按 Z 直接选中。
        SameLayerNearestCenter, ///< 最高层有多个候选，按原始 XY 距离选择最近中心者。
        StableSourceIndex       ///< 深度和 XY 距离完全相同时，按服务端原始下标稳定兜底。
    };

    /// 单个视觉候选；坐标仍处于视觉 API 的原始 offset/depth 空间，单位均为 mm。
    struct TargetCandidate {
        int sourceIndex = -1;        ///< 候选在原始 objects 数组中的下标，用于日志和稳定兜底。
        double x = 0.0;              ///< 原始 offset_mm.x，单位 mm。
        double y = 0.0;              ///< 原始 offset_mm.y，单位 mm。
        double depth = 0.0;          ///< 原始 depth_compensated，值越小代表层越高。
        double angle = 0.0;          ///< 原始箱体角度，单位 deg。
        double confidence = 0.0;     ///< 视觉服务置信度，仅记录，不在本次新增阈值过滤。
        bool valid = false;          ///< 所有抓取所需 JSON 字段均存在、为数值且有限。
        bool insideStationRoi = false; ///< 合法目标是否位于共用当前工位 XY 矩形内。
        QString rejectionReason;     ///< 非法目标的首个拒绝原因；合法目标为空。
    };

    /// 一次推理响应的完整选择结果；selectedCandidateIndex 指向 candidates，而非原 JSON。
    struct TargetSelection {
        QList<TargetCandidate> candidates; ///< 保留所有原始候选的解析/ROI 状态，供单行日志使用。
        int selectedCandidateIndex = -1;   ///< 最终候选在 candidates 中的下标；-1 表示无目标。
        TargetSelectionReason reason = TargetSelectionReason::None; ///< 最终选择规则。

        bool hasTarget() const
        {
            return selectedCandidateIndex >= 0
                && selectedCandidateIndex < candidates.size();
        }
    };

    /// 纯逻辑：先过滤当前工位 XY，再找最高层，最后在同层中选择最近中心目标。
    static TargetSelection selectTarget(const QJsonArray &objects);
    /// 把一次选择的全部候选压缩为单行现场日志；不得包含换行符。
    static QString formatTargetSelectionLog(const TargetSelection &selection);
```

在 `signals:` 信号区加入：

```cpp
    /// 每次 /inference 响应最多发出一次候选选择摘要，由 DeviceManager 转发到现场日志。
    void selectionLogMessage(QString message);
```

- [ ] **步骤5：实现合法性校验、XY 过滤、最高层分组和稳定兜底**

用以下实现替换 `src/visionclient.cpp` 中的 `centerDistSq(const QJsonObject &)`，并放在 `parseInferenceReply()` 前：

```cpp
namespace {

bool readFiniteNumber(const QJsonValue &value, double *result)
{
    if (!result || !value.isDouble())
        return false;
    const double number = value.toDouble();
    if (!qIsFinite(number))
        return false;
    *result = number;
    return true;
}

double centerDistSq(const VisionHttpClient::TargetCandidate &candidate)
{
    return candidate.x * candidate.x + candidate.y * candidate.y;
}

QString selectionReasonText(VisionHttpClient::TargetSelectionReason reason)
{
    using Reason = VisionHttpClient::TargetSelectionReason;
    switch (reason) {
    case Reason::HighestLayer:
        return QStringLiteral("最高层");
    case Reason::SameLayerNearestCenter:
        return QStringLiteral("同层最近中心");
    case Reason::StableSourceIndex:
        return QStringLiteral("原始下标稳定兜底");
    case Reason::None:
        return QStringLiteral("无目标");
    }
    return QStringLiteral("无目标");
}

} // namespace

VisionHttpClient::TargetSelection VisionHttpClient::selectTarget(const QJsonArray &objects)
{
    TargetSelection selection;
    QList<int> insideIndexes;

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

        candidate.valid = true;
        candidate.insideStationRoi = qAbs(candidate.x) <= VISION_STATION_ROI_HALF_X_MM
            && qAbs(candidate.y) <= VISION_STATION_ROI_HALF_Y_MM;
        selection.candidates.append(candidate);
        if (candidate.insideStationRoi)
            insideIndexes.append(selection.candidates.size() - 1);
    }

    if (insideIndexes.isEmpty())
        return selection;

    double highestDepth = selection.candidates.at(insideIndexes.first()).depth;
    for (int candidateIndex : insideIndexes)
        highestDepth = qMin(highestDepth, selection.candidates.at(candidateIndex).depth);

    QList<int> sameLayerIndexes;
    for (int candidateIndex : insideIndexes) {
        if (qAbs(selection.candidates.at(candidateIndex).depth - highestDepth)
            <= kSameLayerTolMm) {
            sameLayerIndexes.append(candidateIndex);
        }
    }

    int bestIndex = sameLayerIndexes.first();
    bool usedStableTieBreak = false;
    for (int candidateIndex : sameLayerIndexes) {
        const double candidateDistance = centerDistSq(selection.candidates.at(candidateIndex));
        const double bestDistance = centerDistSq(selection.candidates.at(bestIndex));
        if (candidateDistance < bestDistance) {
            bestIndex = candidateIndex;
            usedStableTieBreak = false;
        } else if (qFuzzyCompare(candidateDistance + 1.0, bestDistance + 1.0)
                   && selection.candidates.at(candidateIndex).sourceIndex
                       < selection.candidates.at(bestIndex).sourceIndex) {
            bestIndex = candidateIndex;
            usedStableTieBreak = true;
        } else if (candidateIndex != bestIndex
                   && qFuzzyCompare(candidateDistance + 1.0, bestDistance + 1.0)) {
            usedStableTieBreak = true;
        }
    }

    selection.selectedCandidateIndex = bestIndex;
    if (usedStableTieBreak) {
        selection.reason = TargetSelectionReason::StableSourceIndex;
    } else if (sameLayerIndexes.size() > 1) {
        selection.reason = TargetSelectionReason::SameLayerNearestCenter;
    } else {
        selection.reason = TargetSelectionReason::HighestLayer;
    }
    return selection;
}

QString VisionHttpClient::formatTargetSelectionLog(const TargetSelection &selection)
{
    QStringList candidateParts;
    int insideCount = 0;
    for (const TargetCandidate &candidate : selection.candidates) {
        if (!candidate.valid) {
            candidateParts.append(QStringLiteral("#%1 非法(%2)")
                                      .arg(candidate.sourceIndex)
                                      .arg(candidate.rejectionReason));
            continue;
        }
        if (candidate.insideStationRoi)
            ++insideCount;
        candidateParts.append(QStringLiteral("#%1 x=%2 y=%3 z=%4 %5")
                                  .arg(candidate.sourceIndex)
                                  .arg(candidate.x, 0, 'f', 1)
                                  .arg(candidate.y, 0, 'f', 1)
                                  .arg(candidate.depth, 0, 'f', 1)
                                  .arg(candidate.insideStationRoi
                                           ? QStringLiteral("范围内")
                                           : QStringLiteral("范围外")));
    }

    const QString selectedText = selection.hasTarget()
        ? QStringLiteral("#%1")
              .arg(selection.candidates.at(selection.selectedCandidateIndex).sourceIndex)
        : QStringLiteral("无");
    return QStringLiteral("[视觉选择] 候选数=%1 范围内=%2 [%3] 选中=%4 原因=%5")
        .arg(selection.candidates.size())
        .arg(insideCount)
        .arg(candidateParts.join(QStringLiteral("; ")))
        .arg(selectedText)
        .arg(selectionReasonText(selection.reason));
}
```

- [ ] **步骤6：让推理响应解析统一使用已测试的选择器**

在 `parseInferenceReply()` 中保留网络和 JSON 文档错误处理，使用以下代码替换 `object_count`、空数组和当前线性比较部分：

```cpp
    const QJsonArray objects = doc.object().value(QStringLiteral("objects")).toArray();
    const TargetSelection selection = selectTarget(objects);
    emit selectionLogMessage(formatTargetSelectionLog(selection));
    if (!selection.hasTarget()) {
        emit noObjectDetected();
        return;
    }

    const TargetCandidate &candidate =
        selection.candidates.at(selection.selectedCandidateIndex);
    const float cx = static_cast<float>(candidate.x);          // 原始视觉 X 偏移，单位 mm。
    const float cy = static_cast<float>(candidate.y);          // 原始视觉 Y 偏移，单位 mm。
    const float cz = static_cast<float>(candidate.depth);      // 补偿深度，单位 mm。
    const float angle = static_cast<float>(candidate.angle);   // 箱体角度，单位 deg。
    const float conf = static_cast<float>(candidate.confidence); // 仅记录，不新增置信度阈值。
    Q_UNUSED(conf)
```

后面的 `transformToMm()`、`rawCoordinatesReady`、寄存器转换和 `coordinatesReady` 保持原样。

在 `DeviceManager` 构造函数创建 `m_visionClient` 后加入：

```cpp
    connect(m_visionClient, &VisionHttpClient::selectionLogMessage,
            this, &DeviceManager::logMessage);
```

- [ ] **步骤7：运行选择器测试，确认处于绿灯状态**

运行：

```bash
cmake --build build-field-fixes --target vision_target_selection_tests -j2
ctest --test-dir build-field-fixes -R '^vision_target_selection_tests$' --output-on-failure
```

预期：构建成功，并且 `vision_target_selection_tests` 通过。

---

### Task 2：隔离单独阶段一配置并强制执行互斥

**文件：**
- 新建：`tests/test_standalone_pickup_contract.cpp`
- 修改：`tests/CMakeLists.txt`
- 修改：`src/huayanScheduler.h:58-125,333-390`
- 修改：`src/huayanScheduler.cpp:1600-1650`
- 修改：`src/devicemanager.h:45-91`
- 修改：`src/devicemanager.cpp:95-225`
- 修改：`src/linemanager.cpp:70-100`
- 修改：`src/lineorchestrator.cpp:55-80`
- 修改：`src/mainwindow.h:1-80,205-225`
- 修改：`src/mainwindow.cpp:1230-1275,1880-1920,2030-2170`

**接口：**
- 输入：`stationConfig(int)` 以及现有的 `HuayanScheduler::StationArmFunctions` 字段。
- 输出：`bool HuayanScheduler::isBusy() const` 和 `bool DeviceManager::startStandaloneStageOne(int stationId)`。

- [ ] **步骤1：添加会失败的单独测试隔离契约测试**

在 `tests/CMakeLists.txt` 末尾加入：

```cmake
add_executable(standalone_pickup_contract_tests
    test_standalone_pickup_contract.cpp
)

target_include_directories(standalone_pickup_contract_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(standalone_pickup_contract_tests PRIVATE
    Qt6::Core
)

add_test(NAME standalone_pickup_contract_tests
         COMMAND standalone_pickup_contract_tests)
```

创建 `tests/test_standalone_pickup_contract.cpp`：

```cpp
#include <QFile>
#include <QString>

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

QString readSource(const QString &relativePath)
{
    QFile file(QStringLiteral(PROJECT_SOURCE_DIR "/") + relativePath);
    requireTrue(file.open(QIODevice::ReadOnly | QIODevice::Text),
                qPrintable(QStringLiteral("必须能读取 %1").arg(relativePath)));
    return QString::fromUtf8(file.readAll());
}

} // namespace

int main()
{
    const QString schedulerHeader = readSource(QStringLiteral("src/huayanScheduler.h"));
    const QString schedulerSource = readSource(QStringLiteral("src/huayanScheduler.cpp"));
    const QString deviceHeader = readSource(QStringLiteral("src/devicemanager.h"));
    const QString deviceSource = readSource(QStringLiteral("src/devicemanager.cpp"));
    const QString lineManagerSource = readSource(QStringLiteral("src/linemanager.cpp"));
    const QString oldLineSource = readSource(QStringLiteral("src/lineorchestrator.cpp"));
    const QString mainHeader = readSource(QStringLiteral("src/mainwindow.h"));
    const QString mainSource = readSource(QStringLiteral("src/mainwindow.cpp"));
    const QString taskSource = readSource(QStringLiteral("src/taskexecutor.cpp"));

    requireTrue(schedulerHeader.contains(QStringLiteral("bool isBusy() const;")),
                "HuayanScheduler 必须公开只读忙碌判定");
    requireTrue(schedulerSource.contains(QStringLiteral("m_stage != Stage::None"))
                    && schedulerSource.contains(QStringLiteral("m_action != Action::None"))
                    && schedulerSource.contains(QStringLiteral("m_pendingCommand.kind != PendingCommandKind::None")),
                "忙碌判定必须覆盖阶段、独立动作和待下发命令");

    requireTrue(deviceHeader.contains(
                    QStringLiteral("bool startStandaloneStageOne(int stationId);")),
                "DeviceManager 必须提供带工位号的单独测试业务入口");
    requireTrue(deviceSource.contains(QStringLiteral("stationConfig(stationId)")),
                "单独测试必须从统一工位配置表加载配置");
    requireTrue(deviceSource.contains(QStringLiteral("stationFuncs.captureFunc = config->captureFunc"))
                    && deviceSource.contains(QStringLiteral("stationFuncs.afterGripMode = config->afterGripMode"))
                    && deviceSource.contains(QStringLiteral("stationFuncs.afterGripFunc = config->afterGripFunc"))
                    && deviceSource.contains(QStringLiteral("stationFuncs.grabZClearance = config->grabZClearance")),
                "单独测试必须显式注入工位拍照、夹后策略和 Z 余量");
    requireTrue(deviceSource.contains(QStringLiteral("setPreGripScanEnabled(false)")),
                "单独阶段一测试必须明确关闭扫码");
    requireTrue(taskSource.contains(QStringLiteral("setPreGripScanEnabled(true)")),
                "生产 TaskExecutor 必须继续明确开启扫码");

    requireTrue(lineManagerSource.contains(QStringLiteral("m_arm->isBusy()"))
                    && lineManagerSource.contains(QStringLiteral("机械臂正被单独测试占用")),
                "总调度必须在业务层拒绝占用中的机械臂");
    requireTrue(oldLineSource.contains(QStringLiteral("m_arm->isBusy()")),
                "兼容旧整线入口也必须拒绝占用中的机械臂");
    requireTrue(deviceSource.contains(QStringLiteral("LineSystemState::Idle"))
                    && deviceSource.contains(QStringLiteral("currentTask().taskId != 0")),
                "单独测试必须检查新总调度完全空闲");

    requireTrue(mainHeader.contains(QStringLiteral("QComboBox *m_huayanStationCombo")),
                "华研测试面板必须保存工位选择下拉框");
    requireTrue(mainSource.contains(QStringLiteral("for (int stationId = 1; stationId <= 12; ++stationId)"))
                    && mainSource.contains(QStringLiteral("m_huayanStationCombo->currentData().toInt()")),
                "UI 必须提供 1-12 工位并把所选编号传给业务层");
    requireTrue(mainSource.contains(QStringLiteral("updateStandalonePickupControls()")),
                "连接、阶段和整线状态变化必须集中刷新测试互斥控件");

    return 0;
}
```

- [ ] **步骤2：运行契约测试，确认处于红灯状态**

运行：

```bash
cmake --build build-field-fixes --target standalone_pickup_contract_tests -j2
```

预期：测试程序可以构建，但运行失败，因为新接口和源码契约尚不存在。

- [ ] **步骤3：公开带注释的华研只读忙碌状态**

在 `HuayanScheduler` 的 `public:` 公有区加入：

```cpp
    /// 是否有阶段、独立动作、待下发命令或已下发命令占用机械臂；只读接口用于入口互斥。
    bool isBusy() const;
```

在 `src/huayanScheduler.cpp` 的 `isConnected()` 附近实现：

```cpp
bool HuayanScheduler::isBusy() const
{
    return m_stage != Stage::None
        || m_action != Action::None
        || m_pendingCommand.kind != PendingCommandKind::None
        || hasActiveRobotCommand();
}
```

- [ ] **步骤4：增加隔离后的 DeviceManager 业务入口**

在 `DeviceManager` 的 `public slots:` 公有槽区加入：

```cpp
    /**
     * @brief 启动指定工位的手工阶段一测试。
     * @param stationId 现场工位号，允许 1～12。
     * @return 已完成互斥检查和配置注入并发起阶段一时返回 true；拒绝时返回 false 并记录原因。
     *
     * 该入口固定关闭扫码，只复用 HuayanScheduler 的既有阶段一状态机；总调度不得调用。
     */
    bool startStandaloneStageOne(int stationId);
```

在 `src/devicemanager.cpp` 增加完整实现：

```cpp
bool DeviceManager::startStandaloneStageOne(int stationId)
{
    const StationTaskConfig *config = stationConfig(stationId);
    if (!config) {
        emit logMessage(QStringLiteral("[华沿测试] 工位号无效或配置缺失：%1").arg(stationId));
        return false;
    }
    if (!m_huayanScheduler || !m_huayanScheduler->isConnected()) {
        emit logMessage(QStringLiteral("[华沿测试] 机械臂未连接，拒绝启动工位%1阶段一").arg(stationId));
        return false;
    }
    if (m_lineManager
        && (m_lineManager->state() != LineSystemState::Idle
            || m_lineManager->currentTask().taskId != 0)) {
        emit logMessage(QStringLiteral("[华沿测试] 总调度未完全空闲，拒绝启动工位%1阶段一").arg(stationId));
        return false;
    }
    if (m_lineOrch && m_lineOrch->isRunning()) {
        emit logMessage(QStringLiteral("[华沿测试] 兼容整线流程仍在运行，拒绝单独测试"));
        return false;
    }
    if (m_huayanScheduler->isBusy()) {
        emit logMessage(QStringLiteral("[华沿测试] 机械臂已有动作运行，拒绝并发启动"));
        return false;
    }

    HuayanScheduler::StationArmFunctions stationFuncs;
    stationFuncs.captureFunc = config->captureFunc;
    stationFuncs.afterGripMode = config->afterGripMode;
    stationFuncs.afterGripFunc = config->afterGripFunc;
    stationFuncs.grabZClearance = config->grabZClearance;
    stationFuncs.unloadPointFunc = config->unloadPointFunc;
    stationFuncs.unloadFunc = config->unloadFunc;
    m_huayanScheduler->setStationFunctions(stationFuncs);
    m_huayanScheduler->setPreGripScanEnabled(false);

    emit logMessage(QStringLiteral("[华沿测试] 工位%1阶段一配置已隔离注入：capture=%2，扫码=关闭")
                        .arg(stationId)
                        .arg(config->captureFunc));
    m_huayanScheduler->startStageOne();
    return true;
}
```

- [ ] **步骤5：让两个总调度入口都拒绝已被占用的机械臂和对方顶层流程**

在 `LineManager` 增加外部流程只读判定接口，用来接收兼容旧整线是否运行：

```cpp
    /// 注入兼容旧整线是否运行的只读判定；用于拒绝两个顶层调度同时控制 AGV/机械臂。
    void setExternalWorkflowRunning(std::function<bool()> predicate);
```

在 `LineManager::start()` 的 Error/非 Idle 检查之后、`setState(Running, ...)` 之前加入：

```cpp
    if (m_externalWorkflowRunning && m_externalWorkflowRunning()) {
        emit logMessage(QStringLiteral("[LineManager] Start 被拒绝：兼容整线流程正在运行"));
        return;
    }

    if (m_arm && m_arm->isBusy()) {
        emit logMessage(QStringLiteral("[LineManager] Start 被拒绝：机械臂正被单独测试占用"));
        return;
    }
```

在 `LineOrchestrator` 增加新总调度只读判定接口，并在 `start()` 开头替换原有的单行提前返回：

```cpp
    /// 注入新 LineManager 是否运行的只读判定；用于拒绝两个顶层调度同时控制 AGV/机械臂。
    void setExternalWorkflowRunning(std::function<bool()> predicate);

void LineOrchestrator::start()
{
    if (m_state != LineState::Idle)
        return;
    if (m_externalWorkflowRunning && m_externalWorkflowRunning()) {
        emit lineLog(QStringLiteral("[整线] 启动被拒绝：新总调度正在运行"));
        return;
    }
    if (m_arm && m_arm->isBusy()) {
        emit lineLog(QStringLiteral("[整线] 启动被拒绝：机械臂已有阶段或测试动作运行"));
        return;
    }
    emit lineStarted();
    emit lineLog(QStringLiteral("[整线] 流程启动，初始检查"));
    enterState(LineState::InitCheck);
}
```

在 `DeviceManager` 创建完 `m_lineManager` 和 `m_lineOrch` 后注入双向互斥谓词：

```cpp
    // 新旧两个顶层流程共用 AGV/机械臂；这里仅注入只读互斥判定，不转移对象所有权。
    m_lineManager->setExternalWorkflowRunning([this]() {
        return m_lineOrch && m_lineOrch->isRunning();
    });
    // 新 LineManager 在 Running/ReturningHome/Error 或保留当前任务时，都视为占用整线资源。
    m_lineOrch->setExternalWorkflowRunning([this]() {
        return m_lineManager
            && (m_lineManager->state() != LineSystemState::Idle
                || m_lineManager->currentTask().taskId != 0);
    });
```

- [ ] **步骤6：增加 1～12 号工位下拉框，并且只调用 DeviceManager**

在 `src/mainwindow.h` 的头文件引入区加入：

```cpp
#include <QComboBox>
```

在私有辅助函数区加入：

```cpp
    /// 根据机械臂连接/忙碌状态与整线状态，集中刷新手工阶段一和总调度入口的互斥状态。
    void updateStandalonePickupControls();
```

在华研面板成员区加入：

```cpp
    QComboBox *m_huayanStationCombo = nullptr; ///< 仅供手工阶段一测试选择 1～12 号工位。
```

在 `initHuayanPanel()` 的 `row2` 中创建并放到启动按钮之前：

```cpp
    m_huayanStationCombo = new QComboBox();
    m_huayanStationCombo->setToolTip(QStringLiteral("选择单独阶段一测试使用的工位配置；不影响总调度配置表"));
    for (int stationId = 1; stationId <= 12; ++stationId) {
        m_huayanStationCombo->addItem(QStringLiteral("工位%1").arg(stationId), stationId);
    }
    m_huayanStationCombo->setCurrentIndex(0);
    m_huayanStationCombo->setEnabled(false);
    row2->addWidget(m_huayanStationCombo);
```

用以下实现替换 `onHuayanStartStageOne()`：

```cpp
void MainWindow::onHuayanStartStageOne()
{
    const int stationId = m_huayanStationCombo->currentData().toInt();
    m_huayanStartBtn->setEnabled(false);
    m_huayanStationCombo->setEnabled(false);
    if (!m_devMgr->startStandaloneStageOne(stationId))
        updateStandalonePickupControls();
}
```

- [ ] **步骤7：集中刷新 UI 互斥状态**

在 `src/mainwindow.cpp` 中增加：

```cpp
void MainWindow::updateStandalonePickupControls()
{
    HuayanScheduler *arm = m_devMgr ? m_devMgr->huayanScheduler() : nullptr;
    LineManager *line = m_devMgr ? m_devMgr->lineManager() : nullptr;
    LineOrchestrator *legacyLine = m_devMgr ? m_devMgr->lineOrchestrator() : nullptr;
    const bool lineCompletelyIdle = line
        && line->state() == LineSystemState::Idle
        && line->currentTask().taskId == 0;
    const bool legacyIdle = !legacyLine || !legacyLine->isRunning();
    const bool armIdle = arm && !arm->isBusy();
    const bool standaloneEnabled = arm && arm->isConnected()
        && armIdle && lineCompletelyIdle && legacyIdle;

    m_huayanStartBtn->setEnabled(standaloneEnabled);
    m_huayanStationCombo->setEnabled(standaloneEnabled);

    // 总调度只在自身 Idle、兼容旧整线空闲且机械臂未被手工测试占用时允许启动。
    const bool lineStartEnabled = lineCompletelyIdle && armIdle && legacyIdle;
    m_btnStart->setEnabled(lineStartEnabled);
    if (m_lineStartBtn)
        m_lineStartBtn->setEnabled(lineStartEnabled);
}
```

在 `updateLineSystemState()`、`onHuayanConnected()`、`onHuayanDisconnected()`、`onHuayanStop()`、`onHuayanStageStarted()`、`onHuayanStageCompleted()` 和 `onHuayanStageError()` 的既有状态更新末尾调用 `updateStandalonePickupControls()`；删除这些函数中无条件 `m_huayanStartBtn->setEnabled(true)` 的赋值，避免生产阶段完成时误开测试入口。

- [ ] **步骤8：运行隔离测试和现有取料回归测试**

运行：

```bash
cmake --build build-field-fixes --target standalone_pickup_contract_tests station_pickup_config_tests huayan_scheduler_contract_tests -j2
ctest --test-dir build-field-fixes \
  -R '^(standalone_pickup_contract_tests|station_pickup_config_tests|huayan_scheduler_contract_tests)$' \
  --output-on-failure
```

预期：三个测试全部通过；`TaskExecutor` 仍包含 `setPreGripScanEnabled(true)`，单独测试路径包含 `setPreGripScanEnabled(false)`。

---

### Task 3：把所有 AGV 导航等待延长到 5 分钟，同时保持明确失败立即处理

**文件：**
- 新建：`tests/test_navigation_timeout_contract.cpp`
- 修改：`tests/CMakeLists.txt`
- 修改：`src/taskexecutor.h:1-105,140-180`
- 修改：`src/taskexecutor.cpp:198-238,445-460,675-690`
- 修改：`src/linemanager.h:1-100`
- 修改：`src/linemanager.cpp:270-310,365-390`
- 修改：`src/lineorchestrator.h:1-105`
- 修改：`src/lineorchestrator.cpp:20-45,90-155`

**接口：**
- 输入：现有 `QTimer` 超时回调和 `AgvController::NavStatus`。
- 输出：三处 `300000 ms` 上限和包含目标、实际等待、配置上限的超时错误文本。

- [ ] **步骤1：添加会失败的导航超时契约测试**

在 `tests/CMakeLists.txt` 末尾加入：

```cmake
add_executable(navigation_timeout_contract_tests
    test_navigation_timeout_contract.cpp
)

target_include_directories(navigation_timeout_contract_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(navigation_timeout_contract_tests PRIVATE
    Qt6::Core
)

add_test(NAME navigation_timeout_contract_tests
         COMMAND navigation_timeout_contract_tests)
```

创建 `tests/test_navigation_timeout_contract.cpp`：

```cpp
#include <QFile>
#include <QString>

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

QString readSource(const QString &relativePath)
{
    QFile file(QStringLiteral(PROJECT_SOURCE_DIR "/") + relativePath);
    requireTrue(file.open(QIODevice::ReadOnly | QIODevice::Text),
                qPrintable(QStringLiteral("必须能读取 %1").arg(relativePath)));
    return QString::fromUtf8(file.readAll());
}

} // namespace

int main()
{
    const QString taskHeader = readSource(QStringLiteral("src/taskexecutor.h"));
    const QString taskSource = readSource(QStringLiteral("src/taskexecutor.cpp"));
    const QString lineHeader = readSource(QStringLiteral("src/linemanager.h"));
    const QString lineSource = readSource(QStringLiteral("src/linemanager.cpp"));
    const QString oldHeader = readSource(QStringLiteral("src/lineorchestrator.h"));
    const QString oldSource = readSource(QStringLiteral("src/lineorchestrator.cpp"));

    requireTrue(taskHeader.contains(QStringLiteral("kAgvTimeoutMs = 300000")),
                "任务导航上限必须是 300000ms");
    requireTrue(lineHeader.contains(QStringLiteral("kReturnHomeTimeoutMs = 300000")),
                "LM1 返航上限必须是 300000ms");
    requireTrue(oldHeader.contains(QStringLiteral("kAgvTimeoutMs = 300000")),
                "兼容整线上限必须是 300000ms");

    requireTrue(taskHeader.contains(QStringLiteral("QElapsedTimer m_agvNavigationElapsed"))
                    && lineHeader.contains(QStringLiteral("QElapsedTimer m_returnHomeElapsed"))
                    && oldHeader.contains(QStringLiteral("QElapsedTimer m_agvElapsed")),
                "三条导航链路必须记录实际等待时间");
    requireTrue(taskSource.contains(QStringLiteral("已等待 %2 ms（上限 %3 ms）"))
                    && lineSource.contains(QStringLiteral("已等待 %1 ms（上限 %2 ms）"))
                    && oldSource.contains(QStringLiteral("已等待 %2 ms（上限 %3 ms）")),
                "三处上位机超时日志必须包含实际等待和配置上限");

    requireTrue(taskSource.contains(QStringLiteral("NavStatus::Failed"))
                    && taskSource.contains(QStringLiteral("NavStatus::Timeout")),
                "任务导航必须继续立即处理明确失败/取消/超时状态");
    requireTrue(lineSource.contains(QStringLiteral("NavStatus::Failed"))
                    && lineSource.contains(QStringLiteral("NavStatus::Canceled"))
                    && lineSource.contains(QStringLiteral("NavStatus::Timeout")),
                "LM1 返航必须继续立即处理明确失败状态");

    return 0;
}
```

- [ ] **步骤2：运行契约测试，确认处于红灯状态**

运行：

```bash
cmake --build build-field-fixes --target navigation_timeout_contract_tests -j2
ctest --test-dir build-field-fixes -R '^navigation_timeout_contract_tests$' --output-on-failure
```

预期：测试失败，因为当前常量仍为 120000，并且尚未增加实际耗时计时器。

- [ ] **步骤3：把 TaskExecutor 导航改为 300000 ms，并记录实际耗时**

在 `src/taskexecutor.h` 加入 `#include <QElapsedTimer>`，并修改/新增成员：

```cpp
    static constexpr int kAgvTimeoutMs = 300000; ///< 每个 AGV 导航步骤上限，单位 ms（5 分钟）。
    QElapsedTimer m_agvNavigationElapsed; ///< 当前任务导航的单调时钟；仅用于超时诊断，不参与状态推进。
```

在 `startAgvStep()` 中，启动 QTimer 前加入：

```cpp
    m_agvNavigationElapsed.start();
    m_agvTimeout->start(kAgvTimeoutMs);
```

到达分支停止 `m_agvTimeout` 后加入：

```cpp
        m_agvNavigationElapsed.invalidate();
```

用以下实现替换 `onAgvTimeout()`：

```cpp
void TaskExecutor::onAgvTimeout()
{
    if (!isBusy())
        return;

    const qint64 waitedMs = m_agvNavigationElapsed.isValid()
        ? m_agvNavigationElapsed.elapsed()
        : kAgvTimeoutMs;
    raiseSystemError(QStringLiteral("AGV 导航到 LM%1 超时：已等待 %2 ms（上限 %3 ms）")
                         .arg(m_expectedLm)
                         .arg(waitedMs)
                         .arg(kAgvTimeoutMs));
}
```

在 `resetRuntimeState()` 的定时器停止之后加入：

```cpp
    m_agvNavigationElapsed.invalidate();
```

- [ ] **步骤4：修改 LineManager 返航超时和实际耗时日志**

在 `src/linemanager.h` 加入 `#include <QElapsedTimer>`，并修改/新增成员：

```cpp
    static constexpr int kReturnHomeTimeoutMs = 300000; ///< 回 LM1 超时，单位 ms（5 分钟）。
    QElapsedTimer m_returnHomeElapsed; ///< 本轮独立返航的实际等待计时，只用于超时日志。
```

在 `returnHomeIfNeeded()` 启动 QTimer 前加入：

```cpp
    m_returnHomeElapsed.start();
    m_returnHomeTimeout->start(kReturnHomeTimeoutMs);
```

用以下实现替换 `onReturnHomeTimeout()`：

```cpp
void LineManager::onReturnHomeTimeout()
{
    if (!m_returnHomeActive)
        return;

    const qint64 waitedMs = m_returnHomeElapsed.isValid()
        ? m_returnHomeElapsed.elapsed()
        : kReturnHomeTimeoutMs;
    enterError(QStringLiteral("AGV 回 LM1 超时：已等待 %1 ms（上限 %2 ms）")
                   .arg(waitedMs)
                   .arg(kReturnHomeTimeoutMs));
}
```

在 `stopReturnHomeTracking()` 末尾加入：

```cpp
    m_returnHomeElapsed.invalidate();
```

- [ ] **步骤5：同步修改兼容保留的 LineOrchestrator 超时**

在 `src/lineorchestrator.h` 加入 `#include <QElapsedTimer>`，并修改或新增成员：

```cpp
    QElapsedTimer m_agvElapsed; ///< 兼容整线当前 AGV 步骤的实际等待计时，仅用于诊断。
    static constexpr int kAgvTimeoutMs = 300000; ///< 兼容整线单步 AGV 上限，单位 ms（5 分钟）。
```

用构造函数中的超时匿名函数替换旧秒级日志：

```cpp
    connect(m_agvTimeout, &QTimer::timeout, this, [this]() {
        const qint64 waitedMs = m_agvElapsed.isValid()
            ? m_agvElapsed.elapsed()
            : kAgvTimeoutMs;
        abort(QStringLiteral("AGV 导航到站%1超时：已等待 %2 ms（上限 %3 ms）")
                  .arg(m_expectedStation)
                  .arg(waitedMs)
                  .arg(kAgvTimeoutMs));
    });
```

在 `LineState::AgvToPickup`、`AgvToUnload`、`AgvReturnHome` 三个分支的 `m_agvTimeout->start()` 前各加入：

```cpp
        m_agvElapsed.start(); // 新导航步骤重新计时，单位 ms。
```

在 `haltDevices()` 停止 QTimer 后加入：

```cpp
    m_agvElapsed.invalidate();
```

- [ ] **步骤6：运行超时测试和源码回归测试**

运行：

```bash
cmake --build build-field-fixes --target navigation_timeout_contract_tests task_executor_pallet_commit_semantics_tests -j2
ctest --test-dir build-field-fixes \
  -R '^(navigation_timeout_contract_tests|task_executor_pallet_commit_semantics_tests)$' \
  --output-on-failure
```

预期：两个测试都通过。复核明确导航状态分支，确认它们仍在 `QTimer` 到期前直接调用现有错误处理。

---

### Task 4：仅在 SDK 拒绝命令时采集紧凑的 MoveRelL 诊断

**文件：**
- 修改：`tests/test_huayan_scheduler_contract.cpp:20-145`
- 修改：`src/huayanScheduler.h:1-12,285-390`
- 修改：`src/huayanScheduler.cpp:1620-1845`

**接口：**
- 输入：`HRIF_ReadActTcpPos`、`HRIF_ReadCmdTcpPos`、`HRIF_ReadActJointPos`、`HRIF_ReadRobotFlags`、`HRIF_ReadCurFSM`、`HRIF_ReadAxisErrorCode`。
- 输出：`MotionDiagnosticSnapshot readMotionDiagnosticSnapshot(bool, bool) const` 和 `emitMoveRelFailureDiagnostics(...)`；两者都不改变运动决策。

- [ ] **步骤1：增加会失败的命令关联诊断契约断言**

在 `tests/test_huayan_scheduler_contract.cpp` 读取 `header`、`source` 后、`return 0` 前加入：

```cpp
    requireTrue(header.contains(QStringLiteral("struct MotionDiagnosticSnapshot")),
                "HuayanScheduler 必须定义 MoveRelL 失败诊断快照");
    requireTrue(header.contains(QStringLiteral("quint64 diagnosticCommandId")),
                "待下发命令必须携带现场诊断命令序号");
    requireTrue(source.contains(QStringLiteral("HRIF_ReadCmdTcpPos"))
                    && source.contains(QStringLiteral("HRIF_ReadActJointPos"))
                    && source.contains(QStringLiteral("HRIF_ReadAxisErrorCode")),
                "失败快照必须读取指令 TCP、实际关节角和轴错误码");
    requireTrue(source.contains(QStringLiteral("[华沿][MoveRelL诊断][命令=%1]")),
                "MoveRelL 诊断日志必须带本地命令序号");

    const qsizetype moveCall = source.indexOf(QStringLiteral("const int nRet = HRIF_MoveRelL"));
    const qsizetype failureBranch = source.indexOf(QStringLiteral("if (nRet != 0)"), moveCall);
    const qsizetype diagnostics = source.indexOf(
        QStringLiteral("emitMoveRelFailureDiagnostics"), failureBranch);
    const qsizetype successState = source.indexOf(
        QStringLiteral("m_activeCommandKind = cmd.kind"), failureBranch);
    requireTrue(moveCall >= 0 && failureBranch > moveCall
                    && diagnostics > failureBranch && diagnostics < successState,
                "诊断输出必须只位于 MoveRelL SDK 非零返回分支，成功路径不得打印快照");
    requireTrue(source.contains(QStringLiteral("readMotionDiagnosticSnapshot(true, false)"))
                    && source.contains(QStringLiteral("readMotionDiagnosticSnapshot(false, true)")),
                "SDK 调用前必须保存位姿/关节，失败后必须读取控制器/轴错误状态");
```

- [ ] **步骤2：运行现有华研契约测试，确认处于红灯状态**

运行：

```bash
cmake --build build-field-fixes --target huayan_scheduler_contract_tests -j2
ctest --test-dir build-field-fixes -R '^huayan_scheduler_contract_tests$' --output-on-failure
```

预期：测试在第一个新增诊断契约断言处失败。

- [ ] **步骤3：扩展带注释的命令和状态快照类型**

在 `src/huayanScheduler.h` 加入：

```cpp
#include <array>
```

给 `PendingCommand` 增加：

```cpp
        quint64 diagnosticCommandId = 0; ///< 本地单调命令序号，仅用于关联 SDK 返回码和失败快照。
```

扩展现有 `RobotStateSnapshot`，保留原字段并增加读取返回码：

```cpp
    struct RobotStateSnapshot {
        int movingState = 0; ///< 机器人运动标志。
        int pauseState = 0;  ///< 机器人暂停标志。
        int errorState = 0;  ///< 控制器错误标志。
        int errorCode = 0;   ///< 控制器主错误码。
        int nCurFSM = 0;     ///< 当前 FSM 数字状态。
        QString strCurFSM;   ///< 当前 FSM 厂家文本。
        int flagsRet = -1;   ///< HRIF_ReadRobotFlags 返回码；0 表示读取成功。
        int fsmRet = -1;     ///< HRIF_ReadCurFSM 返回码；0 表示读取成功。
        bool valid = false;  ///< flagsRet 和 fsmRet 均为 0 时为 true。
    };

    /// MoveRelL 失败现场快照；按读取开关填充，所有 ret 字段用于区分“值为0”和“读取失败”。
    struct MotionDiagnosticSnapshot {
        Pose actualTcp;                    ///< 实际 TCP 位姿，平移 mm、旋转 deg。
        Pose commandedTcp;                 ///< 控制器指令 TCP 位姿，平移 mm、旋转 deg。
        std::array<double, 6> joints{};     ///< J1～J6 实际关节角，单位 deg。
        RobotStateSnapshot robotState;     ///< flags/FSM 快照。
        int axisErrorCode = 0;             ///< HRIF_ReadAxisErrorCode 返回的总轴错误码。
        std::array<int, 6> axisErrors{};    ///< J1～J6 各轴错误码。
        int actualTcpRet = -1;             ///< HRIF_ReadActTcpPos 返回码；-1 表示本次未读取。
        int commandedTcpRet = -1;          ///< HRIF_ReadCmdTcpPos 返回码；-1 表示本次未读取。
        int jointsRet = -1;                ///< HRIF_ReadActJointPos 返回码；-1 表示本次未读取。
        int axisErrorsRet = -1;             ///< HRIF_ReadAxisErrorCode 返回码；-1 表示本次未读取。
    };
```

在私有方法区加入：

```cpp
    /// 按开关读取 MoveRelL 前/后快照；读取失败只记录 ret，不改变原运动错误。
    MotionDiagnosticSnapshot readMotionDiagnosticSnapshot(bool readPoseAndJoints,
                                                           bool readAxisErrors) const;
    /// 每个 SDK 拒绝命令调用一次，集中输出不超过四行的命令、位姿、关节和错误摘要。
    void emitMoveRelFailureDiagnostics(const PendingCommand &cmd,
                                       const MotionDiagnosticSnapshot &before,
                                       const MotionDiagnosticSnapshot &after,
                                       int sdkReturnCode);
```

- [ ] **步骤4：保留 SDK 读取返回码并实现快照读取**

在 `readRobotStateSnapshot()` 中把局部返回码写入快照：

```cpp
    snapshot.flagsRet = HRIF_ReadRobotFlags(m_boxID, m_rbtID,
                                             snapshot.movingState,
                                             nEnableState,
                                             snapshot.errorState,
                                             snapshot.errorCode,
                                             nErrorAxis,
                                             nBreaking,
                                             snapshot.pauseState,
                                             nBlendingDone);
    string fsmText;
    snapshot.fsmRet = HRIF_ReadCurFSM(m_boxID, m_rbtID,
                                      snapshot.nCurFSM, fsmText);
    snapshot.strCurFSM = snapshot.fsmRet == 0
        ? QString::fromStdString(fsmText)
        : QStringLiteral("未知");
    snapshot.valid = snapshot.flagsRet == 0 && snapshot.fsmRet == 0;
```

并将 `formatRobotStateSnapshot()` 的格式改为：

```cpp
    return QStringLiteral("运动=%1 暂停=%2 错误=%3 主错误码=%4 状态机=%5/%6 标志读取返回码=%7 状态机读取返回码=%8 有效=%9")
        .arg(snapshot.movingState)
        .arg(snapshot.pauseState)
        .arg(snapshot.errorState)
        .arg(snapshot.errorCode)
        .arg(snapshot.nCurFSM)
        .arg(snapshot.strCurFSM)
        .arg(snapshot.flagsRet)
        .arg(snapshot.fsmRet)
        .arg(snapshot.valid ? QStringLiteral("是") : QStringLiteral("否"));
```

增加快照读取实现：

```cpp
HuayanScheduler::MotionDiagnosticSnapshot
HuayanScheduler::readMotionDiagnosticSnapshot(bool readPoseAndJoints,
                                               bool readAxisErrors) const
{
    MotionDiagnosticSnapshot snapshot;
    snapshot.robotState = readRobotStateSnapshot();

    if (readPoseAndJoints) {
        snapshot.actualTcpRet = HRIF_ReadActTcpPos(
            m_boxID, m_rbtID,
            snapshot.actualTcp.x, snapshot.actualTcp.y, snapshot.actualTcp.z,
            snapshot.actualTcp.rx, snapshot.actualTcp.ry, snapshot.actualTcp.rz);
        snapshot.commandedTcpRet = HRIF_ReadCmdTcpPos(
            m_boxID, m_rbtID,
            snapshot.commandedTcp.x, snapshot.commandedTcp.y, snapshot.commandedTcp.z,
            snapshot.commandedTcp.rx, snapshot.commandedTcp.ry, snapshot.commandedTcp.rz);
        snapshot.jointsRet = HRIF_ReadActJointPos(
            m_boxID, m_rbtID,
            snapshot.joints[0], snapshot.joints[1], snapshot.joints[2],
            snapshot.joints[3], snapshot.joints[4], snapshot.joints[5]);
    }

    if (readAxisErrors) {
        snapshot.axisErrorsRet = HRIF_ReadAxisErrorCode(
            m_boxID, m_rbtID, snapshot.axisErrorCode,
            snapshot.axisErrors[0], snapshot.axisErrors[1], snapshot.axisErrors[2],
            snapshot.axisErrors[3], snapshot.axisErrors[4], snapshot.axisErrors[5]);
    }
    return snapshot;
}
```

- [ ] **步骤5：实现一个紧凑的四行失败诊断块**

在 `src/huayanScheduler.cpp` 增加：

```cpp
void HuayanScheduler::emitMoveRelFailureDiagnostics(
    const PendingCommand &cmd,
    const MotionDiagnosticSnapshot &before,
    const MotionDiagnosticSnapshot &after,
    int sdkReturnCode)
{
    static const QString kAxisNames[] = {
        QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z"),
        QStringLiteral("Rx"), QStringLiteral("Ry"), QStringLiteral("Rz")
    };
    const QString axis = cmd.poseId >= 0 && cmd.poseId < 6
        ? kAxisNames[cmd.poseId] : QString::number(cmd.poseId);
    const QString unit = cmd.poseId >= 0 && cmd.poseId < 3
        ? QStringLiteral("mm") : QStringLiteral("deg");
    const QString frame = cmd.kind == PendingCommandKind::MoveRelTool
        ? QStringLiteral("TCP/工具系") : QStringLiteral("UCS/基坐标系");
    const QString detail = describeError(m_boxID, sdkReturnCode);

    emit logMessage(QStringLiteral("[华沿][MoveRelL诊断][命令=%1] SDK拒绝：标签=%2 轴=%3 方向=%4 距离=%5%6 坐标系=%7 返回码=%8（%9）")
                        .arg(cmd.diagnosticCommandId)
                        .arg(cmd.label)
                        .arg(axis)
                        .arg(cmd.direction == 1 ? QStringLiteral("正向")
                                                : QStringLiteral("负向"))
                        .arg(cmd.distance, 0, 'f', 3)
                        .arg(unit)
                        .arg(frame)
                        .arg(sdkReturnCode)
                        .arg(detail.isEmpty() ? QStringLiteral("未知") : detail));
    emit logMessage(QStringLiteral("[华沿][MoveRelL诊断][命令=%1] 命令前TCP 实际=(%2,%3,%4,%5,%6,%7) 返回码=%8 指令=(%9,%10,%11,%12,%13,%14) 返回码=%15")
                        .arg(cmd.diagnosticCommandId)
                        .arg(before.actualTcp.x, 0, 'f', 3).arg(before.actualTcp.y, 0, 'f', 3)
                        .arg(before.actualTcp.z, 0, 'f', 3).arg(before.actualTcp.rx, 0, 'f', 3)
                        .arg(before.actualTcp.ry, 0, 'f', 3).arg(before.actualTcp.rz, 0, 'f', 3)
                        .arg(before.actualTcpRet)
                        .arg(before.commandedTcp.x, 0, 'f', 3).arg(before.commandedTcp.y, 0, 'f', 3)
                        .arg(before.commandedTcp.z, 0, 'f', 3).arg(before.commandedTcp.rx, 0, 'f', 3)
                        .arg(before.commandedTcp.ry, 0, 'f', 3).arg(before.commandedTcp.rz, 0, 'f', 3)
                        .arg(before.commandedTcpRet));
    emit logMessage(QStringLiteral("[华沿][MoveRelL诊断][命令=%1] 命令前关节=(%2,%3,%4,%5,%6,%7) 返回码=%8 状态={%9}")
                        .arg(cmd.diagnosticCommandId)
                        .arg(before.joints[0], 0, 'f', 3).arg(before.joints[1], 0, 'f', 3)
                        .arg(before.joints[2], 0, 'f', 3).arg(before.joints[3], 0, 'f', 3)
                        .arg(before.joints[4], 0, 'f', 3).arg(before.joints[5], 0, 'f', 3)
                        .arg(before.jointsRet)
                        .arg(formatRobotStateSnapshot(before.robotState)));
    emit logMessage(QStringLiteral("[华沿][MoveRelL诊断][命令=%1] 命令后状态={%2} 轴读取返回码=%3 轴总错误=%4 各轴错误=(%5,%6,%7,%8,%9,%10)")
                        .arg(cmd.diagnosticCommandId)
                        .arg(formatRobotStateSnapshot(after.robotState))
                        .arg(after.axisErrorsRet)
                        .arg(after.axisErrorCode)
                        .arg(after.axisErrors[0]).arg(after.axisErrors[1]).arg(after.axisErrors[2])
                        .arg(after.axisErrors[3]).arg(after.axisErrors[4]).arg(after.axisErrors[5]));
}
```

所有读取返回码都出现在固定四行中；读取失败不额外重试或刷行，且原始 `sdkReturnCode` 仍交给既有 `emitOperationError()`。

- [ ] **步骤6：关联命令编号，并且只在失败分支调用诊断**

在 `beginCommandWhenReady()` 中用以下三行替换当前命令序号和赋值：

```cpp
    ++m_commandSeq;
    m_pendingCommand = cmd;
    m_pendingCommand.diagnosticCommandId = m_commandSeq;
```

在 `dispatchReadyCommand()` 的 MoveRelTool/MoveRelBase 分支中替换 SDK 调用和错误分支：

```cpp
        const int toolMotion = cmd.kind == PendingCommandKind::MoveRelTool ? 1 : 0;
        const MotionDiagnosticSnapshot before =
            readMotionDiagnosticSnapshot(true, false);
        const int nRet = HRIF_MoveRelL(m_boxID, m_rbtID,
                                      cmd.poseId, cmd.direction,
                                      cmd.distance, toolMotion);
        if (nRet != 0) {
            const MotionDiagnosticSnapshot after =
                readMotionDiagnosticSnapshot(false, true);
            emitMoveRelFailureDiagnostics(cmd, before, after, nRet);
            const QString detail = describeError(m_boxID, nRet);
            emitOperationError(detail.isEmpty()
                ? QStringLiteral("%1失败：%2").arg(cmd.label).arg(nRet)
                : QStringLiteral("%1失败：%2（%3）").arg(cmd.label).arg(nRet).arg(detail));
            return false;
        }
```

保留该分支后面的 `m_activeCommandKind`、`m_activeCommandLabel`、`startWaitForIdle()` 成功路径，不添加任何成功快照日志，也不添加距离阈值。

- [ ] **步骤7：运行华研回归测试**

运行：

```bash
cmake --build build-field-fixes --target huayan_scheduler_contract_tests station_pickup_config_tests -j2
ctest --test-dir build-field-fixes \
  -R '^(huayan_scheduler_contract_tests|station_pickup_config_tests)$' \
  --output-on-failure
```

预期：两个测试都通过。源码复核确认快照日志只出现在 `if (nRet != 0)` 之后、现有错误返回之前。

---

### Task 5：同步文档、执行完整验证并创建一次本地提交

**文件：**
- 修改：`docs/superpowers/specs/2026-07-16-field-fault-diagnostics-and-workflow-isolation-design.md`
- 修改：仅当实际验证过的名称与本计划不一致时，修改 `docs/superpowers/plans/2026-07-16-field-fault-diagnostics-and-workflow-isolation.md`。
- 验证：上文列出的全部源码和测试文件。

**接口：**
- 输入：任务1～4已经完成的实现和测试输出。
- 输出：同步后的设计/计划、完整 Qt 构建证据和一次本地功能提交，不执行推送。

- [ ] **步骤1：使用实际接口名称和验证状态更新设计文档**

在设计文档顶部把状态改为：

```markdown
**状态：** 已实施并完成自动化验证；49601 根因仍等待现场失败日志确认
```

在设计文档测试章节末尾追加实际结果，使用执行当日的真实输出替换示例中的计数，不得写未运行的结果：

```markdown
### 自动化验证结果

- Qt 配置：`build-field-fixes` 配置成功。
- 定点测试：视觉选择、测试隔离、导航超时、华研调度契约全部通过。
- 全量 CTest：以实际 `N/N tests passed` 输出为准。
- 应用构建：`wh-robot-visual` 构建成功。
- 现场待验证：复现 49601 后检查四行同命令号诊断；AGV 遮挡 120～300 秒恢复；12 工位手工阶段一不扫码；相邻工位多目标选择。
```

- [ ] **步骤2：扫描代码和计划注释，检查用户要求的注释完整性**

运行：

```bash
rg -n "enum class|startStandaloneStageOne|isBusy\(\) const|VISION_STATION_ROI|TargetCandidate|TargetSelection|MotionDiagnosticSnapshot|QElapsedTimer" \
  src tests docs/superpowers/plans/2026-07-16-field-fault-diagnostics-and-workflow-isolation.md
```

预期：每个新增枚举、接口、宏和变量旁都有说明语义及单位的注释。如果发现缺失，先补充准确注释，再继续验证。

- [ ] **步骤3：执行空白、占位文本和改动范围检查**

运行：

```bash
git diff --check
rg -n "TB[D]|TO[D]O|implement[ ]later|fill[ ]in[ ]details|适当处[理]|类似[ ]Task" \
  docs/superpowers/plans/2026-07-16-field-fault-diagnostics-and-workflow-isolation.md \
  src tests
git diff --stat
git status --short
```

预期：`git diff --check` 无输出；占位文本扫描没有发现计划中的新增占位内容；无关状态仍只有三个用户所有的 `test-*.json*` 文件。

- [ ] **步骤4：构建并运行定点测试**

运行：

```bash
cmake --build build-field-fixes --target \
  vision_target_selection_tests \
  standalone_pickup_contract_tests \
  navigation_timeout_contract_tests \
  huayan_scheduler_contract_tests \
  station_pickup_config_tests \
  task_executor_pallet_commit_semantics_tests \
  -j2
ctest --test-dir build-field-fixes \
  -R '^(vision_target_selection_tests|standalone_pickup_contract_tests|navigation_timeout_contract_tests|huayan_scheduler_contract_tests|station_pickup_config_tests|task_executor_pallet_commit_semantics_tests)$' \
  --output-on-failure
```

预期：六个定点测试全部通过。

- [ ] **步骤5：构建应用并运行完整测试集**

运行：

```bash
cmake --build build-field-fixes --target wh-robot-visual -j2
ctest --test-dir build-field-fixes --output-on-failure
```

预期：`wh-robot-visual` 构建成功，全部已配置测试通过。在设计文档中记录准确的测试数量。

- [ ] **步骤6：复核最终差异，排除禁止的行为变化**

运行：

```bash
git diff -- src/huayanScheduler.cpp src/taskexecutor.cpp src/linemanager.cpp \
  src/lineorchestrator.cpp src/visionclient.cpp src/devicemanager.cpp src/mainwindow.cpp
rg -n "350\.0|381|404" src tests
```

预期：

- 不存在新增的 350 mm 机械臂运动拒绝逻辑；
- `HRIF_MoveRelL` 的轴、方向、距离和 `toolMotion` 语义没有变化；
- 生产 `TaskExecutor` 仍注入完整工位/码垛配置并启用扫码；
- 单独测试路径关闭扫码并调用同一个 `startStageOne()`；
- 视觉 ±500 宏只在手眼转换之前使用；
- 明确的 AGV 失败分支仍然立即处理。

- [ ] **步骤7：只暂存本任务文件并创建唯一一次功能提交**

运行：

```bash
git add \
  src/visionclient.h src/visionclient.cpp \
  src/devicemanager.h src/devicemanager.cpp \
  src/huayanScheduler.h src/huayanScheduler.cpp \
  src/taskexecutor.h src/taskexecutor.cpp \
  src/linemanager.h src/linemanager.cpp \
  src/lineorchestrator.h src/lineorchestrator.cpp \
  src/mainwindow.h src/mainwindow.cpp \
  tests/CMakeLists.txt \
  tests/test_vision_target_selection.cpp \
  tests/test_standalone_pickup_contract.cpp \
  tests/test_navigation_timeout_contract.cpp \
  tests/test_huayan_scheduler_contract.cpp \
  docs/superpowers/specs/2026-07-16-field-fault-diagnostics-and-workflow-isolation-design.md \
  docs/superpowers/plans/2026-07-16-field-fault-diagnostics-and-workflow-isolation.md
git diff --cached --check
git diff --cached --stat
git commit -m "fix: 增强现场诊断并隔离取料测试流程"
```

预期：只创建一个新的实现提交；三个 `test-*.json*` 文件保持未跟踪且未暂存。

- [ ] **步骤8：检查本地交付状态，不执行推送**

运行：

```bash
git status --short
git log -2 --oneline
```

预期：没有本任务相关的未暂存改动；只允许显示三个原有未跟踪 JSON 文件。不要执行 `git push`。

现场复测后，把新日志中同一 `[命令=N]` 的四行 49601 诊断提供给后续分析；在获得该证据前，不修改控制器安全空间或机械臂正常运动距离。
