# 空箱码垛真实单步调试 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将空箱码垛从视觉/模拟方案调整为人工确认、配置页真实单步机械臂调试，并让配置页与主流程复用同一套码垛动作链。

**Architecture:** `PalletScheduler` 继续只负责配置、容量、目标层点位和状态缓存；新增纯动作规划 helper 表达码垛动作顺序，方便测试。`HuayanScheduler` 执行统一标准码垛动作链，配置页和 `TaskExecutor` 都调用同一接口，成功后才由调用方 `commitPlaced()`。`PalletParamDialog` 去掉视觉检测和直接推进缓存的模拟放置按钮，保留人工修正、人工清零和不改缓存的点位预览，并提供带关闭保护的“执行一次码垛”真实调试入口。

**Tech Stack:** C++17、Qt 6.8.3 Widgets/Core/Network、CMake/CTest、QSettings、现有华研 SDK 封装。

## Global Constraints

- 文档、代码注释、函数说明、接口说明、变量说明必须使用中文解释关键业务语义。
- 如果实现过程中新增功能或调整行为，必须同步更新 `docs/superpowers/specs/2026-07-09-empty-box-pallet-real-debug-design.md` 和本计划。
- 不要频繁提交；本轮实现先不提交，等现场验证功能正确后再由人工确认做一次功能提交。
- 测试 C++ 文件必须放在 `robot_visual20260625/robot_visual/tests/`，不能和 `src/` 混在一起。
- CMake 必须保持 Qt Creator 中 `tests` 目录独立可见。
- Qt CMake 路径固定为 `Qt6_DIR=/opt/Qt/qt6.8/6.8.3/gcc_64/lib/cmake/Qt6`。
- 配置工具固定为 `/opt/Qt/qt6.8/6.8.3/gcc_64/bin/qt-cmake`。
- 配置页不新增停止按钮；停止统一使用已有华研面板停止按钮。
- 配置页不做连续循环；每次点击“执行一次码垛”只执行一个空箱。
- 任何一步失败、人工停止、华研面板停止，都不能调用 `PalletScheduler::commitPlaced()`。
- 满载后必须提示人工搬运并清零，不能继续下发机械臂动作。

## 当前实现同步结论

- `PalletParamDialog` 当前构造函数为 `PalletParamDialog(PalletScheduler *scheduler, HuayanScheduler *arm, QWidget *parent = nullptr)`，不再依赖 `VisionHttpClient`。
- 配置页真实单步调试期间没有独立停止按钮；停止统一依赖华研面板，窗口通过 `HuayanScheduler::schedulerStopped` 做停止收尾，并在 `closeEvent` 中阻止执行中关闭。
- `HuayanScheduler::startPalletPlace()` 当前签名为 `startPalletPlace(const PalletPose &targetOffset, double releaseZOffsetMm, double robotBaseHeightFromGroundMm)`；`releaseZOffset` 语义是“目标层上方释放高度”，`robotBaseHeightFromGroundMm` 是机器人基座原点离地高度，默认先按实测 850mm 配置并允许现场补偿。
- 标准主流程成功路径保持 `PreparePalletPoint -> ArmPalletPlace -> CommitPallet -> task success`，未恢复旧的视觉占用检测或自动清零。
- 车载机械臂 Z 坐标已修正：`nextRelativeOffset().z` 表示目标层表面离地高度；`palletBaseFunc` 到位后通过 `HRIF_ReadActTcpPos()` 读取当前 TCP Z；真实 Z 相对移动量按 `(目标层表面离地高度 + releaseZOffset) - robotBaseHeightFromGround + PALLET_GRIPPER_RELEASE_Z_OFFSET_MM - 当前TCP_Z` 计算，允许为负值。当前夹爪释放补偿宏 `PALLET_GRIPPER_RELEASE_Z_OFFSET_MM=420.0`。
- 2026-07-09 现场新问题：空垛低层下降高度看起来基本合理，但到第 5 层附近机械臂 Z 值异常偏高，接近奇异点并导致动作挂起。当前不继续修复代码，先记录问题，明日围绕层号、释放高度、TCP/夹爪补偿方向、实际 TCP Z 日志重新头脑风暴。
- 本计划下方早期任务片段保留为实施记录；接口签名和 Z 坐标语义以“当前实现同步结论”和最新 spec 为准。

---

## 文件结构

### 新增文件

| 文件 | 职责 |
|------|------|
| `src/palletplacesequence.h` | 声明码垛标准动作规划数据结构和纯 helper，用于测试动作顺序与释放高度 |
| `src/palletplacesequence.cpp` | 实现“夹紧、基准点、XY、Z下降、松爪、Z抬升、回安全位”的动作规划 |
| `tests/test_pallet_scheduler.cpp` | 验证 `PalletScheduler` 容量、点位、满载、清零 |
| `tests/test_pallet_place_sequence.cpp` | 验证码垛标准动作顺序和 `releaseZOffset` 语义 |

### 修改文件

| 文件 | 职责变化 |
|------|----------|
| `src/palletscheduler.h` | 更新 `releaseZOffset` 中文注释，说明它是真实目标层上方释放高度 |
| `src/palletscheduler.cpp` | 校验文案和推荐提示改为真实释放高度语义 |
| `src/huayanScheduler.h` | 更新 `startPalletPlace()` 注释和信号语义，说明完整成功包含回安全位 |
| `src/huayanScheduler.cpp` | 调整码垛动作链：夹紧、基准点、XY、Z下降、松爪、Z抬升、回 `Func_yun_xing_zhong` |
| `src/taskexecutor.cpp` | 避免码垛动作链成功后重复执行 `StowAfterPallet`，保持成功后再 commit |
| `src/taskexecutor.h` | 更新状态注释，说明 `ArmPalletPlace` 已包含回运行安全位 |
| `src/palletparamdialog.h` | 构造函数接收 `HuayanScheduler*`，删除视觉客户端字段和视觉槽，并声明关闭保护 |
| `src/palletparamdialog.cpp` | 去掉视觉 UI，新增“执行一次码垛”，连接 `schedulerStopped` 做停止收尾，成功后提交，失败不提交 |
| `src/mainwindow.cpp` | 创建配置页时传入华研调度器，不再传入视觉客户端给码垛配置页 |
| `src/mainwindow.h` | 更新配置页注释，说明它会执行真实单步机械臂调试 |
| `src/visionclient.h` | 删除或废弃码垛占用检测接口，避免配置页继续依赖 |
| `src/visionclient.cpp` | 删除或废弃码垛占用检测实现 |
| `CMakeLists.txt` | 加入 `palletplacesequence.*` 源文件 |
| `tests/CMakeLists.txt` | 加入两个新的测试可执行文件，保持测试目录独立 |
| `docs/superpowers/specs/2026-07-09-empty-box-pallet-real-debug-design.md` | 根据最终实现同步修订 |
| `docs/superpowers/plans/2026-07-09-empty-box-pallet-real-debug-plan.md` | 根据最终实现同步修订 |

---

### Task 1: 纯动作规划 helper 和测试

**Files:**
- Create: `robot_visual20260625/robot_visual/src/palletplacesequence.h`
- Create: `robot_visual20260625/robot_visual/src/palletplacesequence.cpp`
- Create: `robot_visual20260625/robot_visual/tests/test_pallet_place_sequence.cpp`
- Modify: `robot_visual20260625/robot_visual/CMakeLists.txt`
- Modify: `robot_visual20260625/robot_visual/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `PalletPose` from `src/palletscheduler.h`
- Produces:
  - `enum class PalletPlaceStepKind`
  - `struct PalletPlaceStep`
  - `QList<PalletPlaceStep> buildPalletPlaceSequence(const PalletPose &targetOffset, double releaseZOffsetMm, double robotBaseHeightFromGroundMm, double palletBaseTcpZMm)`

- [ ] **Step 1: 写失败测试**

在 `tests/test_pallet_place_sequence.cpp` 新建以下测试。该测试先描述产线标准动作顺序，不连接真实华研机械臂。

```cpp
#include <QtTest/QtTest>

#include "palletplacesequence.h"

class PalletPlaceSequenceTest : public QObject
{
    Q_OBJECT

private slots:
    void buildsStandardSingleBoxSequence()
    {
        PalletPose target;
        target.x = 120.0;
        target.y = -80.0;
        target.z = 216.0;
        target.rz = 0.0;

        const QList<PalletPlaceStep> steps = buildPalletPlaceSequence(target, 35.0);

        QCOMPARE(steps.size(), 7);
        QCOMPARE(steps.at(0).kind, PalletPlaceStepKind::ClampAtSafety);
        QCOMPARE(steps.at(1).kind, PalletPlaceStepKind::RunPalletBaseFunction);
        QCOMPARE(steps.at(2).kind, PalletPlaceStepKind::MoveXYAboveTarget);
        QCOMPARE(steps.at(2).offset.x, 120.0);
        QCOMPARE(steps.at(2).offset.y, -80.0);
        QCOMPARE(steps.at(2).offset.z, 0.0);
        QCOMPARE(steps.at(3).kind, PalletPlaceStepKind::DescendToReleaseHeight);
        QCOMPARE(steps.at(3).offset.z, 251.0);
        QCOMPARE(steps.at(4).kind, PalletPlaceStepKind::ReleaseGripper);
        QCOMPARE(steps.at(5).kind, PalletPlaceStepKind::LiftAfterRelease);
        QCOMPARE(steps.at(5).offset.z, -251.0);
        QCOMPARE(steps.at(6).kind, PalletPlaceStepKind::RunStowFunction);
    }

    void rejectsNegativeReleaseHeight()
    {
        PalletPose target;
        target.z = 10.0;
        const QList<PalletPlaceStep> steps = buildPalletPlaceSequence(target, -1.0);
        QVERIFY2(steps.isEmpty(), "释放高度不能为负数，规划必须失败关闭");
    }
};

QTEST_MAIN(PalletPlaceSequenceTest)
#include "test_pallet_place_sequence.moc"
```

- [ ] **Step 2: 注册测试并确认失败**

修改 `tests/CMakeLists.txt`，先加入测试目标：

```cmake
find_package(Qt6 REQUIRED COMPONENTS Core Test)

add_executable(pallet_place_sequence_tests
    test_pallet_place_sequence.cpp
    ../src/palletplacesequence.cpp
)

target_include_directories(pallet_place_sequence_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(pallet_place_sequence_tests PRIVATE
    Qt6::Core
    Qt6::Test
)

add_test(NAME pallet_place_sequence_tests
         COMMAND pallet_place_sequence_tests)
```

运行：

```bash
/opt/Qt/qt6.8/6.8.3/gcc_64/bin/qt-cmake -S robot_visual20260625/robot_visual -B robot_visual20260625/robot_visual/build -DQt6_DIR=/opt/Qt/qt6.8/6.8.3/gcc_64/lib/cmake/Qt6 -DBUILD_TESTING=ON
cmake --build robot_visual20260625/robot_visual/build -j
```

预期：编译失败，提示找不到 `palletplacesequence.h` 或 `buildPalletPlaceSequence`。

- [ ] **Step 3: 实现纯动作规划 helper**

创建 `src/palletplacesequence.h`：

```cpp
#ifndef PALLETPLACESEQUENCE_H
#define PALLETPLACESEQUENCE_H

#include <QList>
#include <QString>

#include "palletscheduler.h"

/**
 * @brief 单次空箱码垛的标准动作类型。
 *
 * 该枚举只描述产线标准顺序，不直接调用华研 SDK；HuayanScheduler 按这里的顺序下发真实命令。
 */
enum class PalletPlaceStepKind {
    ClampAtSafety,
    RunPalletBaseFunction,
    MoveXYAboveTarget,
    DescendToReleaseHeight,
    ReleaseGripper,
    LiftAfterRelease,
    RunStowFunction
};

/**
 * @brief 单个码垛动作步骤。
 *
 * offset 仅对相对移动步骤有效，单位 mm/deg；函数步骤由 HuayanScheduler 使用现有函数名执行。
 */
struct PalletPlaceStep {
    PalletPlaceStepKind kind = PalletPlaceStepKind::ClampAtSafety;
    PalletPose offset;
    QString description;
};

/**
 * @brief 根据目标层中心偏移和目标层上方释放高度，生成一次空箱码垛标准动作。
 *
 * targetOffset 是 PalletScheduler::nextRelativeOffset() 输出的目标层中心偏移；
 * releaseZOffsetMm 是“目标层上方释放高度”。真实 Z 相对移动量 = (targetOffset.z + releaseZOffsetMm) - robotBaseHeightFromGroundMm + PALLET_GRIPPER_RELEASE_Z_OFFSET_MM - palletBaseTcpZMm。
 * releaseZOffsetMm 小于 0 时返回空列表，调用方必须 fail-closed。
 */
QList<PalletPlaceStep> buildPalletPlaceSequence(const PalletPose &targetOffset,
                                                double releaseZOffsetMm);

#endif // PALLETPLACESEQUENCE_H
```

创建 `src/palletplacesequence.cpp`：

```cpp
#include "palletplacesequence.h"

QList<PalletPlaceStep> buildPalletPlaceSequence(const PalletPose &targetOffset,
                                                double releaseZOffsetMm)
{
    if (releaseZOffsetMm < 0.0) {
        return {};
    }

    PalletPose xyOffset;
    xyOffset.x = targetOffset.x;
    xyOffset.y = targetOffset.y;
    xyOffset.rz = targetOffset.rz;

    PalletPose descendOffset;
    descendOffset.z = targetOffset.z + releaseZOffsetMm;

    PalletPose liftOffset;
    liftOffset.z = -descendOffset.z;

    return {
        {PalletPlaceStepKind::ClampAtSafety, {}, QStringLiteral("安全位夹紧，模拟空箱在夹爪上")},
        {PalletPlaceStepKind::RunPalletBaseFunction, {}, QStringLiteral("运动到码垛区上方基准点")},
        {PalletPlaceStepKind::MoveXYAboveTarget, xyOffset, QStringLiteral("先执行 XY 到目标列/行上方")},
        {PalletPlaceStepKind::DescendToReleaseHeight, descendOffset, QStringLiteral("Z 下降到目标层上方释放高度")},
        {PalletPlaceStepKind::ReleaseGripper, {}, QStringLiteral("松爪释放空箱")},
        {PalletPlaceStepKind::LiftAfterRelease, liftOffset, QStringLiteral("松爪后先 Z 抬升，避免横移擦碰")},
        {PalletPlaceStepKind::RunStowFunction, {}, QStringLiteral("回运行安全位 Func_yun_xing_zhong")},
    };
}
```

- [ ] **Step 4: 加入主程序源文件并跑通测试**

在根 `CMakeLists.txt` 的 `PROJECT_SOURCES` 中加入：

```cmake
        src/palletplacesequence.cpp
        src/palletplacesequence.h
```

运行：

```bash
cmake --build robot_visual20260625/robot_visual/build -j
ctest --test-dir robot_visual20260625/robot_visual/build --output-on-failure -R pallet_place_sequence_tests
```

预期：`pallet_place_sequence_tests` 通过。

---

### Task 2: PalletScheduler 语义和单元测试

**Files:**
- Create: `robot_visual20260625/robot_visual/tests/test_pallet_scheduler.cpp`
- Modify: `robot_visual20260625/robot_visual/src/palletscheduler.h`
- Modify: `robot_visual20260625/robot_visual/src/palletscheduler.cpp`
- Modify: `robot_visual20260625/robot_visual/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `PalletScheduler::nextRelativeOffset(PalletArea, PalletPose*, QString*)`
- Produces: 清晰的 `releaseZOffset` 中文语义和测试覆盖

- [ ] **Step 1: 写 PalletScheduler 测试**

创建 `tests/test_pallet_scheduler.cpp`：

```cpp
#include <QtTest/QtTest>

#include "palletscheduler.h"

class PalletSchedulerTest : public QObject
{
    Q_OBJECT

private slots:
    void defaultSmallBoxCapacityAndFirstPoint()
    {
        PalletScheduler scheduler;
        scheduler.reset(PalletArea::SmallBox);
        scheduler.setConfig(PalletArea::SmallBox, PalletScheduler::defaultSmallBoxConfig());

        QCOMPARE(scheduler.columns(PalletArea::SmallBox), 2);
        QCOMPARE(scheduler.rows(PalletArea::SmallBox), 3);
        QCOMPARE(scheduler.perLayerCapacity(PalletArea::SmallBox), 6);
        QCOMPARE(scheduler.totalCapacity(PalletArea::SmallBox), 48);

        PalletPose offset;
        QString error;
        QVERIFY2(scheduler.nextRelativeOffset(PalletArea::SmallBox, &offset, &error),
                 qPrintable(error));
        QCOMPARE(offset.x, -235.0);
        QCOMPARE(offset.y, -290.0);
        QCOMPARE(offset.z, 0.0);
    }

    void manualResetRestartsFromFirstPoint()
    {
        PalletScheduler scheduler;
        scheduler.reset(PalletArea::LargeBox);
        scheduler.setConfig(PalletArea::LargeBox, PalletScheduler::defaultLargeBoxConfig());

        QString error;
        QVERIFY(scheduler.commitPlaced(PalletArea::LargeBox, &error));
        QVERIFY(scheduler.commitPlaced(PalletArea::LargeBox, &error));
        QCOMPARE(scheduler.placedCount(PalletArea::LargeBox), 2);

        scheduler.reset(PalletArea::LargeBox);
        QCOMPARE(scheduler.placedCount(PalletArea::LargeBox), 0);

        PalletPose offset;
        QVERIFY2(scheduler.nextRelativeOffset(PalletArea::LargeBox, &offset, &error),
                 qPrintable(error));
        QCOMPARE(offset.x, 0.0);
        QCOMPARE(offset.y, -200.0);
        QCOMPARE(offset.z, 0.0);
    }

    void fullAreaRejectsNextPoint()
    {
        PalletScheduler scheduler;
        PalletConfig cfg = PalletScheduler::defaultLargeBoxConfig();
        cfg.maxLayers = 1;
        scheduler.setConfig(PalletArea::LargeBox, cfg);
        scheduler.reset(PalletArea::LargeBox);

        QString error;
        QVERIFY(scheduler.commitPlaced(PalletArea::LargeBox, &error));
        QVERIFY(scheduler.commitPlaced(PalletArea::LargeBox, &error));

        PalletPose offset;
        QVERIFY(!scheduler.nextRelativeOffset(PalletArea::LargeBox, &offset, &error));
        QVERIFY(error.contains(QStringLiteral("码垛区已满")));
    }
};

QTEST_MAIN(PalletSchedulerTest)
#include "test_pallet_scheduler.moc"
```

- [ ] **Step 2: 注册并运行测试**

在 `tests/CMakeLists.txt` 加入：

```cmake
add_executable(pallet_scheduler_tests
    test_pallet_scheduler.cpp
    ../src/palletscheduler.cpp
)

target_include_directories(pallet_scheduler_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(pallet_scheduler_tests PRIVATE
    Qt6::Core
    Qt6::Test
)

add_test(NAME pallet_scheduler_tests
         COMMAND pallet_scheduler_tests)
```

运行：

```bash
cmake --build robot_visual20260625/robot_visual/build -j
ctest --test-dir robot_visual20260625/robot_visual/build --output-on-failure -R pallet_scheduler_tests
```

预期：如果默认点位或 QSettings 状态污染导致失败，先按测试修正初始化和 reset 顺序；不能删除测试覆盖。

- [ ] **Step 3: 更新 releaseZOffset 注释和校验文案**

在 `src/palletscheduler.h` 中把 `releaseZOffset` 注释改为：

```cpp
    // 目标层上方释放高度，单位 mm；真实释放地面高度 = nextRelativeOffset().z + releaseZOffset。
    double releaseZOffset = 0.0;
```

在 `validateConfig()` 中把相关提示文案改为：

```cpp
        localSuggestions << QStringLiteral(
            "目标层上方释放高度建议在 %1-%2 mm；真实释放地面高度 = 托盘面离地高度 + 层高 + 该高度")
            .arg(minSuggested, 0, 'f', 1)
            .arg(maxSuggested, 0, 'f', 1);
```

- [ ] **Step 4: 运行全部当前测试**

运行：

```bash
ctest --test-dir robot_visual20260625/robot_visual/build --output-on-failure
```

预期：所有已注册测试通过。

---

### Task 3: HuayanScheduler 统一标准码垛动作链

**Files:**
- Modify: `robot_visual20260625/robot_visual/src/huayanScheduler.h`
- Modify: `robot_visual20260625/robot_visual/src/huayanScheduler.cpp`

**Interfaces:**
- Consumes: `buildPalletPlaceSequence(const PalletPose&, double, double, double)`
- Produces:
  - `void startPalletPlace(const PalletPose &targetOffset, double releaseZOffsetMm, double robotBaseHeightFromGroundMm)`
  - `void palletPlaceCompleted()`
  - `void palletPlaceError(const QString &reason)`

- [ ] **Step 1: 修改头文件接口和注释**

在 `src/huayanScheduler.h` 中把 `startPalletPlace` 声明改为：

```cpp
    /**
     * @brief 执行一次标准空箱码垛动作。
     *
     * 标准动作链：安全位夹紧 -> 码垛基准点 -> XY 到目标上方 -> Z 到释放高度
     * -> 松爪 -> Z 抬升 -> Func_yun_xing_zhong 回运行安全位。
     * 本函数只执行机械臂动作，不更新 PalletScheduler 缓存；调用方必须在
     * palletPlaceCompleted() 后再 commitPlaced()。
     */
    void startPalletPlace(const PalletPose &targetOffset, double releaseZOffsetMm, double robotBaseHeightFromGroundMm);
```

在 `ActionStep` 中保留并扩展码垛步骤：

```cpp
        ClampPalletAtSafety,
        RunPalletBase,
        MovePalletXY,
        DescendPalletZ,
        ReleasePallet,
        LiftAfterPalletRelease,
        StowAfterPalletRelease,
```

- [ ] **Step 2: 调整实现入口**

在 `src/huayanScheduler.cpp` 中包含 helper：

```cpp
#include "palletplacesequence.h"
```

把 `startPalletPlace` 实现改为接收释放高度，并生成动作列表。已有 `m_palletMoves` 可继续用于相对移动，但要保证 XY 和 Z 分段执行：

```cpp
void HuayanScheduler::startPalletPlace(const PalletPose &targetOffset,
                                       double releaseZOffsetMm)
{
    if (m_action != Action::None) {
        emit palletPlaceError(QStringLiteral("当前已有独立动作执行中"));
        return;
    }
    if (m_stage != Stage::None) {
        emit palletPlaceError(QStringLiteral("当前阶段忙碌，不能开始码垛放置动作"));
        return;
    }
    if (m_palletBaseFuncName.isEmpty()) {
        emit palletPlaceError(QStringLiteral("未配置码垛基准点函数"));
        return;
    }
    if (m_releaseFuncName.isEmpty()) {
        emit palletPlaceError(QStringLiteral("未配置松爪函数"));
        return;
    }

    const QList<PalletPlaceStep> steps = buildPalletPlaceSequence(targetOffset, releaseZOffsetMm);
    if (steps.isEmpty()) {
        emit palletPlaceError(QStringLiteral("码垛释放高度无效，不能执行"));
        return;
    }

    m_palletMoves.clear();
    m_palletMoveIdx = 0;
    // 这里后续步骤会按 ActionStep 明确执行 XY、Z下降、Z抬升，不再把所有轴混成一组。
    m_action = Action::PalletPlace;
    m_actionStep = ActionStep::ClampPalletAtSafety;
    m_pendingPalletTargetOffset = targetOffset;
    m_pendingPalletReleaseZ = targetOffset.z + releaseZOffsetMm;

    emit logMessage(QStringLiteral("[码垛] 开始标准单次动作 targetZ=%1 releaseZ=%2")
                    .arg(targetOffset.z, 0, 'f', 1)
                    .arg(m_pendingPalletReleaseZ, 0, 'f', 1));
    executeCurrentStep();
}
```

同时在 `huayanScheduler.h` 私有成员中增加：

```cpp
    PalletPose m_pendingPalletTargetOffset; ///< 本次码垛目标层中心偏移。
    double m_pendingPalletReleaseZ = 0.0;   ///< 本次真实松爪相对 Z。
```

- [ ] **Step 3: 实现步骤执行顺序**

在 `executeCurrentStep()` 的 `Action::PalletPlace` 分支改为：

```cpp
    case Action::PalletPlace:
        switch (m_actionStep) {
        case ActionStep::ClampPalletAtSafety:
            emit logMessage(QStringLiteral("[码垛] 安全位夹紧 %1").arg(m_gripFuncName));
            executeRunFunc(m_gripFuncName, 30000);
            break;
        case ActionStep::RunPalletBase:
            emit logMessage(QStringLiteral("[码垛] 调用基准点函数 %1").arg(m_palletBaseFuncName));
            executeRunFunc(m_palletBaseFuncName, 120000);
            break;
        case ActionStep::MovePalletXY:
            m_palletMoves = {
                {0, m_pendingPalletTargetOffset.x >= 0 ? 1 : 0, qAbs(m_pendingPalletTargetOffset.x)},
                {1, m_pendingPalletTargetOffset.y >= 0 ? 1 : 0, qAbs(m_pendingPalletTargetOffset.y)},
            };
            m_palletMoveIdx = 0;
            executeNextPalletMove();
            break;
        case ActionStep::DescendPalletZ:
            m_palletMoves = {
                {2, m_pendingPalletReleaseZ >= 0 ? 1 : 0, qAbs(m_pendingPalletReleaseZ)},
            };
            m_palletMoveIdx = 0;
            executeNextPalletMove();
            break;
        case ActionStep::ReleasePallet:
            emit logMessage(QStringLiteral("[码垛] 调用松爪函数 %1").arg(m_releaseFuncName));
            executeRunFunc(m_releaseFuncName, 30000);
            break;
        case ActionStep::LiftAfterPalletRelease:
            m_palletMoves = {
                {2, m_pendingPalletReleaseZ >= 0 ? 0 : 1, qAbs(m_pendingPalletReleaseZ)},
            };
            m_palletMoveIdx = 0;
            executeNextPalletMove();
            break;
        case ActionStep::StowAfterPalletRelease:
            emit logMessage(QStringLiteral("[码垛] 回运行安全位 %1").arg(m_stowFuncName));
            executeRunFunc(m_stowFuncName, 120000);
            break;
        default:
            break;
        }
        break;
```

- [ ] **Step 4: 调整步骤推进**

在 `advanceStep()` 的 `Action::PalletPlace` 分支使用明确顺序：

```cpp
    case Action::PalletPlace:
        if (m_actionStep == ActionStep::ClampPalletAtSafety)
            m_actionStep = ActionStep::RunPalletBase;
        else if (m_actionStep == ActionStep::RunPalletBase)
            m_actionStep = ActionStep::MovePalletXY;
        else if (m_actionStep == ActionStep::MovePalletXY)
            m_actionStep = ActionStep::DescendPalletZ;
        else if (m_actionStep == ActionStep::DescendPalletZ)
            m_actionStep = ActionStep::ReleasePallet;
        else if (m_actionStep == ActionStep::ReleasePallet)
            m_actionStep = ActionStep::LiftAfterPalletRelease;
        else if (m_actionStep == ActionStep::LiftAfterPalletRelease)
            m_actionStep = ActionStep::StowAfterPalletRelease;
        else if (m_actionStep == ActionStep::StowAfterPalletRelease)
            completeAction();
        break;
```

在 `executeNextPalletMove()` 完成一组移动后，当前 `ActionStep` 不能直接跳到松爪；应调用 `advanceStep()` 再 `executeCurrentStep()`。

- [ ] **Step 5: 更新所有调用点编译错误**

搜索：

```bash
rg -n "startPalletPlace" robot_visual20260625/robot_visual/src
```

历史计划曾要求把调用从：

```cpp
m_arm->startPalletPlace(m_pendingPalletOffset);
```

改为：

```cpp
const PalletConfig palletConfig = m_pallet->config(m_stationCfg->palletArea);
m_arm->startPalletPlace(m_pendingPalletOffset,
                        palletConfig.releaseZOffset,
                        palletConfig.robotBaseHeightFromGround);
```

2026-07-10 已进一步修正：主流程到达码垛区时空箱已经夹紧，应调用：

```cpp
const PalletConfig palletConfig = m_pallet->config(m_stationCfg->palletArea);
m_arm->startPalletPlaceFromClampedSafety(m_pendingPalletOffset,
                                         palletConfig.releaseZOffset,
                                         palletConfig.robotBaseHeightFromGround);
```

预期：主流程传入同一区域配置里的释放高度和机器人基座离地高度，并跳过重复夹紧。

- [ ] **Step 6: 构建验证**

运行：

```bash
cmake --build robot_visual20260625/robot_visual/build -j
```

预期：构建通过；如出现旧 `ActionStep` 名称残留，按新标准顺序统一修正。

---

### Task 4: TaskExecutor 主流程提交语义修正

**Files:**
- Modify: `robot_visual20260625/robot_visual/src/taskexecutor.h`
- Modify: `robot_visual20260625/robot_visual/src/taskexecutor.cpp`

**Interfaces:**
- Consumes: `HuayanScheduler::palletPlaceCompleted()`
- Produces: 主流程仅在完整动作链成功后提交，并避免重复回安全位

- [ ] **Step 1: 更新状态注释**

在 `taskexecutor.h` 中把状态注释改为：

```cpp
        ArmPalletPlace,          ///< 等待机械臂完成标准码垛动作，动作内已包含松爪后回运行安全位。
        CommitPallet,            ///< 标准码垛动作完整成功后，将已放点位提交到缓存。
```

- [ ] **Step 2: 调整码垛成功后的状态流**

在 `onPalletPlaceCompleted()` 中保持先 commit，再直接完成任务或进入后续 AGV 回家逻辑。把原来的 `enterState(ExecState::StowAfterPallet, ...)` 改为：

```cpp
    emit logMessage(prefix(QStringLiteral("PALLET"))
                    + QStringLiteral(" 已提交 %1 码垛数量，机械臂已在码垛动作内回运行安全位")
                          .arg(palletAreaDisplayName(m_stationCfg->palletArea)));
    finishTaskSuccess();
```

说明：`HuayanScheduler::startPalletPlace()` 已经调用 `Func_yun_xing_zhong`，这里不能重复执行 `StowAfterPallet`。

- [ ] **Step 3: 保留失败不提交规则**

确认 `onPalletPlaceError()` 中没有调用 `commitPlaced()`。若有任何提交逻辑，删除并保留：

```cpp
    beginCleanupAfterTaskFailure(QStringLiteral("码垛放置失败：%1").arg(reason));
```

- [ ] **Step 4: 构建验证**

运行：

```bash
cmake --build robot_visual20260625/robot_visual/build -j
```

预期：构建通过。

---

### Task 5: 配置页去视觉并新增真实单步码垛

**Files:**
- Modify: `robot_visual20260625/robot_visual/src/palletparamdialog.h`
- Modify: `robot_visual20260625/robot_visual/src/palletparamdialog.cpp`
- Modify: `robot_visual20260625/robot_visual/src/mainwindow.h`
- Modify: `robot_visual20260625/robot_visual/src/mainwindow.cpp`

**Interfaces:**
- Consumes:
  - `PalletScheduler *m_scheduler`
  - `HuayanScheduler *m_arm`
  - `palletAreaConfig(PalletArea area)`
  - `HuayanScheduler::startPalletPlace(const PalletPose&, double, double)`
- Produces:
  - `void runSinglePalletPlace(PalletArea area)`
  - 配置页真实单步调试能力

- [ ] **Step 1: 修改构造函数和成员**

在 `palletparamdialog.h` 前置声明：

```cpp
class HuayanScheduler;
```

把构造函数改为：

```cpp
    explicit PalletParamDialog(PalletScheduler *scheduler,
                               HuayanScheduler *arm,
                               QWidget *parent = nullptr);
```

删除：

```cpp
class VisionHttpClient;
VisionHttpClient *m_visionClient = nullptr;
QHash<QString, PalletArea> m_pendingRequests;
```

新增：

```cpp
    /** @brief 执行一次真实空箱码垛调试；成功后才提交已放数量。 */
    void runSinglePalletPlace(PalletArea area);

    /** @brief 当前配置页正在调试的码垛区；无调试时不使用该值。 */
    PalletArea m_runningDebugArea = PalletArea::LargeBox;
    bool m_debugRunning = false;
    HuayanScheduler *m_arm = nullptr;
```

- [ ] **Step 2: 删除 PageWidgets 视觉控件，加入单步按钮**

在 `PageWidgets` 中删除：

```cpp
        QPushButton *detectBtn = nullptr;
        QLabel *detectLabel = nullptr;
        QLabel *emptyCountLabel = nullptr;
```

新增：

```cpp
        QPushButton *singlePlaceBtn = nullptr;
```

在 UI 创建中删除“视觉检测”分组，新增按钮：

```cpp
    w->singlePlaceBtn = new QPushButton(QStringLiteral("执行一次码垛"));
    w->singlePlaceBtn->setToolTip(QStringLiteral("真实机械臂调试：每次只放置一个空箱，完整成功后已放数量加 1。"));
    buttonGrid->addWidget(w->singlePlaceBtn, 3, 0, 1, 3);
    connect(w->singlePlaceBtn, &QPushButton::clicked,
            this, [this, area] { runSinglePalletPlace(area); });
```

- [ ] **Step 3: 删除视觉连接和 detectArea**

在 `palletparamdialog.cpp` 删除构造函数中 `m_visionClient` 相关 `connect` 代码。删除 `detectArea(PalletArea area)` 函数声明和实现。删除所有 `fetchPalletOccupancy`、`palletOccupancyReady`、`palletOccupancyError`、`emptyCountLabel`、`detectLabel` 引用。

- [ ] **Step 4: 实现 runSinglePalletPlace**

在 `palletparamdialog.cpp` 增加：

```cpp
void PalletParamDialog::runSinglePalletPlace(PalletArea area)
{
    PageWidgets *w = widgets(area);
    if (!w || !m_scheduler || !m_arm) {
        if (w)
            setStatus(w, QStringLiteral("机械臂或码垛调度器未就绪"), QStringLiteral("error"));
        return;
    }
    if (m_debugRunning) {
        setStatus(w, QStringLiteral("已有一次码垛调试正在执行"), QStringLiteral("warning"));
        return;
    }

    savePage(area);

    QStringList errors;
    QStringList suggestions;
    Q_UNUSED(suggestions)
    if (!m_scheduler->validateConfig(area, &errors, nullptr)) {
        setStatus(w, QStringLiteral("配置无效：%1").arg(errors.join(QStringLiteral("；"))), QStringLiteral("error"));
        return;
    }
    if (m_scheduler->placedCount(area) >= m_scheduler->totalCapacity(area)) {
        QMessageBox::warning(this,
                             QStringLiteral("码垛区已满"),
                             QStringLiteral("码垛区已满，请人工搬运并清零后再执行。"));
        setStatus(w, QStringLiteral("码垛区已满，请人工搬运并清零"), QStringLiteral("error"));
        return;
    }

    const PalletAreaTaskConfig *areaConfig = palletAreaConfig(area);
    if (!areaConfig) {
        setStatus(w, QStringLiteral("缺少当前码垛区的机械臂函数配置"), QStringLiteral("error"));
        return;
    }

    HuayanScheduler::PalletArmFunctions funcs;
    funcs.palletBaseFunc = areaConfig->palletBaseFunc;
    funcs.releaseFunc = areaConfig->releaseFunc;
    m_arm->setPalletFunctions(funcs);

    PalletPose offset;
    QString error;
    if (!m_scheduler->nextRelativeOffset(area, &offset, &error)) {
        setStatus(w, QStringLiteral("无法计算下一码垛点：%1").arg(error), QStringLiteral("error"));
        return;
    }

    m_debugRunning = true;
    m_runningDebugArea = area;
    w->singlePlaceBtn->setEnabled(false);
    setStatus(w, QStringLiteral("正在执行一次真实码垛调试"), QStringLiteral("warning"));

    const PalletConfig cfg = m_scheduler->config(area);
    m_arm->startPalletPlace(offset, cfg.releaseZOffset, cfg.robotBaseHeightFromGround);
}
```

- [ ] **Step 5: 连接成功/失败信号**

在构造函数中增加：

```cpp
    if (m_arm) {
        connect(m_arm, &HuayanScheduler::palletPlaceCompleted, this, [this] {
            if (!m_debugRunning)
                return;
            const PalletArea area = m_runningDebugArea;
            PageWidgets *w = widgets(area);
            QString error;
            if (!m_scheduler->commitPlaced(area, &error)) {
                if (w)
                    setStatus(w, QStringLiteral("机械臂完成但提交数量失败：%1").arg(error), QStringLiteral("error"));
            } else if (w) {
                setStatus(w, QStringLiteral("单次码垛完成，已放数量加 1"), QStringLiteral("ok"));
            }
            m_debugRunning = false;
            if (w && w->singlePlaceBtn)
                w->singlePlaceBtn->setEnabled(true);
            refreshPage(area);
        });

        connect(m_arm, &HuayanScheduler::palletPlaceError, this, [this](const QString &reason) {
            if (!m_debugRunning)
                return;
            const PalletArea area = m_runningDebugArea;
            PageWidgets *w = widgets(area);
            m_debugRunning = false;
            if (w && w->singlePlaceBtn)
                w->singlePlaceBtn->setEnabled(true);
            if (w)
                setStatus(w, QStringLiteral("单次码垛失败，已放数量未提交：%1").arg(reason), QStringLiteral("error"));
            refreshPage(area);
        });
    }
```

- [ ] **Step 6: 修改 MainWindow 创建配置页**

在 `mainwindow.cpp` 的 `onPalletConfig()` 中把构造改为：

```cpp
    auto *dialog = new PalletParamDialog(
        m_palletScheduler,
        m_devMgr->huayanScheduler(),
        this);
```

在 `mainwindow.h` 注释中说明：

```cpp
    // 打开空箱码垛配置窗口；窗口可执行真实单步码垛调试，停止统一使用华研面板停止按钮。
```

- [ ] **Step 7: 构建验证**

运行：

```bash
cmake --build robot_visual20260625/robot_visual/build -j
```

预期：构建通过，且 `rg -n "palletOccupancy|detectArea|emptyObserve|视觉检测" src/palletparamdialog.*` 不再返回配置页视觉检测引用。

---

### Task 6: 删除或废弃码垛视觉占用接口

**Files:**
- Modify: `robot_visual20260625/robot_visual/src/visionclient.h`
- Modify: `robot_visual20260625/robot_visual/src/visionclient.cpp`
- Modify: `robot_visual20260625/robot_visual/src/palletscheduler.h`
- Modify: `robot_visual20260625/robot_visual/src/palletscheduler.cpp`

**Interfaces:**
- Consumes: 当前 `VisionHttpClient` 和 `PalletScheduler` 视觉空区接口
- Produces: 配置页和码垛主流程不再依赖视觉自动清零

- [ ] **Step 1: 确认视觉接口只被码垛配置页使用**

运行：

```bash
rg -n "fetchPalletOccupancy|palletOccupancyReady|palletOccupancyError|markAreaObservedEmpty|markAreaObservedOccupied|emptyObserveCount|lastAutoResetTime" robot_visual20260625/robot_visual/src
```

预期：删除配置页引用后，只剩声明和实现。若其他主流程仍在使用，停止本任务并先更新 spec 和 plan。

- [ ] **Step 2: 删除 VisionHttpClient 码垛占用接口**

在 `visionclient.h` 删除：

```cpp
void fetchPalletOccupancy(const QString &requestId);
void palletOccupancyReady(QString requestId, bool occupied, int objectCount, double confidence, QString summary);
void palletOccupancyError(QString requestId, QString msg);
```

在 `visionclient.cpp` 删除对应实现。保留普通取料视觉接口不变。

- [ ] **Step 3: 废弃 PalletScheduler 视觉自动清零状态**

如果没有其他调用方，删除 `PalletScheduler` 中以下接口和状态：

```cpp
void setStackingActive(PalletArea area, bool active);
void markAreaObservedEmpty(PalletArea area);
void markAreaObservedOccupied(PalletArea area);
int emptyObserveCount(PalletArea area) const;
QDateTime lastAutoResetTime(PalletArea area) const;
void areaAutoReset(PalletArea area, const QString &reason);
int emptyObserveCount = 0;
bool stackingActive = false;
QDateTime lastAutoResetTime;
```

同步删除 QSettings 中 `emptyObserveCount` 和 `lastAutoResetTime` 的读写。旧配置残留键可以留在用户配置文件中，不需要迁移。

- [ ] **Step 4: 修正 TaskExecutor 中 stacking flag 调用**

如果 `TaskExecutor` 仍调用 `setStackingActive()`，删除相关调用和 `clearPalletStackingFlag()` 函数。码垛已不依赖视觉自动清零，不再需要这个标记。

- [ ] **Step 5: 构建验证**

运行：

```bash
cmake --build robot_visual20260625/robot_visual/build -j
ctest --test-dir robot_visual20260625/robot_visual/build --output-on-failure
```

预期：构建和测试通过；取料视觉相关信号仍正常编译。

---

### Task 7: 文档同步和现场验证清单

**Files:**
- Modify: `robot_visual20260625/robot_visual/docs/superpowers/specs/2026-07-09-empty-box-pallet-real-debug-design.md`
- Modify: `robot_visual20260625/robot_visual/docs/superpowers/plans/2026-07-09-empty-box-pallet-real-debug-plan.md`
- Modify: `robot_visual20260625/robot_visual/docs/superpowers/plans/2026-06-22-empty-box-pallet-plan.md`

**Interfaces:**
- Consumes: 最终实现行为
- Produces: 与代码一致的中文文档

- [ ] **Step 1: 给旧计划增加替代说明**

在 `docs/superpowers/plans/2026-06-22-empty-box-pallet-plan.md` 文件顶部加入：

```markdown
> **已被新方案取代：** 当前现场方案已调整为“人工确认 + 配置页真实单步机械臂调试 + 主流程复用同一动作链”。请优先查看 `docs/superpowers/specs/2026-07-09-empty-box-pallet-real-debug-design.md` 和 `docs/superpowers/plans/2026-07-09-empty-box-pallet-real-debug-plan.md`。旧文档中的码垛视觉检测、视觉连续空区计数和视觉自动清零不再执行。
```

- [ ] **Step 2: 同步 spec**

根据最终代码检查 spec 中以下内容是否准确：

```text
构造函数参数、HuayanScheduler::startPalletPlace 签名、releaseZOffset 语义、是否删除视觉接口、TaskExecutor 标准成功路径是否保持 `PreparePalletPoint -> ArmPalletPlace -> CommitPallet -> task success`。
```

若代码和 spec 不一致，以最终安全设计为准修正文档；不能让文档保留旧视觉方案。

- [ ] **Step 3: 同步 plan**

根据最终代码检查本计划中的文件清单和接口签名。若实现时函数名发生变化，统一修正本计划，保证后续维护人员能按文档找到代码。

- [ ] **Step 4: 最终自动化验证**

运行：

```bash
/opt/Qt/qt6.8/6.8.3/gcc_64/bin/qt-cmake -S robot_visual20260625/robot_visual -B robot_visual20260625/robot_visual/build -DQt6_DIR=/opt/Qt/qt6.8/6.8.3/gcc_64/lib/cmake/Qt6 -DBUILD_TESTING=ON
cmake --build robot_visual20260625/robot_visual/build -j
ctest --test-dir robot_visual20260625/robot_visual/build --output-on-failure
```

预期：配置、构建、测试全部通过。

- [ ] **Step 5: 现场手动验证**

按以下顺序记录验证结果：

```text
1. 打开空箱码垛配置页，确认没有码垛视觉检测分组。
2. 大箱/小箱页签均可校验配置、显示容量、显示下一点、显示点位表。
3. 人工清零后，点击“执行一次码垛”。
4. 观察动作顺序：Func_jiajin -> palletBaseFunc -> XY -> Z下降到目标层上方释放高度 -> releaseFunc -> Z抬升 -> Func_yun_xing_zhong。
5. 成功后已放数量加 1。
6. 华研面板停止中断后，已放数量不增加。
7. 满载后按钮拒绝执行，并提示人工搬运清零。
8. 配置页单步稳定后，运行主流程，确认 PreparePalletPoint -> ArmPalletPlace -> CommitPallet 使用同一动作链。
```

- [ ] **Step 6: 现场验证后再单次功能提交**

本轮代码实现和自动化测试完成后先保持未提交状态。等现场单步配置页验证、主流程验证都通过，并由人工确认可以提交后，再执行一次提交：

```bash
git -C robot_visual20260625/robot_visual status --short
git -C robot_visual20260625/robot_visual add src tests CMakeLists.txt docs/superpowers
git -C robot_visual20260625/robot_visual commit -m "feat: add real single-step empty-box pallet debug"
```

预期：只产生一个包含完整功能、测试和文档同步的提交。

---

## 2026-07-10 补充修正：高层 Z 保护与主流程复用

现场反馈第 5 层附近 Z 值异常偏高后，已按以下口径修正代码和测试：

- 配置页删除“最高安全 Z”和“机械臂初始点位绝对预览”，避免把 UI 输入误当成示教器真实点位或机械臂安全极限。
- `PalletScheduler` 新增 `releaseGroundZ()`、`releaseTcpZ()`，统一解释“目标层表面离地高度 + 目标层上方释放高度 + 基座离地高度 + 夹爪补偿”的换算。
- `PalletPlaceSequence` 在生成 Z 下降动作前判断：目标 TCP Z 不能高于 `palletBaseFunc` 到位后读取的基准 TCP Z；否则拒绝执行，并提示降低最大层数、释放高度或修正现场高度参数。
- `HuayanScheduler` 日志增加 `目标TCP Z`，用于现场排查高层是否接近初始点位上限。
- 配置页“仿真 8 层”改为“按当前层数仿真”，按 UI 最大层数动态生成点位表。
- 保存配置后提示 `QSettings` 实际文件路径，当前常见路径为 `/home/dh/.config/wh-robot/robot-visual.conf`。
- 主流程新增并调用 `HuayanScheduler::startPalletPlaceFromClampedSafety()`；主流程到码垛区时空箱已经夹紧，所以跳过重复夹紧，其余动作链和配置页单步调试复用。
- 自动化测试新增高层目标 TCP Z、目标 TCP Z 高于基准点拒绝执行、配置页不暴露旧字段、主流程跳过重复夹紧等约束。

验证命令：

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
```

结果：2026-07-10 本地验证 7/7 通过。

## 2026-07-10 主流程现场联调记录

单独码垛测试目前基本满足现场要求，可以进入包含码垛的主流程测试。当前阶段满垛、空垛还没有和外部系统通信，现场临时采用以下方式：

- 主流程测试前，先在空箱码垛配置页核对 `placedCount` 是否等于现场真实已放数量。
- 托盘区域为空时，手动清零后再启动主流程。
- 满垛后如果人工已经搬走空箱、空托盘已经复位，可以在配置页手动清零，临时模拟后续外部系统的“满垛已搬走/空垛已就位”信号。
- 如果托盘上仍有箱子，不能随意清零；否则主流程会按第一层点位放置，可能撞箱或落点错误。

现场误差接受范围记录：

- 机器人基座离地高度、车体高度、地面平整度都会带来 Z 误差。
- 夹爪长度和 `PALLET_GRIPPER_RELEASE_Z_OFFSET_MM` 的测量也可能有误差。
- 框体高度会因测量、变形、磨损产生误差，高层码垛时误差可能累积。
- 因此 `releaseZOffset` 不一定表现为理论上的“比箱体高度多 10mm 后释放”。现场观察到第一层释放点接近第一层中部、高层误差更明显，只要单独码垛落箱效果仍在可接受范围，主流程可继续联调。

主流程测试重点：

```text
1. 第一次只跑 1-3 次码垛，确认倒料后空箱仍被夹紧。
2. 确认主流程到码垛区时调用 startPalletPlaceFromClampedSafety()，不重复夹紧。
3. 成功后必须进入 CommitPallet 并让 placedCount 加 1。
4. 失败、华研面板停止、满载拒绝时 placedCount 不能增加。
5. 观察日志中的 基准TCP Z、目标TCP Z、释放地面Z、本次Z相对移动，确认与单独码垛测试一致。
6. 外部满垛/空垛通信接入前，清零必须人工确认现场已搬空并复位托盘。
```

---

## Self-Review

1. Spec coverage: 本计划覆盖去视觉、人工清零、单次真实码垛、释放高度、动作顺序、失败不提交、满载保护、主流程复用、中文注释、测试和文档同步。
2. Placeholder scan: 本计划不保留空泛任务；每个任务都有明确文件、接口、代码片段和验证命令。
3. Type consistency: `releaseZOffset`、`startPalletPlace(const PalletPose&, double, double)`、`buildPalletPlaceSequence(const PalletPose&, double, double, double)` 在任务间命名一致。
