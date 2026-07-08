# 机械臂取料安全修复实施计划

> **给自动化执行者：** 必须使用子技能 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans` 按任务逐项实施本计划。步骤使用复选框 `- [ ]` 记录执行状态。

**目标：** 修复三个现场问题：按工位配置抓取 Z 下探余量、降低/消除 20018 命令状态错误、避免工位 12 夹取后误复用 `Func_capture12`。

**架构：** 保持现有 `TaskExecutor` 调用 `HuayanScheduler` 的业务流程不变，只在现有业务文件里增加显式配置、纯辅助函数和 SDK 命令状态门控。新增测试代码放在独立 `tests/` 目录，通过 CMake/CTest 接入，便于 Qt Creator 展示，不和 `src/` 业务代码混放。

**技术栈：** C++17、Qt6、CMake、CTest、华沿 HRIF SDK、现有 Qt 信号和定时器状态机。

## 全局约束

- 业务代码不新增 `.h` 或 `.cpp` 文件；业务改动只允许落在现有 `src/lineconfig.h`、`src/taskexecutor.cpp`、`src/huayanScheduler.h`、`src/huayanScheduler.cpp`。
- 允许新增测试目录和测试代码：`tests/CMakeLists.txt`、`tests/test_station_pickup_config.cpp`。
- 测试代码必须和 `src/` 分开，Qt Creator 打开工程时应能看到独立 `tests` 目录。
- 允许修改根 `CMakeLists.txt` 接入 CTest 和 `tests/` 子目录。
- 不修改无关 UI、AGV、扫码枪、码垛、相机逻辑。
- 保持原有主流程，只修复本次明确提出的三个 bug。
- 所有新增枚举、字段、辅助函数、接口、安全分支都必须写清楚中文注释，说明用途、单位、默认值和安全行为。
- 不使用 Python 做验证。
- 验证必须包含 CMake 构建、CTest 和 `rg` 静态检查；现场慢速日志检查只要求用于会改变机械臂实际行为的任务以及最终整体验收。
- 20018 属于安全相关的命令状态问题，不能只通过增大固定延时解决。
- 每个任务可以本地提交，提交信息使用中文；不要执行 `git push`。

---

## 文件结构

- 新增：`tests/CMakeLists.txt`
  - 定义独立测试可执行文件。
  - 接入 CTest。
  - 只依赖 Qt Core 和项目头文件，不连接真实机械臂 SDK。

- 新增：`tests/test_station_pickup_config.cpp`
  - 测试 12 个工位的夹后策略、Z 余量、工位 12 不复用 `Func_capture12`。
  - 测试纯函数 `calculateGrabDescend()` 和 `resolveAfterGripFunction()`。

- 修改：`CMakeLists.txt`
  - 启用测试。
  - 添加 `tests/` 子目录。
  - 让 Qt Creator 工程树显示测试目录。

- 修改：`src/lineconfig.h`
  - 增加每工位显式的 Z 下探余量和夹后离开策略。
  - 12 个工位配置都单独写清楚，便于现场维护。

- 修改：`src/taskexecutor.cpp`
  - 将新增工位配置注入 `HuayanScheduler::StationArmFunctions`。
  - 不改变任务编排顺序。

- 修改：`src/huayanScheduler.h`
  - 扩展 `StationArmFunctions`。
  - 增加夹后策略相关纯辅助函数声明或内联实现，供业务逻辑和测试共用。
  - 增加命令状态门控所需声明和状态字段。

- 修改：`src/huayanScheduler.cpp`
  - 使用当前工位的 `grabZClearance`。
  - 将夹后固定复用 `captureFunc` 改为按配置执行。
  - 为 `HRIF_RunFunc` 和 `HRIF_MoveRelL` 增加统一命令前状态检查。
  - 为延迟回调增加序号保护，避免旧回调推进新阶段。

---

### Task 1：新增独立 CTest 测试骨架

**文件：**
- 修改：`robot_visual20260625/robot_visual/CMakeLists.txt`
- 新增：`robot_visual20260625/robot_visual/tests/CMakeLists.txt`
- 新增：`robot_visual20260625/robot_visual/tests/test_station_pickup_config.cpp`

**接口：**
- 使用现有 CMake 工程。
- 产出测试目标：`station_pickup_config_tests`
- 产出 CTest 用例：`station_pickup_config_tests`

- [ ] **步骤 1：修改根 `CMakeLists.txt` 接入测试目录**

在 `qt_finalize_executable(wh-robot-visual)` 前加入：

```cmake
# ── 自动化测试（与 src 业务代码分离，Qt Creator 中显示为独立 tests 目录）──
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
```

- [ ] **步骤 2：新增 `tests/CMakeLists.txt`**

创建 `robot_visual20260625/robot_visual/tests/CMakeLists.txt`：

```cmake
find_package(Qt6 REQUIRED COMPONENTS Core)

add_executable(station_pickup_config_tests
    test_station_pickup_config.cpp
)

target_include_directories(station_pickup_config_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(station_pickup_config_tests PRIVATE
    Qt6::Core
)

add_test(NAME station_pickup_config_tests
         COMMAND station_pickup_config_tests)
```

- [ ] **步骤 3：新增第一版失败测试文件**

创建 `robot_visual20260625/robot_visual/tests/test_station_pickup_config.cpp`：

```cpp
#include "lineconfig.h"
#include "huayanScheduler.h"

#include <QtGlobal>

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
    if (std::fabs(actual - expected) > tolerance) {
        std::cerr << message << " actual=" << actual << " expected=" << expected << std::endl;
        std::exit(1);
    }
}

} // namespace

int main()
{
    const StationTaskConfig *s12 = stationConfig(12);
    requireTrue(s12 != nullptr, "工位12配置必须存在");
    requireTrue(s12->captureFunc == QStringLiteral("Func_capture12"), "工位12拍照函数必须保持 Func_capture12");
    requireTrue(s12->unloadPointFunc == QStringLiteral("Func_daoliao12"), "工位12倒料点位函数必须保持 Func_daoliao12");
    requireTrue(s12->afterGripMode == AfterGripMode::None, "工位12夹紧后不应再回拍照位");

    const StationTaskConfig *s1 = stationConfig(1);
    const StationTaskConfig *s2 = stationConfig(2);
    const StationTaskConfig *s11 = stationConfig(11);
    requireTrue(s1 && s2 && s11, "工位1/2/11配置必须存在");
    requireTrue(s1->afterGripMode == AfterGripMode::None, "工位1夹紧后不回安全位");
    requireTrue(s2->afterGripMode == AfterGripMode::None, "工位2夹紧后不回安全位");
    requireTrue(s11->afterGripMode == AfterGripMode::None, "工位11夹紧后不回安全位");

    for (int station = 3; station <= 10; ++station) {
        const StationTaskConfig *cfg = stationConfig(station);
        requireTrue(cfg != nullptr, "工位3-10配置必须存在");
        requireTrue(cfg->afterGripMode == AfterGripMode::CaptureFunc, "工位3-10默认复用拍照函数回安全位");
    }

    requireTrue(s1->grabZClearance > 425.0, "工位1-11篮筐余量应大于旧值以减少下探");
    requireTrue(s12->grabZClearance < 425.0, "工位12紫框余量应小于旧值以增加下探");

    requireNear(HuayanScheduler::calculateGrabDescend(798.1, 450.0, 1078.0), 348.1, 0.001,
                "Z 下探计算必须使用 visionZ - clearance");
    requireNear(HuayanScheduler::calculateGrabDescend(300.0, 450.0, 1078.0), 0.0, 0.001,
                "Z 下探计算不能返回负值");
    requireNear(HuayanScheduler::calculateGrabDescend(2000.0, 400.0, 1078.0), 1078.0, 0.001,
                "Z 下探计算必须受最大下探量限制");

    bool shouldRun = true;
    QString func = HuayanScheduler::resolveAfterGripFunction(AfterGripMode::None,
                                                             QStringLiteral("Func_capture12"),
                                                             QString(),
                                                             &shouldRun);
    requireTrue(!shouldRun, "None 策略不应调用夹后函数");
    requireTrue(func.isEmpty(), "None 策略返回函数名必须为空");

    func = HuayanScheduler::resolveAfterGripFunction(AfterGripMode::CaptureFunc,
                                                     QStringLiteral("Func_capture3"),
                                                     QString(),
                                                     &shouldRun);
    requireTrue(shouldRun, "CaptureFunc 策略应调用函数");
    requireTrue(func == QStringLiteral("Func_capture3"), "CaptureFunc 策略应返回拍照函数");

    func = HuayanScheduler::resolveAfterGripFunction(AfterGripMode::CustomFunc,
                                                     QStringLiteral("Func_capture7"),
                                                     QStringLiteral("Func_after_grip7"),
                                                     &shouldRun);
    requireTrue(shouldRun, "CustomFunc 策略应调用函数");
    requireTrue(func == QStringLiteral("Func_after_grip7"), "CustomFunc 策略应返回自定义夹后函数");

    return 0;
}
```

- [ ] **步骤 4：运行测试确认失败**

执行：

```bash
cmake -S robot_visual20260625/robot_visual -B build
cmake --build build --target station_pickup_config_tests
ctest --test-dir build --output-on-failure -R station_pickup_config_tests
```

预期：编译失败，错误包含 `AfterGripMode`、`grabZClearance`、`calculateGrabDescend` 或 `resolveAfterGripFunction` 未定义。这是预期失败，说明测试先行。

- [ ] **步骤 5：提交任务 1**

```bash
git -C robot_visual20260625/robot_visual add CMakeLists.txt tests/CMakeLists.txt tests/test_station_pickup_config.cpp
git -C robot_visual20260625/robot_visual commit -m "测试：新增工位取料配置验证"
```

不要执行 `git push`。

---

### Task 2：显式化工位配置并让测试通过第一部分

**文件：**
- 修改：`robot_visual20260625/robot_visual/src/lineconfig.h`
- 修改：`robot_visual20260625/robot_visual/src/huayanScheduler.h`
- 修改：`robot_visual20260625/robot_visual/src/taskexecutor.cpp`
- 测试：`robot_visual20260625/robot_visual/tests/test_station_pickup_config.cpp`

**接口：**
- 产出：
  - `enum class AfterGripMode { None, CaptureFunc, CustomFunc };`
  - `StationTaskConfig::afterGripMode`
  - `StationTaskConfig::afterGripFunc`
  - `StationTaskConfig::grabZClearance`
  - `HuayanScheduler::StationArmFunctions::afterGripMode`
  - `HuayanScheduler::StationArmFunctions::afterGripFunc`
  - `HuayanScheduler::StationArmFunctions::grabZClearance`

- [ ] **步骤 1：在 `lineconfig.h` 增加夹后策略枚举**

在工位配置结构附近加入：

```cpp
/// 夹紧后的机械臂离开策略。
///
/// None：夹紧后不额外回拍照/安全位，阶段一直接完成，适用于 1/2/11/12 等
///       取料点与后续路径已经安全衔接的工位。
/// CaptureFunc：兼容旧逻辑，夹紧后复用 captureFunc 回安全高度；仅适用于
///              captureFunc 本身就是单点或安全回位路径的工位。
/// CustomFunc：夹紧后调用 afterGripFunc；用于 captureFunc 带过渡点、不能
///             作为夹后回位路径复用的工位。
enum class AfterGripMode {
    None,
    CaptureFunc,
    CustomFunc
};
```

- [ ] **步骤 2：扩展 `StationTaskConfig`**

在 `StationTaskConfig` 中增加：

```cpp
    AfterGripMode afterGripMode = AfterGripMode::CaptureFunc; ///< 夹紧后的离开策略，不能再隐式等同 captureFunc。
    QString afterGripFunc;                                    ///< afterGripMode=CustomFunc 时调用的夹后安全离开函数。
    double grabZClearance = 425.0;                            ///< Z 下探余量(mm)：下探量=视觉Z-grabZClearance。
```

- [ ] **步骤 3：显式更新 12 个工位配置**

在 `kStationTaskConfigs` 前加入：

```cpp
// Z 下探余量按现场箱型显式写入每个工位：1-11 为篮筐，12 为紫框。
// 公式保持不变：descend = visionZ - grabZClearance。
// 1-11 现场现象是下降偏多，因此相对旧值 425.0 应调大；12 下降偏少，因此应调小。
static constexpr double kLargeBasketGrabZClearance = 450.0;
static constexpr double kPurpleBasketGrabZClearance = 400.0;
```

将工位行改为：

```cpp
    {1, 3, 3, PalletArea::LargeBox, QStringLiteral("Func_capture1"), AfterGripMode::None, QString(), QStringLiteral("Func_daoliao1"), QStringLiteral("Func_fanzhuan"), kLargeBasketGrabZClearance},
    {2, 4, 4, PalletArea::LargeBox, QStringLiteral("Func_capture2"), AfterGripMode::None, QString(), QStringLiteral("Func_daoliao2"), QStringLiteral("Func_fanzhuan"), kLargeBasketGrabZClearance},
    {3, 5, 9, PalletArea::LargeBox, QStringLiteral("Func_capture3"), AfterGripMode::CaptureFunc, QString(), QStringLiteral("Func_daoliao3"), QStringLiteral("Func_fanzhuan"), kLargeBasketGrabZClearance},
    {4, 5, 9, PalletArea::LargeBox, QStringLiteral("Func_capture4"), AfterGripMode::CaptureFunc, QString(), QStringLiteral("Func_daoliao4"), QStringLiteral("Func_fanzhuan"), kLargeBasketGrabZClearance},
    {5, 5, 10, PalletArea::LargeBox, QStringLiteral("Func_capture5"), AfterGripMode::CaptureFunc, QString(), QStringLiteral("Func_daoliao5"), QStringLiteral("Func_fanzhuan"), kLargeBasketGrabZClearance},
    {6, 6, 10, PalletArea::LargeBox, QStringLiteral("Func_capture6"), AfterGripMode::CaptureFunc, QString(), QStringLiteral("Func_daoliao6"), QStringLiteral("Func_fanzhuan"), kLargeBasketGrabZClearance},
    {7, 6, 11, PalletArea::LargeBox, QStringLiteral("Func_capture7"), AfterGripMode::CaptureFunc, QString(), QStringLiteral("Func_daoliao7"), QStringLiteral("Func_fanzhuan"), kLargeBasketGrabZClearance},
    {8, 6, 11, PalletArea::LargeBox, QStringLiteral("Func_capture8"), AfterGripMode::CaptureFunc, QString(), QStringLiteral("Func_daoliao8"), QStringLiteral("Func_fanzhuan"), kLargeBasketGrabZClearance},
    {9, 7, 12, PalletArea::LargeBox, QStringLiteral("Func_capture9"), AfterGripMode::CaptureFunc, QString(), QStringLiteral("Func_daoliao9"), QStringLiteral("Func_fanzhuan"), kLargeBasketGrabZClearance},
    {10, 7, 12, PalletArea::LargeBox, QStringLiteral("Func_capture10"), AfterGripMode::CaptureFunc, QString(), QStringLiteral("Func_daoliao10"), QStringLiteral("Func_fanzhuan"), kLargeBasketGrabZClearance},
    {11, 7, 7, PalletArea::LargeBox, QStringLiteral("Func_capture11"), AfterGripMode::None, QString(), QStringLiteral("Func_daoliao11"), QStringLiteral("Func_fanzhuan"), kLargeBasketGrabZClearance},
    {12, 15, 15, PalletArea::SmallBox, QStringLiteral("Func_capture12"), AfterGripMode::None, QString(), QStringLiteral("Func_daoliao12"), QStringLiteral("Func_fanzhuan"), kPurpleBasketGrabZClearance},
```

- [ ] **步骤 4：扩展 `StationArmFunctions` 和当前任务字段**

在 `huayanScheduler.h` 中引入 `lineconfig.h`，并在 `StationArmFunctions` 中增加：

```cpp
        AfterGripMode afterGripMode = AfterGripMode::CaptureFunc; ///< 夹紧后离开策略；默认兼容旧逻辑。
        QString afterGripFunc;                                    ///< CustomFunc 模式下使用的夹后安全离开函数。
        double grabZClearance = 425.0;                            ///< 本工位 Z 下探余量(mm)。
```

在私有成员中增加：

```cpp
    AfterGripMode m_afterGripMode = AfterGripMode::CaptureFunc; ///< 当前任务夹紧后离开策略，由 lineconfig 注入。
    QString m_afterGripFuncName;                                ///< 当前任务夹后安全离开函数名，CustomFunc 时必须非空。
    double m_grabZClearance = 425.0;                             ///< 当前任务 Z 下探余量(mm)，替代全局固定值。
```

- [ ] **步骤 5：新增可测试纯辅助函数**

在 `HuayanScheduler` public 区域加入两个静态函数，函数体可以内联写在头文件中，避免测试链接真实 SDK：

```cpp
    /// 计算抓取 Z 下探量。
    ///
    /// 公式：下探量 = 视觉深度 - 工位余量；结果被限制在 0 到 maxDescend。
    /// 余量越大，下探越少；余量越小，下探越多。
    static double calculateGrabDescend(double visionDepth,
                                       double grabZClearance,
                                       double maxDescend)
    {
        return qBound(0.0, visionDepth - grabZClearance, maxDescend);
    }

    /// 根据夹后策略解析实际要调用的函数名。
    ///
    /// shouldRun 返回 false 表示夹紧后不调用任何函数，阶段一可直接完成。
    /// CaptureFunc 返回拍照函数；CustomFunc 返回独立夹后安全离开函数。
    static QString resolveAfterGripFunction(AfterGripMode mode,
                                            const QString &captureFunc,
                                            const QString &afterGripFunc,
                                            bool *shouldRun)
    {
        if (mode == AfterGripMode::None) {
            if (shouldRun)
                *shouldRun = false;
            return QString();
        }
        if (shouldRun)
            *shouldRun = true;
        return mode == AfterGripMode::CustomFunc ? afterGripFunc : captureFunc;
    }
```

- [ ] **步骤 6：注入新增字段**

在 `TaskExecutor::start()` 中已有 `stationFuncs` 赋值后加入：

```cpp
    stationFuncs.afterGripMode = m_stationCfg->afterGripMode;
    stationFuncs.afterGripFunc = m_stationCfg->afterGripFunc;
    stationFuncs.grabZClearance = m_stationCfg->grabZClearance;
```

- [ ] **步骤 7：保存新增字段**

在 `HuayanScheduler::setStationFunctions()` 中加入：

```cpp
    // 夹后策略和 Z 余量来自 lineconfig 的当前工位配置。
    // 这两个值必须随任务注入，不能用全局固定值，否则工位12和带过渡点工位会复用错误路径。
    m_afterGripMode = funcs.afterGripMode;
    m_afterGripFuncName = funcs.afterGripFunc;
    m_grabZClearance = funcs.grabZClearance;
```

- [ ] **步骤 8：运行测试**

执行：

```bash
cmake --build build --target station_pickup_config_tests
ctest --test-dir build --output-on-failure -R station_pickup_config_tests
```

预期：测试通过。

- [ ] **步骤 9：提交任务 2**

```bash
git -C robot_visual20260625/robot_visual add src/lineconfig.h src/huayanScheduler.h src/huayanScheduler.cpp src/taskexecutor.cpp tests/test_station_pickup_config.cpp
git -C robot_visual20260625/robot_visual commit -m "修复：显式配置工位取料策略"
```

不要执行 `git push`。

---

### Task 3：使用配置化 Z 余量和夹后策略

**文件：**
- 修改：`robot_visual20260625/robot_visual/src/huayanScheduler.cpp`
- 修改：`robot_visual20260625/robot_visual/src/huayanScheduler.h`
- 测试：`robot_visual20260625/robot_visual/tests/test_station_pickup_config.cpp`

**接口：**
- 使用 `m_afterGripMode`、`m_afterGripFuncName`、`m_grabZClearance`。
- 产出：阶段一 `DescendZ` 使用当前工位 Z 余量；阶段一 `LiftLoad` 按 `AfterGripMode` 执行。

- [ ] **步骤 1：替换固定 Z 余量**

将 `DescendZ` 中：

```cpp
const double descend = qBound(0.0, m_grabOffset.z - kGrabZClearance, kMaxDescend);
```

替换为：

```cpp
            // 本工位 Z 余量由 lineconfig 注入。
            // descend = 视觉深度 - 余量；余量越大，下探越少。
            const double descend = calculateGrabDescend(m_grabOffset.z, m_grabZClearance, kMaxDescend);
```

同一日志中的余量参数改为：

```cpp
                                .arg(m_grabZClearance, 0, 'f', 1).arg(kMaxDescend, 0, 'f', 1));
```

- [ ] **步骤 2：替换夹后固定复用拍照函数**

将 `StageStep::LiftLoad` 分支替换为：

```cpp
        case StageStep::LiftLoad: {
            // 夹紧后的离开路径不能再隐式等同拍照路径。
            // 工位12的 Func_capture12 包含“过渡点→拍照点”，夹后复用会导致去倒料前多绕路；
            // 其他工位未来也可能增加过渡点，因此这里按 lineconfig 的 afterGripMode 执行。
            bool shouldRunAfterGrip = false;
            const QString afterGripFunc = resolveAfterGripFunction(m_afterGripMode,
                                                                    m_captureFuncName,
                                                                    m_afterGripFuncName,
                                                                    &shouldRunAfterGrip);
            if (!shouldRunAfterGrip) {
                emit logMessage(QStringLiteral("[阶段一] 夹紧后配置为不回安全位，直接完成取料阶段"));
                m_stageStep = StageStep::None;
                proceedStage();
                break;
            }
            if (afterGripFunc.isEmpty()) {
                emitOperationError(QStringLiteral("[阶段一] 夹后策略需要函数，但函数名为空"));
                break;
            }
            emit logMessage(m_afterGripMode == AfterGripMode::CustomFunc
                ? QStringLiteral("[阶段一] 夹紧后调用安全离开函数 %1").arg(afterGripFunc)
                : QStringLiteral("[阶段一] 夹紧后复用拍照位函数 %1 回安全高度").arg(afterGripFunc));
            executeRunFunc(afterGripFunc);
            break;
        }
```

- [ ] **步骤 3：检查工位 12 不再复用 `Func_capture12`**

执行：

```bash
rg -n "Func_capture12|夹紧后配置为不回安全位|夹紧后复用拍照位函数|抬升（调用拍照位函数" robot_visual20260625/robot_visual/src/huayanScheduler.cpp robot_visual20260625/robot_visual/src/lineconfig.h
```

预期：
- 工位 12 行包含 `AfterGripMode::None`。
- `huayanScheduler.cpp` 不再包含 `抬升（调用拍照位函数`。
- `LiftLoad` 包含 `夹紧后配置为不回安全位`。

- [ ] **步骤 4：运行测试和构建**

执行：

```bash
cmake --build build
ctest --test-dir build --output-on-failure -R station_pickup_config_tests
```

预期：构建成功，测试通过。

- [ ] **步骤 5：提交任务 3**

```bash
git -C robot_visual20260625/robot_visual add src/huayanScheduler.cpp src/huayanScheduler.h src/lineconfig.h tests/test_station_pickup_config.cpp
git -C robot_visual20260625/robot_visual commit -m "修复：按工位执行夹后路径和Z余量"
```

不要执行 `git push`。

---

### Task 4：为 20018 增加统一命令状态门控

**文件：**
- 修改：`robot_visual20260625/robot_visual/src/huayanScheduler.h`
- 修改：`robot_visual20260625/robot_visual/src/huayanScheduler.cpp`

**接口：**
- 使用现有 `HRIF_RunFunc` 和 `HRIF_MoveRelL` 调用点。
- 产出：
  - `enum class PendingCommandKind { None, RunFunc, MoveRelTool, MoveRelBase };`
  - `struct PendingCommand`
  - `bool beginCommandWhenReady(const PendingCommand &cmd);`
  - `void pollCommandReady();`
  - `bool dispatchReadyCommand(const PendingCommand &cmd);`

- [ ] **步骤 1：增加命令门控类型和字段**

在 `huayanScheduler.h` 私有区域加入：

```cpp
    /// 待下发的 SDK 运动命令类型。
    ///
    /// RunFunc：调用示教器函数，如 Func_captureX / Func_daoliaoX。
    /// MoveRelTool：工具坐标系相对移动，视觉微调、搜索、Z 下探使用。
    /// MoveRelBase：基坐标系相对移动，码垛 offset 使用。
    enum class PendingCommandKind {
        None,
        RunFunc,
        MoveRelTool,
        MoveRelBase
    };

    /// 统一命令门控使用的待执行命令。
    ///
    /// timeoutMs 是命令执行后的到位等待超时，不是状态门控超时。
    struct PendingCommand {
        PendingCommandKind kind = PendingCommandKind::None;
        QString label;        ///< 日志标签，说明阶段和动作，便于现场追踪 20018。
        QString funcName;     ///< kind=RunFunc 时使用。
        int poseId = 0;       ///< kind=MoveRelTool/MoveRelBase 时使用，0~5=X/Y/Z/Rx/Ry/Rz。
        int direction = 1;    ///< 相对移动方向，0=负向，1=正向。
        double distance = 0;  ///< 相对移动距离(mm或deg，取决于 poseId)。
        int timeoutMs = 30000;
    };

    bool beginCommandWhenReady(const PendingCommand &cmd);
    void pollCommandReady();
    bool dispatchReadyCommand(const PendingCommand &cmd);

    QTimer *m_commandReadyTimer = nullptr; ///< SDK 命令下发前的状态门控轮询定时器。
    PendingCommand m_pendingCommand;       ///< 当前等待状态可执行后再下发的命令。
    int m_commandReadyElapsedMs = 0;       ///< 已等待可执行状态的时间(ms)。
    bool m_commandResetIssued = false;     ///< 本轮门控是否已对 ProgramStopped 执行过 GrpReset。
    quint64 m_commandSeq = 0;              ///< 命令序号，防止旧 singleShot 回调推进新阶段。
```

- [ ] **步骤 2：初始化命令门控定时器**

在构造函数中加入：

```cpp
    m_commandReadyTimer = new QTimer(this);
    m_commandReadyTimer->setInterval(100);
    connect(m_commandReadyTimer, &QTimer::timeout, this, &HuayanScheduler::pollCommandReady);
```

在 `stopPollingAndTimers()` 中加入：

```cpp
    m_commandReadyTimer->stop();
    m_pendingCommand = PendingCommand();
    m_commandReadyElapsedMs = 0;
    m_commandResetIssued = false;
```

- [ ] **步骤 3：实现命令前状态门控**

在 `huayanScheduler.cpp` 中加入：

```cpp
bool HuayanScheduler::beginCommandWhenReady(const PendingCommand &cmd)
{
    if (!ensureConnected())
        return false;

    // 每次命令下发前都重新检查控制器状态。
    // 20018 的现场根因是串行命令之间只判断“不运动”，没有确认控制器已允许下一条命令。
    m_pendingCommand = cmd;
    m_commandReadyElapsedMs = 0;
    m_commandResetIssued = false;
    pollCommandReady();
    return true;
}

void HuayanScheduler::pollCommandReady()
{
    if (m_pendingCommand.kind == PendingCommandKind::None)
        return;

    int nMovingState = 0;
    int nEnableState = 0;
    int nErrorState = 0;
    int nErrorCode = 0;
    int nErrorAxis = 0;
    int nBreaking = 0;
    int nPause = 0;
    int nBlendingDone = 0;
    int nRet = HRIF_ReadRobotFlags(m_boxID, m_rbtID,
                                   nMovingState, nEnableState, nErrorState,
                                   nErrorCode, nErrorAxis, nBreaking,
                                   nPause, nBlendingDone);
    if (nRet != 0) {
        const QString msg = QStringLiteral("命令前读取机器人状态失败：%1").arg(nRet);
        m_pendingCommand = PendingCommand();
        m_commandReadyTimer->stop();
        emitOperationError(msg);
        return;
    }

    if (nErrorState != 0) {
        const QString detail = describeError(m_boxID, nErrorCode);
        const QString msg = detail.isEmpty()
            ? QStringLiteral("命令前机器人报错，错误码：%1").arg(nErrorCode)
            : QStringLiteral("命令前机器人报错，错误码：%1（%2）").arg(nErrorCode).arg(detail);
        m_pendingCommand = PendingCommand();
        m_commandReadyTimer->stop();
        emitOperationError(msg);
        return;
    }

    int nCurFSM = 0;
    string strCurFSM;
    const int fsmRet = HRIF_ReadCurFSM(m_boxID, m_rbtID, nCurFSM, strCurFSM);
    const QString fsmText = fsmRet == 0 ? QString::fromStdString(strCurFSM) : QStringLiteral("unknown");

    if (nMovingState != 0 || nPause != 0) {
        m_commandReadyElapsedMs += m_commandReadyTimer->interval();
    } else if (fsmRet == 0 && fsmText.contains(QStringLiteral("ProgramStopped"), Qt::CaseInsensitive)) {
        if (!m_commandResetIssued) {
            emit logMessage(QStringLiteral("[华沿] 命令前检测到 ProgramStopped，执行 GrpReset 后等待可执行状态：%1")
                                .arg(m_pendingCommand.label));
            HRIF_GrpReset(m_boxID, m_rbtID);
            m_commandResetIssued = true;
        }
        m_commandReadyElapsedMs += m_commandReadyTimer->interval();
    } else {
        PendingCommand cmd = m_pendingCommand;
        m_pendingCommand = PendingCommand();
        m_commandReadyTimer->stop();
        dispatchReadyCommand(cmd);
        return;
    }

    static constexpr int kCommandReadyTimeoutMs = 8000;
    if (m_commandReadyElapsedMs >= kCommandReadyTimeoutMs) {
        const QString msg = QStringLiteral("命令前等待机器人可执行状态超时：%1（moving=%2 pause=%3 fsm=%4/%5）")
            .arg(m_pendingCommand.label)
            .arg(nMovingState)
            .arg(nPause)
            .arg(nCurFSM)
            .arg(fsmText);
        m_pendingCommand = PendingCommand();
        m_commandReadyTimer->stop();
        emitOperationError(msg);
        return;
    }

    if (!m_commandReadyTimer->isActive())
        m_commandReadyTimer->start();
}
```

- [ ] **步骤 4：实现门控通过后的真实下发**

加入：

```cpp
bool HuayanScheduler::dispatchReadyCommand(const PendingCommand &cmd)
{
    if (cmd.kind == PendingCommandKind::RunFunc) {
        std::vector<string> params;
        int nRet = HRIF_RunFunc(m_boxID, cmd.funcName.toStdString(), params);
        if (nRet != 0) {
            const QString detail = describeError(m_boxID, nRet);
            emitOperationError(detail.isEmpty()
                ? QStringLiteral("调用函数 %1 失败：%2").arg(cmd.funcName).arg(nRet)
                : QStringLiteral("调用函数 %1 失败：%2（%3）").arg(cmd.funcName).arg(nRet).arg(detail));
            return false;
        }
        startWaitForIdle(cmd.timeoutMs);
        return true;
    }

    if (cmd.kind == PendingCommandKind::MoveRelTool || cmd.kind == PendingCommandKind::MoveRelBase) {
        const int toolMotion = cmd.kind == PendingCommandKind::MoveRelTool ? 1 : 0;
        int nRet = HRIF_MoveRelL(m_boxID, m_rbtID, cmd.poseId, cmd.direction, cmd.distance, toolMotion);
        if (nRet != 0) {
            const QString detail = describeError(m_boxID, nRet);
            emitOperationError(detail.isEmpty()
                ? QStringLiteral("%1失败：%2").arg(cmd.label).arg(nRet)
                : QStringLiteral("%1失败：%2（%3）").arg(cmd.label).arg(nRet).arg(detail));
            return false;
        }
        startWaitForIdle(cmd.timeoutMs);
        return true;
    }

    return false;
}
```

- [ ] **步骤 5：将 `executeRunFunc()` 改为走门控**

替换为：

```cpp
bool HuayanScheduler::executeRunFunc(const QString &funcName, int timeoutMs)
{
    PendingCommand cmd;
    cmd.kind = PendingCommandKind::RunFunc;
    cmd.label = QStringLiteral("RunFunc %1").arg(funcName);
    cmd.funcName = funcName;
    cmd.timeoutMs = timeoutMs;
    return beginCommandWhenReady(cmd);
}
```

- [ ] **步骤 6：将所有 `HRIF_MoveRelL` 调用改为走门控**

转换以下调用点：

- `executeNextGrabMove()`：`MoveRelTool`，标签 `相对运动 X/Y/Z/Rz`
- `DescendZ`：`MoveRelTool`，标签 `Z 下探`
- `onVisionNoObject()`：`MoveRelTool`，标签 `搜索下移`
- `proceedAction()` 工具旋转：`MoveRelTool`，标签 `工具旋转`
- `proceedAction()` 扫码搜索 Y：`MoveRelTool`，标签 `扫码搜索 Y 轴移动`
- `executeNextPalletMove()`：`MoveRelBase`，标签 `码垛相对移动`

对于 `onVisionNoObject()`，如果 `beginCommandWhenReady(cmd)` 返回 `false`，必须保留现有回滚行为：恢复 `m_stageStep`、`m_searchDescendCount`、`m_searchDescendedMm`。

- [ ] **步骤 7：检查 SDK 运动调用集中情况**

执行：

```bash
rg -n "HRIF_RunFunc|HRIF_MoveRelL" robot_visual20260625/robot_visual/src/huayanScheduler.cpp
```

预期：`HRIF_RunFunc` 和 `HRIF_MoveRelL` 只出现在 `dispatchReadyCommand()` 内，或存在明确中文注释说明的非运动例外。若 `releaseGripper()` 仍直接调用 `HRIF_RunFunc`，改为 `executeRunFunc(m_releaseFuncName, 30000)`。

- [ ] **步骤 8：构建和测试**

执行：

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

预期：构建成功，CTest 通过。

- [ ] **步骤 9：提交任务 4**

```bash
git -C robot_visual20260625/robot_visual add src/huayanScheduler.h src/huayanScheduler.cpp
git -C robot_visual20260625/robot_visual commit -m "修复：机械臂命令下发前检查状态"
```

不要执行 `git push`。

---

### Task 5：保护延迟回调并完成整体验证

**文件：**
- 修改：`robot_visual20260625/robot_visual/src/huayanScheduler.h`
- 修改：`robot_visual20260625/robot_visual/src/huayanScheduler.cpp`
- 测试：`robot_visual20260625/robot_visual/tests/test_station_pickup_config.cpp`

**接口：**
- 使用现有 `QTimer::singleShot` 回调。
- 产出：旧延迟回调不能在 stop、阶段完成或新阶段启动后继续推进当前流程。

- [ ] **步骤 1：增加回调序号辅助函数**

在 `huayanScheduler.h` 中加入：

```cpp
    quint64 nextCallbackSeq(); ///< 生成延迟回调序号，避免旧 singleShot 推进新阶段。
```

在 `huayanScheduler.cpp` 中加入：

```cpp
quint64 HuayanScheduler::nextCallbackSeq()
{
    return ++m_commandSeq;
}
```

在 `stop(bool emitStoppedLog)` 清理状态前加入：

```cpp
    ++m_commandSeq; // 让已经排队的 singleShot 回调全部失效，避免旧阶段推进新阶段。
```

- [ ] **步骤 2：保护 `resetAndProceed()` 延迟回调**

将：

```cpp
    QTimer::singleShot(kResetSettleMs, this, [this] {
        if (m_stage != Stage::None)
            proceedStage();
    });
```

替换为：

```cpp
    const quint64 seq = nextCallbackSeq();
    QTimer::singleShot(kResetSettleMs, this, [this, seq] {
        if (seq == m_commandSeq && m_stage != Stage::None)
            proceedStage();
    });
```

- [ ] **步骤 3：保护其他 `singleShot` 回调**

更新以下回调，捕获序号并在执行前校验：

- `MoveToGrab` 的 300ms 回调
- `SearchDescend` 的视觉稳定回调
- `executeNextGrabMove()` 中微调完成后的视觉稳定回调
- `setGripper()` 的 1500ms 回调
- `executeGripFunc()` 的 2500ms 回调
- 码垛 offset 的 300ms 回调

使用以下模式，按各自延时和状态条件调整：

```cpp
const quint64 seq = nextCallbackSeq();
QTimer::singleShot(300, this, [this, seq] {
    if (seq == m_commandSeq && m_stage == Stage::StageOne && m_stageStep == StageStep::MoveToGrab)
        executeNextGrabMove();
});
```

- [ ] **步骤 4：完整构建、测试和静态检查**

执行：

```bash
cmake --build build
ctest --test-dir build --output-on-failure
rg -n "HRIF_RunFunc|HRIF_MoveRelL" robot_visual20260625/robot_visual/src/huayanScheduler.cpp
rg -n "AfterGripMode::None|AfterGripMode::CaptureFunc|kLargeBasketGrabZClearance|kPurpleBasketGrabZClearance" robot_visual20260625/robot_visual/src/lineconfig.h
git -C robot_visual20260625/robot_visual status --short
```

预期：
- 构建成功。
- CTest 通过。
- SDK 运动调用集中在门控下发函数。
- 工位配置显式。
- 新增代码文件只在 `tests/` 目录内，业务代码没有新增文件。

- [ ] **步骤 5：现场慢速验证工位 12 日志**

在现场调试环境降低机械臂速度，执行一次工位 12 取料和倒料。

预期关键日志：

```text
[阶段一] 调用拍照位函数 Func_capture12
[阶段一] 调用夹紧函数 Func_jiajin
[阶段一] 夹紧后配置为不回安全位，直接完成取料阶段
[T#... S12][ARM] 阶段一：取料 已完成
[T#... S12][AGV] 取料已回拍照安全高度，取料位与倒料位同一 LM15，跳过 AGV 导航，直接进入倒料准备点
[倒料] 移动到倒料点位 Func_daoliao12
```

禁止在 `Func_jiajin` 后出现：

```text
[阶段一] 夹紧后复用拍照位函数 Func_capture12 回安全高度
```

- [ ] **步骤 6：现场慢速验证 20018 和 Z 余量**

如果控制器进入 `ProgramStopped`，预期出现：

```text
[华沿] 命令前检测到 ProgramStopped，执行 GrpReset 后等待可执行状态：...
```

正常恢复时不应出现：

```text
相对运动失败：20018
调用函数 Func_capture...失败：20018
```

分别选一个 1-11 代表工位和工位 12 慢速执行取料，预期：

```text
[阶段一] Z 下探 ...（视觉深度 ... - 余量 ...，上限 ...）
```

1-11 日志中的余量应大于 `425.0`，工位 12 日志中的余量应小于 `425.0`。

- [ ] **步骤 7：提交任务 5**

```bash
git -C robot_visual20260625/robot_visual add src/huayanScheduler.h src/huayanScheduler.cpp src/lineconfig.h src/taskexecutor.cpp CMakeLists.txt tests/CMakeLists.txt tests/test_station_pickup_config.cpp
git -C robot_visual20260625/robot_visual commit -m "修复：忽略过期机械臂延迟回调"
```

不要执行 `git push`。

---

## 最终验证清单

- [ ] 构建通过：

```bash
cmake --build build
```

- [ ] CTest 通过：

```bash
ctest --test-dir build --output-on-failure
```

- [ ] 验证过程中没有使用 Python。

- [ ] 测试代码位于 `tests/`，业务代码没有新增文件：

```bash
git -C robot_visual20260625/robot_visual status --short
```

- [ ] SDK 运动调用已集中：

```bash
rg -n "HRIF_RunFunc|HRIF_MoveRelL" robot_visual20260625/robot_visual/src/huayanScheduler.cpp
```

- [ ] 工位 12 夹紧后不再复用 `Func_capture12`：

```bash
rg -n "夹紧后复用拍照位函数|夹紧后配置为不回安全位|Func_capture12" robot_visual20260625/robot_visual/src
```

- [ ] 不执行 push：

```bash
git -C robot_visual20260625/robot_visual status --short
```

---

## 自检

- 需求覆盖：任务 1 覆盖独立测试目录和 Qt Creator 结构；任务 2 覆盖每工位显式配置；任务 3 覆盖 Z 余量和夹后路径；任务 4 覆盖 20018 命令状态门控；任务 5 覆盖旧延迟回调和最终验证。
- 占位符检查：没有保留占位符；每个实施步骤都包含具体文件、代码形状、命令和预期结果。
- 类型一致性：`AfterGripMode`、`afterGripMode`、`afterGripFunc`、`grabZClearance`、`PendingCommand`、`PendingCommandKind`、`calculateGrabDescend()`、`resolveAfterGripFunction()` 在各任务中命名一致。
