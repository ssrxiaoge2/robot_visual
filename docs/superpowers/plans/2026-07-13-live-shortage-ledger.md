# 现场真实缺料账本、完整逻辑测试与主流程接入 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 先完整删除已废弃的客户系统通信测试并原地建立唯一真实通信层，再在不修改现有设备动作和 FIFO 顺序的前提下，实现可持久化的 12 工位正式库存账本、隔离的完整逻辑测试弹窗，以及模拟/真实二选一的主流程接入。

**Architecture:** `customSysScheduler.h/.cpp` 原地删除 `.229` 诊断 API 并改造成真实缺料唯一 MES/PLC 协议层，`DeviceManager` 只持有一个 `m_liveShortageScheduler`。`ShortageEngine` 统一编排库存事实层 `ShortageLedger` 和非抢占补料计划层 `ReplenishmentPlanner`，生产态与独立测试态复用同一核心但使用不同状态存储命名空间；`LiveShortageCoordinator` 只把“一箱意图”追加到现有 `LineManager` FIFO，并消费任务事实。

**Tech Stack:** C++17、Qt 6.8.3（Core、Widgets、Network、Test）、CMake/CTest、JSON、`QSaveFile`，Linux 首轮验证。

**Status:** Tasks 1～12 已按用户最新要求改为“每个任务单独中文提交”执行；当前 Task 12 负责全量 C++ 回归、文档同步、现场验收清单和本任务中文提交。现场 Step 6～13 是待现场实机执行的验收记录清单，本轮只写入文档，不冒充已完成现场验证。

## Global Constraints

- 基线分支固定为 `codex/v0.2.5-shortage-ledger-rebuild`，其基线来自 `pallet_test@0967c8f`；旧缺料分支不整体合并，但其已验证 HTTP/PLC 协议代码必须按函数级优先移植，禁止无依据重新实现一套协议。
- 实施前 Linux Debug 主程序已构建成功，当前既有 C++ 测试为 7/7 通过；每个任务完成时都必须保持这 7 项全绿。
- 不修改 AGV、视觉、扫码、机械臂动作、倒料动作、收姿态、码垛算法和现有 FIFO 的队首/队尾顺序。
- 保留现有 `LineManager::reportShortage(int)` 模拟缺料逻辑；真实和模拟来源严格二选一，默认模拟。
- “客户系统通信测试”属于已废弃临时逻辑；Task 1 必须删除其全部 UI、配置、DeviceManager 入口/信号和 Scheduler 诊断 API，`.229`、`DayRecord`、旧 signals 与兼容空壳零残留。`customSysScheduler.h/.cpp` 文件保留并原地改造成真实缺料唯一协议层，不新增替代文件。
- 正式账本和完整逻辑测试状态必须隔离；测试不得进入 FIFO、控制硬件或修改正式文件。
- 生产运行期不解析 `安全库存.xlsx`；最新 Sheet3 的 36 条默认值固化到 C++，运行参数由 UI 手工编辑保存。
- 采样间隔默认 15 秒、合法范围 5～300 秒；单轮超时默认 5 秒、合法范围 1～30 秒且小于采样间隔；通信报警默认 10 分钟、合法范围 1～60 分钟；连续倒料前失败默认 3 次、合法范围 1～10 次。
- `actualQty` 清零只根据数值序列判断，不依据工控机时间；候选期继续下降只更新最小候选值，首次低位回升才确认清零。`1000→2→5` 和 `1000→900→500→0→2→5` 都按新周期累计 5 扣减，`1000→2→1005` 按旧周期增量 5 扣减。
- C++ 数量统一使用 `qint64`，补料单号和 taskId 使用 `quint64`；所有乘法和加减必须检查溢出。
- 只使用 C++/Qt Test 验证，不使用 Python；所有测试代码放在 `tests/`，并作为 Qt Creator 可见的独立 CMake target。
- Linux `Qt6_DIR=/opt/Qt/qt6.8/6.8.3/gcc_64/lib/cmake/Qt6`，`qt-cmake=/opt/Qt/qt6.8/6.8.3/gcc_64/bin/qt-cmake`；Windows 本轮不验证。
- 新增/修改的枚举及每个枚举值、结构体及字段、函数参数/返回值/失败语义、成员所有权/生命周期、状态分支、信号唯一触发点和跨模块接线必须写中文注释。
- 修改既有代码时，注释必须说明修改前行为、修改原因、修改后行为和不受影响的既有逻辑；本 plan 的代码片段也遵守相同注释要求。
- 实施中如需新增本计划“文件职责图”之外的文件，必须停止并先请求用户同意。
- 当前实施节奏按用户最新要求调整为每个任务完成后单独中文提交；Task 12 只提交文档同步、全量验证报告和验收清单，不把任务外已有 dirty 文件纳入提交。

---

## 0. 实施前必读与当前接口事实

实施者必须先阅读：

- `docs/superpowers/specs/2026-07-13-live-shortage-ledger-design.md`
- `docs/shortage-signal-analysis/2026-07-12-customer-shortage-confirmation.md`
- `docs/shortage-signal-analysis/2026-07-12-internal-shortage-logic-analysis.md`
- `docs/superpowers/specs/2026-06-25-12-station-continuous-replenishment-design.md`
- `src/lineconfig.h`
- `src/taskqueue.h`
- `src/taskexecutor.h/.cpp`
- `src/linemanager.h/.cpp`
- `src/devicemanager.h/.cpp`
- `src/customSysScheduler.h/.cpp`
- `src/mainwindow.h/.cpp`

当前接口事实：

- `TaskQueue::enqueue(int stationId, TaskSource source)` 只追加队尾，`takeNext()` 只取队首。
- `LineManager::reportShortage(int)` 只创建 `TaskSource::UiMock` 任务。
- `TaskExecutor::onArmStageCompleted()` 在 `ExecState::ArmUnload` 分支直接进入 `StowAfterUnload`；这里是新增唯一倒料事实的准确位置。
- `LineManager::onExecutorSystemError()` 当前忽略 `Task`，是真实任务占用无法释放的旧问题之一。
- 当前分支 `CustomSysScheduler` 只有 `.229` MES 日数据诊断读取；该 UI/API/配置路径将在 Task 1 完整删除，不作为兼容资产。
- 旧分支已经实现 MES/PLC 双端点、`RequestContext`、`roundId` 透传、PLC 查询参数、严格 JSON 解析和中文错误回传；Task 1 只按函数级把这些能力移植进原 `customSysScheduler.h/.cpp`。
- 真实缺料 MES 默认地址为 `.228`，后续由 Task 2 缺料配置持久化；Task 1 协议测试直接注入 QUrl，PLC URL 从该地址派生，不读取 `.229` 旧值。

### 0.1 实施前基线门禁

在 Task 1 修改任何文件前先执行：

```bash
cmake --build build -j2
ctest --test-dir build --output-on-failure
git diff --check
```

Expected：主程序构建成功，当前既有测试 7/7 通过，`git diff --check` 无输出。若基线不满足，先查明当前分支问题，不得把基线故障和缺料功能修改混在同一轮修复。

### 0.2 旧缺料分支移植门禁

- 禁止整体 merge、rebase 或 cherry-pick 旧缺料功能提交到当前分支。
- Task 1 只能从旧分支按函数级移植 HTTP/PLC 协议代码；每段移植都要列出旧函数、当前目标函数和必要差异。
- 先让 MES/PLC 解析与 `roundId` 协议测试通过，再实现 `ShortageSampleCoordinator`；采样测试通过后，才允许连接 Engine 和生产协调器。
- 旧 `ShortageCalculator`、`ShortageMonitor`、`ShortageTestSession` 的库存、阈值、派单和状态所有权逻辑不得复制到新核心。
- 任何为解决分支差异而提出的额外文件，仍必须先请求用户同意。

## 1. 文件职责图

### 1.1 经用户批准本计划后才允许创建的业务文件

| 文件 | 单一职责 |
| --- | --- |
| `src/shortagetypes.h` | 缺料领域枚举、配置/状态/事件值类型及中文转换函数，不含 I/O 和定时器 |
| `src/shortageconfigstore.h/.cpp` | 36 条 Sheet3 默认值、配置校验、加载、原子保存和导出 |
| `src/shortageledger.h/.cpp` | 0 建账、`actualQty` 基线/清零/毛刺、库存扣减和一箱入账 |
| `src/replenishmentplanner.h/.cpp` | 首次低位顺序、活动工位非抢占、一箱在途、失败暂停和补料单状态 |
| `src/shortageengine.h/.cpp` | 账本与计划的唯一事务门面，统一生成状态变化和持久化事件 |
| `src/shortagestatestore.h/.cpp` | 正式/测试命名空间、主快照、备份、流水、校验和、恢复和写频控制 |
| `src/shortagesamplecoordinator.h/.cpp` | 15 秒轮询、5 秒超时、roundId 聚合、三选一和两轮稳定 |
| `src/shortagetestcontroller.h/.cpp` | 独立测试 Engine 生命周期、手工采样、真实采样和任务事件注入 |
| `src/shortageconfigdialog.h/.cpp` | 宽屏“参数配置/完整逻辑测试”两页签 UI |
| `src/liveshortagecoordinator.h/.cpp` | 正式模式门禁、一箱意图到 FIFO、任务事实回送及人工补料 |
| `src/shortagerecoverydialog.h/.cpp` | 维护人员单工位异常修正、原因/工位号确认、备份和审计 |

### 1.2 经用户批准本计划后才允许创建的 C++ 测试文件

| 文件 | 覆盖范围 |
| --- | --- |
| `tests/test_shortage_config.cpp` | CF-01～CF-06 |
| `tests/test_shortage_ledger.cpp` | LD-01～LD-08 |
| `tests/test_replenishment_planner.cpp` | PL-01～PL-06、TK-04～TK-08 |
| `tests/test_shortage_state_store.cpp` | PS-01～PS-10、PS-12～PS-14 |
| `tests/test_shortage_sample_coordinator.cpp` | Task 1 先覆盖 CP-01～CP-05 删除/协议契约，Task 6 再追加 CM-01～CM-12、CH-01～CH-05 |
| `tests/test_shortage_test_controller.cpp` | IN-01～IN-04 |
| `tests/test_shortage_dialog.cpp` | UI-01、UI-02、UI-07、UI-08 |
| `tests/test_shortage_task_lifecycle.cpp` | TK-01～TK-03、TK-06、RG-01～RG-05 |
| `tests/test_live_shortage_coordinator.cpp` | IN-05～IN-08、ER-01～ER-05、FIFO/人工补料闭环 |
| `tests/test_live_shortage_ui.cpp` | UI-03～UI-06、主界面页签和来源显示契约 |

### 1.3 修改文件

- `src/lineconfig.h`：扩展任务来源并给真实任务携带补料单号。
- `src/taskqueue.h`：只扩展 `enqueue()` 参数以保存补料单号，不改变 FIFO 算法。
- `src/taskexecutor.h/.cpp`：只新增唯一倒料事实信号。
- `src/linemanager.h/.cpp`：新增可返回接受结果的队尾追加接口和任务事实信号；模拟包装接口保持原行为。
- `src/customSysScheduler.h/.cpp`：删除全部旧诊断 API，在原文件中建立唯一真实 roundId MES/PLC 协议接口。
- `src/devicemanager.h/.cpp`：Task 1 删除旧诊断配置/入口/信号/对象；Task 10 再持有唯一真实通信对象和正式协调器，恢复安装、对象生命周期和 signals 只接线一次。
- `src/mainwindow.h/.cpp`：Task 1 删除旧通信测试整个面板；Task 11 再实现来源单选、12 按钮复用、状态/FIFO 页签、弹窗和恢复入口。
- `CMakeLists.txt`：加入新增生产源文件。
- `tests/CMakeLists.txt`：加入全部 C++ 测试 target。
- `README.md`、`changelog/CHANGELOG.md`：删除旧通信测试现行说明，并在实现完成后同步真实缺料用户可见说明。
- `docs/superpowers/plans/2026-06-16-custom-system-communication.md`：保留历史内容，顶部标记“已废弃，由真实缺料通信替代”。
- 两份 2026-07-12 分析文档、本文 spec 和本 plan：实现中规则或接口改变时同步。

### 1.4 CMake 中必须使用的确定写法

主 `CMakeLists.txt` 的 `PROJECT_SOURCES` 追加以下文件；注释说明这些类不替代既有设备控制器：

```cmake
# 真实缺料按“配置、采样、账本、计划、持久化、测试/生产接线和 UI”拆分；
# 这些文件只决定缺料事实与派单时机，不修改 AGV、机械臂、扫码和码垛算法。
list(APPEND PROJECT_SOURCES
    src/shortagetypes.h
    src/shortageconfigstore.h src/shortageconfigstore.cpp
    src/shortageledger.h src/shortageledger.cpp
    src/replenishmentplanner.h src/replenishmentplanner.cpp
    src/shortageengine.h src/shortageengine.cpp
    src/shortagestatestore.h src/shortagestatestore.cpp
    src/shortagesamplecoordinator.h src/shortagesamplecoordinator.cpp
    src/shortagetestcontroller.h src/shortagetestcontroller.cpp
    src/shortageconfigdialog.h src/shortageconfigdialog.cpp
    src/liveshortagecoordinator.h src/liveshortagecoordinator.cpp
    src/shortagerecoverydialog.h src/shortagerecoverydialog.cpp
)
```

`tests/CMakeLists.txt` 先增加统一函数，再按任务追加 target；测试全部链接 Qt Test，避免每个任务复制不一致的 CMake 配置：

```cmake
# 缺料测试统一放 tests target；Widgets/Network 只为 UI 和协议测试提供依赖，
# 测试不会链接或调用 AGV、机械臂、视觉、扫码厂商 SDK。
find_package(Qt6 REQUIRED COMPONENTS Core Test Widgets Network)

function(add_shortage_cpp_test target_name)
    add_executable(${target_name} ${ARGN})
    target_include_directories(${target_name} PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(${target_name} PRIVATE
        Qt6::Core Qt6::Test Qt6::Widgets Qt6::Network)
    add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

add_shortage_cpp_test(shortage_config_tests
    test_shortage_config.cpp
    ../src/shortageconfigstore.cpp)
add_shortage_cpp_test(shortage_state_store_tests
    test_shortage_state_store.cpp
    ../src/shortagestatestore.cpp
    ../src/shortageconfigstore.cpp)
add_shortage_cpp_test(shortage_ledger_tests
    test_shortage_ledger.cpp
    ../src/shortageledger.cpp
    ../src/shortageconfigstore.cpp)
add_shortage_cpp_test(replenishment_planner_tests
    test_replenishment_planner.cpp
    ../src/shortageengine.cpp
    ../src/replenishmentplanner.cpp
    ../src/shortageledger.cpp
    ../src/shortagestatestore.cpp
    ../src/shortageconfigstore.cpp)
add_shortage_cpp_test(shortage_sample_coordinator_tests
    test_shortage_sample_coordinator.cpp
    ../src/shortagesamplecoordinator.cpp
    ../src/customSysScheduler.cpp)
# CP-05 读取当前源码验证旧诊断入口零残留；宏不进入生产 target。
target_compile_definitions(shortage_sample_coordinator_tests PRIVATE
    ROBOT_VISUAL_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_shortage_cpp_test(shortage_test_controller_tests
    test_shortage_test_controller.cpp
    ../src/shortagetestcontroller.cpp
    ../src/shortagesamplecoordinator.cpp
    ../src/customSysScheduler.cpp
    ../src/shortageengine.cpp
    ../src/replenishmentplanner.cpp
    ../src/shortageledger.cpp
    ../src/shortagestatestore.cpp
    ../src/shortageconfigstore.cpp)
add_shortage_cpp_test(shortage_dialog_tests
    test_shortage_dialog.cpp
    ../src/shortageconfigdialog.cpp
    ../src/shortagerecoverydialog.cpp
    ../src/shortagetestcontroller.cpp
    ../src/shortagesamplecoordinator.cpp
    ../src/customSysScheduler.cpp
    ../src/shortageengine.cpp
    ../src/replenishmentplanner.cpp
    ../src/shortageledger.cpp
    ../src/shortagestatestore.cpp
    ../src/shortageconfigstore.cpp)
add_shortage_cpp_test(shortage_task_lifecycle_tests
    test_shortage_task_lifecycle.cpp)
add_shortage_cpp_test(live_shortage_coordinator_tests
    test_live_shortage_coordinator.cpp
    ../src/liveshortagecoordinator.cpp
    ../src/shortagesamplecoordinator.cpp
    ../src/customSysScheduler.cpp
    ../src/shortageengine.cpp
    ../src/replenishmentplanner.cpp
    ../src/shortageledger.cpp
    ../src/shortagestatestore.cpp
    ../src/shortageconfigstore.cpp)
add_shortage_cpp_test(live_shortage_ui_tests
    test_live_shortage_ui.cpp)
```

每个任务只增加当时已经创建的 target；上面代码块表示 Task 12 完成后的最终内容。若链接器证明某个 target 需要额外业务源文件，必须优先通过接口隔离修正设计；确需新增源文件时按全局约束先取得用户同意。

---

### Task 1: 删除旧客户系统通信测试并原地建立唯一真实协议层

**Requirements:** CP-01～CP-05；spec 第 16.1、16.4、16.5 节和第 19 节阶段 0。

**Files:**

- Create: `tests/test_shortage_sample_coordinator.cpp`（本任务先写 CP-01～CP-05；Task 6 在同一文件追加采样测试）
- Modify: `src/customSysScheduler.h`
- Modify: `src/customSysScheduler.cpp`
- Modify: `src/devicemanager.h`
- Modify: `src/devicemanager.cpp`
- Modify: `src/mainwindow.h`
- Modify: `src/mainwindow.cpp`
- Modify: `tests/CMakeLists.txt`
- Verify: `CMakeLists.txt`（继续编译原 `customSysScheduler.h/.cpp`，不新增通信源文件或替代 target）

**Interfaces:**

- Consumes: 旧分支 `codex/v0.2.5-shortage-signals` 的 `RequestContext`、MES/PLC 请求、严格解析和 `roundId` 透传；Task 1 测试直接注入真实 MES `QUrl`，不依赖尚未建立的 Task 2 配置类型。
- Produces: `CustomSysScheduler::setLiveMesDayEndpoint/setRequestTimeoutMs/fetchMesDayData/fetchPlcBits`、`mesReplyReady/plcReplyReady`，供 Task 6 `ShortageSampleCoordinator` 使用。
- Produces: `DeviceManager::liveShortageScheduler()` 返回唯一非空真实通信对象；本任务不启动定时采样、不接账本、不创建 FIFO 任务。
- Removes: MainWindow 全部 `m_customSys*` UI/槽/主题样式，DeviceManager 的 `customSysEndpoint`、旧诊断入口/信号/对象，以及 Scheduler 的 `.229`、`DayRecord` 和旧诊断 API。

- [ ] **Step 1: 在同一测试文件先写 CP-01～CP-05 失败测试**

```cpp
class ShortageSampleCoordinatorTest final : public QObject
{
    Q_OBJECT
private slots:
    void mesParserKeepsOldBranchPayloadContract();           // CP-01：MES 成功/失败 JSON 与旧缺料分支一致，actualQty 为 qint64。
    void plcParserKeepsOldBranchPayloadContract();           // CP-02：逐字段验证 success/timestamp/data/address/value。
    void plcParserRejectsMissingDuplicateOrWrongTypeBits();  // CP-03：缺失、重复、越界和错误类型整包拒绝。
    void protocolReplyKeepsRoundIdAndAddressRange();         // CP-04：roundId、StartAddress、length 原样回传。
    void legacyDiagnosticSurfaceIsAbsent();                  // CP-05：旧 UI/API/.229 和第二通信路径零残留。
};
```

`legacyDiagnosticSurfaceIsAbsent()` 使用 C++/Qt 读取源码文本，不使用 Python；`tests/CMakeLists.txt` 给该 target 注入源码根目录：

```cmake
# 删除契约需要读取当前源码验证旧入口零残留；宏只在测试 target 可见。
target_compile_definitions(shortage_sample_coordinator_tests PRIVATE
    ROBOT_VISUAL_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
```

测试中的禁止项必须逐文件固定，不能只检查 UI 标题：

```cpp
// 修改前这些标识真实存在，因此测试必须先红；修改后任一标识回归都会使 CP-05 失败。
const QHash<QString, QStringList> forbiddenByFile = {
    {QStringLiteral("src/mainwindow.h"),
     {QStringLiteral("m_customSys"), QStringLiteral("onCustomSystem"),
      QStringLiteral("initCustomSystemPanel"), QStringLiteral("setCustomSystemInputsEnabled")}},
    {QStringLiteral("src/mainwindow.cpp"),
     {QStringLiteral("客户系统通信测试"), QStringLiteral("m_customSys"),
      QStringLiteral("onCustomSystem"), QStringLiteral("initCustomSystemPanel"),
      QStringLiteral("setCustomSystemInputsEnabled")}},
    {QStringLiteral("src/devicemanager.h"),
     {QStringLiteral("customSysEndpoint"), QStringLiteral("testCustomSystem"),
      QStringLiteral("fetchCustomSystemDayData"), QStringLiteral("customSystemStatusChanged"),
      QStringLiteral("customSystemDayDataReady"), QStringLiteral("m_customSysScheduler")}},
    {QStringLiteral("src/devicemanager.cpp"),
     {QStringLiteral("CustomSysScheduler::DayRecord"), QStringLiteral("testCustomSystem"),
      QStringLiteral("fetchCustomSystemDayData"), QStringLiteral("m_customSysScheduler")}},
    {QStringLiteral("src/customSysScheduler.h"),
     {QStringLiteral("DayRecord"), QStringLiteral("defaultEndpoint("),
      QStringLiteral("testConnectivity("), QStringLiteral("fetchDayData("),
      QStringLiteral("connectivityChecked"), QStringLiteral("dayDataReady")}},
    {QStringLiteral("src/customSysScheduler.cpp"),
     {QStringLiteral("192.168.115.229"), QStringLiteral("DayRecord"),
      QStringLiteral("testConnectivity("), QStringLiteral("fetchDayData(")}}
};
```

同一测试还必须正向断言 `devicemanager.h/.cpp` 各自包含 `m_liveShortageScheduler`，并使用 `QDirIterator` 扫描全部 `src/*.cpp`，统计生产构造路径只出现一次 `new CustomSysScheduler(this)`；这样不是“删到没有通信”，而是全工程只留下一个真实通信所有者。

- [ ] **Step 2: 配置测试 target 并运行，确认失败原因准确**

Task 1 阶段的 target 只编译协议层和测试，不引用尚未创建的 `shortagesamplecoordinator.cpp`：

```cmake
add_shortage_cpp_test(shortage_sample_coordinator_tests
    test_shortage_sample_coordinator.cpp
    ../src/customSysScheduler.cpp)
target_compile_definitions(shortage_sample_coordinator_tests PRIVATE
    ROBOT_VISUAL_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
```

Run:

```bash
/opt/Qt/qt6.8/6.8.3/gcc_64/bin/qt-cmake \
  -S . -B build-shortage -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-shortage --target shortage_sample_coordinator_tests --parallel
QT_QPA_PLATFORM=offscreen \
  build-shortage/tests/shortage_sample_coordinator_tests legacyDiagnosticSurfaceIsAbsent
```

Expected：测试进程失败，并明确列出当前旧面板、`customSysEndpoint`、`DayRecord`、旧诊断接口或 `.229` 标识；不得先删除断言或把禁止项改为空。

- [ ] **Step 3: 只读提取旧分支协议函数并建立移植矩阵**

Run:

```bash
git show codex/v0.2.5-shortage-signals:src/customSysScheduler.h
git show codex/v0.2.5-shortage-signals:src/customSysScheduler.cpp
git diff codex/v0.2.5-shortage-signals..HEAD -- \
  src/customSysScheduler.h src/customSysScheduler.cpp
```

移植记录固定为：

| 旧分支函数/类型 | 当前目标 | 必要调整 |
| --- | --- | --- |
| `RequestContext`、MES/PLC `Operation` | 原文件同名私有协议上下文 | 删除 Connectivity/FetchDayData 诊断分支，只保留真实请求种类 |
| `fetchMesDayData(roundId)` | 同名 public slot | endpoint 改由 `setLiveMesDayEndpoint()` 注入，`actualQty` 使用 `qint64` |
| `fetchPlcBits(roundId,start,length)` | 同名 public slot | 保留 `StartAddress/length` 查询名和范围回传 |
| `parseMesDayReply()`、`parsePlcBitReply()` | 同名静态解析 | 保留旧严格字段校验，错误不返回部分数据 |
| `mesReplyReady/plcReplyReady` | 同名 signals | 保持 roundId、startAddress 原值 |
| 固定 `.228`/3 秒 | `.228` 默认函数/可配置超时 | 默认 5000 ms，PLC URL 从 MES scheme/host/port 派生 |
| 旧库存、阈值、Monitor 调用 | 不移植 | 业务状态由后续 Engine/Coordinator 独立实现 |

- [ ] **Step 4: 原地替换 `CustomSysScheduler` 公共协议接口**

`src/customSysScheduler.h` 的旧诊断类型和接口全部删除，保留类名和文件名；公共边界固定为：

```cpp
class CustomSysScheduler : public QObject
{
    Q_OBJECT
public:
    /// MES 日数据解析结果；真实账本数量必须使用 64 位有符号整数。
    struct LiveMesDayReply {
        bool ok = false;       ///< true 表示 HTTP 和 JSON 契约完整。
        qint64 actualQty = 0;  ///< 非负累计产量；负值或溢出文本解析失败。
        QString errorMessage;  ///< 失败字段、原值和原因；成功时为空。
    };

    /// PLC 位读取结果；values 的 key 必须完整覆盖请求地址范围。
    struct PlcBitReply {
        bool ok = false;       ///< true 表示 success/timestamp/data 均合法。
        QString timestamp;     ///< 客户返回时间文本，只记录不参与库存判断。
        QMap<int, bool> values;///< address 到布尔位；缺失/重复/越界整包失败。
        QString errorMessage;  ///< 失败字段、地址和处理动作。
    };

    explicit CustomSysScheduler(QObject *parent = nullptr);
    /// 返回真实缺料 `.228` 日数据默认值；工程中不再提供 `.229` 默认函数。
    static QUrl defaultLiveMesDayEndpoint();
    /// 仅替换固定 PLC path，保留真实 MES URL 的 scheme/host/port。
    static QUrl livePlcBitEndpointFor(const QUrl &liveMesDayEndpoint);
    /// 只接受完整 HTTP/HTTPS 日数据 URL；失败不覆盖当前有效地址。
    bool setLiveMesDayEndpoint(const QUrl &endpoint, QString *errorMessage = nullptr);
    /// 请求级超时必须为正数；默认 5000 ms，失败不覆盖当前有效值。
    bool setRequestTimeoutMs(int timeoutMs, QString *errorMessage = nullptr);
    /// 纯函数解析 MES payload，供协议测试和网络完成回调共同复用。
    static LiveMesDayReply parseMesDayReply(const QByteArray &payload);
    /// 纯函数解析 PLC payload，并严格校验请求范围内地址唯一且完整。
    static PlcBitReply parsePlcBitReply(const QByteArray &payload,
                                        int startAddress,
                                        int length);

public slots:
    /// roundId 由采样层生成；协议层只透传并执行一次 MES GET。
    virtual void fetchMesDayData(quint64 roundId);
    /// 读取连续 PLC 位并带回相同 roundId/startAddress；不解释产品或模式。
    virtual void fetchPlcBits(quint64 roundId, int startAddress, int length);

signals:
    /// MES 成功和失败都回传原 roundId，便于采样层丢弃迟到响应。
    void mesReplyReady(quint64 roundId,
                       CustomSysScheduler::LiveMesDayReply reply);
    /// PLC 成功和失败都回传原 roundId 和起始地址。
    void plcReplyReady(quint64 roundId,
                       int startAddress,
                       CustomSysScheduler::PlcBitReply reply);
    /// 结构化中文日志只描述通信，不修改库存或任务。
    void logMessage(QString messageZh);
};

// 信号参数需要被 QSignalSpy 和潜在 queued connection 安全识别；替代已删除的 DayRecord 注册。
Q_DECLARE_METATYPE(CustomSysScheduler::LiveMesDayReply)
Q_DECLARE_METATYPE(CustomSysScheduler::PlcBitReply)
```

实现保留当前 `QNetworkAccessManager` 生命周期模式；删除 `DayRecord`、`ParseResult` 诊断含义、`defaultEndpoint()`、`setEndpoint()`、`testConnectivity()`、`fetchDayData()`、旧 signals 和所有 `.229` 字面量。网络错误、非 2xx、JSON 错误、字段缺失、重复 PLC 地址、地址越界和类型错误必须各自返回中文原因。

- [ ] **Step 5: 删除 MainWindow 和 DeviceManager 的旧诊断表面，只创建一个真实对象**

删除范围必须逐项完成：

- `MainWindow`：`#include "customSysScheduler.h"`（若无其他使用）、构造函数中的四组旧 connect、`initCustomSystemPanel(leftPanel)`、整个初始化函数、全部 `onCustomSystem*`/`setCustomSystemInputsEnabled()`、`buildConfig()` 的旧 endpoint 读取、主题中的 `m_customSys*` 分支，以及头文件全部旧成员/声明。
- `DeviceManager::Config`：删除 `customSysEndpoint`，不得新增 `.229` 迁移字段。
- `DeviceManager`：删除旧 getter、`testCustomSystem()`、`fetchCustomSystemDayData()`、`customSystem*` signals、`qRegisterMetaType<DayRecord>()`、旧 connect 和 `applyConfig()` 的 endpoint 更新；改为注册 `LiveMesDayReply` 和 `PlcBitReply`，注册只服务真实协议 signals。
- `DeviceManager`：把成员改为 `CustomSysScheduler *m_liveShortageScheduler = nullptr;`，构造函数只执行一次 `m_liveShortageScheduler = new CustomSysScheduler(this);`；增加带中文所有权注释的只读 getter，Task 10 再把它连接到生产采样协调器。

不得删除模拟缺料按钮、网络配置面板、日志区或其他设备测试逻辑。旧面板释放的左侧空间保持自然布局，不在本任务提前实现 Task 11 的真实缺料主监控 UI。

- [ ] **Step 6: 运行 CP-01～CP-05 和主程序构建**

Run:

```bash
cmake --build build-shortage --target shortage_sample_coordinator_tests wh-robot-visual --parallel
QT_QPA_PLATFORM=offscreen \
  build-shortage/tests/shortage_sample_coordinator_tests \
  mesParserKeepsOldBranchPayloadContract \
  plcParserKeepsOldBranchPayloadContract \
  plcParserRejectsMissingDuplicateOrWrongTypeBits \
  protocolReplyKeepsRoundIdAndAddressRange \
  legacyDiagnosticSurfaceIsAbsent
```

Expected：五个函数全部通过，主程序链接成功；测试输出证明 `.229`、旧 UI/API/signals 零残留，MES/PLC 严格协议与旧缺料分支兼容。

- [ ] **Step 7: 运行现有回归并按任务中文提交**

Run:

```bash
QT_QPA_PLATFORM=offscreen ctest --test-dir build-shortage --output-on-failure
git diff --check
```

Expected：当前 7 个既有 CTest 和本任务新增协议 target 全部通过，0 failed；`git diff --check` 无输出。只记录 Task 1 检查点，不执行 `git commit`。

---

### Task 2: 建立领域类型和 36 条可保存配置

**Requirements:** CF-01～CF-06、CH-05 的配置选择部分。

**Files:**

- Create: `src/shortagetypes.h`
- Create: `src/shortageconfigstore.h`
- Create: `src/shortageconfigstore.cpp`
- Create: `tests/test_shortage_config.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Produces: `ProductModel`、`ProductionMode`、`ShortageStationConfig`、`ShortageParameters`、`ShortageConfiguration`、`ShortageOperationResult`。
- Produces: `ShortageConfigStore::sheet3Defaults()`、`validate()`、`load()`、`save()`、`exportCopy()`。
- Consumes: Qt Core 的 `QList`、`QString`、`QJsonDocument`、`QSaveFile`。

- [ ] **Step 1: 先写失败的配置测试**

在 `tests/test_shortage_config.cpp` 建立 Qt Test 类，测试函数必须逐一命名：

```cpp
class ShortageConfigTest final : public QObject
{
    Q_OBJECT
private slots:
    void sheet3DefaultsContainExactlyThirtySixRows(); // CF-01：全表逐字段核对。
    void plcBitsMapToConfirmedModesOnly();             // CF-02：L68/L69/L1998 映射。
    void parameterBoundariesAreValidated();            // CF-03：所有边界和越界。
    void stationRowsRejectInvalidFields();              // CF-04：逐字段错误原因。
    void saveLoadAndBackupAreAtomic();                  // CF-05：保存、损坏、备份。
    void busyGuardListsEveryBlockingCondition();        // CF-06：五类忙碌门禁。
    void allNineProductModeCombinationsResolveUsage();  // CH-05：3×3 用量选择。
};
```

`sheet3DefaultsContainExactlyThirtySixRows()` 不只抽查：用 12 行期望数组循环验证三个产品的工位、NO、位置、品号、每箱、最低、最高和三列用量。

`parameterBoundariesAreValidated()` 还必须验证真实缺料 MES URL：默认为 `.228` 日数据地址，空值、无 scheme、无 host 和非 HTTP/HTTPS 全部拒绝；不得读取、迁移或回退到 Task 1 已删除的 `.229` 旧值。

测试期望数组必须直接写出最新 Sheet3 数值，不能调用生产默认表生成期望：

```cpp
struct ExpectedSheet3Row {
    int stationId;           ///< 代码工位。
    QString temporaryNo;     ///< Sheet3 NO。
    QString sitePosition;    ///< Sheet3 现场位置。
    QString partNumber;      ///< 完整显示品号。
    qint64 boxQuantity;      ///< 每箱数量。
    qint64 minimumStock;     ///< 最低安全位。
    qint64 maximumStock;     ///< 最高安全位。
    qint64 usageLeftRight;   ///< L/R 用量。
    qint64 usageLeftOnly;    ///< L/L 用量。
    qint64 usageRightOnly;   ///< R/H 用量。
};

// 这 12 行会分别核对 88、88R、92；因此总断言记录数必须是 36。
const QList<ExpectedSheet3Row> expectedRows = {
    {11, "1",  "2",          "18112-RM8S0WS",       114,  600,  800,  1, 1, 1},
    {10, "2",  "3",          "18115-RM8S0DE（左）", 2000, 2000, 4000, 1, 1, 0},
    {9,  "3",  "4",          "18125-RM8S0BR（左）", 2000, 2000, 4000, 1, 1, 0},
    {8,  "4",  "5",          "18215-RM8S0DE（右）", 2000, 2000, 4000, 1, 0, 1},
    {7,  "5",  "6",          "18225-RM8S0BR（右）", 2000, 2000, 4000, 1, 0, 1},
    {6,  "6",  "7",          "18114-RM8S0BR（左）", 2000, 2000, 4000, 1, 1, 0},
    {5,  "7",  "8",          "18214-RM8S0BR（右）", 2000, 2000, 4000, 1, 0, 1},
    {4,  "8",  "9",          "18116-RM8S0（左）",    836,  800, 1600, 1, 1, 0},
    {3,  "T",  "暂未确定",    "18116-RM8S0（右）",    836,  800, 1600, 1, 0, 1},
    {2,  "9",  "10",         "18167-RM700HT（右）",  250,  600, 1200, 1, 0, 1},
    {1,  "10", "11",         "18117-RM8S0HT（左）",  250,  600, 1200, 1, 1, 0},
    {12, "11", "13",         "18118-RM8S0",          300,  600, 1200, 1, 1, 1},
};
```

- [ ] **Step 2: 运行测试并确认因类型/接口尚不存在而失败**

Run:

```bash
/opt/Qt/qt6.8/6.8.3/gcc_64/bin/qt-cmake -S . -B build-shortage -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-shortage --target shortage_config_tests --parallel
```

Expected: 编译失败，错误明确指向 `shortagetypes.h` 或 `ShortageConfigStore` 尚不存在；不得通过删除断言使测试编译。

- [ ] **Step 3: 实现带完整中文注释的领域配置类型**

`src/shortagetypes.h` 至少包含以下真实定义；每个枚举值和字段保留注释：

```cpp
/// 客户三种产品；枚举值只表达业务结果，不直接保存 PLC 地址。
enum class ProductModel {
    Model88,  ///< PLC L71 单独为 true 时的 88 产品。
    Model88R, ///< PLC L72 单独为 true 时的 88R 产品。
    Model92   ///< PLC L73 单独为 true 时的 92 产品。
};

/// 客户三种生产方式；与 L68/L69/L1998 的映射由采样层唯一维护。
enum class ProductionMode {
    LeftRight, ///< L68：L/R，左右生产。
    LeftOnly,  ///< L69：L/L，目前只有左。
    RightOnly  ///< L1998：R/H，只有右。
};

/// 正式补料单来源；模拟任务没有正式补料单，因此不在此枚举中。
enum class ReplenishmentOrigin {
    Automatic, ///< 正式账本自动计划生成。
    Manual     ///< 人工二次确认生成，仍进入同一账本和审计。
};

/// 主调度缺料来源开关；默认 Mock，任一时刻只能激活一个值。
enum class ShortageInputSource {
    Mock, ///< 保持现有模拟缺料按钮行为，正式采样停止。
    Live  ///< 启动正式采样和自动计划，模拟按钮改为人工补料确认。
};

/// 采样通信状态只表达可用性，不直接修改库存。
enum class ShortageCommunicationState {
    Stopped,             ///< 尚未启动或已人工停止。
    Sampling,            ///< 正在等待当前 roundId 的四组响应。
    Interrupted,         ///< 当前轮失败但尚未达到红色报警时间。
    Alarm,               ///< 连续失败已经达到配置报警时间。
    RecoveryNeedsReview  ///< 恢复值小于旧基线，必须联系维护人员。
};

/// 正式一箱补料单生命周期；Unloaded 后不得因后续失败回滚库存。
enum class ReplenishmentOrderState {
    AwaitingDispatch,  ///< 等待 LineManager 接受，拒收重试仍使用同一补料单号。
    Queued,            ///< 已绑定当前程序 taskId 并追加 FIFO。
    Running,           ///< 原有主流程已经开始执行。
    Unloaded,          ///< 唯一倒料事实已经入账。
    Succeeded,         ///< 倒料和全部设备收尾成功。
    FailedBeforeUnload,///< 倒料前失败，本箱未入账。
    FailedAfterUnload, ///< 倒料后失败，已入账的一箱保留。
    Canceled           ///< Stop 或系统错误在倒料前取消。
};

/// 一个产品下一个代码工位的完整 Sheet3 配置；三列用量同处一行。
struct ShortageStationConfig {
    ProductModel product = ProductModel::Model88; ///< 所属产品，决定 36 条中的分组。
    int stationId = 0;                            ///< 上位机代码工位，合法范围 1～12。
    QString temporaryNo;                          ///< Sheet3 临时 NO，可为数字文本或 T。
    QString sitePosition;                         ///< 现场位置文本，允许保存“暂未确定”。
    QString partNumber;                           ///< 现场物料品号；启用时不能为空。
    bool enabled = true;                          ///< false 时该产品下不扣料、不自动补料。
    qint64 boxQuantity = 0;                       ///< 有效倒料一次增加的 ea 数量。
    qint64 minimumStock = 0;                      ///< 库存严格小于此值才触发。
    qint64 maximumStock = 0;                      ///< 库存达到或超过此值即停止。
    qint64 usageLeftRight = 0;                    ///< L/R 每个 actualQty 的消耗。
    qint64 usageLeftOnly = 0;                     ///< L/L 每个 actualQty 的消耗。
    qint64 usageRightOnly = 0;                    ///< R/H 每个 actualQty 的消耗。
};

/// 真实通信、定时采样和工位保护参数，单位全部写入字段名或注释。
struct ShortageParameters {
    QString liveMesDayEndpoint = QStringLiteral(
        "http://192.168.115.228:5084/api/MesData/day"); ///< 真实缺料唯一 MES 地址，不读取已删除的 .229 旧配置。
    int sampleIntervalSeconds = 15;      ///< 两轮启动间隔，合法 5～300 秒。
    int roundTimeoutSeconds = 5;         ///< 单轮等待上限，合法 1～30 秒且小于间隔。
    int communicationAlarmMinutes = 10;  ///< 连续异常达到该分钟数转红色报警。
    int preUnloadFailureLimit = 3;        ///< 同工位连续倒料前失败暂停阈值。
};

/// 可保存的完整配置；revision 每次正式保存成功后单调增加。
struct ShortageConfiguration {
    QList<ShortageStationConfig> stations; ///< 36 条独立产品/工位记录。
    ShortageParameters parameters;         ///< 全产品共享的通信和保护参数。
    quint64 revision = 1;                  ///< 状态恢复时校验的配置修订号。
};

/// 所有无异常抛出的业务接口统一返回中文结果，失败时不得部分修改状态。
struct ShortageOperationResult {
    bool ok = false;       ///< true 表示整个操作已经完成。
    QString messageZh;     ///< 成功摘要或包含字段/原值/原因/处理动作的失败说明。
};

/// 配置保存的五项运行门禁；UI 和业务层都调用同一个判定函数。
struct ShortageEditConditions {
    bool standaloneTestStopped = true; ///< 独立测试已停止。
    bool liveSamplingStopped = true;    ///< 正式采样已停止。
    bool lineStopped = true;            ///< LineManager 不在 Running/ReturningHome。
    bool currentTaskEmpty = true;       ///< 当前执行任务为空。
    bool fifoEmpty = true;              ///< Pending FIFO 为空。
};

/// 异常库存修正是领域命令而不是 Dialog 私有类型，生产协调器会再次校验全部门禁。
struct ShortageMaintenanceCorrection {
    int stationId = 0;          ///< 本次唯一目标工位。
    qint64 oldStock = 0;        ///< 打开窗口时的账本值，用于并发校验。
    qint64 newStock = 0;        ///< 维护人员确认的新库存。
    QString reason;             ///< 非空原因，写入审计流水。
    QString typedStationId;     ///< 必须与 stationId 的十进制文本完全一致。
};

/// 一轮已经完成三选一和稳定确认的采样，才能交给账本。
struct ShortageSample {
    quint64 roundId = 0;                         ///< 采样层单调轮次号。
    ProductModel product = ProductModel::Model88;///< 本轮稳定产品。
    ProductionMode mode = ProductionMode::LeftRight; ///< 本轮稳定模式。
    qint64 actualQty = 0;                        ///< MES 64 位累计产量，必须非负。
    QDateTime capturedAtUtc;                     ///< 完整轮次完成 UTC 时间。
    bool recoveredAfterInterruption = false;     ///< true 时禁止把回退值自动当作日清零。
};

/// 跨程序重启的正式补料单；程序 taskId 只是一段运行期绑定。
struct ReplenishmentOrder {
    quint64 orderNo = 0;                         ///< 正式命名空间内单调编号。
    int stationId = 0;                          ///< 目标代码工位。
    ReplenishmentOrigin origin = ReplenishmentOrigin::Automatic; ///< 自动或人工。
    ReplenishmentOrderState state = ReplenishmentOrderState::AwaitingDispatch;
    quint64 taskId = 0;                          ///< 当前运行期绑定，未入队为 0。
    bool unloadAccounted = false;                ///< true 后重复倒料不能再次加箱。
    QDateTime createdAtUtc;                      ///< 建单 UTC 时间。
    QString lastReasonZh;                        ///< 最近拒收/失败/终态原因。
};
```

- [ ] **Step 4: 实现默认值、校验、原子保存和导出**

接口固定为：

```cpp
class ShortageConfigStore final
{
public:
    /// 返回 3 个产品各 12 行的独立副本；运行期修改不会污染编译期默认表。
    static ShortageConfiguration sheet3Defaults();
    /// 返回全部字段错误；ok=false 时 message 是可直接展示的中文汇总。
    static ShortageOperationResult validate(const ShortageConfiguration &configuration);
    /// 五项条件全部为 true 才允许生产配置保存；失败消息逐项列出阻止原因。
    static ShortageOperationResult canEditConfiguration(
        const ShortageEditConditions &conditions);
    /// 先加载主配置，主文件缺失/损坏/非法时再读同目录备份；两者都失败不覆盖传入内存值。
    static ShortageOperationResult load(const QString &filePath,
                                        ShortageConfiguration *configuration);
    /// 先校验；将当前有效主文件原子写入备份后，再用 QSaveFile 原子替换主文件。
    static ShortageOperationResult save(const QString &filePath,
                                        const ShortageConfiguration &configuration);
    /// 只导出副本，不改变正式配置路径和当前内存配置。
    static ShortageOperationResult exportCopy(const QString &targetPath,
                                              const ShortageConfiguration &configuration);
};
```

校验必须一次返回：行号/产品/工位/字段/错误值，覆盖 36 条数量、每产品工位唯一、每箱、上下限、用量、品号、真实 MES URL、参数范围和 `timeout < interval`。

备份名称由 Store 固定派生：`configuration.json` 对应 `configuration.backup.json`，`test-configuration.json` 对应 `test-configuration.backup.json`。第一次保存时没有旧主文件，可以暂无备份；第二次保存后破坏主文件，CF-05 必须证明能读回上一份有效配置并明确报告“来自备份”。

- [ ] **Step 5: 运行 Task 2 测试**

Run:

```bash
cmake --build build-shortage --target shortage_config_tests --parallel
ctest --test-dir build-shortage -R '^shortage_config_tests$' --output-on-failure
```

Expected: 1/1 tests passed；CF-01～CF-06、CH-05 配置选择断言全部执行。

- [ ] **Step 6: 记录检查点并按任务中文提交**

Run: `git diff --check`

Expected: 无输出。记录变更文件和测试结果；遵守用户要求，不执行 `git commit`。

---

### Task 3: 实现正式/测试隔离的快照、备份和流水

**Requirements:** PS-01～PS-10、PS-12～PS-14；PS-11 的补料单字段由 Task 5 补齐。

**Files:**

- Create: `src/shortagestatestore.h`
- Create: `src/shortagestatestore.cpp`
- Create: `tests/test_shortage_state_store.cpp`
- Modify: `src/shortagetypes.h`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: Task 2 的 `ShortageConfiguration` 和基础枚举。
- Produces: `ShortageRuntimeState`、`ShortageAuditEvent`、`ShortageStateNamespace`、`ShortageStateStore::load/saveCritical/savePeriodic/backupBeforeMaintenance`。

- [ ] **Step 1: 写失败的持久化测试**

测试类函数名固定为：

```cpp
class ShortageStateStoreTest final : public QObject
{
    Q_OBJECT
private slots:
    void unchangedStateDoesNotWrite();                 // PS-01。
    void oneSampleWritesOneAggregateEvent();            // PS-02。
    void periodicSnapshotIsLimitedToSixtySeconds();     // PS-03。
    void criticalEventsForceImmediateSnapshot();        // PS-04。
    void interruptedTemporaryWriteKeepsMainFile();      // PS-05。
    void corruptedMainFallsBackToBackup();              // PS-06。
    void journalReplayRestoresPostSnapshotState();      // PS-07。
    void journalGapOrDuplicateRejectsBareSnapshot();    // PS-07/PS-08：不能用丢事件的裸快照冒充恢复成功。
    void checksumAndVersionErrorsAreRejected();         // PS-08。
    void allCorruptSourcesLockAutomaticMode();          // PS-09。
    void testNamespaceNeverTouchesProductionFiles();    // PS-10。
    void persistenceFailureAfterUnloadKeepsMemoryFact();// PS-12。
    void safeRestoreRequiresOperatorConfirmation();     // PS-13。
    void zeroLedgerRequiresSiteClearConfirmation();     // PS-14。
};
```

所有测试使用 `QTemporaryDir`，不得读写可执行目录中的真实状态。

- [ ] **Step 2: 运行并确认失败**

Run: `cmake --build build-shortage --target shortage_state_store_tests --parallel`

Expected: 编译失败，缺少 `ShortageStateStore` 或运行状态类型。

- [ ] **Step 3: 定义运行状态与审计事件**

在 `shortagetypes.h` 增加完整状态；字段不得用无类型 `QVariantMap` 代替：

```cpp
/// 正式和独立测试使用不同目录前缀，禁止调用方自由拼接文件名。
enum class ShortageStateNamespace {
    Production,    ///< 正式账本、正式任务和正式审计。
    StandaloneTest ///< 独立测试状态，不得影响 Production。
};

/// 一条聚合审计事件；details 保存确定字段，不保存不可解析的整段日志文本。
struct ShortageAuditEvent {
    quint64 sequence = 0;      ///< 状态命名空间内单调递增流水号。
    QDateTime occurredAtUtc;   ///< UTC 事件时间，用于排序和现场追踪。
    QString eventType;         ///< 稳定英文键，例如 sample_applied、box_unloaded。
    QString messageZh;         ///< 面向维护人员的完整中文说明。
    QJsonObject details;       ///< 工位、原值、新值、taskId、补料单等结构化字段。
};

/// 单工位的正式账本状态；库存属于事实，等待/暂停属于计划恢复所需状态。
struct ShortageStationRuntime {
    int stationId = 0;               ///< 代码工位 1～12。
    qint64 stock = 0;                ///< 当前正式库存，允许为负数。
    QDateTime firstLowAtUtc;         ///< 首次跌破最低位时间；不在等待表时为空。
    int consecutivePreUnloadFailures = 0; ///< 连续倒料前失败次数。
    bool automaticPaused = false;    ///< true 时只阻止该工位自动补料。
    QString pauseReasonZh;           ///< 暂停原因和维护处理提示。
};

/// actualQty 基线及两轮清零候选；恢复回退时不能复用普通清零分支。
struct ActualQtyRuntime {
    bool hasBaseline = false;        ///< false 时首个有效样本只建立基线。
    qint64 baseline = 0;             ///< 最近一次已经入账扣减的累计产量。
    bool hasResetCandidate = false;  ///< true 表示观察到一次小于 baseline 的值。
    qint64 resetCandidate = 0;       ///< 等待下一有效轮确认的较小值。
    bool interrupted = false;        ///< 通信中断后恢复值回退必须联系维护。
};

inline constexpr int kShortageStateFormatVersion = 1; ///< JSON 格式版本，不兼容版本拒绝加载。

/// 可从快照和流水完整恢复的全部正式/测试运行状态。
struct ShortageRuntimeState {
    int formatVersion = kShortageStateFormatVersion; ///< 序列化版本。
    quint64 configurationRevision = 0;               ///< 建账/保存时配置修订号。
    bool initialized = false;                        ///< 是否已经安全恢复或从现场清空建账。
    bool operatorConfirmedRestore = false;           ///< 启动后人工确认前不得自动派单。
    bool hasStableContext = false;                   ///< 当前产品/模式是否已两轮稳定。
    ProductModel product = ProductModel::Model88;    ///< 当前稳定产品。
    ProductionMode mode = ProductionMode::LeftRight;///< 当前稳定模式。
    bool hasPendingContext = false;                  ///< 换型已确认但旧任务尚未排空。
    ProductModel pendingProduct = ProductModel::Model88; ///< 待切换产品。
    ProductionMode pendingMode = ProductionMode::LeftRight; ///< 待切换模式。
    bool hasPendingActualQty = false;                ///< 换型等待期间是否保存了最新产量。
    qint64 pendingActualQty = 0;                     ///< 旧任务排空后按新用量一次补扣到该值。
    ActualQtyRuntime actualQty;                       ///< 产量基线和清零候选。
    QList<ShortageStationRuntime> stations;           ///< 恰好 12 个代码工位状态。
    QList<int> waitingStationIds;                     ///< 按首次时间/工位号排好的等待表。
    int activeStationId = 0;                          ///< 0 表示无活动计划。
    QList<ReplenishmentOrder> orders;                 ///< 恢复和幂等所需未完成/近期补料单。
    quint64 nextReplenishmentOrderNo = 1;             ///< 下一个正式补料单号。
    quint64 nextAuditSequence = 1;                    ///< 下一条流水号。
    bool criticalLock = false;                        ///< true 时停止所有新自动派单。
    QString criticalReasonZh;                         ///< 严重锁定原因和处理动作。
    QDateTime lastSavedAtUtc;                         ///< 最近完整快照时间。
};

/// 恢复来源用于 UI 摘要和审计，不能只返回一个 bool。
enum class ShortageRestoreSource {
    None,    ///< 没有任何状态文件，可在现场清空确认后新建。
    Main,    ///< 主快照校验通过。
    Backup,  ///< 主快照失败，使用备份。
    Journal  ///< 在有效快照后重放流水得到最终状态。
};

struct ShortageStateLoadResult {
    bool ok = false;                         ///< false 时 state 不得进入自动模式。
    bool stateFound = false;                 ///< None 且无文件时为 false。
    bool requiresMaintenance = false;        ///< 三份损坏或语义不确定时为 true。
    ShortageRestoreSource source = ShortageRestoreSource::None; ///< 实际恢复来源。
    ShortageRuntimeState state;              ///< 仅 ok=true 时可使用。
    QString messageZh;                       ///< 安全恢复摘要或联系维护人员原因。
};
```

- [ ] **Step 4: 实现存储接口和写入策略**

```cpp
class ShortageStateStore final
{
public:
    /// baseDirectory 通常为可执行文件目录下 shortage-state；测试传 QTemporaryDir。
    explicit ShortageStateStore(QString baseDirectory,
                                ShortageStateNamespace stateNamespace);

    /// 先尝试“主快照+其后连续流水”，再尝试“备份快照+其后连续流水”；不返回丢失后续事件的裸快照。
    ShortageStateLoadResult load() const;
    /// 关键事实先追加流水，再用 QSaveFile 写快照；任一步失败均返回中文原因。
    ShortageOperationResult saveCritical(const ShortageRuntimeState &state,
                                          const ShortageAuditEvent &event);
    /// 非关键变化只追加一条聚合流水，并按距上次快照 60 秒条件保存。
    ShortageOperationResult savePeriodic(const ShortageRuntimeState &state,
                                          const ShortageAuditEvent &event,
                                          const QDateTime &nowUtc);
    /// 异常修正前强制复制一份带时间戳的维护备份。
    ShortageOperationResult backupBeforeMaintenance(const ShortageRuntimeState &state);
};
```

校验和固定为对“去除 checksum 字段后的紧凑 JSON 字节”计算 `QCryptographicHash::Sha256`；流水每行一个紧凑 JSON 对象，只重放快照 `nextAuditSequence` 之后的事件，要求 sequence 连续、严格递增且不重复。主快照可用但其后流水缺口/损坏时，允许改用备份+完整后续流水；两条路径都不完整时 `ok=false`、`requiresMaintenance=true`。

生产命名空间固定使用 `production-state.json`、`production-state.backup.json`、`production-events.jsonl`；测试命名空间固定使用 `test-state.json`、`test-state.backup.json`、`test-events.jsonl`。配置文件由 Task 2 分别使用 `configuration.json`/`configuration.backup.json` 和 `test-configuration.json`/`test-configuration.backup.json`。正式程序的 baseDirectory 固定为 `QCoreApplication::applicationDirPath() + "/shortage-state"`，维护备份进入其 `maintenance-backups/` 子目录；单元测试只传 `QTemporaryDir`。

- [ ] **Step 5: 运行持久化测试**

Run:

```bash
cmake --build build-shortage --target shortage_state_store_tests --parallel
ctest --test-dir build-shortage -R '^shortage_state_store_tests$' --output-on-failure
```

Expected: 1/1 tests passed；临时目录中 Production 和 StandaloneTest 文件名互不重叠。

- [ ] **Step 6: 检查点并按任务中文提交**

Run: `git diff --check`

Expected: 无输出；按用户最新要求只暂存本任务范围文件并使用中文提交信息。

---

### Task 4: 实现 0 建账、产量扣减、清零候选和唯一一箱入账

**Requirements:** LD-01～LD-08、ER-01、ER-04、ER-05。

**Files:**

- Create: `src/shortageledger.h`
- Create: `src/shortageledger.cpp`
- Create: `tests/test_shortage_ledger.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: Task 2 的配置和 Task 3 的 `ShortageRuntimeState`。
- Produces: `ShortageLedger::initializeZero/applyStableSample/recordUnloadedBox/validateRestoredState`。
- Produces: `LedgerApplyResult`，明确是否变化、是否需要持久化、是否严重锁定。
- Ownership: `ShortageEngine` 唯一持有 `ShortageRuntimeState`；Ledger 只修改传入副本的库存、上下文和 actualQty 字段，不保存第二份运行状态。

- [ ] **Step 1: 写 LD-01～LD-08 的失败测试**

```cpp
class ShortageLedgerTest final : public QObject
{
    Q_OBJECT
private slots:
    void zeroInitializationRequiresExplicitConfirmation(); // LD-01。
    void firstSampleOnlyEstablishesBaseline();              // LD-02。
    void deltaDeductsUsageForCurrentProductAndMode();       // LD-03：返回并记录每个正增量，不发明巨大增量阈值。
    void unchangedQtyDoesNotWriteAndStockMayBeNegative();   // LD-04。
    void automaticAndManualUnloadAddOneBoxOnly();           // LD-05。
    void failuresBeforeAndAfterUnloadKeepExactFacts();      // LD-06。
    void resetCandidateDistinguishesResetFromGlitch();      // LD-07。
    void arithmeticOverflowLocksWithoutPartialMutation();   // LD-08。
};
```

其中 `resetCandidateDistinguishesResetFromGlitch()` 必须断言三条完整序列：

- `1000,2,5`：确认新周期，最终合计扣 5。
- `1000,900,500,0,2,5`：900/500/0 只更新更小候选值且库存/旧基线不变；2 时确认清零并扣 2，5 时再扣 3。
- `1000,2,1005`：2 不改库存，回到旧基线以上后只扣 5。

- [ ] **Step 2: 运行并确认失败**

Run: `cmake --build build-shortage --target shortage_ledger_tests --parallel`

Expected: 缺少 `ShortageLedger` 导致编译失败。

- [ ] **Step 3: 实现账本接口与无部分更新算法**

```cpp
/// 账本操作先在副本计算；ok=false 时 changed 必须为 false，正式状态保持原值。
struct LedgerApplyResult {
    bool ok = false;          ///< 整个账本事务是否成功。
    bool changed = false;     ///< 库存、基线或候选状态是否发生变化。
    bool criticalLock = false;///< 溢出或重复事实是否要求严重锁定。
    bool hasProductionDelta = false; ///< true 表示本次事务确认了可展示的正产量增量。
    qint64 productionDelta = 0;      ///< 本轮已入账扣减的产量增量；不设置任意“巨大增量”黄色阈值。
    QString messageZh;        ///< 包含规则、原值、事件值和处理动作。
};

class ShortageLedger final
{
public:
    explicit ShortageLedger(ShortageConfiguration configuration);

    /// 只有 siteIsConfirmedEmpty=true 才把 12 工位设为 0；否则不修改任何状态。
    LedgerApplyResult initializeZero(ShortageRuntimeState *state,
                                     bool siteIsConfirmedEmpty,
                                     const QDateTime &nowUtc);
    /// 应用采样前先在局部副本完成全部 64 位溢出校验，通过后一次替换正式状态。
    LedgerApplyResult applyStableSample(ShortageRuntimeState *state,
                                        const ShortageSample &sample);
    /// 只执行一箱库存事实；补料单/taskId/重复事件已由 Engine 在调用前校验。
    LedgerApplyResult recordUnloadedBox(ShortageRuntimeState *state,
                                        int stationId,
                                        ReplenishmentOrigin origin,
                                        const QDateTime &nowUtc);
    /// 校验恢复状态的 12 工位、基线和配置修订号，不修改传入状态。
    ShortageOperationResult validateRestoredState(
        const ShortageRuntimeState &state) const;

private:
    ShortageConfiguration m_configuration; ///< 值语义配置副本，构造后只读。
};
```

扣减使用 `checkedMultiply(qint64 delta, qint64 usage, qint64 *result)` 和 `checkedSubtract(...)` 私有函数；任一工位失败时 12 工位、基线和清零候选全部保持调用前状态。

- [ ] **Step 4: 实现明确的回退状态机**

状态分支固定为：

```text
无基线 + 有效样本             -> 只建立基线
current == baseline           -> 无变化
current > baseline            -> 按差值扣减并更新基线
current < baseline + 无候选   -> 保存候选，不改库存/基线
有候选 + current < candidate -> 只下移 candidate，不改库存/oldBaseline
有候选 + current == candidate -> 继续等待，无状态写入
有候选 + candidate < current < oldBaseline -> 确认清零，按 current 从 0 累计扣减
有候选 + current >= oldBaseline -> 候选为毛刺，按 current-oldBaseline 扣减
断线恢复标记 + current<baseline -> 返回 RequiresMaintenance，不走上述自动清零
```

- [ ] **Step 5: 运行账本测试**

Run:

```bash
cmake --build build-shortage --target shortage_ledger_tests --parallel
ctest --test-dir build-shortage -R '^shortage_ledger_tests$' --output-on-failure
```

Expected: 1/1 tests passed；LD-01～LD-08 全部通过。

- [ ] **Step 6: 检查点并按任务中文提交**

Run: `git diff --check`

Expected: 无输出。

---

### Task 5: 实现非抢占补料计划和统一 Engine 事务门面

**Requirements:** PL-01～PL-06、TK-04～TK-08、PS-11、PS-15、ER-03、ER-04。

**Files:**

- Create: `src/replenishmentplanner.h`
- Create: `src/replenishmentplanner.cpp`
- Create: `src/shortageengine.h`
- Create: `src/shortageengine.cpp`
- Create: `tests/test_replenishment_planner.cpp`
- Modify: `src/shortagetypes.h`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: `ShortageLedger`、`ShortageStateStore`、配置和运行状态。
- Produces: `ReplenishmentPlanner::reevaluate/requestManualBox/nextDispatchRequest/markDispatchResult/markTaskStarted/validateUnloadFact/markUnloaded/markTerminal`。
- Produces: `ShortageEngine`，供测试和生产态唯一复用。
- Ownership: `ShortageEngine` 唯一持有运行状态；Planner 只修改同一事务副本中的等待表、活动工位、补料单和失败计数。

- [ ] **Step 1: 写计划、幂等和失败保护测试**

```cpp
class ReplenishmentPlannerTest final : public QObject
{
    Q_OBJECT
private slots:
    void belowMinimumTriggersButEqualityDoesNot();       // PL-01。
    void reachingMaximumStopsWithoutExtraBox();          // PL-02。
    void triggerTimeThenStationIdDefinesStableOrder();   // PL-03。
    void activeStationIsNotPreemptedBeforeMaximum();     // PL-04。
    void oneStationHasAtMostOneNotUnloadedTask();        // PL-05。
    void rejectedDispatchRetriesSameOrderNumber();       // PL-06。
    void duplicateUnloadLocksAndDoesNotAddSecondBox();   // TK-04。
    void unknownOrMismatchedTaskLocksWithoutMutation();  // TK-05。
    void systemErrorReleasesNotUnloadedOccupation();     // TK-06/PS-11。
    void thirdPreUnloadFailurePausesOnlyThatStation();   // TK-07/ER-03。
    void manualHighStockOrderRequiresRiskConfirmation(); // TK-08。
    void restoredStateInstallsOnlyAfterFullValidation(); // PS-15：恢复值只能经 Engine 安装。
    void unsafeRestoreLocksWithoutReplacingEngineState();// PS-15：不安全恢复不发布部分状态。
};
```

- [ ] **Step 2: 运行并确认失败**

Run: `cmake --build build-shortage --target replenishment_planner_tests --parallel`

Expected: 编译失败，缺少 Planner/Engine 接口。

- [ ] **Step 3: 定义补料单和调度请求**

```cpp
/// Planner 只请求追加一箱，不暴露 FIFO 插队、重排或删除能力。
struct ShortageDispatchRequest {
    quint64 replenishmentOrderNo = 0; ///< 跨重启幂等编号；拒收重试不得改变。
    int stationId = 0;                ///< 目标代码工位 1～12。
    ReplenishmentOrigin origin = ReplenishmentOrigin::Automatic; ///< 自动或人工。
};

/// LineManager 任务信号翻译后的领域事实；Engine 不依赖 UI Task 结构。
enum class TaskFactKind {
    Started,         ///< 已从 FIFO 取出并开始执行。
    MaterialUnloaded,///< 唯一倒料动作已经完成。
    Succeeded,       ///< 全流程成功终态。
    Failed,          ///< 任务失败；是否倒料由补料单状态判断。
    Canceled,        ///< Stop/清队列取消。
    SystemError      ///< 设备级错误进入 Error。
};

struct TaskFact {
    TaskFactKind kind = TaskFactKind::Started; ///< 本次事实类型。
    quint64 replenishmentOrderNo = 0;          ///< 跨重启幂等键。
    quint64 taskId = 0;                        ///< 当前程序任务号。
    int stationId = 0;                         ///< 事实声称的代码工位。
    ReplenishmentOrigin origin = ReplenishmentOrigin::Automatic; ///< 正式来源。
    QDateTime occurredAtUtc;                   ///< 事实发生 UTC 时间。
    QString reasonZh;                          ///< 失败/取消/错误原因。
};

/// Engine 的统一结果；changed=false 时 UI 可以只追加日志而不重绘整表。
struct ShortageEngineResult {
    bool ok = false;          ///< 事务是否完整成功。
    bool changed = false;     ///< 正式运行状态是否改变。
    bool criticalLock = false;///< 是否已停止新的自动意图。
    bool hasProductionDelta = false; ///< 账本确认正增量时透传给 UI 和结构化日志。
    qint64 productionDelta = 0;      ///< 本轮实际用于扣减的产量增量，不做未定义上限判断。
    QString messageZh;        ///< 完整中文结果。
};

/// UI 只读快照；不暴露可修改的 Ledger/Planner 引用。
struct ShortageUiSnapshot {
    ShortageInputSource inputSource = ShortageInputSource::Mock; ///< 当前二选一来源。
    ShortageCommunicationState communication = ShortageCommunicationState::Stopped;
    ShortageRuntimeState runtime;              ///< 复制后的账本/计划摘要。
    bool hasLastProductionDelta = false;        ///< 最近一次已确认正增量是否可显示。
    qint64 lastProductionDelta = 0;             ///< 最近一次已入账扣减增量；每次有效增量均更新日志和该字段。
    QString summaryLine1Zh;                    ///< 产品/模式/actualQty/通信。
    QString summaryLine2Zh;                    ///< 活动工位/等待数/账本报警。
};

/// Planner 操作结果；ok=false 时传入状态副本不得再发布为正式状态。
struct PlannerApplyResult {
    bool ok = false;          ///< 计划状态是否完整更新。
    bool changed = false;     ///< 等待表、活动工位、补料单或失败计数是否变化。
    bool criticalLock = false;///< 未知/重复/错工位事实是否触发严重锁定。
    QString messageZh;        ///< 包含补料单、taskId、工位和处理动作。
};

class ReplenishmentPlanner final
{
public:
    explicit ReplenishmentPlanner(ShortageConfiguration configuration);

    /// 根据同一事务副本中的库存重建等待顺序；已有 firstLowAtUtc 不得刷新。
    PlannerApplyResult reevaluate(ShortageRuntimeState *state,
                                  const QDateTime &nowUtc);
    /// 高位人工任务需要风险确认；成功后在 state->orders 中只新增一箱。
    PlannerApplyResult requestManualBox(ShortageRuntimeState *state,
                                        int stationId,
                                        bool highStockRiskConfirmed,
                                        const QDateTime &nowUtc);
    /// 返回 AwaitingDispatch 补料单副本；不存在可派单时返回空且不改变状态。
    std::optional<ShortageDispatchRequest> nextDispatchRequest(
        const ShortageRuntimeState &state) const;
    /// accepted=false 只记录拒收原因；true 时绑定非 0 taskId 并标记 Queued。
    PlannerApplyResult markDispatchResult(ShortageRuntimeState *state,
                                          quint64 replenishmentOrderNo,
                                          bool accepted,
                                          quint64 taskId,
                                          const QString &reasonZh);
    /// 校验并把 Queued 标为 Running。
    PlannerApplyResult markTaskStarted(ShortageRuntimeState *state,
                                       const TaskFact &fact);
    /// 只校验倒料事实，供 Engine 在加库存前保证补料单/taskId/工位完全匹配。
    PlannerApplyResult validateUnloadFact(const ShortageRuntimeState &state,
                                          const TaskFact &fact) const;
    /// Engine 完成库存加箱后才调用，设置 unloadAccounted 和 Unloaded。
    PlannerApplyResult markUnloaded(ShortageRuntimeState *state,
                                    const TaskFact &fact);
    /// 按是否已经 Unloaded 写准确终态，并执行连续倒料前失败保护。
    PlannerApplyResult markTerminal(ShortageRuntimeState *state,
                                    const TaskFact &fact,
                                    int failureLimit);

private:
    ShortageConfiguration m_configuration; ///< 值语义配置副本，只供阈值/箱量查询。
};
```

- [ ] **Step 4: 实现稳定排序和非抢占规则**

`reevaluate()` 必须先保留已有首次触发时间，再为新低位工位记录 `nowUtc`；排序 comparator 固定为 `firstLowAtUtc`、`stationId`。已有活动工位只有在 `stock>=maximum`、暂停或严重锁定时释放；不得因后续工位库存更低而抢占。

拒收只把补料单保留在 `AwaitingDispatch`，不增加序列。一个工位存在 Queued/Running 未倒料单时 `nextDispatchRequest()` 返回空。

- [ ] **Step 5: 实现 Engine 的事务顺序**

```cpp
class ShortageEngine final
{
public:
    /// stateStore 为非拥有指针，调用方必须保证其生命周期覆盖 Engine。
    ShortageEngine(ShortageConfiguration configuration,
                   ShortageStateStore *stateStore);

    /// 只接受 StateStore::load() 的值结果；先校验来源、配置修订号、12 工位、基线、补料单和等待顺序，全部通过后才一次替换 m_state。
    /// 失败或 requiresMaintenance=true 时不安装传入 state，设置恢复锁定并禁止自动派单。
    ShortageEngineResult installRestoredState(
        const ShortageStateLoadResult &loadResult,
        const QDateTime &nowUtc);
    /// 新账本只有现场清空确认后才能建立；成功后仍等待操作员确认/LineManager Start。
    ShortageEngineResult initializeZero(bool siteIsConfirmedEmpty,
                                        const QDateTime &nowUtc);
    /// 启动恢复摘要确认；accepted=false 保持自动派单门禁关闭。
    ShortageEngineResult confirmRestoredState(bool accepted,
                                              const QDateTime &nowUtc);
    /// 采样事务：账本局部计算 -> Planner 重评估 -> 一条聚合流水 -> 发布新快照。
    /// 新上下文且旧任务未排空时只保存 pendingActualQty，不提前创建新产品任务。
    ShortageEngineResult applyStableSample(const ShortageSample &sample);
    /// oldTasksDrained=true 时激活待切换上下文，并按新用量从旧基线补扣到最新待处理产量。
    ShortageEngineResult activatePendingContextIfDrained(bool oldTasksDrained,
                                                         const QDateTime &nowUtc);
    /// 人工补一箱也生成正式补料单；高位时必须 highStockRiskConfirmed=true。
    ShortageEngineResult requestManualBox(int stationId,
                                          bool highStockRiskConfirmed,
                                          const QDateTime &nowUtc);
    /// 返回当前可派的一箱；没有条件时返回 std::nullopt，不偷偷创建 taskId。
    std::optional<ShortageDispatchRequest> nextDispatchRequest();
    /// 主调度接受后绑定 taskId；accepted=false 保留同一补料单等待重试。
    ShortageEngineResult recordDispatchResult(quint64 replenishmentOrderNo,
                                               bool accepted,
                                               quint64 taskId,
                                               const QString &reason);
    /// 任务从 FIFO 取出时标记 Running；未知绑定立即严重锁定。
    ShortageEngineResult recordTaskStarted(const TaskFact &fact);
    /// 唯一倒料事务：先校验补料单/taskId/工位，再加箱、标记 Unloaded、立即持久化。
    ShortageEngineResult recordMaterialUnloaded(const TaskFact &fact);
    /// 终态区分倒料前后，更新失败次数/暂停，绝不回滚已经入账的一箱。
    ShortageEngineResult recordTaskTerminal(const TaskFact &fact);
    /// 只允许在生产协调器复核全部门禁并完成维护备份后调用。
    ShortageEngineResult applyMaintenanceCorrection(
        const ShortageMaintenanceCorrection &correction,
        const QDateTime &nowUtc);
    /// 只读接口供 UI 确认和状态刷新；调用方不得 const_cast 修改。
    const ShortageConfiguration &configuration() const;
    const ShortageRuntimeState &state() const;

private:
    ShortageConfiguration m_configuration; ///< Engine 使用的配置修订副本。
    ShortageStateStore *m_stateStore = nullptr; ///< 非拥有指针，生命周期由调用方保证。
    ShortageLedger m_ledger;                    ///< 只操作 m_state 事务副本的账本规则。
    ReplenishmentPlanner m_planner;             ///< 只操作 m_state 事务副本的计划规则。
    ShortageRuntimeState m_state;                ///< 本进程唯一权威运行状态。
    bool m_restoreInstalled = false;             ///< true 表示已安全安装恢复状态，但仍可能等待操作员确认。
    bool m_restoreLocked = false;                ///< true 表示恢复结果不可安全使用，只允许维护流程。
};
```

恢复时，`AwaitingDispatch` 补料单尚未进入主 FIFO，可在安全安装和人工确认后继续使用原补料单号。任何未终态 `Queued`、`Running` 或 `Unloaded` 补料单都表示重启时无法仅凭文件确认主 FIFO/硬件现状，`installRestoredState()` 必须拒绝自动恢复并进入维护锁定，不让普通工人选择历史 taskId。

物理倒料后 `saveCritical()` 失败：Engine 内存保留新库存，把 `criticalLock=true` 和原因写入内存，`nextDispatchRequest()` 返回空。

- [ ] **Step 6: 运行计划与 Engine 测试**

Run:

```bash
cmake --build build-shortage --target replenishment_planner_tests --parallel
ctest --test-dir build-shortage -R '^replenishment_planner_tests$' --output-on-failure
```

Expected: 1/1 tests passed；PL-01～PL-06、TK-04～TK-08、PS-11、PS-15 通过。

- [ ] **Step 7: 检查点并按任务中文提交**

Run: `git diff --check`

Expected: 无输出。

---

### Task 6: 实现同轮采样、稳定确认和换型上下文

**Requirements:** CM-01～CM-12、CH-01、CH-02、CH-05；CP-01～CP-05 已由 Task 1 完成，CH-03/CH-04 由知道旧任务状态的 Task 10 验证。

**Files:**

- Create: `src/shortagesamplecoordinator.h`
- Create: `src/shortagesamplecoordinator.cpp`
- Modify: `tests/test_shortage_sample_coordinator.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: Task 1 唯一 `m_liveShortageScheduler` 的 `mesReplyReady/plcReplyReady` 和 Task 2 真实 MES/时间参数；工程中不存在第二通信对象或旧诊断 signals。
- Produces: `ShortageSampleCoordinator::stableSampleReady/sampleRejected/communicationStateChanged/contextChangeConfirmed`。
- 不调用 `ShortageEngine`；测试/生产调用方自行选择目标 Engine。

- [ ] **Step 1: 在 Task 1 测试类中追加采样和换型失败测试**

```cpp
class ShortageSampleCoordinatorTest final : public QObject
{
    Q_OBJECT
private slots:
    // CP-01～CP-05 已在 Task 1 保留于同一测试类；以下只追加采样状态机用例。
    void emitsOnlyAfterAllFourRepliesOfSameRound();       // CM-01。
    void timeoutRejectsWholeRound();                       // CM-02。
    void staleReplyCannotCompleteNewRound();               // CM-03。
    void productBitsRequireExactlyOneTrue();               // CM-04。
    void modeBitsRequireExactlyOneTrue();                  // CM-05。
    void contextRequiresTwoConsecutiveValidRounds();       // CM-06。
    void newTimingParametersApplyToNextRound();            // CM-07。
    void reconnectAtOrAboveBaselineProducesCatchupSample();// CM-08。
    void alarmTurnsRedAtConfiguredDuration();              // CM-09。
    void reconnectBelowBaselineRequiresMaintenance();      // CM-10。
    void httpAndJsonErrorsRejectWithoutPartialState();     // CM-11。
    void actualQtyUsesSignedSixtyFourBitValidation();       // CM-12。
    void oneRoundContextGlitchDoesNotSwitch();              // CH-01。
    void twoStableRoundsCreatePendingContext();             // CH-02。
    void allNineContextCombinationsAreRecognized();         // CH-05。
};
```

使用 C++ `FakeCustomSysScheduler` 子类主动发响应，不做真实网络请求，不使用 sleep；通过可注入 `QTimer`/测试触发方法推进超时。

- [ ] **Step 2: 运行并确认失败**

Run: `cmake --build build-shortage --target shortage_sample_coordinator_tests --parallel`

Expected: Task 1 的 CP-01～CP-05 继续通过，新追加 CM/CH 函数因 `ShortageSampleCoordinator` 尚不存在而编译或运行失败；不得修改 Task 1 协议断言使测试转绿。

- [ ] **Step 3: 把 Task 1 协议 target 扩展为采样协调器 target**

在现有 target 中只追加本任务新源文件，不重复创建 target，也不移除 Task 1 的源码根目录宏：

```cmake
# Task 1 已建立协议/删除契约 target；Task 6 只追加采样协调器实现。
target_sources(shortage_sample_coordinator_tests PRIVATE
    ../src/shortagesamplecoordinator.cpp)
```

- [ ] **Step 4: 实现一轮四响应聚合**

```cpp
class ShortageSampleCoordinator final : public QObject
{
    Q_OBJECT
public:
    /// scheduler 必须是唯一真实通信对象；工程中已不存在诊断对象或第二通信路径。
    explicit ShortageSampleCoordinator(CustomSysScheduler *scheduler,
                                        QObject *parent = nullptr);
    void setParameters(const ShortageParameters &parameters);

public slots:
    void start(); ///< 立即开始首轮，此后按 sampleIntervalSeconds 启动。
    void stop();  ///< 终止当前轮并使迟到响应失效，不清除上层账本。

signals:
    void stableSampleReady(ShortageSample sample);
    void sampleRejected(quint64 roundId, QString reason);
    void communicationStateChanged(ShortageCommunicationState state, QString reason);
    void contextChangeConfirmed(ProductModel product, ProductionMode mode);
};
```

每轮固定发四个请求：MES 日数据、PLC `StartAddress=68&length=2`、PLC `StartAddress=71&length=3`、PLC `StartAddress=1998&length=1`。聚合结构只接受 `roundId == activeRoundId` 且尚未完成的响应。三选一错误消息列出六个位真实值。

- [ ] **Step 5: 实现两轮稳定和换型等待**

第一轮合法组合只写候选；第二轮相同才输出稳定上下文。稳定上下文变化时只发 `contextChangeConfirmed`；是否可以激活由 Engine/生产协调器根据旧任务是否排空决定。采样层不得修改库存。

- [ ] **Step 6: 运行采样测试**

Run:

```bash
cmake --build build-shortage --target shortage_sample_coordinator_tests --parallel
ctest --test-dir build-shortage -R '^shortage_sample_coordinator_tests$' --output-on-failure
```

Expected: 1/1 tests passed；CP-01～CP-05、CM-01～CM-12、CH-01、CH-02、CH-05 全部通过。

- [ ] **Step 7: 运行全部既有回归并按任务中文提交**

Run:

```bash
ctest --test-dir build-shortage --output-on-failure
git diff --check
```

Expected：截至 Task 6 已加入的测试全部通过，原有 7 项测试仍为 7/7 通过，`git diff --check` 无输出。

---

### Task 7: 实现与正式状态隔离的完整逻辑测试控制器

**Requirements:** IN-01～IN-04，以及 CF/LD/PL/TK/CM/CH/PS 公开事件在测试态可驱动。

**Files:**

- Create: `src/shortagetestcontroller.h`
- Create: `src/shortagetestcontroller.cpp`
- Create: `tests/test_shortage_test_controller.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: 同一个 `ShortageEngine` 类型、`ShortageStateNamespace::StandaloneTest`，以及 Task 6 的唯一 `ShortageSampleCoordinator`；不直接连接 Task 1 Scheduler。
- Produces: 手工样本和任务事实注入 API、测试状态快照信号。

- [ ] **Step 1: 写隔离和完整事件测试**

```cpp
class ShortageTestControllerTest final : public QObject
{
    Q_OBJECT
private slots:
    void controllerOwnsTheSameEngineTypeAsProduction(); // IN-01。
    void simulatedDispatchNeverCallsMainFifo();          // IN-02。
    void everyTestActionEmitsNoHardwareCommand();        // IN-03。
    void testSaveClearReloadKeepsProductionHash();       // IN-04。
    void fullScenarioCoversSamplePlanUnloadFailureRestart(); // 完整闭环。
};
```

完整场景必须从 0 开始，手工输入产量，产生计划，模拟拒收/重试/接受、倒料前失败、成功倒料、倒料后失败、重复倒料严重锁定、保存重启恢复。

- [ ] **Step 2: 运行并确认失败**

Run: `cmake --build build-shortage --target shortage_test_controller_tests --parallel`

Expected: 缺少 Controller 导致编译失败。

- [ ] **Step 3: 实现测试控制器公共接口**

```cpp
class ShortageTestController final : public QObject
{
    Q_OBJECT
public:
    ShortageTestController(ShortageConfiguration configuration,
                           QString testStateDirectory,
                           ShortageSampleCoordinator *sampleCoordinator,
                           std::function<ShortageOperationResult()> fieldSamplingStartGuard,
                           QObject *parent = nullptr);

public slots:
    void initializeZeroAfterConfirmation();
    void applyManualSample(ProductModel product, ProductionMode mode, qint64 actualQty);
    void startFieldSampling();
    void stop();
    void simulateDispatchAccepted();
    void simulateDispatchRejected();
    void simulateFailureBeforeUnload();
    void simulateMaterialUnloaded();
    void simulateFailureAfterUnload();
    void simulateTaskSucceeded();
    void resendLastUnloadFact();
    void saveTestState();
    void reloadTestState();
    void clearTestStateAfterConfirmation();

signals:
    void snapshotChanged(ShortageUiSnapshot snapshot);
    void eventLogged(QString messageZh);
    void operationRejected(QString reasonZh);
};
```

Controller 不包含 `LineManager*`、`AgvController*`、`HuayanScheduler*`。模拟接受时由测试控制器生成测试专用 taskId，不能调用主 `TaskQueue`。

`sampleCoordinator` 和 guard 均为非拥有依赖：生产和测试共享同一个协议/采样实现，但 Controller 只有 `fieldSamplingStartGuard()` 返回 ok 时才把 `m_fieldSamplingActive` 置为 true。DeviceManager 注入的 guard 必须检查正式 Live 已停止；收到 `stableSampleReady` 时只有该标志为 true 才送测试 Engine，正式协调器处于 Mock 时同样忽略该样本。`stop()` 先清标志再停止采样，防止迟到响应进入任一 Engine。

- [ ] **Step 4: 运行隔离测试**

Run:

```bash
cmake --build build-shortage --target shortage_test_controller_tests --parallel
ctest --test-dir build-shortage -R '^shortage_test_controller_tests$' --output-on-failure
```

Expected: 1/1 tests passed；完整场景最终正式目录哈希、主 FIFO 计数和硬件信号计数均保持原值。

- [ ] **Step 5: 检查点并按任务中文提交**

Run: `git diff --check`

Expected: 无输出。

---

### Task 8: 实现宽屏配置与完整逻辑测试弹窗

**Requirements:** UI-01、UI-02、UI-07、UI-08；配置保存门禁和测试操作可视化。

**Files:**

- Create: `src/shortageconfigdialog.h`
- Create: `src/shortageconfigdialog.cpp`
- Create: `src/shortagerecoverydialog.h`
- Create: `src/shortagerecoverydialog.cpp`
- Create: `tests/test_shortage_dialog.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: `ShortageConfigStore`、`ShortageTestController`、只读 `ShortageUiSnapshot`。
- Produces: `configurationSaved()`、`maintenanceCorrectionRequested(...)`，不直接修改生产 Engine。

- [ ] **Step 1: 写 Qt Widgets 结构测试**

```cpp
class ShortageDialogTest final : public QObject
{
    Q_OBJECT
private slots:
    void configHasThreeProductTabsAndTwelveRowsEach(); // UI-01。
    void liveMesEndpointHasIndependentNamedEditor();   // UI-01：.228 真实地址由缺料配置独立管理，工程无 .229 旧输入框。
    void testBoundaryWarningIsAlwaysVisible();         // UI-02。
    void manualAndFieldSourcesAreExclusive();          // UI-02。
    void recoveryEditsOnlyOneStationWithReasonAndTypedId(); // UI-07。
    void ordinaryRecoveryShowsNoTaskDecisionControls();     // UI-08。
};
```

通过 `objectName` 查找控件，禁止测试依赖中文按钮在布局中的下标。

- [ ] **Step 2: 运行并确认失败**

Run: `cmake --build build-shortage --target shortage_dialog_tests --parallel`

Expected: 缺少 Dialog 类导致失败。

- [ ] **Step 3: 实现配置页确定布局**

`ShortageConfigDialog` 使用 `QTabWidget` 两个主页签；配置主页签内再放 88/88R/92 三个页签。每个 `QTableWidget` 恰好 12 行 11 列：

```text
代码工位 | NO | 现场位置 | 品号 | 启用 | 每箱 | 最低 | 最高 | L/R | L/L | R/H
```

表格横向自适应、窗口最小尺寸 1200×720、可缩放。参数区使用带 `objectName=liveMesDayEndpointEdit` 的 `QLineEdit` 编辑“真实缺料 MES 地址”，默认展示 `.228` 日数据接口并由 Task 2 校验完整 HTTP/HTTPS URL；这是工程中唯一 MES 地址输入框，不提供 `.229` 旧值迁移。采样间隔、单轮超时、通信报警时间和连续失败阈值使用 `QSpinBox` 显式限制合法范围。保存前调用配置校验和外部忙碌门禁，失败定位到产品页/行/列或具体参数控件。

- [ ] **Step 4: 实现完整逻辑测试页**

左表 12 行；右侧计划摘要和 8 个任务事件按钮；底部只读事件日志。手工源/现场源用 `QButtonGroup` exclusive。测试页所有按钮只连接 `ShortageTestController`。

- [ ] **Step 5: 实现异常恢复对话框**

`ShortageRecoveryDialog` 只接受一个只读快照，并提交 Task 2 已定义的
`ShortageMaintenanceCorrection`。对话框不得直接取得 Engine 指针。

不提供全清零和批量提交控件。

- [ ] **Step 6: 运行弹窗测试**

Run:

```bash
cmake --build build-shortage --target shortage_dialog_tests --parallel
QT_QPA_PLATFORM=offscreen ctest --test-dir build-shortage -R '^shortage_dialog_tests$' --output-on-failure
```

Expected: 1/1 tests passed；offscreen 下 5 个 UI 契约全部通过。

- [ ] **Step 7: 检查点并按任务中文提交**

Run: `git diff --check`

Expected: 无输出。

---

### Task 9: 在现有任务链增加来源、补料单和唯一倒料事实

**Requirements:** TK-01～TK-03、TK-06、RG-01～RG-05。

**Files:**

- Create: `tests/test_shortage_task_lifecycle.cpp`
- Modify: `src/lineconfig.h`
- Modify: `src/taskqueue.h`
- Modify: `src/taskexecutor.h`
- Modify: `src/taskexecutor.cpp:253-305`
- Modify: `src/linemanager.h`
- Modify: `src/linemanager.cpp:154-250` and error/queue cleanup helpers
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

- Produces: `TaskSource::{UiMock,LiveAutomatic,LiveManual}`、`Task::replenishmentOrderNo`。
- Produces: `TaskEnqueueResult LineManager::enqueueShortageTask(...)`。
- Produces: `TaskExecutor::materialUnloaded(Task)` 和 LineManager 事实信号。
- Consumes: 现有 `TaskQueue`、`TaskExecutor` 状态机；不改变动作调用。

- [ ] **Step 1: 写现有链路契约测试**

```cpp
class ShortageTaskLifecycleTest final : public QObject
{
    Q_OBJECT
private slots:
    void taskSourceHasThreeDistinctChineseLabels();       // TK-01。
    void enqueueAlwaysAppendsAndTakeNextKeepsOrder();      // TK-02/RG-01。
    void unloadFactExistsOnlyAtArmUnloadTransition();      // TK-03/RG-04。
    void systemErrorPublishesTerminalFactForCurrentTask(); // TK-06/RG-05。
    void stopStillClearsPendingAndEntersError();            // RG-02。
    void resetStillReturnsIdleWithoutStarting();            // RG-03。
};
```

无法无硬件驱动完整 `TaskExecutor` 时，使用 C++ 契约测试读取源码文本，断言 `materialUnloaded` 只位于 `case ExecState::ArmUnload`，并保留现有 `enterState(StowAfterUnload)` 紧邻顺序；仍不得使用 Python。

- [ ] **Step 2: 先运行并确认失败**

Run: `cmake --build build-shortage --target shortage_task_lifecycle_tests --parallel`

Expected: 新来源、补料单字段或事实信号不存在导致失败。

- [ ] **Step 3: 扩展 Task 但保持模拟默认值**

```cpp
/// 任务来源决定是否允许修改正式账本；模拟来源永远不能入账。
enum class TaskSource {
    UiMock,        ///< 现有 UI 模拟缺料。
    LiveAutomatic, ///< 正式账本自动计划的一箱任务。
    LiveManual     ///< 人工二次确认的一箱正式任务。
};

/// 统一 UI/FIFO/日志文案，避免各界面自行翻译产生不一致来源名称。
inline QString taskSourceText(TaskSource source)
{
    switch (source) {
    case TaskSource::UiMock:
        return QStringLiteral("模拟");
    case TaskSource::LiveAutomatic:
        return QStringLiteral("真实自动");
    case TaskSource::LiveManual:
        return QStringLiteral("人工补料");
    }
    return QStringLiteral("未知来源");
}

struct Task {
    // 保留现有字段和顺序；以下字段用于跨重启真实任务幂等，模拟任务固定为 0。
    quint64 replenishmentOrderNo = 0;
};
```

实际实现不得删除现有 `Task` 字段。`TaskQueue::enqueue()` 增加默认参数 `quint64 replenishmentOrderNo = 0` 并原样赋值，`m_pending.append(task)` 和 `takeFirst()` 完全不变。

- [ ] **Step 4: 增加唯一倒料事实信号**

在 `TaskExecutor` signals 增加：

```cpp
/// 仅在 ArmUnload 动作成功后、进入 StowAfterUnload 前发一次；不表示整个任务成功。
void materialUnloaded(const Task &task);
```

`onArmStageCompleted()` 只修改该分支：

```cpp
case ExecState::ArmUnload:
    // 修改前：倒料完成后直接进入收姿态，上层无法知道物料何时已经进入工位。
    // 修改后：先发布唯一物料事实，再保持原顺序进入 StowAfterUnload。
    // 不影响：机械臂倒料动作、收姿态、码垛和任务成功判定均不改变。
    emit materialUnloaded(m_task);
    enterState(ExecState::StowAfterUnload, QStringLiteral("倒料完成，机械臂收姿态"));
    break;
```

- [ ] **Step 5: 增加返回接受结果的真实队尾接口**

```cpp
// 放在 lineconfig.h，使 LiveShortageCoordinator 的抽象网关无需依赖 LineManager 类。
struct TaskEnqueueResult {
    bool accepted = false; ///< true 表示任务已经追加到 FIFO。
    quint64 taskId = 0;    ///< accepted=false 时必须为 0。
    QString reason;        ///< 拒收时可直接展示的中文原因。
};

/// 只追加任务；不插队、不重排、不启动未处于允许状态的任务。
TaskEnqueueResult enqueueShortageTask(int stationId,
                                      TaskSource source,
                                      quint64 replenishmentOrderNo);
```

`reportShortage(int)` 改为调用 `enqueueShortageTask(stationId, UiMock, 0)` 并保留原日志、Idle/Running/ReturningHome 行为。真实 source 必须要求非 0 补料单号；UiMock 必须要求 0。

- [ ] **Step 6: 转发完整任务事实和错误终态**

LineManager 新 signals：

```cpp
void shortageTaskAccepted(Task task);              ///< 已追加 FIFO。
void shortageTaskStarted(Task task);               ///< takeNext 后开始执行。
void shortageMaterialUnloaded(Task task);          ///< 原样转发唯一倒料事实。
void shortageTaskTerminal(Task task, QString reason); ///< 成功、失败、取消、系统错误均一次。
```

系统错误前复制当前任务、设置明确终态和错误原因，先发 `shortageTaskTerminal` 再走原 `enterError()`；清 Pending 前遍历 `pendingSnapshot()` 为真实任务发取消终态。模拟任务也可供 UI 日志使用，但生产协调器按 source 过滤。

- [ ] **Step 7: 运行任务链测试和既有码垛回归**

Run:

```bash
cmake --build build-shortage --target shortage_task_lifecycle_tests task_executor_pallet_commit_semantics_tests --parallel
ctest --test-dir build-shortage -R '^(shortage_task_lifecycle_tests|task_executor_pallet_commit_semantics_tests)$' --output-on-failure
```

Expected: 2/2 tests passed；原倒料后码垛状态顺序不变。

- [ ] **Step 8: 检查点并按任务中文提交**

Run: `git diff --check`

Expected: 无输出。

---

### Task 10: 实现生产协调器并在 DeviceManager 中一次性接线

**Requirements:** IN-05～IN-08、CH-03、CH-04、PS-15 启动接线、ER-01～ER-05、人工补料闭环、拒收重试。

**Files:**

- Create: `src/liveshortagecoordinator.h`
- Create: `src/liveshortagecoordinator.cpp`
- Create: `tests/test_live_shortage_coordinator.cpp`
- Modify: `src/devicemanager.h`
- Modify: `src/devicemanager.cpp:143-220`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: `ShortageEngine`、`ShortageSampleCoordinator`、`LineManager` 事实信号。
- Produces: `setInputSource/requestManualBox/confirmRecoveredState/applyMaintenanceCorrection` 和 UI 快照。
- 内部使用可替换 `IShortageTaskGateway`，测试用 fake，生产用 LineManager adapter。

- [ ] **Step 1: 写生产协调器测试**

```cpp
class LiveShortageCoordinatorTest final : public QObject
{
    Q_OBJECT
private slots:
    void mockTaskUnloadNeverChangesFormalLedger();       // IN-05。
    void switchingToMockStopsNewIntentButFinishesReal(); // IN-06。
    void selectingLiveDoesNotStartLineManager();         // IN-07。
    void repeatedModeSwitchDoesNotDuplicateConnections();// IN-08。
    void oldTasksMustDrainBeforeContextActivation();      // CH-03。
    void contextSwitchDoesNotChangeStationStocks();       // CH-04。
    void dispatchAppendsOneBoxAndRetriesRejection();      // PL-05/PL-06。
    void manualBoxUsesSameLedgerAndAudit();               // 人工闭环。
    void inputErrorRejectsOnlyCurrentRound();             // ER-01。
    void invalidConfigBlocksLiveMode();                   // ER-02。
    void pausedStationDoesNotBlockOtherStations();        // ER-03。
    void criticalLockStopsNewAutomaticOnly();             // ER-04。
    void everyErrorContainsStructuredChineseDetails();    // ER-05。
    void startupInstallsRestoredStateBeforeUserConfirm();  // PS-15：先安装恢复值，仍不自动启动。
    void deviceManagerUsesOnlyTheSingleLiveScheduler();      // CP-05/IN-08：只复用 Task 1 唯一对象且只接线一次。
};
```

- [ ] **Step 2: 运行并确认失败**

Run: `cmake --build build-shortage --target live_shortage_coordinator_tests --parallel`

Expected: 缺少生产协调器导致失败。

- [ ] **Step 3: 定义可测试的队尾网关和生产接口**

```cpp
class IShortageTaskGateway
{
public:
    virtual ~IShortageTaskGateway() = default;
    /// 实现只能调用 LineManager 队尾追加接口；不得暴露重排或删除函数。
    virtual TaskEnqueueResult append(int stationId,
                                     TaskSource source,
                                     quint64 replenishmentOrderNo) = 0;
    virtual LineSystemState lineState() const = 0;
};

/// 真实按钮二次确认所需的只读内容；普通 UI 不展示内部补料单号。
struct ManualBoxConfirmation {
    ProductModel product = ProductModel::Model88; ///< 当前稳定产品。
    ProductionMode mode = ProductionMode::LeftRight; ///< 当前稳定模式。
    int stationId = 0;             ///< 用户点击的代码工位。
    QString sitePosition;          ///< 当前配置的现场位置。
    QString partNumber;            ///< 当前配置品号。
    qint64 boxQuantity = 0;        ///< 确认后只创建这一箱。
    qint64 currentStock = 0;       ///< 点击时正式库存。
    qint64 minimumStock = 0;       ///< 当前最低位。
    qint64 maximumStock = 0;       ///< 当前最高位。
    bool highStockRisk = false;    ///< 当前库存已达到/超过最高位时为 true。
};

class LiveShortageCoordinator final : public QObject
{
    Q_OBJECT
public:
    /// 三个依赖均由 DeviceManager 持有且生命周期长于本对象；本类不删除非拥有指针。
    LiveShortageCoordinator(ShortageEngine *engine,
                            ShortageSampleCoordinator *sampleCoordinator,
                            IShortageTaskGateway *taskGateway,
                            QObject *parent = nullptr);
    /// 返回工位当前配置和账本摘要；失败时 stationId=0，由 UI 显示协调器错误。
    ManualBoxConfirmation manualBoxConfirmation(int stationId) const;
public slots:
    void setInputSource(ShortageInputSource source);
    /// 启动恢复后只有 accepted=true 才解除“等待操作员确认”；false 保持停止。
    void confirmRecoveredState(bool accepted);
    void requestManualBox(int stationId, bool highStockRiskConfirmed);
    /// 保存前再次校验整线/采样/当前任务/FIFO，并先强制维护备份。
    void applyMaintenanceCorrection(ShortageMaintenanceCorrection correction);
    void onStableSample(ShortageSample sample);
    void onLineStateChanged(LineSystemState state, QString text);
    void onTaskAccepted(Task task);
    void onTaskStarted(Task task);
    void onMaterialUnloaded(Task task);
    void onTaskTerminal(Task task, QString reason);

signals:
    void snapshotChanged(ShortageUiSnapshot snapshot);
    void criticalAlarmRaised(QString reasonZh);
    void operationRejected(QString reasonZh);
};
```

`ShortageInputSource` 只有 `Mock` 和 `Live` 两值；默认 `Mock`。切换 Live 不调用 gateway 的 Start 方法，因为接口中根本不提供 Start。

- [ ] **Step 4: 实现来源过滤和自动派单泵**

`pumpDispatch()` 只在 Live、账本已确认、配置有效、无严重锁定、LineManager Running 时调用 `engine.nextDispatchRequest()`。gateway 拒收后调用 `recordDispatchResult(...false...)`；收到下一次 Running 状态或队列事实后重试同一补料单。

`onMaterialUnloaded()` 对 UiMock 只写日志；对 LiveAutomatic/LiveManual 构造 `TaskFact` 并交 Engine。不得在协调器直接 `stock += box`。

- [ ] **Step 5: 在 DeviceManager 中只创建和连接一次**

对象所有权：Task 1 已删除 `m_customSysScheduler` 并创建唯一 parent 持有的 `m_liveShortageScheduler`；本任务复用该对象，再由 `DeviceManager` 持有唯一 `ShortageSampleCoordinator`、`ShortageTestController` 和 `LiveShortageCoordinator`。测试与生产协调器共享采样实现但各持有不同 Engine/StateStore，且通过“测试现场源/正式 Live 互斥”门禁隔离运行。所有 connect 只在 `DeviceManager` 构造函数执行，弹窗打开和来源切换不得重新 connect。

`DeviceManager` 的成员顺序固定为：正式/测试 `ShortageStateStore` 和 Engine 的 `std::unique_ptr`、`std::unique_ptr<IShortageTaskGateway>`，随后是真实通信对象、唯一采样协调器、测试控制器和生产协调器指针。必须在 `~DeviceManager()` 中先停止测试现场源和正式采样，再显式删除生产协调器、测试控制器、采样协调器和真实通信对象，最后才释放非 QObject 核心；不依赖 QObject 基类析构阶段的子对象删除顺序。

启动顺序固定为：

1. 加载并校验缺料配置，将 `liveMesDayEndpoint` 只设置给 `m_liveShortageScheduler`。
2. 构造 StateStore/Engine，调用 `StateStore::load()` 后把完整 LoadResult 交给 `Engine::installRestoredState()`。
3. 安装成功才创建并接线唯一采样协调器、测试控制器和生产协调器；测试现场源初始停止，生产协调器初始为 Mock/Stopped，等待界面显示恢复摘要和人工确认。
4. 恢复失败时可创建只读锁定协调器用于展示错误，但不启动 `m_liveShortageScheduler`、不调用 Engine 派单接口。

生产网关在 `devicemanager.cpp` 定义为只转发适配器，接口固定如下；该类没有重排方法：

```cpp
class LineManagerShortageGateway final : public IShortageTaskGateway
{
public:
    /// lineManager 由 DeviceManager 持有；适配器不负责释放。
    explicit LineManagerShortageGateway(LineManager *lineManager)
        : m_lineManager(lineManager) {}

    TaskEnqueueResult append(int stationId,
                             TaskSource source,
                             quint64 replenishmentOrderNo) override
    {
        return m_lineManager->enqueueShortageTask(
            stationId, source, replenishmentOrderNo);
    }

    LineSystemState lineState() const override
    {
        return m_lineManager->state();
    }

private:
    LineManager *m_lineManager = nullptr; ///< 非拥有指针，只提供队尾追加和状态读取。
};
```

唯一 `m_liveShortageScheduler` 的 roundId signals 只连接采样层；源码契约必须继续证明不存在旧诊断 signals、第二通信对象或 `.229` 输入路径。

- [ ] **Step 6: 运行协调器测试**

Run:

```bash
cmake --build build-shortage --target live_shortage_coordinator_tests --parallel
ctest --test-dir build-shortage -R '^live_shortage_coordinator_tests$' --output-on-failure
```

Expected: 1/1 tests passed；重复切换 20 次后每个倒料事实只处理一次。

- [ ] **Step 7: 检查点并按任务中文提交**

Run: `git diff --check`

Expected: 无输出。

---

### Task 11: 重构主调度监控 UI 并接入配置、测试和异常恢复弹窗

**Requirements:** UI-03～UI-08、IN-06～IN-08。

**Files:**

- Create: `tests/test_live_shortage_ui.cpp`
- Modify: `src/mainwindow.h`
- Modify: `src/mainwindow.cpp:602-703`
- Modify: `src/mainwindow.cpp:953-1045`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

- Consumes: `DeviceManager::liveShortageCoordinator()`、`ShortageUiSnapshot`、两个 Dialog。
- UI 不调用 `ShortageLedger`/`ReplenishmentPlanner`。

- [ ] **Step 1: 写主界面 C++ UI 契约测试**

```cpp
class LiveShortageUiTest final : public QObject
{
    Q_OBJECT
private slots:
    void mockAndLiveRadiosAreExclusiveAndMockIsDefault(); // UI-03。
    void twelveButtonsKeepCompactStationLabels();          // UI-04。
    void liveButtonBuildsMatchingStationConfirmation();    // UI-04。
    void stationAndFifoTablesShareOneTabWidget();           // UI-05。
    void fifoShowsTaskSourceAndCountInTabTitle();            // UI-05。
    void activeAutomaticPlanDisablesManualButtons();        // UI-06。
    void configAndRecoveryOpenSeparateDialogs();             // UI-07/UI-08。
};
```

若 `MainWindow` 无法在测试环境链接厂商 SDK，则使用 C++ 源码契约测试验证对象名、列标题、exclusive 组和调用方向；不得用 Python。

- [ ] **Step 2: 运行并确认失败**

Run: `cmake --build build-shortage --target live_shortage_ui_tests --parallel`

Expected: 缺少来源单选、页签或来源列导致失败。

- [ ] **Step 3: 调整调度监控布局**

在 Start/Stop 下增加 exclusive 单选组，默认模拟。12 按钮仍为 4×3 和“工位X”；标题随来源变成“模拟缺料”或“人工补料”。

原 FIFO 表替换为一个 `QTabWidget`：

- 工位状态：4 列“工位、库存、最低/最高、状态”，12 行只读。
- FIFO 队列(n)：5 列“任务号、工位、来源、状态、入队时间”。

真实摘要固定两行，禁止把“580→600/补料中”塞进 12 个按钮。

- [ ] **Step 4: 实现工位按钮二选一行为**

模拟来源继续调用 `LineManager::reportShortage(stationId)`。真实来源先调用协调器只读接口 `manualBoxConfirmation(stationId)` 构造确认框，确认文本逐项显示产品、模式、工位、位置、品号、每箱、当前库存、上下限；确认后只调用 `requestManualBox(stationId, riskConfirmed)`。

高位风险确认是第二个明确对话框；任一取消均不产生任务。

- [ ] **Step 5: 接入宽屏弹窗和异常恢复**

旧的狭长内嵌缺料测试区改为一个“缺料配置与完整逻辑测试…”按钮。Task 1 已删除的“客户系统通信测试”区域不得重新加入；真实 MES 地址只出现在宽屏缺料配置页。

异常恢复入口只有协调器报告允许条件时启用；提交后由协调器再次校验“整线停止、真实停止、FIFO 空、当前任务空”，UI 预检查不能代替业务层检查。

- [ ] **Step 6: 运行 UI 契约和弹窗测试**

Run:

```bash
cmake --build build-shortage --target live_shortage_ui_tests shortage_dialog_tests --parallel
QT_QPA_PLATFORM=offscreen ctest --test-dir build-shortage -R '^(live_shortage_ui_tests|shortage_dialog_tests)$' --output-on-failure
```

Expected: 2/2 tests passed；UI-01～UI-08 全部有自动契约验证。

- [ ] **Step 7: 检查点并按任务中文提交**

Run: `git diff --check`

Expected: 无输出。

---

### Task 12: 全量 C++ 回归、文档同步和现场验收清单

**Requirements:** IN-09、IN-10、RG-06，以及 spec 第 19 节全部现场步骤。

**Files:**

- Modify: `README.md`
- Modify: `changelog/CHANGELOG.md`
- Modify: `docs/superpowers/plans/2026-06-16-custom-system-communication.md`
- Modify when implementation differs: `docs/superpowers/specs/2026-07-13-live-shortage-ledger-design.md`
- Modify when implementation differs: `docs/superpowers/plans/2026-07-13-live-shortage-ledger.md`
- Modify when conclusions differ: `docs/shortage-signal-analysis/2026-07-12-customer-shortage-confirmation.md`
- Modify when analysis differs: `docs/shortage-signal-analysis/2026-07-12-internal-shortage-logic-analysis.md`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

- Consumes: Tasks 1～11 全部 targets。
- Produces: 可复现 Linux 构建结果、编号追踪表、现场逐项记录表和 Task 12 中文提交。

- [ ] **Step 1: 检查全部自动用例编号都有唯一测试函数**

Run:

```bash
rg -n 'CF-|LD-|PL-|TK-|CP-|CM-|CH-|PS-|UI-|IN-|ER-|RG-' \
  docs/superpowers/specs/2026-07-13-live-shortage-ledger-design.md \
  docs/superpowers/plans/2026-07-13-live-shortage-ledger.md \
  tests/test_shortage_*.cpp \
  tests/test_live_shortage_*.cpp
```

Expected: spec 第 18 节每个编号都能追踪到本 plan 的任务和一个明确 C++ 测试函数；没有只有文档编号而无测试的条目。

- [ ] **Step 2: 从干净构建目录配置和编译全部目标**

Run:

```bash
cmake -E remove_directory build-shortage
/opt/Qt/qt6.8/6.8.3/gcc_64/bin/qt-cmake \
  -S . \
  -B build-shortage \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-shortage --parallel
```

Expected: configure 和 build 退出码均为 0；Qt Creator 可看到生产源在 `src`、十个新测试 target 在 `tests`；旧分支文件没有被整体带入当前工作树。

- [ ] **Step 3: 运行全部 C++ 测试**

Run:

```bash
QT_QPA_PLATFORM=offscreen ctest --test-dir build-shortage --output-on-failure
```

Expected: 现有 7 个 CTest 用例和新增 10 个 CTest 用例全部通过，即 17/17、0 failed；不得通过排除旧测试获得通过结果。

- [ ] **Step 4: 执行源码边界检查**

Run:

```bash
rg -n 'insert|prepend|move|swap' src/taskqueue.h src/linemanager.cpp
rg -n 'Shortage(TestController|StateNamespace::StandaloneTest)' src tests
rg -n 'python|\.py' CMakeLists.txt tests/CMakeLists.txt docs/superpowers/plans/2026-07-13-live-shortage-ledger.md
! rg -n '192\.168\.115\.229|DayRecord|defaultEndpoint\(|testConnectivity\(|fetchDayData\(|dayDataReady|m_customSys|customSysEndpoint|onCustomSystem|initCustomSystemPanel|客户系统通信测试' \
  src/mainwindow.h src/mainwindow.cpp \
  src/devicemanager.h src/devicemanager.cpp \
  src/customSysScheduler.h src/customSysScheduler.cpp
git diff --check
```

Expected:

- `TaskQueue` 仍只有 append/takeFirst，不出现真实缺料重排调用。
- StandaloneTest 只流向测试 Controller/测试目录，不流向正式协调器。
- CMake 测试不调用 Python；plan 中出现“Python”只能是禁止使用的约束文本。
- 旧 `.229`、DayRecord、诊断 API/UI/配置和 `m_customSysScheduler` 零匹配；`m_liveShortageScheduler` 的唯一所有权由 CP-05 正向断言覆盖。
- `git diff --check` 无输出。

- [ ] **Step 5: 同步用户文档**

`README.md` 删除“客户系统通信测试”功能、文件职责和架构节点的现行说明，再写明：模拟/真实二选一、唯一真实通信层、真实账本需安全恢复、完整测试与正式隔离、状态文件位置和维护联系提示。

`changelog/CHANGELOG.md` 用中文写明：新增正式账本、Sheet3 36 条配置、完整测试弹窗、主流程真实缺料、FIFO 不重排、持久化/恢复和 Linux C++ 验证。

`docs/superpowers/plans/2026-06-16-custom-system-communication.md` 保留历史正文，但在标题下增加醒目状态：“历史方案，现行客户系统通信测试已废弃；UI 和诊断 API 已由 2026-07-13 真实缺料方案删除，禁止按本文重新接回。”

如果代码接口或规则与 spec/plan 不一致，先修改文档使之与最终代码完全一致，再重新执行 Steps 1～4。

- [ ] **Step 6: 复核现场阶段 0——旧诊断删除与真实协议层**

1. 启动主程序，检查原“客户系统通信测试”分组、`.229` 地址框、“测试连接”和“读取数据”按钮均不存在。
2. 打开缺料配置弹窗，确认工程中只有“真实缺料 MES 地址”一个输入入口，默认值为 `.228` 日数据接口。
3. 在不启用自动派单的条件下发起真实采样，记录一轮 MES 和三组 PLC 的 roundId、地址范围及结果。
4. 运行 CP-05 删除契约并保存测试输出；再次执行 Step 4 的源码零残留扫描。

通过：旧 UI、配置、API、signals、`DayRecord` 和 `.229` 零残留；`DeviceManager` 只有一个 `m_liveShortageScheduler`；四个真实请求可按同一 roundId 返回，且 FIFO 新增任务数为 0。任一旧入口仍可见或真实协议失败，停止阶段 1。

- [ ] **Step 7: 建立现场逐项记录表并执行阶段 1**

对每个用例记录：编号、日期、操作人、配置/输入、操作步骤、预期、实际、日志/截图路径、通过/失败、问题号。

阶段 1 实际步骤：

1. 打开弹窗，逐产品逐行核对 36 条配置和真实缺料 MES 地址，连续保存两版配置后破坏测试主配置，确认从上一版备份恢复；执行 CF-01～CF-06。
2. 手工输入依次执行 LD-01～LD-08；LD-07 必须分别输入 `1000→2→5`、`1000→900→500→0→2→5`、`1000→2→1005` 并核对扣减均符合用例定义。
3. 构造多工位低位，执行 PL-01～PL-06。
4. 逐个任务事件按钮执行 TK-01～TK-08。
5. 执行 CM、CH、PS 中无需现场网络/硬件的全部场景。
6. 关闭弹窗，对比主 FIFO、正式状态文件哈希和设备日志计数。

通过：每个编号都有实际值且等于预期；主 FIFO/正式账本/硬件计数完全不变。任一缺项或不一致，停止后续阶段。

- [ ] **Step 8: 执行现场阶段 2——真实通信只监控**

1. 真实自动派单保持禁用，使用 Task 1 建立的唯一 `.228` 真实通信对象连续采样 30～60 分钟。
2. 导出每轮 roundId、开始/结束时间、四响应、产品、模式、`actualQty`。
3. 断网一次并恢复；制造或等待一个迟到响应。
4. 用 C++ 删除契约和现场界面检查再次确认没有 `.229` 输入框、旧通信测试按钮或第二通信请求路径。
5. 核对期间 FIFO 新增真实任务数为 0。

通过：没有跨轮响应；两轮稳定正确；无效轮不改账本；旧诊断 UI/API 零残留且唯一真实通信工作正常；FIFO 新增数为 0。

- [ ] **Step 9: 执行现场阶段 3——单工位真实闭环**

1. 只启用一个经现场确认的工位，维护人员确认现场清空后从 0 建账。
2. 开启真实来源但仍由原 Start 控制整线。
3. 观察一箱进入 FIFO、任务开始、倒料事实、库存增加、收姿态/码垛终态。
4. 重复补箱直到库存达到或超过最高位。

通过：同工位最多一个未倒料任务；每次倒料只加一箱；达到高位后无新意图；FIFO 原顺序不变。

- [ ] **Step 10: 执行现场阶段 4——失败和通信异常**

1. 在倒料前制造一次可控失败，记录库存原值和终态。
2. 倒料完成后在收姿态/后续步骤制造可控失败。
3. 连续三次倒料前失败，确认只暂停目标工位。
4. 验证维护人员人工补料和解除暂停审计。
5. 制造短断线、达到报警时长的断线和恢复值回退条件。

通过：倒料前库存保持原值；倒料后保留一箱；第三次前失败暂停；回退恢复锁定；中文报警包含原因和处理动作。

- [ ] **Step 11: 执行现场阶段 5——重启与恢复**

1. 分别在无任务、任务排队、倒料前、倒料后收尾状态保存并重启。
2. 验证主快照恢复，并确认 LoadResult 必须先经 `Engine::installRestoredState()` 完整校验和一次性安装；安装后仍等待人工确认，不自动 Start。
3. 在测试副本损坏主快照，验证备份恢复。
4. 在测试副本验证流水重放和三份全损坏锁定。

通过：安全状态逐字段一致且不自动 Start；`AwaitingDispatch` 可在安全安装和人工确认后继续，存在 Queued/Running/Unloaded 非终态任务时进入维护锁定；不可确认状态只提示联系维护；同一补料单重启前后最多入账一次。

- [ ] **Step 12: 执行现场阶段 6——12 工位顺序和 FIFO**

1. 恢复 12 工位配置。
2. 用同一轮和不同时刻构造多个工位低位。
3. 记录待补料工位顺序、活动工位和每次 FIFO 追加。
4. 同时保留一条既有模拟/调试任务验证队列顺序。

通过：时间优先、同时间工位号优先；活动工位补至高位才切换；其他低位工位不提前全塞 FIFO；既有任务不移动。

- [ ] **Step 13: 执行现场阶段 7——换型和长时间运行**

1. 逐一选择 88/88R/92 与 L/R、L/L、R/H 九种组合。
2. 在 FIFO/运行任务存在时换型，记录两轮稳定和实际切换点。
3. 连续运行一个完整班次，覆盖一次 `actualQty` 清零。
4. 抽查现场库存与上位机账本；单独记录清零采样尾数误差。

通过：九种用量正确；旧任务终态后才切新上下文；库存不清零；清零只重建基线；客户在验收单签字接受当前采样尾数误差边界，或在不接受时提供额外清零/最终计数信号后再实施相应变更。

### Task 12 文档同步与现场记录表

#### 自动编号追踪表

| 编号组 | 自动验证入口 | Task 12 复核动作 |
| --- | --- | --- |
| CF-01～CF-06 | `shortage_config_tests` | `rg` 编号追踪确认配置默认值、校验、保存/恢复和编辑门禁均有测试函数。 |
| LD-01～LD-08 | `shortage_ledger_tests` | 确认三种 `actualQty` 清零/回退序列仍由账本测试覆盖。 |
| PL-01～PL-06 | `replenishment_planner_tests`、`live_shortage_coordinator_tests` | 确认非抢占补至最高位、同时间工位号排序和 FIFO 只追加覆盖。 |
| TK-01～TK-08 | `shortage_task_lifecycle_tests`、`replenishment_planner_tests` | 确认任务事实、倒料入账、失败保护和幂等终态覆盖。 |
| CP-01～CP-05 | `shortage_sample_coordinator_tests` | 确认旧诊断删除契约和 `.228` 真实协议契约覆盖。 |
| CM-01～CM-12、CH-01～CH-05 | `shortage_sample_coordinator_tests`、`live_shortage_coordinator_tests` | 确认采样轮次、迟到响应、两轮稳定、换型等待和生产接线覆盖。 |
| PS-01～PS-15 | `shortage_state_store_tests`、`replenishment_planner_tests`、`live_shortage_coordinator_tests` | 确认主快照、备份、流水、全损坏锁定和 `installRestoredState()` 安装门禁覆盖。 |
| UI-01～UI-08 | `shortage_dialog_tests`、`live_shortage_ui_tests` | 确认配置弹窗接入 `ShortageTestController`，主界面来源切换、FIFO/状态页签和恢复入口覆盖。 |
| IN-01～IN-10 | `shortage_test_controller_tests`、`live_shortage_coordinator_tests`、Task 12 全量 build/ctest | 确认测试态与生产态状态隔离、正式接线和 Linux 全量验证覆盖。 |
| ER-01～ER-05、RG-01～RG-06 | `live_shortage_coordinator_tests`、`shortage_task_lifecycle_tests`、Task 12 边界扫描 | 确认异常报警、正式任务终态释放、既有 FIFO 不重排和全量回归覆盖。 |

#### 现场逐项记录表模板

现场执行 Step 6～13 时，每个编号或场景必须保留一行记录：

| 编号/阶段 | 日期 | 操作人 | 配置/输入 | 操作步骤 | 预期 | 实际 | 日志/截图路径 | 通过/失败 | 问题号 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 示例：LD-07 | 待填 | 待填 | `1000→2→5` / `1000→900→500→0→2→5` / `1000→2→1005` | 在完整逻辑测试页输入序列并观察扣减 | 扣减分别符合用例定义 | 待现场填写 | 待现场填写 | 待现场填写 | 待现场填写 |

#### 上位机实际操作验证步骤

1. 从干净构建产物启动主程序，确认左侧不再有“客户系统通信测试”、`.229` 地址框、“测试连接”和“读取数据”按钮。
2. 打开“缺料配置与完整逻辑测试...”弹窗，逐产品核对 36 条配置、真实缺料 MES 地址、采样间隔、超时和保护参数。
3. 在完整逻辑测试页执行手工采样、真实采样和任务事件按钮，观察测试摘要变化；同时记录正式状态文件哈希和 FIFO 计数，确认测试不改正式账本、不入主 FIFO、不控制硬件。
4. 回到主界面切换“模拟缺料/真实缺料”，确认默认模拟；真实模式下有活动正式补料计划时，手工工位按钮被业务门禁阻止，现有 FIFO 顺序不移动。
5. 执行“异常恢复...”入口，确认普通 UI 只允许单工位、原因和二次确认；提交后仍由业务层再次复验 LineManager、采样、当前任务和 FIFO 门禁。

#### 产线现场验证成功标准

- 旧通信测试 UI/API/配置、`.229`、`DayRecord` 和第二通信对象零残留；真实通信只通过 `.228` MES/PLC 协议层进入采样协调器。
- 真实自动派单关闭时，真实采样不会新增 FIFO 任务；真实自动派单开启后，同一工位最多一个未倒料正式任务，倒料完成才入账一箱。
- 多工位低位按首次触发时间排序，同时间按工位号排序；活动工位补至达到或超过最高位后才切换。
- 倒料前失败不加库存，倒料后失败保留一箱，连续倒料前失败达到阈值只暂停目标工位并给出中文处理动作。
- 程序重启或断电恢复后，安全状态逐字段一致且不自动 Start；不可确认状态只提示联系维护人员。
- 一个完整班次内 88/88R/92 与 L/R、L/L、R/H 九种组合用量正确，`actualQty` 清零只重建基线、不清库存；尾数误差边界由客户签字确认或提供额外信号后再改造。

#### 遗留/现场待确认清单

1. Step 6～13 尚需在真实 MES、PLC、AGV、机械臂和现场物料条件下逐项实机执行；本轮 Task 12 只完成文档清单和自动验证。
2. 主界面工位表“最低/最高”列当前暂显示 `-`，配置弹窗和人工补料确认弹窗显示准确上下限；若现场要求主界面逐行展示上下限，需要新增后续 UI 任务。
3. `actualQty` 清零采样尾数误差当前按既定规则记录并待客户签字确认；如客户不接受，需提供清零/最终计数信号后再实施规则变更。
4. 状态文件不可恢复时的人工库存核对流程需由现场维护人员确认权限、签字和日志归档路径。
5. 真实通信 30～60 分钟监控、短断线/长断线恢复和一个完整班次验证需在客户网络稳定窗口执行。

- [ ] **Step 14: 最终检查并执行 Task 12 单独中文提交**

Run:

```bash
git status --short
git diff --stat
git diff --check
```

Expected: 只包含计划内文件；无空白错误；按用户最新要求，Task 12 完成全量自动验证和文档同步后执行本任务单独中文提交。现场 Step 6～13 仍属于后续实机验收记录，不能用本地 CTest 冒充现场通过。本任务提交示例：

```bash
git add README.md changelog/CHANGELOG.md \
  docs/superpowers/plans/2026-06-16-custom-system-communication.md \
  docs/superpowers/specs/2026-07-13-live-shortage-ledger-design.md \
  docs/superpowers/plans/2026-07-13-live-shortage-ledger.md \
  docs/shortage-signal-analysis/2026-07-12-customer-shortage-confirmation.md \
  docs/shortage-signal-analysis/2026-07-12-internal-shortage-logic-analysis.md \
  .superpowers/sdd/task-12-report.md
git commit -m "同步真实缺料文档与最终验收清单"
```

Expected: 只暂存 Task 12 范围文件；明确排除既有任务外 dirty 文件，例如 `docs/superpowers/specs/2026-06-25-12-station-continuous-replenishment-design.md` 和 `tests/test_station_pickup_config.cpp`。

---

## 2. 自动验收编号到任务映射

| 编号组 | 实施任务 | C++ target |
| --- | --- | --- |
| CF-01～CF-06 | Task 2 | `shortage_config_tests` |
| LD-01～LD-08 | Task 4 | `shortage_ledger_tests` |
| PL-01～PL-06 | Task 5，Task 10 做生产闭环 | `replenishment_planner_tests`、`live_shortage_coordinator_tests` |
| TK-01～TK-03、TK-06 | Task 9 | `shortage_task_lifecycle_tests` |
| TK-04～TK-08 | Task 5 | `replenishment_planner_tests` |
| CP-01～CP-05 | Task 1 | `shortage_sample_coordinator_tests` |
| CM-01～CM-12 | Task 6 | `shortage_sample_coordinator_tests` |
| CH-01、CH-02、CH-05 | Task 6 | `shortage_sample_coordinator_tests` |
| CH-03、CH-04 | Task 10 | `live_shortage_coordinator_tests` |
| PS-01～PS-10、PS-12～PS-14 | Task 3 | `shortage_state_store_tests` |
| PS-11 | Task 5 | `replenishment_planner_tests` |
| PS-15 | Task 5 验证 Engine、Task 10 验证启动接线 | `replenishment_planner_tests`、`live_shortage_coordinator_tests` |
| UI-01、UI-02、UI-07、UI-08 | Task 8 | `shortage_dialog_tests` |
| UI-03～UI-06 | Task 11 | `live_shortage_ui_tests` |
| IN-01～IN-04 | Task 7 | `shortage_test_controller_tests` |
| IN-05～IN-08 | Task 10 | `live_shortage_coordinator_tests` |
| IN-09、IN-10 | Task 12 | 全量 build/ctest 和目录检查 |
| ER-01～ER-05 | Task 10 | `live_shortage_coordinator_tests` |
| RG-01～RG-05 | Task 9 | `shortage_task_lifecycle_tests` 加既有回归 |
| RG-06 | Task 12 | Linux 全量 build/ctest/diff check |

## 3. 每任务实施门禁

每个 Task 只有同时满足以下条件才算完成：

1. 该 Task 的失败测试先真实失败，失败原因与缺失功能一致。
2. 最小实现后该 Task 的目标测试通过。
3. 受影响的既有测试同时通过。
4. 新增/修改代码的中文注释覆盖类型、字段、接口、状态分支、信号唯一性和跨模块接线。
5. `git diff --check` 无输出。
6. spec、plan 和代码不存在规则不一致。
7. 未创建本计划之外的新文件。
8. 按用户最新要求，每个任务完成后单独中文提交；提交前只暂存该任务范围文件。
9. 若本 Task 涉及从旧分支移植，必须保留“旧函数 → 当前函数 → 必要调整”的审计记录，且不得覆盖当前分支已经修复的同名代码。

## 4. 计划评审结论

本计划把正式账本、补料计划、现有 FIFO 和任务执行事实分成四个边界，独立测试复用核心但隔离状态。全部新增文件已经列明；任何额外文件需再次获得用户同意。开始实施前，用户需要确认本 plan，尤其确认以下四点仍保持不变：

1. 活动工位采用非抢占补至最高位，其他低位工位只在待补料顺序表等待。
2. 现有 FIFO 只追加、不重排。
3. `actualQty` 清零采样尾数误差按当前规则记录并留待客户现场签字确认。
4. Task 1 先删除旧客户系统通信测试全部 UI/API/配置/signals，并在原 `customSysScheduler.h/.cpp` 中建立唯一 `.228` 真实协议层；不保留 `.229` 或兼容空壳。

旧分支复用结论同时固定为：HTTP/PLC 协议实现按函数级移植并通过兼容测试，旧库存、阈值、派单和状态模型不移植；禁止为省事整体合并旧缺料提交。
