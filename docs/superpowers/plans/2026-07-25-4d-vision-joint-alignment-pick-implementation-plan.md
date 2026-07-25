# 4D 视觉联合对准抓取闭环实施计划

> **面向智能体执行者：** 必须按任务逐项执行本计划，并使用 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans`。所有步骤使用复选框（`- [ ]`）跟踪。

**目标：** 在不修改 Python 视觉算法、不改变 C++ 目标选择规则且继续保持 4D 抓取的前提下，把阶段一抓取从 X、Y、Rz 逐轴多轮修正改成“一次绝对预抓取位 MoveJ、最多 0～2 次联合 MoveL 精修正、统一视觉稳定窗口确认、沿工具 Z 下探”的有限闭环。

**架构：** 保留 `VisionHttpClient` 的手眼转换、目标选择、锚点锁定和 Rz 大角度保护；新增一个不依赖机器人 SDK 的纯判定单元，统一决定继续观察、下探、精修正或停止。`HuayanScheduler` 负责读取当前 TCP 与关节角、调用华沿 SDK 组合绝对预抓取位、下发联合运动，并在运动确认完成后更新锚点累计位移；所有异常均安全停止，不自动返回拍照位或重选目标。

**技术栈：** C++17、Qt 6.8.3（Core、Widgets、Test）、CMake 3.16、MSVC 2022 64 位、华沿机器人 C++ SDK V1.0.15.0、CTest。

## 全局约束

- 本计划对应设计文档：`docs/superpowers/specs/2026-07-25-4d-vision-joint-alignment-pick-design.md`。
- 只修改 `D:\project\CompositeRobot\code\C++\robot_visual` 上位机工程；Python 视觉工程保持不变。
- 保留 4D：只修正 X、Y、Rz，初始联合对准的 Z 增量固定为 0，Rx、Ry 保持拍照位实际姿态。
- 使用 `visionclient.cpp` 内现有 C++ 手眼标定矩阵，不读取或替换另一套标定矩阵。
- 固定拍照位、工位 ROI、最高层优先、同层选择、锚点锁定、可信范围和 Rz 大角度两帧确认规则保持不变。
- 首次联合对准固定最多一次，不计入精修正次数；精修正次数配置范围为 0～2，默认 1。
- XY 与 Rz 收敛阈值继续分别配置；调度器必须使用 `xyToleranceMm` 和 `rzToleranceDeg`，不得保留硬编码 Rz 阈值。
- 每次联合运动完成后重新开启 4～8 秒观察窗口；8 秒是硬上限，不新增额外等待帧。
- 达到精修正次数仍未收敛、目标丢失、目标可信规则拒绝、Z 不稳定或观察超时，均直接停止调度。
- 失败后不自动返回固定拍照位、不重新选择目标、不继续旧的 15 轮逐轴循环、不下探、不夹紧。
- 删除范围仅限旧阶段一 15 轮 X/Y/Rz 逐轴闭环及其专用配置、状态和辅助函数；不得改动目标选择、手眼转换、锚点锁定、Rz 大角度保护、深度处理、搜索下移、最终 Z 下探、扫码和夹爪行为。
- 保留现有 Z 深度定义、深度自动下探、抓取余量、Z 下探硬上限、扫码与夹爪流程。
- 首版不增加三维运动包络、碰撞模型、MoveJ 整条路径预测、逆解评分、动态工位占用或复杂轨迹规划器。
- 所有新增接口、状态、单位、SDK 参数和复杂分支均添加中文注释。
- 每个任务只提交本任务列出的文件；不得把现有 `AGENTS.md`、`agents.d` 或 `Testing` 目录的无关改动带入提交。
- 可以创建本地中文提交，但未经用户明确同意不得推送。

---

## 文件职责映射

### 新增文件

- `src/visionalignmentdecision.h`
  - 定义视觉观察窗口输入、策略、动作和决定结果。
  - 提供 XY/Rz 收敛判断、工具系联合修正量换算和窗口动作决策接口。
  - 不依赖 `HuayanScheduler`、网络或机器人 SDK，便于穷举单元测试。
- `src/visionalignmentdecision.cpp`
  - 实现 4～8 秒窗口、0～2 次精修正和安全停止的纯判定规则。
- `tests/test_vision_alignment_decision.cpp`
  - 覆盖阈值边界、方向换算、窗口时序、精修正次数和停止条件。
- `tests/test_vision_joint_motion_contract.cpp`
  - 通过源码契约检查华沿 SDK 调用、参数符号、关节参考、状态迁移和旧循环退出生产路径。

### 修改文件

- `src/runtimesettings.h`
  - 在视觉闭环配置中增加 `maxFineCorrectionCount`，默认 1。
  - 删除旧 `maxGrabIterations` 字段。
- `src/runtimesettings.cpp`
  - 校验 `maxFineCorrectionCount` 必须位于 0～2。
- `src/settingsmanager.cpp`
  - 使用 `vision/maxFineCorrectionCount` 保存、读取和修复新配置。
  - 不再读取旧 `vision/maxGrabIterations`，保存时移除该旧键。
- `src/settingsdialog.cpp`
  - 将现有“最大矫正次数”所在行直接替换为“联合精修正次数”，范围 0～2，不增加第二个次数控件。
- `CMakeLists.txt`
  - 把纯判定源文件加入主程序。
- `tests/CMakeLists.txt`
  - 注册纯判定测试和联合运动契约测试。
- `tests/test_runtime_settings.cpp`
  - 覆盖新配置合法范围。
- `tests/test_settings_manager.cpp`
  - 覆盖新配置持久化、缺省回退和越界修复。
- `tests/test_settings_dialog.cpp`
  - 覆盖新控件范围、读取、写回和变更摘要。
- `src/huayanScheduler.h`
  - 增加联合对准状态、关节参考、联合修正命令载荷和观察窗口状态。
  - 删除仅服务于阶段一逐轴循环的成员；保留码垛仍使用的 `RelMove`。
- `src/huayanScheduler.cpp`
  - 读取实际 TCP/J1～J6，调用 `HRIF_PoseTrans` 组合绝对预抓取位。
  - 使用 `HRIF_WayPoint` 下发初始 MoveJ，使用 `HRIF_WayPointRel` 下发联合 MoveL。
  - 将阶段一迁移到有限联合闭环，并复用现有目标锁定、Z 深度和夹爪流程。
- `tests/test_huayan_scheduler_contract.cpp`
  - 删除与阶段一逐轴循环绑定的旧断言，保留停止、锚点、深度和其他调度契约。
- `tests/test_stable_z_validation_contract.cpp`
  - 将仅验证 Z 的契约扩展为 XY/Rz 与 Z 共用观察窗口的契约。
- `tests/test_depth_descent_contract.cpp`
  - 确认新流程仍沿用原有 Z 深度、余量与硬上限。

---

### 任务 1：用联合精修正次数替换旧 15 轮配置

**文件：**

- 修改：`src/runtimesettings.h:25-40`
- 修改：`src/runtimesettings.cpp:40-60`
- 修改：`src/settingsmanager.cpp:20-32, 140-155, 265-282`
- 修改：`src/settingsdialog.cpp:171-181, 270-280, 317-327, 395-405`
- 修改：`tests/test_runtime_settings.cpp`
- 修改：`tests/test_settings_manager.cpp`
- 修改：`tests/test_settings_dialog.cpp`

**接口：**

- 产生：`RuntimeSettings::VisionClosedLoop::maxFineCorrectionCount`，类型 `int`，默认值 `1`，合法范围 `[0, 2]`。
- 产生：INI 键 `vision/maxFineCorrectionCount`。
- 删除：`RuntimeSettings::VisionClosedLoop::maxGrabIterations`。
- 删除：INI 键 `vision/maxGrabIterations` 的保存和读取；保存设置时主动移除磁盘中遗留的旧键。

- [ ] **步骤 1：先在运行时配置测试中写失败用例**

在 `tests/test_runtime_settings.cpp` 增加以下断言：

```cpp
void testFineCorrectionCountRange()
{
    RuntimeSettings settings;
    QString error;

    settings.vision.maxFineCorrectionCount = 0;
    QVERIFY2(validateRuntimeSettings(settings, &error), qPrintable(error));

    settings.vision.maxFineCorrectionCount = 2;
    QVERIFY2(validateRuntimeSettings(settings, &error), qPrintable(error));

    settings.vision.maxFineCorrectionCount = -1;
    QVERIFY(!validateRuntimeSettings(settings, &error));
    QVERIFY(error.contains(QStringLiteral("联合精修正次数")));

    settings.vision.maxFineCorrectionCount = 3;
    QVERIFY(!validateRuntimeSettings(settings, &error));
    QVERIFY(error.contains(QStringLiteral("联合精修正次数")));
}
```

把该槽函数加入现有测试类的 `private slots`。

- [ ] **步骤 2：在设置持久化与界面测试中写失败用例**

在 `tests/test_settings_manager.cpp` 增加三种场景：

```cpp
settings.vision.maxFineCorrectionCount = 2;
QVERIFY(manager.save(settings, &error));
QCOMPARE(manager.load().vision.maxFineCorrectionCount, 2);

ini.setValue(QStringLiteral("vision/maxFineCorrectionCount"), 3);
QCOMPARE(manager.load().vision.maxFineCorrectionCount, 1);

ini.remove(QStringLiteral("vision/maxFineCorrectionCount"));
QCOMPARE(manager.load().vision.maxFineCorrectionCount, 1);

ini.setValue(QStringLiteral("vision/maxGrabIterations"), 15);
QCOMPARE(manager.load().vision.maxFineCorrectionCount, 1);
QVERIFY(manager.save(manager.load(), &error));
QVERIFY(!ini.contains(QStringLiteral("vision/maxGrabIterations")));
```

在 `tests/test_settings_dialog.cpp` 按现有 `findChild` 写法验证：

```cpp
auto *spin = dialog.findChild<QSpinBox *>(QStringLiteral("visionMaxFineCorrections"));
QVERIFY(spin);
QCOMPARE(spin->minimum(), 0);
QCOMPARE(spin->maximum(), 2);
QCOMPARE(spin->value(), 1);
```

再将控件设为 2，验证 `dialog.candidate().vision.maxFineCorrectionCount == 2`，并验证变更摘要包含“联合精修正次数”。同时验证界面中不存在旧对象名 `visionMaxIterationsSpinBox`，确保没有保留第二行旧次数控件。

- [ ] **步骤 3：运行三个测试，确认它们先失败**

在已经初始化 Qt/MSVC 环境的 PowerShell 中运行：

```powershell
$BuildDirectory = "build\Desktop_Qt_6_8_3_MSVC2022_64bit_Debug"
cmake --build $BuildDirectory --target runtime_settings_tests settings_manager_tests settings_dialog_tests
ctest --test-dir $BuildDirectory -C Debug --output-on-failure -R "runtime_settings_tests|settings_manager_tests|settings_dialog_tests"
```

预期：编译因 `maxFineCorrectionCount` 尚未定义而失败，或替换后的界面控件断言失败。

- [ ] **步骤 4：实现配置字段、校验与持久化**

在 `RuntimeSettings::VisionClosedLoop` 中加入：

```cpp
int maxFineCorrectionCount = 1; ///< 初始联合 MoveJ 后允许的联合 MoveL 精修正次数，范围 0～2。
```

同时删除：

```cpp
int maxGrabIterations = 15;
```

在 `validateRuntimeSettings()` 中加入：

```cpp
require(settings.vision.maxFineCorrectionCount >= 0
            && settings.vision.maxFineCorrectionCount <= 2,
        QStringLiteral("联合精修正次数必须位于 0～2"));
```

在 `SettingsManager` 的保存、读取和修复段分别加入：

```cpp
ini->setValue(QStringLiteral("vision/maxFineCorrectionCount"),
              s.vision.maxFineCorrectionCount);
ini->remove(QStringLiteral("vision/maxGrabIterations"));
```

```cpp
readInt(QStringLiteral("vision/maxFineCorrectionCount"),
        &loaded.vision.maxFineCorrectionCount);
```

```cpp
if (loaded.vision.maxFineCorrectionCount < 0
    || loaded.vision.maxFineCorrectionCount > 2) {
    loaded.vision.maxFineCorrectionCount =
        defaults.vision.maxFineCorrectionCount;
}
```

删除对 `vision/maxGrabIterations` 的 `setValue()`、`readInt()` 和合法性修复调用。旧 INI 文件可以继续加载，但旧键不参与任何运行决策，并在用户下一次保存设置时被清除。

- [ ] **步骤 5：实现设置界面读写**

在视觉闭环页面中，删除现有：

```cpp
addInt(page,
       "visionMaxIterations",
       QStringLiteral("最大矫正次数"),
       1,
       100,
       QStringLiteral("次"));
```

在同一位置替换为：

```cpp
addInt(page,
       "visionMaxFineCorrections",
       QStringLiteral("联合精修正次数"),
       0,
       2,
       QStringLiteral("次"));
```

在 `candidate()`、`writeSettings()` 和变更摘要中使用同一个字段：

```cpp
s.vision.maxFineCorrectionCount =
    m_ints["visionMaxFineCorrections"]->value();
```

```cpp
m_ints["visionMaxFineCorrections"]->setValue(
    s.vision.maxFineCorrectionCount);
```

```cpp
addInt(QStringLiteral("联合精修正次数"),
       m_original.vision.maxFineCorrectionCount,
       s.vision.maxFineCorrectionCount,
       "次");
```

删除 `candidate()`、`writeSettings()` 和变更摘要中对 `visionMaxIterations`、`maxGrabIterations` 的旧读写。界面最终只能看到一行“联合精修正次数”，不能同时出现新旧两个次数控件。

- [ ] **步骤 6：运行测试并确认通过**

```powershell
cmake --build $BuildDirectory --target runtime_settings_tests settings_manager_tests settings_dialog_tests
ctest --test-dir $BuildDirectory -C Debug --output-on-failure -R "runtime_settings_tests|settings_manager_tests|settings_dialog_tests"
```

预期：三个测试全部通过。

- [ ] **步骤 7：提交本任务**

```powershell
git add src/runtimesettings.h src/runtimesettings.cpp src/settingsmanager.cpp src/settingsdialog.cpp tests/test_runtime_settings.cpp tests/test_settings_manager.cpp tests/test_settings_dialog.cpp
git commit -m "配置: 用联合精修次数替换旧迭代参数"
```

---

### 任务 2：建立可单测的联合对准窗口判定

**文件：**

- 新增：`src/visionalignmentdecision.h`
- 新增：`src/visionalignmentdecision.cpp`
- 新增：`tests/test_vision_alignment_decision.cpp`
- 修改：`CMakeLists.txt:34-55`
- 修改：`tests/CMakeLists.txt`

**接口：**

- 产生：

```cpp
namespace VisionAlignment {

struct Sample {
    double xMm = 0.0;
    double yMm = 0.0;
    double zMm = 0.0;
    double rzDeg = 0.0;
    bool targetValid = false;
};

struct ToolCorrection {
    double xMm = 0.0;
    double yMm = 0.0;
    double rzDeg = 0.0;
};

struct WindowPolicy {
    double xyToleranceMm = 2.0;
    double rzToleranceDeg = 1.0;
    int maxFineCorrectionCount = 1;
    qint64 minElapsedMs = 4000;
    qint64 maxElapsedMs = 8000;
};

enum class WindowAction {
    ContinueObserving,
    Descend,
    FineCorrect,
    Stop
};

struct WindowInput {
    Sample latest;
    bool zStable = false;
    qint64 elapsedMs = 0;
    int completedFineCorrectionCount = 0;
};

struct WindowDecision {
    WindowAction action = WindowAction::ContinueObserving;
    ToolCorrection correction;
    QString reason;
};

bool isPlanarAligned(const Sample &sample, const WindowPolicy &policy);
ToolCorrection toToolCorrection(const Sample &sample);
WindowDecision decideWindow(const WindowInput &input,
                            const WindowPolicy &policy);

}
```

- 消费：任务 1 的 `maxFineCorrectionCount` 在调度器中组装 `WindowPolicy`，纯判定单元本身不依赖 `RuntimeSettings`。

- [ ] **步骤 1：创建判定测试并写出完整失败矩阵**

在 `tests/test_vision_alignment_decision.cpp` 使用 Qt Test 数据驱动测试，至少包含：

```cpp
QTest::newRow("四秒前继续观察")
    << 3999LL << true << true << 0
    << VisionAlignment::WindowAction::ContinueObserving;
QTest::newRow("四秒后对准且Z稳定则下探")
    << 4000LL << true << true << 0
    << VisionAlignment::WindowAction::Descend;
QTest::newRow("四秒后失准且有余量则精修")
    << 4000LL << false << true << 0
    << VisionAlignment::WindowAction::FineCorrect;
QTest::newRow("精修次数耗尽则停止")
    << 4000LL << false << true << 1
    << VisionAlignment::WindowAction::Stop;
QTest::newRow("八秒Z仍不稳定则停止")
    << 8000LL << true << false << 0
    << VisionAlignment::WindowAction::Stop;
```

另写独立用例验证：

```cpp
QVERIFY(isPlanarAligned({2.0, -2.0, 100.0, 1.0, true}, policy));
QVERIFY(!isPlanarAligned({2.01, 0.0, 100.0, 0.0, true}, policy));
QVERIFY(!isPlanarAligned({0.0, 0.0, 100.0, 1.01, true}, policy));

const auto correction = toToolCorrection({10.0, 20.0, 100.0, 30.0, true});
QCOMPARE(correction.xMm, 10.0);
QCOMPARE(correction.yMm, -20.0);
QCOMPARE(correction.rzDeg, -30.0);
```

增加目标无效时立即 `Stop` 的用例，并验证 `reason` 非空。

- [ ] **步骤 2：注册测试并确认先失败**

在 `tests/CMakeLists.txt` 添加 `vision_alignment_decision_tests`，链接 `Qt6::Core`、`Qt6::Test`，并编译 `../src/visionalignmentdecision.cpp`。

```powershell
cmake --build $BuildDirectory --target vision_alignment_decision_tests
ctest --test-dir $BuildDirectory -C Debug --output-on-failure -R vision_alignment_decision_tests
```

预期：因为头文件和实现尚不存在而编译失败。

- [ ] **步骤 3：实现阈值与方向换算**

实现规则：

```cpp
bool VisionAlignment::isPlanarAligned(const Sample &sample,
                                      const WindowPolicy &policy)
{
    return sample.targetValid
        && qAbs(sample.xMm) <= policy.xyToleranceMm
        && qAbs(sample.yMm) <= policy.xyToleranceMm
        && qAbs(sample.rzDeg) <= policy.rzToleranceDeg;
}

VisionAlignment::ToolCorrection
VisionAlignment::toToolCorrection(const Sample &sample)
{
    return {sample.xMm, -sample.yMm, -sample.rzDeg};
}
```

方向必须与现有联机结论一致：X 同向，Y 取反，Rz 取反。

- [ ] **步骤 4：实现观察窗口决策**

`decideWindow()` 按以下固定优先级实现：

```cpp
if (!input.latest.targetValid)
    return {WindowAction::Stop, {}, QStringLiteral("锁定目标无效或已丢失")};

const bool aligned = isPlanarAligned(input.latest, policy);

if (input.elapsedMs < policy.minElapsedMs)
    return {WindowAction::ContinueObserving, {}, QString()};

if (aligned && input.zStable)
    return {WindowAction::Descend, {}, QString()};

if (input.elapsedMs >= policy.maxElapsedMs)
    return {WindowAction::Stop, {}, QStringLiteral("观察窗口达到8秒仍未同时满足对准与Z稳定")};

if (!aligned
    && input.completedFineCorrectionCount
           < policy.maxFineCorrectionCount) {
    return {WindowAction::FineCorrect,
            toToolCorrection(input.latest),
            QString()};
}

if (!aligned)
    return {WindowAction::Stop, {}, QStringLiteral("联合精修正次数已耗尽")};

return {WindowAction::ContinueObserving, {}, QString()};
```

对策略参数增加 `Q_ASSERT`：XY/Rz 阈值大于 0、精修正次数在 0～2、最小时间不小于 0、最大时间大于最小时间且不超过 8000。当前默认值固定为 4000 和 8000，未来更换硬件后允许缩短而不允许突破 8 秒硬上限。

- [ ] **步骤 5：把纯判定源文件加入主程序并运行测试**

在根 `CMakeLists.txt` 的 `PROJECT_SOURCES` 中加入：

```cmake
src/visionalignmentdecision.cpp
src/visionalignmentdecision.h
```

执行：

```powershell
cmake --build $BuildDirectory --target vision_alignment_decision_tests
ctest --test-dir $BuildDirectory -C Debug --output-on-failure -R vision_alignment_decision_tests
```

预期：全部通过。

- [ ] **步骤 6：提交本任务**

```powershell
git add CMakeLists.txt tests/CMakeLists.txt src/visionalignmentdecision.h src/visionalignmentdecision.cpp tests/test_vision_alignment_decision.cpp
git commit -m "测试: 建立视觉联合对准窗口判定"
```

---

### 任务 3：扩展调度命令模型并接入华沿联合运动接口

**文件：**

- 新增：`tests/test_vision_joint_motion_contract.cpp`
- 修改：`tests/CMakeLists.txt`
- 修改：`src/huayanScheduler.h:247-380, 460-490`
- 修改：`src/huayanScheduler.cpp:681-850, 2442-2535`

**接口：**

- 产生：

```cpp
enum class PendingCommandKind {
    None,
    RunFunc,
    MoveRelTool,
    MoveRelBase,
    MoveJ,
    VisionPregraspMoveJ,
    VisionFineCorrectionMoveL
};
```

- 扩展 `PendingCommand`：

```cpp
Pose targetPose;                         ///< 绝对目标位姿或联合相对位姿。
std::array<double, 6> referenceJoints{}; ///< 初始视觉 MoveJ 使用的当前实际关节参考。
```

- 新增：

```cpp
bool dispatchVisionPregraspMoveJ(const PendingCommand &cmd);
bool dispatchVisionFineCorrectionMoveL(const PendingCommand &cmd);
```

- 消费：任务 2 的 `VisionAlignment::ToolCorrection`，由后续任务负责组装命令。

- [ ] **步骤 1：写联合 SDK 调用契约的失败测试**

在 `tests/test_vision_joint_motion_contract.cpp` 读取 `src/huayanScheduler.h/.cpp`，沿用现有契约测试的 `requireContainsInOrder()`，断言：

```cpp
requireContainsInOrder(source,
    {QStringLiteral("case PendingCommandKind::VisionPregraspMoveJ"),
     QStringLiteral("HRIF_WayPoint("),
     QStringLiteral("cmd.referenceJoints[0]"),
     QStringLiteral("cmd.referenceJoints[5]")},
    "初始联合对准必须通过 HRIF_WayPoint 使用当前关节参考");

requireContainsInOrder(source,
    {QStringLiteral("case PendingCommandKind::VisionFineCorrectionMoveL"),
     QStringLiteral("HRIF_WayPointRel("),
     QStringLiteral("cmd.targetPose.x"),
     QStringLiteral("cmd.targetPose.y"),
     QStringLiteral("cmd.targetPose.rz")},
    "精修正必须通过一次 HRIF_WayPointRel 联合下发 X/Y/Rz");
```

并断言联合精修正参数中：

- `nType = 1`，表示线性运动；
- `nPointList = 0`；
- `nrelMoveType = 1`，表示叠加相对量；
- X、Y、Rz 掩码为 1；
- Z、Rx、Ry 掩码为 0；
- TCP 名称和 UCS 名称沿用当前调度器实际配置；
- 速度、加速度、圆滑半径分别使用 `m_runtimeSettings.motion.velocity`、`m_runtimeSettings.motion.acceleration`、`m_runtimeSettings.motion.radius`，不写魔法常量。

- [ ] **步骤 2：注册契约测试并确认先失败**

```powershell
cmake --build $BuildDirectory --target vision_joint_motion_contract_tests
ctest --test-dir $BuildDirectory -C Debug --output-on-failure -R vision_joint_motion_contract_tests
```

预期：命令类型和 SDK 分支尚不存在，契约测试失败。

- [ ] **步骤 3：扩展命令载荷和状态日志**

在 `PendingCommandKind` 与 `PendingCommand` 中加入上述字段。两个新命令必须沿用现有：

- `beginCommandWhenReady()` 控制器状态门控；
- `diagnosticCommandId`；
- 运动超时；
- `m_pollTimer` 到位轮询；
- 华沿返回码转中文错误日志；
- fail-closed 停止路径。

在命令日志中输出：

```text
[阶段一][初始联合MoveJ] Base目标=(X,Y,Z,Rx,Ry,Rz)，关节参考=(J1...J6)
[阶段一][联合精修MoveL] Tool增量=(X,Y,0,0,0,Rz)，第N/M次
```

- [ ] **步骤 4：实现初始绝对 MoveJ 分支**

在 `dispatchReadyCommand()` 中为 `VisionPregraspMoveJ` 调用：

```cpp
HRIF_WayPoint(
    m_boxID, m_rbtID,
    0,
    cmd.targetPose.x, cmd.targetPose.y, cmd.targetPose.z,
    cmd.targetPose.rx, cmd.targetPose.ry, cmd.targetPose.rz,
    cmd.referenceJoints[0], cmd.referenceJoints[1],
    cmd.referenceJoints[2], cmd.referenceJoints[3],
    cmd.referenceJoints[4], cmd.referenceJoints[5],
    tcpName, ucsName,
    velocity, acceleration, radius,
    0, 0, 0, 0, commandId);
```

其中 `nMoveType=0` 表示 MoveJ，`nIsUseJoint=0` 表示目标仍是笛卡尔位姿，J1～J6 只作为逆解参考。不得把关节参考替换为六个 0。

- [ ] **步骤 5：实现联合相对 MoveL 分支**

在 `dispatchReadyCommand()` 中为 `VisionFineCorrectionMoveL` 调用：

```cpp
HRIF_WayPointRel(
    m_boxID, m_rbtID,
    1, 0,
    0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0,
    1,
    1, 1, 0, 0, 0, 1,
    cmd.targetPose.x,
    cmd.targetPose.y,
    0,
    0,
    0,
    cmd.targetPose.rz,
    tcpName, ucsName,
    velocity, acceleration, radius,
    0, 0, 0, 0, commandId);
```

SDK 参数含义必须用中文逐项注释，尤其说明该命令是“工具位姿叠加量、X/Y/Rz 三轴在同一个线性轨迹命令中完成”，避免后续误改为三个 `HRIF_MoveRelL`。

- [ ] **步骤 6：运行契约测试和现有调度契约**

```powershell
cmake --build $BuildDirectory --target vision_joint_motion_contract_tests huayan_scheduler_contract_tests
ctest --test-dir $BuildDirectory -C Debug --output-on-failure -R "vision_joint_motion_contract_tests|huayan_scheduler_contract_tests"
```

预期：联合运动契约通过；若旧调度契约仍绑定逐轴逻辑，只删除或改写与阶段一逐轴抓取直接相关的断言，其他契约不得放宽。

- [ ] **步骤 7：提交本任务**

```powershell
git add src/huayanScheduler.h src/huayanScheduler.cpp tests/CMakeLists.txt tests/test_vision_joint_motion_contract.cpp tests/test_huayan_scheduler_contract.cpp
git commit -m "功能: 接入视觉联合MoveJ和MoveL命令"
```

---

### 任务 4：读取实际位姿与关节角并组合绝对预抓取位

**文件：**

- 修改：`src/huayanScheduler.h:290-330, 390-420, 460-490`
- 修改：`src/huayanScheduler.cpp:500-540, 1624-1860, 2195-2250`
- 修改：`tests/test_vision_joint_motion_contract.cpp`

**接口：**

- 产生：

```cpp
struct RobotPoseAndJoints {
    Pose actualTcp;
    std::array<double, 6> actualJoints{};
};

bool readActualPoseAndJoints(RobotPoseAndJoints *snapshot,
                            QString *error) const;
bool composeVisionPregraspPose(
    const Pose &capturePose,
    const VisionAlignment::ToolCorrection &correction,
    Pose *pregraspPose,
    QString *error) const;
bool queueInitialVisionPregrasp(
    const VisionAlignment::Sample &sample);
```

- 消费：任务 2 的 `toToolCorrection()` 和任务 3 的 `VisionPregraspMoveJ`。
- 产生：后续观察窗口使用的 `m_pendingAlignmentCorrection`，只在运动确认完成后累计到锚点。

- [ ] **步骤 1：扩展契约测试，先描述读取与组合顺序**

增加以下顺序断言：

```cpp
requireContainsInOrder(queueBody,
    {QStringLiteral("readActualPoseAndJoints"),
     QStringLiteral("VisionAlignment::toToolCorrection"),
     QStringLiteral("composeVisionPregraspPose"),
     QStringLiteral("PendingCommandKind::VisionPregraspMoveJ"),
     QStringLiteral("beginCommandWhenReady")},
    "首次视觉结果必须先读取实位姿与关节，再组合目标并下发");
```

对 `composeVisionPregraspPose()` 函数体断言：

```cpp
requireContainsInOrder(composeBody,
    {QStringLiteral("HRIF_PoseTrans("),
     QStringLiteral("capturePose.x"),
     QStringLiteral("correction.xMm"),
     QStringLiteral("correction.yMm"),
     QStringLiteral("correction.rzDeg"),
     QStringLiteral("pregraspPose->x")},
    "绝对预抓取位必须由拍照位实际TCP右乘工具系联合修正得到");
```

并断言相对位姿的 Z、Rx、Ry 都传 0。

- [ ] **步骤 2：运行契约测试并确认失败**

```powershell
cmake --build $BuildDirectory --target vision_joint_motion_contract_tests
ctest --test-dir $BuildDirectory -C Debug --output-on-failure -R vision_joint_motion_contract_tests
```

预期：读取和组合函数尚不存在，测试失败。

- [ ] **步骤 3：实现同一时刻的 TCP 与关节快照**

`readActualPoseAndJoints()` 必须依次调用：

```cpp
HRIF_ReadActTcpPos(m_boxID, m_rbtID,
                   snapshot->actualTcp.x,
                   snapshot->actualTcp.y,
                   snapshot->actualTcp.z,
                   snapshot->actualTcp.rx,
                   snapshot->actualTcp.ry,
                   snapshot->actualTcp.rz);

HRIF_ReadActJointPos(m_boxID, m_rbtID,
                     snapshot->actualJoints[0],
                     snapshot->actualJoints[1],
                     snapshot->actualJoints[2],
                     snapshot->actualJoints[3],
                     snapshot->actualJoints[4],
                     snapshot->actualJoints[5]);
```

任一返回码非 0 时填充包含 SDK 返回码和 `describeError()` 文本的错误消息并返回 `false`，不得使用部分有效快照继续运动。

- [ ] **步骤 4：使用 SDK 位姿运算组合预抓取位**

`composeVisionPregraspPose()` 调用：

```cpp
const int ret = HRIF_PoseTrans(
    m_boxID, m_rbtID,
    capturePose.x, capturePose.y, capturePose.z,
    capturePose.rx, capturePose.ry, capturePose.rz,
    correction.xMm, correction.yMm, 0,
    0, 0, correction.rzDeg,
    pregraspPose->x, pregraspPose->y, pregraspPose->z,
    pregraspPose->rx, pregraspPose->ry, pregraspPose->rz);
```

这对应：

```text
T_base_pregrasp =
    T_base_capture ×
    Trans(toolX, -toolY, 0) ×
    RotZ(-normalizedRz)
```

不得用 `capturePose.x += correction.xMm` 一类欧拉角与平移直接相加替代 SDK 位姿组合。

- [ ] **步骤 5：实现首次视觉结果到初始命令的组装**

`queueInitialVisionPregrasp()` 执行：

1. 读取实际 TCP 和 J1～J6；
2. 通过 `toToolCorrection(sample)` 得到 X、-Y、-Rz；
3. 保持修正 Z=0、Rx=0、Ry=0；
4. 调用 `composeVisionPregraspPose()`；
5. 保存本次拟执行的工具系 X/Y/Rz 到 `m_pendingAlignmentCorrection`；
6. 创建 `VisionPregraspMoveJ`，目标为组合后的绝对位姿，关节参考为当前实际关节；
7. 通过 `beginCommandWhenReady()` 下发。

若读取、组合或命令门控失败，调用现有 `emitOperationError()` 停止调度。

- [ ] **步骤 6：确认契约和主程序可以编译**

```powershell
cmake --build $BuildDirectory --target vision_joint_motion_contract_tests wh-robot-visual
ctest --test-dir $BuildDirectory -C Debug --output-on-failure -R vision_joint_motion_contract_tests
```

预期：契约通过，主程序链接成功。

- [ ] **步骤 7：提交本任务**

```powershell
git add src/huayanScheduler.h src/huayanScheduler.cpp tests/test_vision_joint_motion_contract.cpp
git commit -m "功能: 组合视觉绝对预抓取位"
```

---

### 任务 5：把阶段一迁移为一次初始联合对准和有限联合精修正

**文件：**

- 修改：`src/huayanScheduler.h:247-330, 460-490`
- 修改：`src/huayanScheduler.cpp:504-540, 681-850, 873-1005, 1150-1205, 1624-2055`
- 修改：`tests/test_huayan_scheduler_contract.cpp`
- 修改：`tests/test_vision_joint_motion_contract.cpp`

**接口：**

- 新增阶段步骤：

```cpp
MoveToPregrasp,          ///< 一次绝对 MoveJ 到目标正上方。
ValidateVisionAlignment, ///< 运动后统一验证 XY/Rz 和 Z。
FineCorrectAlignment,    ///< 一次联合 MoveL 精修正。
```

- 新增成员：

```cpp
bool m_initialVisionMoveCompleted = false;
int m_completedFineCorrectionCount = 0;
VisionAlignment::ToolCorrection m_pendingAlignmentCorrection;
```

- 新增方法：

```cpp
bool queueVisionFineCorrection(
    const VisionAlignment::ToolCorrection &correction);
void recordCompletedVisionAlignmentMove();
void enterVisionAlignmentValidation();
```

- 删除阶段一专用成员和方法：
  - `m_grabMoves`
  - `m_grabMoveIdx`
  - `m_grabIterations`
  - `executeNextGrabMove()`
  - `recordCompletedGrabMove()`
  - `validateStageOneRelMoveBeforeDispatch()`
- 保留 `RelMove` 和 `m_palletMoves`，因为码垛路径仍使用它们。

- [ ] **步骤 1：先写状态迁移和旧循环退出契约**

在联合运动契约测试中断言：

```cpp
requireTrue(!header.contains(QStringLiteral("m_grabMoves")),
            "阶段一不得继续持有逐轴抓取动作队列");
requireTrue(!header.contains(QStringLiteral("m_grabIterations")),
            "新闭环不得继续使用旧15轮计数");
requireTrue(!source.contains(QStringLiteral("executeNextGrabMove()")),
            "生产路径不得继续执行逐轴抓取循环");
requireTrue(!source.contains(QStringLiteral(
                "m_runtimeSettings.vision.maxGrabIterations")),
            "新生产路径不得读取旧最大迭代参数");
```

断言完成顺序：

```cpp
requireContainsInOrder(completionBranch,
    {QStringLiteral("recordCompletedVisionAlignmentMove()"),
     QStringLiteral("enterVisionAlignmentValidation()")},
    "联合运动必须确认完成后再累计锚点并开启视觉窗口");
```

- [ ] **步骤 2：运行契约测试并确认失败**

```powershell
cmake --build $BuildDirectory --target vision_joint_motion_contract_tests huayan_scheduler_contract_tests
ctest --test-dir $BuildDirectory -C Debug --output-on-failure -R "vision_joint_motion_contract_tests|huayan_scheduler_contract_tests"
```

预期：旧逐轴成员仍存在，新状态迁移不存在，测试失败。

- [ ] **步骤 3：重置阶段一有限闭环状态**

在 `startStageOne()` 中固定执行：

```cpp
m_initialVisionMoveCompleted = false;
m_completedFineCorrectionCount = 0;
m_pendingAlignmentCorrection = {};
resetStableZValidation();
resetVisionAnchorTracking();
```

初始进入拍照位和 `WaitForVision` 的现有逻辑保持不变。

- [ ] **步骤 4：让首次有效目标只创建一次初始 MoveJ**

在 `setGrabOffset()` 完成现有目标可信、深度、Rz 归一化和大角度确认后：

- 若 `m_initialVisionMoveCompleted == false`，调用 `queueInitialVisionPregrasp(sample)`，固定只排队一条初始联合 MoveJ；
- 设置 `StageStep::MoveToPregrasp`；
- 不创建 `m_grabMoves`；
- 不递增旧迭代计数；
- 不直接进入 Z 下探。

首次样本即使已经落在 XY/Rz 阈值内，也不得进入另一套分支；仍按相同的位姿组合和单条 MoveJ 路径执行，以确保“首次联合 MoveJ 固定一次”的状态与日志语义唯一。

- [ ] **步骤 5：实现联合精修正命令**

`queueVisionFineCorrection()` 先分别验证两个平移轴：

```cpp
qAbs(correction.xMm)
    <= m_runtimeSettings.safety.maxSingleXyAdjustMm
qAbs(correction.yMm)
    <= m_runtimeSettings.safety.maxSingleXyAdjustMm
```

Rz 继续使用现有大角度确认和“一次大角度执行”结果，不新增修正专用上限。验证通过后：

```cpp
PendingCommand cmd;
cmd.kind = PendingCommandKind::VisionFineCorrectionMoveL;
cmd.targetPose.x = correction.xMm;
cmd.targetPose.y = correction.yMm;
cmd.targetPose.z = 0;
cmd.targetPose.rx = 0;
cmd.targetPose.ry = 0;
cmd.targetPose.rz = correction.rzDeg;
m_pendingAlignmentCorrection = correction;
m_stageStep = StageStep::FineCorrectAlignment;
return beginCommandWhenReady(cmd);
```

在真正成功排队后递增“已下发”或“已完成”计数只能选一种语义。本计划统一使用“已完成”：仅在运动轮询确认完成后执行 `++m_completedFineCorrectionCount`。

- [ ] **步骤 6：运动完成后再更新锚点累计量**

`recordCompletedVisionAlignmentMove()` 必须执行：

```cpp
m_anchorAccumulatedToolX += m_pendingAlignmentCorrection.xMm;
m_anchorAccumulatedToolY += m_pendingAlignmentCorrection.yMm;
m_pendingAlignmentCorrection = {};
```

初始 MoveJ 完成时设置 `m_initialVisionMoveCompleted = true`；联合精修 MoveL 完成时递增 `m_completedFineCorrectionCount`。SDK 下发失败、运动超时或调度停止时不得累计。

- [ ] **步骤 7：统一 MoveJ 和精修 MoveL 的完成分支**

在 `onPollTick()` 的运动完成分支中：

1. 识别 `MoveToPregrasp` 或 `FineCorrectAlignment`；
2. 调用 `recordCompletedVisionAlignmentMove()`；
3. 调用 `enterVisionAlignmentValidation()`；
4. 不再等待 300ms 后执行下一轴；
5. 不再返回旧 `MoveToGrab` 队列。

- [ ] **步骤 8：删除阶段一逐轴实现并修订旧契约**

删除 `executeNextGrabMove()` 和仅供它使用的阶段一校验函数。`tests/test_huayan_scheduler_contract.cpp` 中：

- 删除“X/Y/Rz 逐轴队列”和 `m_grabMoveIdx` 的断言；
- 保留并更新“运动完成后才累计锚点”的意图；
- 保留调度停止信号、20018 诊断、控制器状态门控、Z 下探保护和码垛相关断言。

- [ ] **步骤 9：运行调度与联合运动契约**

```powershell
cmake --build $BuildDirectory --target vision_joint_motion_contract_tests huayan_scheduler_contract_tests wh-robot-visual
ctest --test-dir $BuildDirectory -C Debug --output-on-failure -R "vision_joint_motion_contract_tests|huayan_scheduler_contract_tests"
```

预期：契约通过，主程序编译链接成功。

- [ ] **步骤 10：提交本任务**

```powershell
git add src/huayanScheduler.h src/huayanScheduler.cpp tests/test_huayan_scheduler_contract.cpp tests/test_vision_joint_motion_contract.cpp
git commit -m "重构: 阶段一切换为有限联合对准"
```

---

### 任务 6：把 XY/Rz 最终确认与 Z 稳定合并到同一个 4～8 秒窗口

**文件：**

- 修改：`src/huayanScheduler.h:306-325, 475-485`
- 修改：`src/huayanScheduler.cpp:25-35, 944-980, 1527-1623, 1624-2010`
- 修改：`tests/test_stable_z_validation_contract.cpp`
- 修改：`tests/test_vision_joint_motion_contract.cpp`
- 修改：`tests/test_depth_descent_contract.cpp`

**接口：**

- 将 `ValidateStableZ` 生产状态替换为 `ValidateVisionAlignment`。
- 保留现有新帧去重字段和 Z 样本窗口，字段可重命名但语义不变。
- `enterVisionAlignmentValidation()` 每次联合运动完成后重新清空样本并启动计时。
- `setGrabOffset()` 在观察状态内构造 `VisionAlignment::WindowInput`，调用 `VisionAlignment::decideWindow()`。

- [ ] **步骤 1：先改写稳定窗口契约**

在 `tests/test_stable_z_validation_contract.cpp` 中要求：

```cpp
requireTrue(source.contains(QStringLiteral("kStableZMinElapsedMs = 4000")),
            "统一观察窗口最短时间必须保持4秒");
requireTrue(source.contains(QStringLiteral("kStableZMaxElapsedMs = 8000")),
            "统一观察窗口硬上限必须保持8秒");
requireTrue(source.contains(QStringLiteral(
                "m_runtimeSettings.vision.xyToleranceMm")),
            "统一窗口必须使用运行时XY阈值");
requireTrue(source.contains(QStringLiteral(
                "m_runtimeSettings.vision.rzToleranceDeg")),
            "统一窗口必须使用运行时Rz阈值");
requireTrue(!source.contains(QStringLiteral("kRzTolerance")),
            "调度器不得继续使用硬编码Rz阈值");
```

增加动作分支顺序断言：

```cpp
ContinueObserving -> requestNextStableZFrame
Descend -> StageStep::DescendZ
FineCorrect -> queueVisionFineCorrection
Stop -> emitOperationError
```

- [ ] **步骤 2：运行三个契约测试并确认先失败**

```powershell
cmake --build $BuildDirectory --target stable_z_validation_contract_tests vision_joint_motion_contract_tests depth_descent_contract_tests
ctest --test-dir $BuildDirectory -C Debug --output-on-failure -R "stable_z_validation_contract_tests|vision_joint_motion_contract_tests|depth_descent_contract_tests"
```

预期：统一窗口动作分支和运行时 Rz 阈值尚未完全接入，测试失败。

- [ ] **步骤 3：每次运动后重新开启窗口**

`enterVisionAlignmentValidation()` 必须：

```cpp
resetStableZValidation();
m_stableZLastFrameId = m_visionClient->lastInferenceFrameId();
m_stableZLastTimestampMs = m_visionClient->lastInferenceTimestampMs();
m_stableZElapsedTimer.start();
m_stageStep = StageStep::ValidateVisionAlignment;
requestNextStableZFrame();
```

记录运动完成时的帧号和时间戳作为基线，重复缓存响应不得计入真实新帧。

- [ ] **步骤 4：在每个真实新帧上同时更新 XY/Rz 与 Z**

观察状态收到视觉结果时：

1. 继续执行现有锚点目标一致性和可信范围检查；
2. 使用已经归一化并经过大角度保护的 Rz；
3. 仅对不同 `frame_id` 且时间戳更新的帧增加 `m_stableZUniqueFrames`；
4. 把 Z 加入最近三帧窗口；
5. 用最近三帧最大值减最小值不大于 5mm 判断 `zStable`；
6. 用运行时 `xyToleranceMm`、`rzToleranceDeg` 和 `maxFineCorrectionCount` 组装 `WindowPolicy`；
7. 调用 `decideWindow()`。

不得在 4 秒前因为单帧对准直接下探。

- [ ] **步骤 5：实现四种窗口动作**

使用显式 `switch`：

```cpp
switch (decision.action) {
case VisionAlignment::WindowAction::ContinueObserving:
    requestNextStableZFrame();
    return;
case VisionAlignment::WindowAction::Descend:
    m_grabOffset.z = sample.zMm;
    m_stageStep = StageStep::DescendZ;
    proceedStage();
    return;
case VisionAlignment::WindowAction::FineCorrect:
    resetStableZValidation();
    queueVisionFineCorrection(decision.correction);
    return;
case VisionAlignment::WindowAction::Stop:
    emitOperationError(
        QStringLiteral("[阶段一][联合对准] %1").arg(decision.reason));
    return;
}
```

精修正后必须等待运动确认完成，再重新开启一个新的 4～8 秒窗口；不得额外“再等一帧”，也不得复用上一个窗口已累计的 Z 样本。

- [ ] **步骤 6：统一所有视觉失败入口**

以下回调在 `ValidateVisionAlignment` 状态都调用 `emitOperationError()`：

- `onVisionNoObject()`；
- 锚点可信拒绝；
- `onVisionError()`；
- 8 秒仍无足够真实新帧；
- 8 秒 Z 仍不稳定；
- 达到配置精修正次数后 XY/Rz 仍失准。

错误日志必须包含：工位、锁定目标锚点、当前 X/Y/Z/Rz、XY/Rz 阈值、已完成/允许精修正次数、窗口耗时、真实新帧数和停止原因。

- [ ] **步骤 7：确认 Z 下探算法未改变**

`tests/test_depth_descent_contract.cpp` 保持并补强以下契约：

```text
plannedDescend = m_grabOffset.z - m_grabZClearance
plannedDescend > HUAYAN_MAX_Z_DESCEND_MM 时拒绝下发
最终下探仍是工具 Z
扫码和夹爪步骤顺序不变
```

新流程只更新进入 `DescendZ` 的门槛，不修改深度物理含义和运动方向。

- [ ] **步骤 8：运行相关测试和主程序构建**

```powershell
cmake --build $BuildDirectory --target stable_z_validation_contract_tests vision_joint_motion_contract_tests depth_descent_contract_tests huayan_scheduler_contract_tests wh-robot-visual
ctest --test-dir $BuildDirectory -C Debug --output-on-failure -R "stable_z_validation_contract_tests|vision_joint_motion_contract_tests|depth_descent_contract_tests|huayan_scheduler_contract_tests"
```

预期：全部通过。

- [ ] **步骤 9：提交本任务**

```powershell
git add src/huayanScheduler.h src/huayanScheduler.cpp tests/test_stable_z_validation_contract.cpp tests/test_vision_joint_motion_contract.cpp tests/test_depth_descent_contract.cpp
git commit -m "功能: 合并视觉对准与深度稳定窗口"
```

---

### 任务 7：补齐停止语义、诊断日志和回归保护

**文件：**

- 修改：`src/huayanScheduler.cpp:500-540, 1500-2050, 2442-2535`
- 修改：`tests/test_huayan_scheduler_contract.cpp`
- 修改：`tests/test_vision_joint_motion_contract.cpp`
- 修改：`tests/test_stable_z_validation_contract.cpp`

**接口：**

- 失败统一通过现有 `emitOperationError(const QString &message)` 收口。
- `emitOperationError()` 必须停止定时器、作废异步回调并发出停止信号。
- 不新增自动返回拍照位、整轮重试或目标重选入口。

- [ ] **步骤 1：写安全停止的失败契约**

增加源码契约，验证每个失败分支都包含 `emitOperationError()`，并验证联合观察/精修正分支内不存在：

```cpp
QStringLiteral("MoveToSurvey")
QStringLiteral("startStageOne()")
QStringLiteral("m_grabIterations")
QStringLiteral("maxGrabIterations")
```

同时验证 `stop()` 或 `emitOperationError()` 会：

- 停止视觉等待和运动超时计时器；
- 通过命令序号使旧 `QTimer::singleShot` 回调失效；
- 清空待执行联合修正；
- 不触发 `CloseGripper`。

- [ ] **步骤 2：运行契约测试并确认失败**

```powershell
cmake --build $BuildDirectory --target vision_joint_motion_contract_tests huayan_scheduler_contract_tests stable_z_validation_contract_tests
ctest --test-dir $BuildDirectory -C Debug --output-on-failure -R "vision_joint_motion_contract_tests|huayan_scheduler_contract_tests|stable_z_validation_contract_tests"
```

预期：尚未补齐的日志字段或清理动作使契约失败。

- [ ] **步骤 3：补齐运动前、运动后和窗口日志**

至少输出下列结构化中文日志：

```text
[阶段一][视觉输入] frame=... anchor=(...) raw=(X,Y,Z,Rz) normalizedRz=...
[阶段一][位姿组合] captureBase=(...) toolDelta=(X,Y,0,0,0,Rz) pregraspBase=(...)
[阶段一][初始联合MoveJ] command=... referenceJoints=(...)
[阶段一][联合运动完成] kind=初始/精修 accumulatedToolXY=(...) completedFine=N/M
[阶段一][统一窗口] elapsed=...ms uniqueFrames=... XY/Rz=(...) ZRange=... action=...
[阶段一][安全停止] station=... anchor=... reason=...
```

日志数值明确单位：平移 mm、旋转 °、时间 ms。

- [ ] **步骤 4：补齐停止时状态清理**

在阶段停止/错误清理路径中：

```cpp
m_pendingAlignmentCorrection = {};
resetStableZValidation();
stopVisionWaitTimeout();
nextCallbackSeq();
```

保留锁定目标和现场诊断所需数据到停止日志输出完成后再清理。不得排队拍照位函数或新的视觉请求。

- [ ] **步骤 5：验证 Rz 大角度保护完整保留**

契约测试继续要求：

- 大于 `largeRzJumpThresholdDeg` 时需要两个真实帧确认；
- 两帧差不超过 `largeRzDeltaToleranceDeg`；
- 同一锁定目标大角度执行不超过 `maxLargeRzExecutions`；
- 初始 MoveJ 和精修 MoveL 共用同一计数，不能每次开启观察窗口都清零。

- [ ] **步骤 6：运行全部阶段一相关测试**

```powershell
cmake --build $BuildDirectory --target vision_alignment_decision_tests vision_joint_motion_contract_tests huayan_scheduler_contract_tests stable_z_validation_contract_tests depth_descent_contract_tests vision_target_selection_tests anchor_target_selection_tests locked_target_selection_tests
ctest --test-dir $BuildDirectory -C Debug --output-on-failure -R "vision_alignment_decision_tests|vision_joint_motion_contract_tests|huayan_scheduler_contract_tests|stable_z_validation_contract_tests|depth_descent_contract_tests|vision_target_selection_tests|anchor_target_selection_tests|locked_target_selection_tests"
```

预期：全部通过，且目标选择相关测试没有变化。

- [ ] **步骤 7：提交本任务**

```powershell
git add src/huayanScheduler.cpp tests/test_huayan_scheduler_contract.cpp tests/test_vision_joint_motion_contract.cpp tests/test_stable_z_validation_contract.cpp
git commit -m "安全: 完善联合对准失败收口与诊断"
```

---

### 任务 8：执行全量构建、自动化回归和现场分级验收

**文件：**

- 验证：`D:\project\CompositeRobot\code\C++\robot_visual`
- 不新增代码文件。

**接口：**

- 消费：任务 1～7 的全部实现。
- 产生：可供用户审核的构建、CTest 和现场验收记录；本任务不推送远端。

- [ ] **步骤 1：检查工作区，隔离用户原有改动**

```powershell
git status --short
git diff --name-only HEAD
```

确认本计划提交没有包含 `AGENTS.md`、`agents.d`、`Testing` 或其他用户原有改动。

- [ ] **步骤 2：在同一个 PowerShell 进程初始化固定 Qt/MSVC 环境**

```powershell
$vsEnvLines = & cmd.exe /d /c '"D:\Tool\AInstall\VS2022\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && set'

foreach ($vsEnvLine in $vsEnvLines) {
    $separatorIndex = $vsEnvLine.IndexOf('=')
    if ($separatorIndex -gt 0) {
        $envName = $vsEnvLine.Substring(0, $separatorIndex)
        $envValue = $vsEnvLine.Substring($separatorIndex + 1)
        Set-Item -Path "Env:$envName" -Value $envValue
    }
}

$env:Path = "D:\Tool\AInstall\Qt\Tools\QtCreator\bin\jom;" +
            "D:\Tool\AInstall\Qt\6.8.3\msvc2022_64\bin;" +
            $env:Path
$env:CMAKE_PREFIX_PATH = "D:\Tool\AInstall\Qt\6.8.3\msvc2022_64"
$env:QT_QPA_PLATFORM = "offscreen"
```

- [ ] **步骤 3：直接构建已有的有效构建目录**

```powershell
$BuildDirectory = "build\Desktop_Qt_6_8_3_MSVC2022_64bit_Debug"
Test-Path "$BuildDirectory\CMakeCache.txt"
cmake --build $BuildDirectory --config Debug
```

预期：`CMakeCache.txt` 存在，完整工程构建成功。

- [ ] **步骤 4：运行全量 CTest**

```powershell
Push-Location $BuildDirectory
try {
    ctest -C Debug --output-on-failure
}
finally {
    Pop-Location
}
```

预期：全部测试通过。若所有测试均以 `0xc0000135` 退出，先确认 Qt `bin` 已进入当前进程 PATH，再重跑；不得把运行库缺失误判为业务失败。

- [ ] **步骤 5：执行无机械臂运动的日志演练**

使用现有模拟/断开机器人模式输入固定视觉样本，核对：

1. 初始样本只生成一条绝对联合 MoveJ 意图；
2. 工具增量符号为 X、-Y、-Rz，Z/Rx/Ry 为 0；
3. J1～J6 来自实际读取值而不是六个 0；
4. 运动完成前不更新锚点累计位移；
5. 4 秒前不下探；
6. 8 秒时安全停止；
7. 配置 0、1、2 分别允许 0、1、2 次联合精修正；
8. 失败后不返回拍照位、不重选目标、不夹紧。

- [ ] **步骤 6：低速空载上机验收**

在夹爪无工件、工位清空、人工可急停条件下：

1. 将机器人全局速度降到现场批准的调试速度；
2. 对每个固定工位各选一个本工位合法目标；
3. 确认 MoveJ 从固定拍照位一次到目标正上方；
4. 确认 Z、Rx、Ry 在初始联合对准中没有视觉修正；
5. 确认联合精修正是一条 MoveL，而不是 X/Y/Rz 三次启停；
6. 故意设置严格阈值触发精修正次数耗尽，确认调度立即停止；
7. 恢复现场阈值前不得进入带料测试。

- [ ] **步骤 7：单件带料验收**

每个工位至少记录以下数据：

```text
拍照位实际 TCP 与 J1～J6
视觉原始 X/Y/Z/Rz
组合后的 Base 预抓取位
初始 MoveJ 实际耗时
每次观察窗口耗时和真实新帧数
精修正次数
最终 XY/Rz 残差
Z 三帧极差
下探量
成功/停止原因
```

验收通过条件：

- 合法目标上方路径与现场确认一致；
- 未出现逐轴三次启停；
- 最终下探前 XY、Y、Rz 分别满足运行时阈值且 Z 稳定；
- 任何不收敛样本均在配置次数或 8 秒硬上限内停止；
- 不发生自动返回拍照位和整轮重试。

- [ ] **步骤 8：形成提交清单并等待用户决定是否推送**

```powershell
git log --oneline --decorate -8
git status --short
```

向用户报告：

- 各任务提交哈希；
- 完整构建结果；
- CTest 通过数；
- 尚需真实机器人执行的验收项；
- 工作区中保留的用户原有改动。

不得执行 `git push`。只有用户明确同意后才能推送。

---

## 实施完成后的逻辑对照检查

执行者在宣布完成前逐项核对：

| 检查项 | 旧逻辑 | 完成后的逻辑 |
|---|---|---|
| 首次 XY/Rz 对准 | 三条 `MoveRelL` 逐轴执行 | 一条 `HRIF_WayPoint` MoveJ |
| 首次目标位计算 | 每轴相对移动 | 实际 TCP 右乘工具系联合修正 |
| MoveJ 关节参考 | 不适用或六个 0 | 当前实际 J1～J6 |
| 精修正 | 最多 15 轮逐轴动作 | 配置 0～2 次联合 MoveL，默认 1 |
| 阈值 | XY 配置、Rz 存在硬编码 | XY、Rz 均读取运行时配置 |
| 视觉等待 | 每轮动作后反复等待 | 每次联合运动后一个 4～8 秒统一窗口 |
| XY/Rz 与 Z | 分阶段确认 | 同一个观察窗口确认 |
| 失败处理 | 可能回到旧视觉循环 | 直接停止调度 |
| 自动重试 | 可能继续下一轮 | 不返回拍照位、不重选目标 |
| Python 与目标选择 | 当前实现 | 保持不变 |
| Z 下探与夹爪 | 当前实现 | 保持不变 |
| 6D、碰撞规划 | 不具备 | 本轮不增加 |
