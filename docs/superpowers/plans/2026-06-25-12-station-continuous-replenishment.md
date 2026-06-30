# 12工位连续补料调度系统 实现计划

> **面向 AI 代理或工程师的执行说明：** 实施本计划时建议使用 `superpowers:subagent-driven-development` 或 `superpowers:executing-plans`，按任务逐项实现、逐项验证。步骤使用复选框（`- [ ]`）跟踪进度。

**目标：** 在现有单工位 `LineOrchestrator` 已验证流程基础上，新增 12 工位 FIFO 连续补料调度层，串起 UI 模拟缺料、AGV 取料/倒料/码垛点移动、视觉搜索与闭环、夹取前扫码、倒料、空箱码垛、空闲回 LM1 和系统级报警恢复。

**架构：** 新增 `LineManager + TaskQueue + TaskExecutor`。`LineManager` 管系统状态和队列，`TaskExecutor` 管单任务大流程，`HuayanScheduler` 管机械臂细节，`PalletScheduler` 继续只做码垛点位规划和缓存。现有 `LineOrchestrator`、所有设备测试面板、`WorkflowWidget` 保留，不作为新调度主线扩展点。

**技术栈：** Qt 6 Widgets/Core/Network/SerialBus、C++17、CMake、华研 SDK、N-ScanHub SDK、仙工 AGV Modbus TCP。

**规格来源：** `robot/robot_visual/docs/superpowers/specs/2026-06-25-12-station-continuous-replenishment-design.md`

**当前状态：** 开发联调中，十二工位端到端现场实测尚未通过；本计划中的任务完成标记不能替代现场验收。

**唯一计划约束：** 本文件是十二工位连续补料调度目标的唯一实现计划。后续 Bug 修复、新功能、诊断和现场调整必须继续追加/修订本文件，不允许创建新的 spec 或 plan 分流该目标的上下文。

---

## 0. 全局约束

- v1 缺料来源只使用 UI 模拟按钮；客户系统不自动派单。
- 队列 FIFO，先来后到；同一工位允许多个任务。
- 单 AGV + 单机械臂，设备动作严格串行。
- 有下一任务时不回 LM1；无任务时 AGV 回 LM1 待机；回 LM1 途中来新任务则主动取消回站并执行新任务。
- 系统级 ERROR 清空所有 Pending 任务，Reset 后回 Idle，不自动恢复。
- 任务级失败先尝试收安全姿态；收姿态成功后继续队列，失败则升级系统级 ERROR。
- AGV 任务导航失败、超时、异常取消均为系统级 ERROR。
- `PalletArea::LargeBox` 表示共享码垛区（工位 1-11），`PalletArea::SmallBox` 表示工位12独立码垛区。
- 临时码垛 LM：共享区 `16`，工位12独立区 `17`。
- 视觉搜索默认每次下移 `20mm`，最多 `80mm`；视觉闭环沿用 XY `2mm`、Rz `1deg`、最多 `6` 次。
- 扫码默认 `port=30000`、`timeoutMs=2000`、`maxAttempts=3`、`retryIntervalMs=500`；空码按失败处理；非空默认通过。
- 现有测试面板不禁用、不改动运行权限；后续再做互锁。
- 不实现断电恢复、客户系统自动缺料算法、客户系统完成回传、全设备 Mock 框架。
- 代码必须增加必要中文注释，解释调度状态、错误分级、安全边界、现场临时配置和后续接入点。
- 此后新增的每个任务都必须在任务条目中单列“源码注释要求”和“注释验收清单”。新增类、结构体、枚举、状态、函数、变量、常量、宏定义和信号槽必须有详尽中文注释；注释未补齐或与实现不一致时，该任务不得标记完成、不得提交。
- Bug 修复或新功能必须预先列明文件范围。实施中若需要新增文件或修改任务原定范围之外的文件，必须暂停实施，向用户提交原因、文件清单、职责、影响和可选方案，并在获得明确确认后才能继续。
- 后续所有修改必须以达成十二工位连续补料现场验收为目标，并同步更新本计划及对应唯一设计文档；不得另建新的 spec/plan。
- 编译、自动测试或局部动作通过只代表阶段验证，不代表十二工位任务已经实测成功。
- 需要 Git 提交时，必须先向用户展示文件范围、改动摘要、验证结果和建议提交信息，并获得明确确认后才能执行 `git commit`。
- 只允许本地 commit，禁止执行 `git push`；远端 push 始终由用户人工完成。

---

## 1. 文件结构

| 文件 | 操作 | 职责 |
|------|------|------|
| `src/lineconfig.h` | 创建 | 12 工位固定配置、码垛区配置、任务/状态枚举、日志辅助函数 |
| `src/taskqueue.h` | 创建 | FIFO 队列容器，支持入队、取队首、清空 Pending、快照 |
| `src/taskexecutor.h/.cpp` | 创建 | 单任务状态机，协调 AGV、机械臂、扫码、码垛 |
| `src/linemanager.h/.cpp` | 创建 | 系统状态机，Start/Stop/Reset、队列调度、空闲回 LM1 |
| `src/huayanScheduler.h/.cpp` | 修改 | 工位函数注入、视觉搜索、扫码暂停、Rz 180 旋转、码垛放置动作 |
| `src/devicemanager.h/.cpp` | 修改 | 持有 `LineManager`，提供调度扫码 worker 请求，连接日志/报警 |
| `src/mainwindow.h/.cpp` | 修改 | 新增调度监控 UI：12 工位按钮、状态、队列、报警；现有面板不动 |
| `CMakeLists.txt` | 修改 | 加入新增调度源码 |

验证命令（Linux 当前仓库）：

```bash
cmake --build robot/robot_visual/build --target wh-robot-visual -j$(nproc)
```

如果本地没有 `robot/robot_visual/build`，先从 `robot/robot_visual` 目录按现有 Qt/CMake Kit 配置构建目录，再运行同等 `cmake --build` 命令。每个任务结束必须至少编译一次。

---

## 2. 阶段总览与验收出口

| 阶段 | 目标 | 阶段通过标准 |
|------|------|--------------|
| 阶段一 | 建立任务模型、工位配置、FIFO 队列 | 工程编译通过；配置表能覆盖 12 工位；队列支持同工位多任务 FIFO |
| 阶段二 | 扩展 `HuayanScheduler` | 工程编译通过；新增接口不破坏现有机械臂测试面板；能表达扫码暂停、旋转、码垛动作 |
| 阶段三 | 实现 `TaskExecutor` | 工程编译通过；单任务状态机覆盖取料、扫码、倒料、码垛、失败收尾 |
| 阶段四 | 实现 `LineManager` | 工程编译通过；Start/Stop/Reset、回 LM1、新任务打断回站、ERROR 清队列规则齐全 |
| 阶段五 | 接入 `DeviceManager` 和扫码 worker | 工程编译通过；调度层可访问真实设备对象和扫码线程；现有测试面板仍可用 |
| 阶段六 | 新增调度 UI | 工程编译通过；可通过 UI 模拟 12 工位缺料、查看当前任务和队列 |
| 阶段七 | 增强视觉搜索与日志报警 | 工程编译通过；视觉无目标搜索、视觉通信失败、日志前缀、弹窗报警策略齐全 |
| 阶段八 | 总体验收 | 按“总流程验收清单”通过离线/真机验证 |

---

## 3. 阶段一：固定配置与 FIFO 队列

### 任务 1：新增固定配置与任务模型

**文件：**
- 创建：`robot/robot_visual/src/lineconfig.h`
- 修改：`robot/robot_visual/CMakeLists.txt`

**要实现的接口：**

```cpp
enum class LineSystemState { Idle, Running, ReturningHome, Error };
enum class TaskState { Pending, Running, Succeeded, Failed, Canceled };
enum class TaskSource { UiMock, CustomerSystem };
enum class TaskStep {
    Waiting,
    AgvToPickup,
    ArmPickup,
    PreGripScan,
    GripAndLift,
    StowAfterPickup,
    AgvToUnload,
    ArmUnload,
    StowAfterUnload,
    AgvToPallet,
    PreparePalletPoint,
    ArmPalletPlace,
    CommitPallet,
    StowAfterPallet,
    ReturningHome,
    Done
};
```

```cpp
struct Task {
    quint64 taskId = 0;
    int stationId = 0;
    TaskSource source = TaskSource::UiMock;
    TaskState state = TaskState::Pending;
    TaskStep step = TaskStep::Waiting;
    int stepIndex = 0;
    QDateTime createdAt;
    QString statusText;
    QString lastError;
};

struct StationTaskConfig {
    int stationId = 0;
    int pickupLm = 0;
    int unloadLm = 0;
    PalletArea palletArea = PalletArea::LargeBox;
    QString captureFunc;
    QString unloadPointFunc;
    QString unloadFunc;
};

struct PalletAreaTaskConfig {
    PalletArea area = PalletArea::LargeBox;
    int palletLm = 0;
    QString palletBaseFunc;
    QString releaseFunc;
};
```

必须提供：

```cpp
const StationTaskConfig *stationConfig(int stationId);
const PalletAreaTaskConfig *palletAreaConfig(PalletArea area);
QString palletAreaDisplayName(PalletArea area);
QString taskStateText(TaskState state);
QString formatTaskPrefix(const Task &task, const QString &stage);
```

配置表必须写入：

- 工位 1：取料 3，倒料 3，共享码垛区。
- 工位 2：取料 4，倒料 4，共享码垛区。
- 工位 3：取料 5，倒料 9，共享码垛区。
- 工位 4：取料 5，倒料 9，共享码垛区。
- 工位 5：取料 5，倒料 10，共享码垛区。
- 工位 6：取料 6，倒料 10，共享码垛区。
- 工位 7：取料 6，倒料 11，共享码垛区。
- 工位 8：取料 6，倒料 11，共享码垛区。
- 工位 9：取料 7，倒料 12，共享码垛区。
- 工位 10：取料 7，倒料 12，共享码垛区。
- 工位 11：取料 7，倒料 7，共享码垛区。
- 工位 12：取料 15，倒料 15，工位12独立码垛区。
- 共享码垛区：`PalletArea::LargeBox`，临时 `palletLm = 16`，`palletBaseFunc = Func_pallet_shared_base`，`releaseFunc = Func_songzhua`。
- 工位12独立码垛区：`PalletArea::SmallBox`，临时 `palletLm = 17`，`palletBaseFunc = Func_pallet_s12_base`，`releaseFunc = Func_songzhua`。

**代码注释要求：**

- 在 `palletLm = 16/17` 附近写中文注释：这是现场待替换的模拟值。
- 在 `PalletArea::LargeBox/SmallBox` 映射处写中文注释：当前复用现有码垛枚举，业务含义分别是共享码垛区和工位12独立码垛区。

**怎么验证本任务通过：**

1. 执行构建命令，编译必须通过。
2. 在代码中能搜索到 12 条 `StationTaskConfig`，且数字 LM 与 spec 完全一致。
3. `stationConfig(1)` 到 `stationConfig(12)` 都能返回非空；`stationConfig(13)` 返回空。
4. `palletAreaConfig(PalletArea::LargeBox)` 和 `palletAreaConfig(PalletArea::SmallBox)` 都能返回非空。
5. `formatTaskPrefix()` 输出格式应类似 `[T#18 S05][AGV]`，用于后续日志定位。

### 任务 2：新增 FIFO `TaskQueue`

**文件：**
- 创建：`robot/robot_visual/src/taskqueue.h`
- 修改：`robot/robot_visual/CMakeLists.txt`

**要实现的接口：**

```cpp
class TaskQueue
{
public:
    Task enqueue(int stationId, TaskSource source);
    bool hasPending() const;
    int pendingCount() const;
    Task takeNext();
    QList<Task> pendingSnapshot() const;
    void clearPendingAsCanceled(const QString &reason);
    quint64 nextTaskId() const;
};
```

**实现规则：**

- `enqueue()` 每调用一次都创建一个新任务，不做同工位去重。
- `taskId` 从 1 开始自增。
- `takeNext()` 取最早入队任务，并把状态改为 `Running`。
- `clearPendingAsCanceled()` v1 可以直接清空 Pending；历史追溯依靠日志，队列表不长期保留历史。

**代码注释要求：**

- 在类注释中说明：这里是任务实例队列，不是工位状态表；同一工位允许多次入队。

**怎么验证本任务通过：**

1. 执行构建命令，编译必须通过。
2. 人工检查 `enqueue(3)` 连续调用两次会产生两个不同 `taskId` 的任务。
3. 人工检查 `takeNext()` 使用 `takeFirst()` 或等价逻辑，保证 FIFO。
4. 人工检查 `pendingSnapshot()` 不会改变队列内容。
5. 人工检查 `clearPendingAsCanceled()` 后 `pendingCount() == 0`。

---

## 4. 阶段二：扩展 HuayanScheduler

### 任务 3：新增工位函数、扫码暂停、旋转和码垛动作接口

**文件：**
- 修改：`robot/robot_visual/src/huayanScheduler.h`
- 修改：`robot/robot_visual/src/huayanScheduler.cpp`

**要实现的接口：**

```cpp
struct StationArmFunctions {
    QString captureFunc;
    QString unloadPointFunc;
    QString unloadFunc;
};

struct PalletArmFunctions {
    QString palletBaseFunc;
    QString releaseFunc;
};

void setStationFunctions(const StationArmFunctions &funcs);
void setPalletFunctions(const PalletArmFunctions &funcs);
void setPreGripScanEnabled(bool enabled);
void continueAfterPreGripScan();
void rotateToolRz180();
void startPalletPlace(const PalletPose &offset);
```

新增信号：

```cpp
void preGripScanRequested();
void toolRotationCompleted();
void toolRotationError(QString reason);
void palletPlaceCompleted();
void palletPlaceError(QString reason);
```

**实现规则：**

- `setStationFunctions()` 只替换非空函数名；保留现有默认函数用于单独调试。
- `setPreGripScanEnabled(true)` 后，`StageOne` 在 Z 下降到扫码/夹取前位置时发 `preGripScanRequested()` 并暂停。
- `continueAfterPreGripScan()` 只能在 `StageOne` 且正在等待扫码时生效，随后进入夹紧/抬升。
- `rotateToolRz180()` 使用工具坐标系 Rz 相对旋转 180 度；旋转失败发 `toolRotationError`。
- `startPalletPlace(offset)` 流程：调用 `palletBaseFunc` → 按 offset 分轴相对 MoveRelL → 调用 `releaseFunc` → 成功发 `palletPlaceCompleted`。
- offset 移动顺序固定为 X、Y、Z、Rz；小于 `0.5mm/0.5deg` 的偏移可忽略。
- `HuayanScheduler` 仍不管理任务队列。

**代码注释要求：**

- 在扫码暂停处注释：这是主流程插入扫码比对的安全暂停点，不能被普通阶段完成自动跳过。
- 在 `rotateToolRz180()` 注释：用于扫码枪扫不到有码面时，让箱体侧面二维码对准扫码枪。
- 在 `startPalletPlace()` 注释：码垛 offset 来自 `PalletScheduler::nextRelativeOffset()`，此处只执行机械臂动作，不推进码垛缓存。

**怎么验证本任务通过：**

1. 执行构建命令，编译必须通过。
2. 现有华研 SDK 测试面板仍能调用原 `startStageOne()`、`startStow()`、`startUnload()` 等接口。
3. 人工检查 `preGripScanRequested()` 只在 `m_preGripScanEnabled == true` 时触发。
4. 人工检查 `continueAfterPreGripScan()` 不会在非取料阶段误触发。
5. 人工检查 `startPalletPlace()` 成功路径不会调用 `PalletScheduler::commitPlaced()`，commit 必须留给 `TaskExecutor`。
6. 人工检查新增 `Stage`/`StageStep` 都在 `proceedStage()`、`advanceStep()`、`executeCurrentStep()` 中有对应处理。

---

## 5. 阶段三：实现 TaskExecutor 单任务状态机

### 任务 4：新增 `TaskExecutor`

**文件：**
- 创建：`robot/robot_visual/src/taskexecutor.h`
- 创建：`robot/robot_visual/src/taskexecutor.cpp`
- 修改：`robot/robot_visual/CMakeLists.txt`

**要实现的核心接口：**

```cpp
class TaskExecutor : public QObject
{
    Q_OBJECT
public:
    explicit TaskExecutor(AgvController *agv,
                          HuayanScheduler *arm,
                          NScanScheduler *scanner,
                          PalletScheduler *pallet,
                          QObject *parent = nullptr);

    bool isBusy() const;
    Task currentTask() const;

public slots:
    void start(const Task &task);
    void stopForSystemError(const QString &reason);
    void onScanFinished(const NScanScheduler::ScanResult &result);

signals:
    void taskUpdated(const Task &task);
    void taskSucceeded(const Task &task);
    void taskFailed(const Task &task, const QString &reason);
    void systemError(const Task &task, const QString &reason);
    void logMessage(const QString &message);
    void agvDispatchRequested(int lm);
    void scanRequested(const NScanScheduler::ScanOptions &options);
};
```

**内部状态必须覆盖：**

```cpp
enum class ExecState {
    Idle,
    AgvToPickup,
    ArmPickup,
    PreGripScan,
    RotateForScan,
    StowAfterPickup,
    AgvToUnload,
    ArmUnload,
    StowAfterUnload,
    AgvToPallet,
    PreparePalletPoint,
    ArmPalletPlace,
    CommitPallet,
    StowAfterPallet,
    CleanupStow
};
```

**主流程要求：**

1. `start(task)` 查 `StationTaskConfig` 和 `PalletAreaTaskConfig`，缺失则系统级 ERROR。
2. 注入当前工位机械臂函数和码垛函数。
3. AGV 到 `pickupLm`，严格等待到站。
4. 调 `HuayanScheduler::startStageOne()`。
5. 收到 `preGripScanRequested()` 后发扫码请求。
6. 扫码非空成功则调用 `continueAfterPreGripScan()`。
7. 首轮扫码失败/空码则调用 `rotateToolRz180()`，旋转完成后第二轮扫码。
8. 两轮仍失败则任务 Failed，进入 `CleanupStow`。
9. 取料完成后收姿态，再去 `unloadLm`。
10. 调 `startUnload()` 倒料，倒料后收姿态。
11. 去码垛区 `palletLm`。
12. 调 `PalletScheduler::validateConfig()` 和 `nextRelativeOffset()`。
13. 调 `HuayanScheduler::startPalletPlace(offset)`。
14. 放置成功后调 `PalletScheduler::commitPlaced()`。
15. 收姿态成功后任务 `Succeeded`。

**AGV 到站判断要求：**

- 复用 `LineOrchestrator` 已验证逻辑。
- 必须先看到 `navStatus == 1` 或 `navStatus == 2`。
- 到站必须同时满足 `navStatus == 4`、`navStation == expectedLm`、`curStation == expectedLm`。
- `navStatus == 5/6/7` 在任务导航中都是系统级 ERROR。
- 单步超时 `120s`。

**扫码配置要求：**

- `scanRequested` 发出的 options 使用默认值：`port=30000`、`timeoutMs=2000`、`maxAttempts=3`、`retryIntervalMs=500`。
- `scannerIP` 由 `DeviceManager` 在转发扫码请求时填入。
- `result.isSuccess() == true` 但 `result.code.trimmed().isEmpty()` 也算失败。

**错误处理要求：**

- 任务级失败：视觉失败、扫码失败、普通机械臂失败、机械臂码垛动作失败。
- 任务级失败后进入 `CleanupStow`；收姿态成功发 `taskFailed`，收姿态失败发 `systemError`。
- 系统级 ERROR：AGV 任务导航失败/超时/异常取消、码垛满/配置无效/无点位、`commitPlaced()` 失败。

**代码注释要求：**

- 在 AGV 到站判断处注释为什么必须同时看 `navStation` 和 `curStation`。
- 在 `CleanupStow` 注释：这是任务级失败后的安全恢复路径；如果收姿态失败必须升级系统级 ERROR。
- 在扫码空码判断处注释：后续客户比对规则接入前，非空码默认通过，空码不可追溯所以失败。
- 在 `commitPlaced()` 前注释：只有机械臂到位并松爪成功后才能推进码垛缓存。

**怎么验证本任务通过：**

1. 执行构建命令，编译必须通过。
2. 人工检查 `TaskExecutor` 不直接调用华研 SDK 原语，只调用 `HuayanScheduler` 高层方法。
3. 人工检查所有 AGV 任务导航失败路径都调用 `systemError`。
4. 人工检查所有任务级失败都先进入 `CleanupStow`。
5. 人工检查 `commitPlaced()` 只在 `palletPlaceCompleted()` 后调用。
6. 人工检查 `scanRequested` 第一轮失败后进入 `RotateForScan`，第二轮失败才任务失败。
7. 断开 AGV 或不给监控数据启动任务，应在超时/错误路径进入系统级 ERROR，程序不崩溃。

---

## 6. 阶段四：实现 LineManager 系统状态机

### 任务 5：新增 `LineManager`

**文件：**
- 创建：`robot/robot_visual/src/linemanager.h`
- 创建：`robot/robot_visual/src/linemanager.cpp`
- 修改：`robot/robot_visual/CMakeLists.txt`

**要实现的接口：**

```cpp
class LineManager : public QObject
{
    Q_OBJECT
public:
    explicit LineManager(AgvController *agv,
                         HuayanScheduler *arm,
                         NScanScheduler *scanner,
                         PalletScheduler *pallet,
                         QObject *parent = nullptr);

    LineSystemState state() const;
    QList<Task> queueSnapshot() const;
    Task currentTask() const;

public slots:
    void start();
    void stop();
    void resetError();
    void reportShortage(int stationId);
    void onScanFinished(const NScanScheduler::ScanResult &result);

signals:
    void systemStateChanged(LineSystemState state, const QString &text);
    void queueChanged(QList<Task> tasks);
    void currentTaskChanged(Task task);
    void alarmRaised(QString reason);
    void logMessage(QString message);
    void agvDispatchRequested(int lm);
    void scanRequested(NScanScheduler::ScanOptions options);
};
```

**实现规则：**

- `Idle` 状态允许 `reportShortage()` 入队，但不自动执行。
- `Running` 状态允许入队；如果当前无任务，立即执行。
- `ReturningHome` 状态收到新任务：设置“主动取消回站”标记，取消 AGV 回站，启动新任务。
- `Error` 状态拒绝入队，只记录日志或发提示。
- `start()`：
  - 如果队列有 Pending，直接执行队首任务。
  - 如果队列为空，进入 Running/等待缺料，并让 AGV 回 LM1。
- `stop()`：
  - 立即取消 AGV、停止机械臂。
  - 当前任务 Canceled。
  - 清空 Pending。
  - 进入 Error。
- `resetError()`：
  - 仅从 Error 生效。
  - 回到 Idle。
  - 不自动执行任务。
- 任务成功或任务级失败收尾成功后：
  - 有 Pending：直接下一任务，不回 LM1。
  - 无 Pending：进入 ReturningHome，AGV 回 LM1。
- 回 LM1 失败、非主动取消、超时：系统级 ERROR。

**代码注释要求：**

- 在 `ReturningHome` 取消逻辑处注释：主动取消回 LM1 是正常切换，不能当 AGV fatal。
- 在 `stop()` 注释：Stop 是人工急停报警，不是普通暂停，所以清空队列并进入 Error。
- 在 `start()` 注释：LM1 是空闲待机点，不是每次任务链路的强制起点。

**怎么验证本任务通过：**

1. 执行构建命令，编译必须通过。
2. 人工检查 `Idle -> reportShortage -> queueChanged`，但不会执行任务。
3. 人工检查 `start()` 有队列时不会先回 LM1。
4. 人工检查 `start()` 无队列时会进入回 LM1 逻辑。
5. 人工检查任务完成且队列非空时直接执行下一任务。
6. 人工检查 `stop()` 会清空 Pending 并进入 Error。
7. 人工检查 `resetError()` 只回 Idle，不会自动重新开始。

---

## 7. 阶段五：接入 DeviceManager 与扫码线程

### 任务 6：DeviceManager 持有 LineManager 并转发扫码

**文件：**
- 修改：`robot/robot_visual/src/devicemanager.h`
- 修改：`robot/robot_visual/src/devicemanager.cpp`

**实现要求：**

- `DeviceManager` 新增 `LineManager *m_lineManager` 和 getter：

```cpp
LineManager *lineManager() const { return m_lineManager; }
```

- `DeviceManager` 持有同一个 `PalletScheduler` 实例，供主流程和码垛配置 UI 共用。
- 注册元类型：

```cpp
Q_DECLARE_METATYPE(Task)
Q_DECLARE_METATYPE(QList<Task>)
Q_DECLARE_METATYPE(LineSystemState)
```

构造函数中：

```cpp
qRegisterMetaType<Task>("Task");
qRegisterMetaType<QList<Task>>("QList<Task>");
qRegisterMetaType<LineSystemState>("LineSystemState");
```

- 创建 `LineManager`，注入 `m_agvCtrl`、`m_huayanScheduler`、`m_nscanScheduler.get()`、`m_palletScheduler`。
- 连接 `LineManager::logMessage` 到现有日志。
- 连接 `LineManager::agvDispatchRequested(int lm)` 到 `AgvController::sendToStation(int)`，这里直接发送数字 LM，不走旧工位映射 UI。
- 连接 `LineManager::scanRequested` 到扫码 worker。转发时填入 `m_cfg.scannerIP`。

**扫码 worker 注意事项：**

- 现有 NScan 测试面板也使用 worker。不能让调度扫码结果误送到测试面板，也不能让测试扫码结果误送到 `LineManager`。
- 如果现有 worker 的 `finished` 信号已直接连接测试 UI，需要给调度扫码单独建 worker 或加 operation 标记。
- 调度扫码必须使用后台线程，不允许在 UI 线程直接调用 `NScanScheduler::scan()`。

**代码注释要求：**

- 在 `agvDispatchRequested -> sendToStation` 接线处注释：新调度层配置表已保存数字 LM，不再走旧 AGV 调试面板的工位映射。
- 在扫码 worker 接线处注释：调度扫码和 UI 测试扫码必须隔离，避免结果串线。

**怎么验证本任务通过：**

1. 执行构建命令，编译必须通过。
2. 启动程序，现有 AGV、机械臂、视觉、扫码、客户系统、码垛测试面板仍能创建。
3. 人工检查 `lineManager()` getter 非空。
4. 人工检查调度层 AGV 派发使用数字 LM。
5. 人工检查调度扫码结果只进入 `LineManager::onScanFinished()`。
6. 手动触发现有 NScan 测试面板，确认测试结果仍进入测试 UI，不进入调度任务。

---

## 8. 阶段六：新增调度监控 UI

### 任务 7：MainWindow 增加调度监控区

**文件：**
- 修改：`robot/robot_visual/src/mainwindow.h`
- 修改：`robot/robot_visual/src/mainwindow.cpp`

**新增 UI 内容：**

- 系统状态：未启动、等待缺料、正在送料、回待机点、系统报警。
- 当前任务：工位、业务状态、错误原因。
- 待送料数量。
- 12 个工位“模拟缺料”按钮。
- Start、Stop、Reset Error。
- FIFO 队列表：任务号、工位、状态、入队时间。只显示 Pending + Running。
- 最近完成、最近失败、报警信息。

**MainWindow 新增成员建议：**

```cpp
QLabel *m_lineStateLabel = nullptr;
QLabel *m_lineCurrentLabel = nullptr;
QLabel *m_lineQueueCountLabel = nullptr;
QLabel *m_lineAlarmLabel = nullptr;
QTableWidget *m_lineQueueTable = nullptr;
QPushButton *m_lineResetBtn = nullptr;
QList<QPushButton *> m_stationButtons;
```

**实现规则：**

- 新增 `initLineDispatchPanel(QVBoxLayout *leftPanel)`，插入左侧面板靠前位置。
- 每个工位按钮设置 `stationId` property，点击后调用 `m_devMgr->lineManager()->reportShortage(stationId)`。
- `onStart()` 改为调用 `m_devMgr->lineManager()->start()`。
- `onStop()` 改为调用 `m_devMgr->lineManager()->stop()`。
- Reset Error 按钮调用 `m_devMgr->lineManager()->resetError()`。
- 队列表只展示 Pending + Running，不展示历史任务。
- `WorkflowWidget` 不改，不依赖它展示新调度状态。
- 现有测试面板不禁用。

**客户文案要求：**

- 工位X已加入送料队列。
- 工位X开始送料。
- 工位X正在取料。
- 工位X正在扫码。
- 工位X正在送料。
- 工位X正在倒料。
- 工位X正在放空箱。
- 工位X送料完成。
- 工位X送料失败：原因。
- 系统报警：原因。

**代码注释要求：**

- 在调度监控区初始化函数注释：这是客户/现场关注的任务看板，不替代现有设备测试面板。
- 在队列表刷新处注释：长期历史走日志，表格只显示未完成任务，避免长时间运行 UI 无限增长。

**怎么验证本任务通过：**

1. 执行构建命令，编译必须通过。
2. 启动程序，新调度监控区可见。
3. 现有所有测试面板仍然可见。
4. Idle 下点击工位 3 两次，队列表出现两个工位 3 任务，任务号不同，顺序不变。
5. Error 状态点击工位按钮，不应新增队列任务。
6. Stop 后队列表清空，状态显示系统报警。
7. Reset Error 后状态回未启动或 Idle 文案。

---

## 9. 阶段七：视觉搜索、日志与报警完善

### 任务 8：HuayanScheduler 增加视觉搜索下移

**文件：**
- 修改：`robot/robot_visual/src/huayanScheduler.h`
- 修改：`robot/robot_visual/src/huayanScheduler.cpp`
- 修改：`robot/robot_visual/src/devicemanager.cpp`

**实现规则：**

- 在 `HuayanScheduler` 中新增视觉搜索计数：

```cpp
int m_searchDescendCount = 0;
double m_searchDescendedMm = 0.0;
```

- 常量：

```cpp
static constexpr double kSearchDescendStep = 20.0;
static constexpr double kMaxSearchDescend = 80.0;
```

- `startStageOne()` 重置搜索计数。
- `VisionHttpClient::noObjectDetected` 连接到 `HuayanScheduler::onVisionNoObject()`。
- `VisionHttpClient::errorOccurred` 连接到 `HuayanScheduler::onVisionErrorForPickup()`。
- `noObjectDetected` 且当前处于 `StageOne/WaitForVision` 时：
  - 若累计下移未超过 80mm，机械臂 Z 向下相对移动 20mm。
  - 等待视觉稳定后重新请求视觉。
  - 超过 80mm 后发 `stageError`，当前任务失败。
- 视觉 HTTP/解析/服务错误不做下移，直接发 `stageError`。

**代码注释要求：**

- 在搜索下移处注释：这是为算法初始识别不稳定增加的保护搜索，不是抓取 Z 下探。
- 在 80mm 限制处注释：保守默认值，防止算法一直识别不到时机械臂持续下移触碰料箱。
- 在视觉错误处理处注释：通信/解析错误不代表目标不在视野内，继续下移没有意义。

**怎么验证本任务通过：**

1. 执行构建命令，编译必须通过。
2. 人工检查 `noObjectDetected` 不再直接 `stop()`，而是进入搜索下移。
3. 人工检查视觉服务错误仍然直接任务失败，不下移。
4. 人工检查累计下移超过 80mm 后必然发 `stageError`。
5. 人工检查搜索下移和已有视觉闭环 B 的 `kMaxGrabIterations = 6` 互不混淆。

### 任务 9：日志、报警和可定位性完善

**文件：**
- 修改：`robot/robot_visual/src/taskexecutor.cpp`
- 修改：`robot/robot_visual/src/linemanager.cpp`
- 修改：`robot/robot_visual/src/mainwindow.cpp`

**实现规则：**

- 所有任务日志使用：

```cpp
formatTaskPrefix(m_task, QStringLiteral("AGV"))
```

阶段名固定使用：`TASK`、`AGV`、`ARM`、`VISION`、`SCAN`、`PALLET`、`CLEANUP`、`ERROR`。

- 系统级报警必须：
  - 写日志。
  - 更新 UI 红色报警文本。
  - 弹出 `QMessageBox::warning`。
  - 清空 Pending。

- 任务级失败必须：
  - 写明失败阶段和原因。
  - 写明开始收安全姿态。
  - 写明收姿态成功或失败。

**代码注释要求：**

- 在系统级 ERROR 入口处注释：该路径代表现场需要人工介入，所以清空队列并禁止继续派单。
- 在任务级失败入口处注释：该路径允许收姿态成功后继续下一任务。

**怎么验证本任务通过：**

1. 执行构建命令，编译必须通过。
2. 人工检查日志包含 `[T#任务号 S工位][阶段]`。
3. 手动触发 Stop，确认 UI 报警、日志、队列清空。
4. 模拟扫码失败路径，确认任务日志里有 SCAN 和 CLEANUP 阶段。
5. 模拟码垛配置满/无点位，确认弹窗、日志、系统 ERROR。

---

## 10. 总流程验收清单

### 10.1 离线/半离线验收

没有真实设备时，至少验证：

1. 程序能启动，不崩溃。
2. 新调度监控 UI 可见，现有测试面板可见。
3. Idle 下点击多个工位，队列表按点击顺序显示。
4. 同一工位连续点击多次，生成多个不同任务号。
5. 点击 Start 后，如果 AGV 未连接，系统进入可读错误或等待超时路径，不崩溃。
6. 点击 Stop 后，当前任务取消，Pending 清空，系统进入 Error。
7. Error 下点击工位，不新增任务。
8. Reset Error 后回 Idle。

### 10.2 真机基础验收

有 AGV、机械臂、视觉、扫码设备时，按顺序验证：

1. 空队列 Start：AGV 回 LM1；若已在 LM1，不重复误判。
2. 单工位任务：AGV 去取料 LM，机械臂进入取料流程。
3. 视觉无目标：机械臂按 20mm 搜索下移，最多 80mm。
4. 视觉识别成功：进入闭环对准，误差达标后 Z 下降。
5. 夹取前扫码：扫码成功非空后继续夹紧。
6. 扫码失败：机械臂 Rz 旋转 180 度并重试。
7. 倒料：AGV 到倒料 LM，机械臂执行倒料函数。
8. 码垛：AGV 到码垛 LM，`PalletScheduler` 给 offset，机械臂到基准点并相对移动，松爪后 `commitPlaced()`。
9. 任务完成：UI 显示“工位X送料完成”。
10. 队列为空：AGV 回 LM1。

### 10.3 连续任务验收

1. 点击工位 3、工位 5、工位 3。
2. 确认执行顺序为 3 → 5 → 3。
3. 第一个任务完成后，如果队列非空，AGV 不回 LM1，直接去下一个任务取料 LM。
4. 所有任务完成后，AGV 回 LM1。
5. AGV 回 LM1 途中点击新工位，确认回站被主动取消并执行新任务，不报警。

### 10.4 异常验收

1. AGV 任务导航失败或超时：系统 ERROR，Pending 清空。
2. 用户 Stop：系统 ERROR，Pending 清空，当前任务 Canceled。
3. 视觉服务错误：当前任务 Failed，收姿态成功后继续下一任务。
4. 视觉搜索 80mm 仍无目标：当前任务 Failed，收姿态成功后继续下一任务。
5. 扫码两轮失败或空码：当前任务 Failed，收姿态成功后继续下一任务。
6. 收姿态失败：系统 ERROR，Pending 清空。
7. 码垛满或无点位：弹窗警告，系统 ERROR，Pending 清空。
8. `commitPlaced()` 失败：系统 ERROR，Pending 清空。

### 10.5 总流程通过标准

总流程只有同时满足以下条件，才算通过：

- 工程完整编译通过。
- UI 能模拟 12 工位缺料入队。
- FIFO 顺序正确，同工位多任务正确。
- 单任务能走到“送料完成”的业务状态。
- 连续任务之间有任务时不回 LM1。
- 队列为空时能回 LM1。
- 回 LM1 途中来新任务能取消回站并执行。
- AGV fatal、Stop、收姿态失败、码垛 fatal 都进入 ERROR 并清空队列。
- 视觉/扫码/普通机械臂失败能任务失败并收姿态，收姿态成功后继续队列。
- 日志能通过 taskId、stationId、阶段定位问题。
- 现有测试面板仍保留可用。

---

## 11. 未来要做的事情

本次 v1 不做，但必须在后续计划中继续：

1. **客户系统自动缺料算法**
   - 根据客户产量、12 工位消耗比例、安全阈值生成任务。
   - 需要明确客户接口数据含义和刷新周期。

2. **扫码比对规则**
   - 当前非空码默认通过。
   - 后续客户给标准后实现 `validateBarcode(task, barcode)`。

3. **任务队列持久化与恢复**
   - 增加 `task_queue_snapshot.json` 或 SQLite。
   - 保存 Pending/Running/current task。
   - 程序启动时提示人工确认是否恢复。

4. **配置外置**
   - 工位 LM、机械臂函数名、码垛 LM 从集中代码表迁移到 JSON、QSettings 或配置 UI。
   - 当前 `palletLm = 16/17` 必须替换成现场真实值。

5. **调度运行中的 UI 互锁**
   - Running 时禁用危险测试按钮。
   - Error 时只开放恢复相关操作。

6. **机械臂错误分级**
   - 区分普通动作失败、SDK 连接断开、急停、安全门、伺服报警。
   - 不可恢复错误直接系统级 ERROR。

7. **Mock 调度模式**
   - Mock AGV 到站、机械臂阶段完成、扫码成功/失败、码垛点位。
   - 用于无设备环境验证状态机。

8. **客户展示界面优化**
   - 弱化或移除 `WorkflowWidget`。
   - 做更通俗的“正在送料/已送达/报警”看板。

9. **现场参数调优**
   - 视觉搜索下移步长和最大下移量。
   - 码垛 offset 方向、`invertX/invertY`。
   - 码垛区尺寸、层数、安全 Z。

---

## 12. 与已有计划对齐检查

参考过的已有计划：

- `2026-06-12-line-flow-pilot.md`
- `2026-06-13-nscan-network-scanner.md`
- `2026-06-22-empty-box-pallet-plan.md`

对齐项：

- 延续 `LineOrchestrator` 已验证的 AGV 严格到站逻辑。
- 延续 NScan 同步扫码必须走 worker 线程，避免阻塞 UI。
- 延续 `PalletScheduler` 只负责点位规划和缓存，不直接控制机械臂。
- 延续 MainWindow 动态构建 UI 的方式，不新增 `.ui` 文件。
- 延续每阶段编译验证、每阶段独立验收的执行方式。

---

## 13. 详细接口草案

这一节把后续实现者最容易自由发挥的接口先收口。实现时可以按实际代码风格微调函数位置，但名称、职责和信号语义应保持一致，避免调度层、设备层、UI 层互相污染。

### 13.1 `lineconfig.h`

`lineconfig.h` 是新调度层的静态配置和共享轻量类型文件。它不应该持有 QObject，不应该访问设备，不应该发信号。

建议职责：

- 定义任务、系统、步骤枚举。
- 定义 12 工位固定配置表。
- 定义两套码垛区配置表。
- 提供客户友好显示文本。
- 提供统一日志前缀。

推荐辅助函数：

```cpp
inline QString taskStepText(TaskStep step)
{
    switch (step) {
    case TaskStep::Waiting: return QStringLiteral("等待执行");
    case TaskStep::AgvToPickup: return QStringLiteral("前往取料点");
    case TaskStep::ArmPickup: return QStringLiteral("正在取料");
    case TaskStep::PreGripScan: return QStringLiteral("正在扫码");
    case TaskStep::GripAndLift: return QStringLiteral("正在夹取");
    case TaskStep::StowAfterPickup: return QStringLiteral("取料后收安全姿态");
    case TaskStep::AgvToUnload: return QStringLiteral("正在送料");
    case TaskStep::ArmUnload: return QStringLiteral("正在倒料");
    case TaskStep::StowAfterUnload: return QStringLiteral("倒料后收安全姿态");
    case TaskStep::AgvToPallet: return QStringLiteral("前往码垛区");
    case TaskStep::PreparePalletPoint: return QStringLiteral("计算空箱放置点");
    case TaskStep::ArmPalletPlace: return QStringLiteral("正在放空箱");
    case TaskStep::CommitPallet: return QStringLiteral("更新码垛缓存");
    case TaskStep::StowAfterPallet: return QStringLiteral("码垛后收安全姿态");
    case TaskStep::ReturningHome: return QStringLiteral("回待机点");
    case TaskStep::Done: return QStringLiteral("送料完成");
    }
    return QStringLiteral("未知步骤");
}
```

推荐日志前缀：

```cpp
inline QString formatTaskPrefix(const Task &task, const QString &stage)
{
    return QStringLiteral("[T#%1 S%2][%3]")
        .arg(task.taskId)
        .arg(task.stationId, 2, 10, QLatin1Char('0'))
        .arg(stage);
}
```

配置表注释必须写清：

```cpp
// 现场待替换：LM16/LM17 只是当前调试占位值，真实码垛点位由现场地图确认后修改。
// 业务映射：PalletArea::LargeBox 暂复用为共享码垛区，PalletArea::SmallBox 暂复用为工位12独立码垛区。
```

### 13.2 `TaskQueue`

`TaskQueue` 是普通 C++ 类，不继承 QObject。它不记录历史，不落盘，不决定任务失败策略。它只负责 Pending 队列。

推荐完整接口：

```cpp
class TaskQueue
{
public:
    Task enqueue(int stationId, TaskSource source);
    bool hasPending() const;
    int pendingCount() const;
    Task takeNext();
    QList<Task> pendingSnapshot() const;
    void clearPendingAsCanceled(const QString &reason);
    quint64 nextTaskId() const;

private:
    QList<Task> m_pending;
    quint64 m_nextTaskId = 1;
};
```

关键行为：

- `enqueue(3)` 连续两次必须产生两个任务。
- `clearPendingAsCanceled()` 当前 v1 清空即可；未来如果要历史 UI，可改为返回被取消任务列表。
- `TaskQueue` 不知道 `LineSystemState`，避免队列类掺入系统状态。

### 13.3 `TaskExecutor`

`TaskExecutor` 是单任务状态机。它一次只执行一个任务，不接收 UI 缺料信号，不保存队列。

推荐构造：

```cpp
TaskExecutor::TaskExecutor(AgvController *agv,
                           HuayanScheduler *arm,
                           NScanScheduler *scanner,
                           PalletScheduler *pallet,
                           QObject *parent = nullptr);
```

推荐信号：

```cpp
void taskUpdated(const Task &task);
void taskSucceeded(const Task &task);
void taskFailed(const Task &task, const QString &reason);
void systemError(const Task &task, const QString &reason);
void logMessage(const QString &message);
void agvDispatchRequested(int lm);
void scanRequested(const NScanScheduler::ScanOptions &options);
```

推荐槽：

```cpp
void start(const Task &task);
void stopForSystemError(const QString &reason);
void onScanFinished(const NScanScheduler::ScanResult &result);
```

内部状态建议：

```cpp
enum class ExecState {
    Idle,
    AgvToPickup,
    ArmPickup,
    PreGripScan,
    RotateForScan,
    StowAfterPickup,
    AgvToUnload,
    ArmUnload,
    StowAfterUnload,
    AgvToPallet,
    PreparePalletPoint,
    ArmPalletPlace,
    CommitPallet,
    StowAfterPallet,
    CleanupStow
};
```

关键私有方法：

```cpp
void enterState(ExecState state);
void dispatchAgvTo(int lm, TaskStep step, const QString &statusText);
void updateTask(TaskStep step, const QString &statusText);
void finishSucceeded();
void failTask(const QString &reason);
void failSystem(const QString &reason);
void startScanRound(bool afterRotation);
bool isWaitingForAgv() const;
```

注意：

- `TaskExecutor` 不应该调用 `DeviceManager::dispatchAgv(int workstation)`，因为新配置表里已经是数字 LM。
- `TaskExecutor` 不应该直接调用华研 SDK 原语，机械臂动作通过 `HuayanScheduler` 高层接口完成。
- `TaskExecutor` 可以直接调用 `PalletScheduler::nextRelativeOffset()` 和 `commitPlaced()`，因为这不是设备动作，是主流程状态的一部分。

### 13.4 `LineManager`

`LineManager` 是系统状态机。它持有 `TaskQueue` 和一个 `TaskExecutor`。

推荐公开接口：

```cpp
LineSystemState state() const;
QList<Task> queueSnapshot() const;
Task currentTask() const;

public slots:
void start();
void stop();
void resetError();
void reportShortage(int stationId);
void onScanFinished(const NScanScheduler::ScanResult &result);
```

推荐信号：

```cpp
void systemStateChanged(LineSystemState state, const QString &text);
void queueChanged(QList<Task> tasks);
void currentTaskChanged(Task task);
void alarmRaised(QString reason);
void logMessage(QString message);
void agvDispatchRequested(int lm);
void scanRequested(NScanScheduler::ScanOptions options);
```

关键私有方法：

```cpp
void tryStartNext();
void returnHomeIfNeeded();
void enterError(const QString &reason);
void setState(LineSystemState state, const QString &text);
void clearPendingForError(const QString &reason);
```

`LineManager` 必须处理的边界：

- `Idle + reportShortage`：只入队，不执行。
- `Running + reportShortage + executor空闲`：立即执行。
- `Running + reportShortage + executor忙`：只入队。
- `ReturningHome + reportShortage`：主动取消回 LM1，然后执行新任务。
- `Error + reportShortage`：拒绝入队。

### 13.5 `HuayanScheduler`

新增接口要保持向后兼容，不破坏现有华研测试面板。已有 `startStageOne()`、`startUnload()`、`startStow()` 继续可独立使用。

新增数据结构：

```cpp
struct StationArmFunctions {
    QString captureFunc;
    QString unloadPointFunc;
    QString unloadFunc;
};

struct PalletArmFunctions {
    QString palletBaseFunc;
    QString releaseFunc;
};
```

新增接口：

```cpp
void setStationFunctions(const StationArmFunctions &funcs);
void setPalletFunctions(const PalletArmFunctions &funcs);
void setPreGripScanEnabled(bool enabled);
void continueAfterPreGripScan();
void rotateToolRz180();
void startPalletPlace(const PalletPose &offset);
```

新增信号：

```cpp
void preGripScanRequested();
void toolRotationCompleted();
void toolRotationError(QString reason);
void palletPlaceCompleted();
void palletPlaceError(QString reason);
```

关键约束：

- `preGripScanRequested()` 只在 Z 下降到扫码/夹取前位置后发出。
- 发出 `preGripScanRequested()` 后不得继续夹紧，必须等待 `continueAfterPreGripScan()`。
- `rotateToolRz180()` 是独立动作，不应改变当前任务队列。
- `startPalletPlace()` 不调用 `PalletScheduler::commitPlaced()`。

### 13.6 `DeviceManager`

`DeviceManager` 是设备对象持有者。新调度接入后，`MainWindow` 不直接 new `LineManager`。

需要新增：

```cpp
LineManager *lineManager() const { return m_lineManager; }
PalletScheduler *palletScheduler() const { return m_palletScheduler; }
```

如当前 `PalletScheduler` 在 `MainWindow` 中创建，需要移到 `DeviceManager`，保证调度流程和码垛参数窗口使用同一个实例。

扫码 worker 注意：

- UI 测试扫码和调度扫码都可以复用 `NScanScheduler`，但结果不能串线。
- 如果现有 `NScanTestWorker::finished` 同时连 UI 和调度，会导致测试扫码误推进任务。必须拆分信号或增加 operation tag。

### 13.7 `MainWindow`

新 UI 只加调度监控区，不删除、不禁用现有面板。

新增控件建议：

```cpp
QLabel *m_lineStateLabel = nullptr;
QLabel *m_lineCurrentLabel = nullptr;
QLabel *m_lineQueueCountLabel = nullptr;
QLabel *m_lineAlarmLabel = nullptr;
QTableWidget *m_lineQueueTable = nullptr;
QPushButton *m_lineResetBtn = nullptr;
QList<QPushButton *> m_stationButtons;
```

新增方法建议：

```cpp
void initLineDispatchPanel(QVBoxLayout *leftPanel);
void refreshLineQueue(const QList<Task> &tasks);
void onLineSystemStateChanged(LineSystemState state, const QString &text);
void onLineCurrentTaskChanged(const Task &task);
void onLineAlarmRaised(const QString &reason);
void onLineResetError();
void onLineShortageClicked();
```

UI 文案用业务语言，不显示 enum 名称。

---

## 14. 关键状态机伪代码

### 14.1 `LineManager::reportShortage`

```cpp
void LineManager::reportShortage(int stationId)
{
    if (m_state == LineSystemState::Error) {
        emit logMessage(QStringLiteral("[调度] 系统报警中，请先复位，忽略工位%1缺料").arg(stationId));
        return;
    }

    if (!stationConfig(stationId)) {
        emit logMessage(QStringLiteral("[调度] 未知工位%1，忽略缺料信号").arg(stationId));
        return;
    }

    const Task task = m_queue.enqueue(stationId, TaskSource::UiMock);
    emit logMessage(formatTaskPrefix(task, QStringLiteral("TASK"))
                    + QStringLiteral(" 已加入送料队列"));
    emit queueChanged(m_queue.pendingSnapshot());

    if (m_state == LineSystemState::ReturningHome) {
        m_returningHomeCancelForTask = true;
        emit logMessage(QStringLiteral("[调度] 回待机途中收到新任务，取消回 LM1"));
        m_agv->cancelNavigation();
        QTimer::singleShot(300, this, [this] { tryStartNext(); });
        return;
    }

    if (m_state == LineSystemState::Running && !m_executor->isBusy())
        tryStartNext();
}
```

验证点：

- Error 下不入队。
- ReturningHome 下能取消回站。
- Running 空闲时能立刻执行。
- Idle 下只入队，不执行。

### 14.2 `LineManager::start`

```cpp
void LineManager::start()
{
    if (m_state == LineSystemState::Error) {
        emit logMessage(QStringLiteral("[调度] 系统报警中，请先复位"));
        return;
    }

    if (m_state == LineSystemState::Running || m_state == LineSystemState::ReturningHome)
        return;

    setState(LineSystemState::Running, QStringLiteral("等待缺料"));

    if (m_queue.hasPending()) {
        tryStartNext();
    } else {
        returnHomeIfNeeded();
    }
}
```

验证点：

- 有任务时不先回 LM1。
- 无任务时回 LM1。
- Error 不能 Start。

### 14.3 `LineManager::stop`

```cpp
void LineManager::stop()
{
    emit logMessage(QStringLiteral("[调度] 人工停止，进入系统报警"));

    if (m_executor->isBusy())
        m_executor->stopForSystemError(QStringLiteral("人工停止"));

    m_agv->cancelNavigation();
    m_arm->stop();
    m_queue.clearPendingAsCanceled(QStringLiteral("人工停止"));
    emit queueChanged(m_queue.pendingSnapshot());

    setState(LineSystemState::Error, QStringLiteral("系统报警：人工停止"));
    emit alarmRaised(QStringLiteral("人工停止，待执行任务已清空"));
}
```

验证点：

- Stop 不是暂停，是系统报警。
- Pending 必须清空。
- Reset 后不自动恢复。

### 14.4 `TaskExecutor::enterState`

```cpp
void TaskExecutor::enterState(ExecState state)
{
    m_state = state;

    switch (state) {
    case ExecState::AgvToPickup:
        dispatchAgvTo(m_stationConfig->pickupLm,
                      TaskStep::AgvToPickup,
                      QStringLiteral("工位%1正在前往取料点").arg(m_task.stationId));
        break;
    case ExecState::ArmPickup:
        updateTask(TaskStep::ArmPickup, QStringLiteral("工位%1正在取料").arg(m_task.stationId));
        m_arm->startStageOne();
        break;
    case ExecState::PreGripScan:
        updateTask(TaskStep::PreGripScan, QStringLiteral("工位%1正在扫码").arg(m_task.stationId));
        startScanRound(false);
        break;
    case ExecState::RotateForScan:
        emit logMessage(formatTaskPrefix(m_task, QStringLiteral("SCAN"))
                        + QStringLiteral(" 首轮扫码失败，准备旋转180度"));
        m_arm->rotateToolRz180();
        break;
    case ExecState::StowAfterPickup:
        updateTask(TaskStep::StowAfterPickup, QStringLiteral("工位%1取料后收安全姿态").arg(m_task.stationId));
        m_arm->startStow();
        break;
    case ExecState::AgvToUnload:
        dispatchAgvTo(m_stationConfig->unloadLm,
                      TaskStep::AgvToUnload,
                      QStringLiteral("工位%1正在送料").arg(m_task.stationId));
        break;
    case ExecState::ArmUnload:
        updateTask(TaskStep::ArmUnload, QStringLiteral("工位%1正在倒料").arg(m_task.stationId));
        m_arm->startUnload();
        break;
    case ExecState::StowAfterUnload:
        updateTask(TaskStep::StowAfterUnload, QStringLiteral("工位%1倒料后收安全姿态").arg(m_task.stationId));
        m_arm->startStow();
        break;
    case ExecState::AgvToPallet:
        dispatchAgvTo(m_palletConfig->palletLm,
                      TaskStep::AgvToPallet,
                      QStringLiteral("工位%1正在前往码垛区").arg(m_task.stationId));
        break;
    case ExecState::PreparePalletPoint:
        preparePalletPoint();
        break;
    case ExecState::ArmPalletPlace:
        updateTask(TaskStep::ArmPalletPlace, QStringLiteral("工位%1正在放空箱").arg(m_task.stationId));
        m_arm->startPalletPlace(m_palletOffset);
        break;
    case ExecState::CommitPallet:
        commitPalletPlaced();
        break;
    case ExecState::StowAfterPallet:
        updateTask(TaskStep::StowAfterPallet, QStringLiteral("工位%1码垛后收安全姿态").arg(m_task.stationId));
        m_arm->startStow();
        break;
    case ExecState::CleanupStow:
        emit logMessage(formatTaskPrefix(m_task, QStringLiteral("CLEANUP"))
                        + QStringLiteral(" 任务失败，准备收安全姿态"));
        m_arm->startStow();
        break;
    case ExecState::Idle:
        break;
    }
}
```

验证点：

- 每个状态都更新客户可读文案。
- 每个机械臂动作都只调用高层接口。
- 码垛 offset 和 commit 分成两个状态。

### 14.5 AGV 到站判断伪代码

```cpp
void TaskExecutor::onAgvMonitor(const AgvMonitorData &data)
{
    if (!isWaitingForAgv())
        return;

    if (data.navStatus == 1 || data.navStatus == 2)
        m_agvSeenMoving = true;

    if (!m_agvSeenMoving)
        return;

    if (data.navStatus == 4
        && data.navStation == m_expectedLm
        && data.curStation == m_expectedLm) {
        m_agvTimeout->stop();
        advanceAfterAgvArrived();
        return;
    }

    if (data.navStatus >= 5 && data.navStatus <= 7) {
        failSystem(QStringLiteral("AGV 导航异常，状态=%1").arg(data.navStatus));
        return;
    }
}
```

验证点：

- 没看到移动前不能用旧 Arrived 误判。
- `curStation` 必须匹配目标 LM。
- 任务导航中的 canceled 是 fatal；只有 `LineManager` 的 ReturningHome 主动取消不是 fatal。

### 14.6 扫码处理伪代码

```cpp
void TaskExecutor::onScanFinished(const NScanScheduler::ScanResult &result)
{
    const bool hasCode = result.isSuccess() && !result.code.trimmed().isEmpty();

    if (hasCode) {
        emit logMessage(formatTaskPrefix(m_task, QStringLiteral("SCAN"))
                        + QStringLiteral(" 扫码成功：%1").arg(result.code));
        m_arm->continueAfterPreGripScan();
        return;
    }

    if (!m_scanAfterRotation) {
        if (!kEnableScanFailureRotationRecovery) {
            emit logMessage(formatTaskPrefix(m_task, QStringLiteral("SCAN"))
                            + QStringLiteral(" 首轮扫码失败或空码，旋转补救已关闭"));
            failTask(QStringLiteral("扫码失败或空码"));
            return;
        }

        emit logMessage(formatTaskPrefix(m_task, QStringLiteral("SCAN"))
                        + QStringLiteral(" 首轮扫码失败或空码，准备旋转180度"));
        enterState(ExecState::RotateForScan);
        return;
    }

    failTask(QStringLiteral("扫码失败或空码"));
}
```

验证点：

- 空码不能通过。
- 默认开关关闭时，第一轮失败直接任务失败且不得旋转。
- 仅当 `kEnableScanFailureRotationRecovery` 开启时，第一轮失败旋转、第二轮失败才任务失败。

### 14.7 码垛点准备伪代码

```cpp
void TaskExecutor::preparePalletPoint()
{
    QStringList errors;
    if (!m_pallet->validateConfig(m_stationConfig->palletArea, &errors, nullptr)) {
        failSystem(QStringLiteral("码垛配置无效：%1").arg(errors.join(QStringLiteral("；"))));
        return;
    }

    QString error;
    if (!m_pallet->nextRelativeOffset(m_stationConfig->palletArea, &m_palletOffset, &error)) {
        failSystem(QStringLiteral("%1无可用放置点：%2")
                   .arg(palletAreaDisplayName(m_stationConfig->palletArea))
                   .arg(error));
        return;
    }

    emit logMessage(formatTaskPrefix(m_task, QStringLiteral("PALLET"))
                    + QStringLiteral(" %1 offset X=%2 Y=%3 Z=%4 Rz=%5")
                        .arg(palletAreaDisplayName(m_stationConfig->palletArea))
                        .arg(m_palletOffset.x, 0, 'f', 1)
                        .arg(m_palletOffset.y, 0, 'f', 1)
                        .arg(m_palletOffset.z, 0, 'f', 1)
                        .arg(m_palletOffset.rz, 0, 'f', 1));

    enterState(ExecState::ArmPalletPlace);
}
```

验证点：

- 满料/配置错误是系统级 ERROR。
- 计算 offset 不推进 placedCount。
- 日志能看到区域和 offset。

---

## 15. 逐文件实现细节

### 15.1 `src/lineconfig.h`

容易遗漏点：

- 需要包含 `palletscheduler.h`，否则拿不到 `PalletArea`。
- 如果在信号里传 `Task` 和 `QList<Task>`，要在 `devicemanager.h` 或合适公共头中 `Q_DECLARE_METATYPE`。
- `stationConfig()` 返回静态数组元素指针，调用方不能保存后修改内容。
- 函数名使用 `QStringLiteral`，避免运行时重复构造。

### 15.2 `src/taskqueue.h`

容易遗漏点：

- 不要用 `stationId` 当唯一标识。
- 不要把 Failed/Succeeded 历史塞回 Pending 队列。
- 不要在这里做客户系统差值算法。

### 15.3 `src/taskexecutor.cpp`

必须小心的连接：

```cpp
connect(m_agv, &AgvController::monitorUpdated,
        this, &TaskExecutor::onAgvMonitor);
connect(m_agv, &AgvController::errorOccurred,
        this, &TaskExecutor::onAgvError);
connect(m_arm, &HuayanScheduler::stageCompleted,
        this, &TaskExecutor::onArmStageCompleted);
connect(m_arm, &HuayanScheduler::stageError,
        this, &TaskExecutor::onArmStageError);
connect(m_arm, &HuayanScheduler::preGripScanRequested,
        this, &TaskExecutor::onPreGripScanRequested);
connect(m_arm, &HuayanScheduler::toolRotationCompleted,
        this, &TaskExecutor::onToolRotationCompleted);
connect(m_arm, &HuayanScheduler::toolRotationError,
        this, &TaskExecutor::onToolRotationError);
connect(m_arm, &HuayanScheduler::palletPlaceCompleted,
        this, &TaskExecutor::onPalletPlaceCompleted);
connect(m_arm, &HuayanScheduler::palletPlaceError,
        this, &TaskExecutor::onPalletPlaceError);
```

注意：

- `stageCompleted` 是通用信号，必须结合 `ExecState` 判断下一步，不要靠 stageName 字符串。
- `stageError` 在 `CleanupStow` 期间代表收姿态失败，要升级系统级 ERROR。
- 普通 `stageError` 先走任务级失败。
- AGV 任务导航期间 `navStatus == 6` 也是系统级 ERROR，因为不是主动回站取消。
- `TaskExecutor::stopForSystemError()` 只用于系统级停机，内部应停计时器、停机械臂、取消 AGV。

### 15.4 `src/linemanager.cpp`

必须小心的状态：

- `Running` 包含“正在执行任务”和“等待缺料”。
- `ReturningHome` 是独立状态，因为它的 AGV cancel 可能是正常行为。
- `Error` 状态下不允许入队。
- `Stop` 后 Pending 全清，不保留。

建议状态文案：

```cpp
Idle -> "未启动"
Running 且无当前任务 -> "等待缺料"
Running 且有当前任务 -> task.statusText
ReturningHome -> "回待机点"
Error -> "系统报警"
```

### 15.5 `src/huayanScheduler.cpp`

容易遗漏点：

- `setStationFunctions()` 不要清空默认函数；只覆盖非空字段。
- `setPreGripScanEnabled(false)` 时原 `startStageOne()` 应接近原行为，不等待扫码。
- `preGripScanRequested()` 发出后必须暂停，不能同时继续夹紧。
- `rotateToolRz180()` 完成后要恢复到空闲阶段，避免影响后续 `continueAfterPreGripScan()`。
- `startPalletPlace()` 完成松爪后只发 `palletPlaceCompleted()`，不做 `commitPlaced()`。
- `onVisionNoObject()` 只在 `StageOne + WaitForVision` 生效，避免相机预览或码垛视觉检测误触发机械臂下移。

### 15.6 `src/devicemanager.cpp`

容易遗漏点：

- 如果移动 `PalletScheduler` 所有权，要同步改 `MainWindow::onPalletConfig()`，保证配置窗口拿的是 `m_devMgr->palletScheduler()`。
- 调度扫码 worker 和 UI 测试扫码 worker 的结果不能串。
- `lineManager` 的日志要接到现有 `logMessage`，这样日志文件也能记录。
- 调度层 AGV 派发直接发数字 LM，不走 `resolveStation()`。

### 15.7 `src/mainwindow.cpp`

容易遗漏点：

- 不要删除现有测试面板。
- 不要改 `WorkflowWidget`。
- 12 个工位按钮只是模拟缺料，不代表真实客户算法。
- 队列表只显示 Pending + Running。
- 系统报警弹窗不要在普通任务失败时弹，只在系统级 ERROR 时弹。

---

## 16. 更细的验证矩阵

| 编号 | 场景 | 操作 | 预期 UI | 预期日志 | 通过标准 |
|------|------|------|---------|----------|----------|
| Q1 | Idle 入队 | 未 Start，点 S3 两次 | 队列 2 条 S3 | 两条 TASK 入队日志 | 两个 taskId 不同 |
| Q2 | FIFO | 依次点 S5、S2、S5 | 队列表按 S5/S2/S5 | 日志顺序一致 | 不按工位号重排 |
| Q3 | Start 有队列 | Q2 后点 Start | 当前任务 S5 | S5 开始送料 | 不先回 LM1 |
| Q4 | Start 空队列 | 无任务点 Start | 状态等待缺料/回待机 | AGV 回 LM1 | AGV 目标为 1 |
| Q5 | 回站打断 | ReturningHome 点 S6 | 状态变 S6 任务 | 主动取消回站 | 不报警 |
| Q6 | Error 入队 | Error 下点 S1 | 队列不变 | 提示先复位 | 不新增任务 |
| Q7 | Stop | Running 中点 Stop | 系统报警，队列清空 | 人工停止 | Reset 后 Idle |
| A1 | AGV 到站 | 任务导航到目标 LM | 进入下一阶段 | AGV 到站日志 | navStation/curStation 都匹配 |
| A2 | AGV 失败 | 导航返回 5/7 | 系统报警 | AGV ERROR | Pending 清空 |
| A3 | AGV 取消 | 任务导航返回 6 | 系统报警 | AGV 取消异常 | Pending 清空 |
| V1 | 视觉无目标 | noObjectDetected | 仍在取料 | 下移 20mm | 未超 80mm 重试 |
| V2 | 视觉超限 | 连续无目标超 80mm | 当前任务失败 | 视觉搜索超限 | 收姿态后继续 |
| V3 | 视觉错误 | HTTP/解析错误 | 当前任务失败 | 视觉服务错误 | 不下移 |
| S1 | 扫码成功 | 返回非空 code | 继续夹紧 | 扫码成功 code | 不旋转 |
| S2 | 默认开关关闭时扫码空码 | Success 但 code 空 | 当前任务失败 | 旋转补救已关闭 | 不下发旋转动作 |
| S3 | 默认开关关闭时扫码超时 | 返回 Timeout | 当前任务失败 | 旋转补救已关闭 | 不发起第二轮扫码 |
| S4 | 开关开启时首轮失败 | 返回 Timeout | 旋转180 | SCAN 失败 | 第二轮发起 |
| P1 | 码垛正常 | next offset 成功 | 正在放空箱 | offset 日志 | 松爪后 commit |
| P2 | 码垛满 | placedCount 到容量 | 系统报警 | 码垛区满 | 不调用放箱 |
| P3 | commit 失败 | commitPlaced false | 系统报警 | commit 失败 | Pending 清空 |
| C1 | 任务级失败收尾成功 | 扫码失败后 stow 成功 | 下个任务继续 | CLEANUP 成功 | 不进入 Error |
| C2 | 收尾失败 | 失败后 stow 失败 | 系统报警 | 收姿态失败 | Pending 清空 |

---

## 17. 代码注释清单

实现完成前，逐项检查这些注释是否存在：

- `lineconfig.h`
  - 说明 LM16/LM17 是现场待替换占位值。
  - 说明 `LargeBox/SmallBox` 当前业务映射。

- `taskqueue.h`
  - 说明队列保存的是任务实例，同工位允许多个任务。

- `taskexecutor.cpp`
  - AGV 到站处说明旧到达态误判风险，必须等 SeenMoving 且校验 `curStation`。
  - 扫码空码处说明空码不可追溯，因此按失败处理。
  - pre-grip 扫码处说明这是夹紧前安全暂停点。
  - CleanupStow 处说明任务级失败和系统级 ERROR 的边界。
  - `commitPlaced()` 前说明只有松爪成功后才能推进码垛缓存。

- `linemanager.cpp`
  - Stop 处说明 Stop 是人工急停报警，不是暂停。
  - ReturningHome 取消处说明主动取消回 LM1 不算 AGV fatal。
  - Start 处说明 LM1 是空闲待机点，不是每单强制起点。

- `huayanScheduler.cpp`
  - 视觉搜索下移处说明这是算法不稳定保护，不是抓取下探。
  - 视觉错误处说明通信错误不做下移。
  - `startPalletPlace()` 处说明只执行动作，不推进 `PalletScheduler` 缓存。

- `devicemanager.cpp`
  - 调度 AGV 派发处说明新调度使用数字 LM，不走旧工位映射。
  - 调度扫码 worker 处说明和 UI 测试扫码隔离。

- `mainwindow.cpp`
  - 调度监控区说明客户关心任务状态，现有设备测试面板保留。
  - 队列表刷新处说明只显示未完成任务，历史走日志。

---

## 18. 已知风险与规避

1. **扫码 worker 结果串线**
   - 风险：UI 测试扫码完成后误推进主流程。
   - 规避：调度扫码和测试扫码使用独立 worker 或 operation tag。

2. **AGV 取消语义混淆**
   - 风险：回 LM1 主动取消被当成系统故障，或任务导航取消被当成正常。
   - 规避：`LineManager::ReturningHome` 独立处理，`TaskExecutor` 任务导航中取消一律 fatal。

3. **HuayanScheduler 通用 stageCompleted 难区分**
   - 风险：靠字符串判断阶段导致误推进。
   - 规避：`TaskExecutor` 只根据自身 `ExecState` 判断下一步。

4. **视觉搜索与闭环混淆**
   - 风险：无目标搜索次数和闭环对准次数互相影响。
   - 规避：搜索计数 `m_searchDescendedMm` 独立于 `m_grabIterations`。

5. **码垛缓存提前推进**
   - 风险：机械臂没有放下空箱，`placedCount` 已增加。
   - 规避：只在 `palletPlaceCompleted()` 后调用 `commitPlaced()`。

6. **现有测试面板误操作**
   - 风险：调度运行中人工点击测试面板抢设备。
   - 规避：v1 按需求不禁用，由现场人为保证；后续加入互锁。

7. **配置表现场值不准**
   - 风险：函数名或 LM 占位不符合现场。
   - 规避：集中配置表，日志打印每次任务使用的 LM 和函数名，方便现场替换。

---

本计划特意不包含 git 提交步骤；执行者可以按项目实际协作方式提交。

---

## 19. 源码注释完善实施计划

**目标：** 在不改变任何可执行逻辑、接口签名、变量值、宏值和构建行为的前提下，为 12 工位连续补料相关代码补齐可维护的中文注释。

### 任务 10：核心模型与队列注释

**文件：** `src/lineconfig.h`、`src/taskqueue.h`

- 为系统状态、任务状态、任务来源和任务步骤逐项解释。
- 为 `Task`、工位配置、码垛区配置的每个字段说明含义和单位。
- 为 FIFO、不去重、清空 Pending 等队列规则说明业务原因。

### 任务 11：单任务与整线状态机注释

**文件：** `src/taskexecutor.h/.cpp`、`src/linemanager.h/.cpp`

- 为类职责、信号槽、状态枚举、计时器和运行期字段建立完整注释。
- 为取料、扫码、倒料、码垛和失败清理的推进条件说明原因。
- 为任务级失败与系统级 ERROR 的边界、AGV 严格到站条件和码垛 commit 时机重点说明。

### 任务 12：机械臂状态机注释

**文件：** `src/huayanScheduler.h/.cpp`

- 为阶段、步骤、独立动作、视觉搜索与闭环字段说明生命周期。
- 为公开接口说明前置条件、暂停/恢复语义和完成/错误信号。
- 为 SDK 调用、坐标系、运动单位、安全阈值及异步轮询说明风险。

### 任务 13：设备接线与 UI 注释

**文件：** `src/devicemanager.h/.cpp`、`src/mainwindow.h/.cpp`、`src/agvcontroller.h/.cpp`、`src/visionclient.cpp`、`CMakeLists.txt`

- 只注释 12 工位调度相关新增/修改区域，不重写无关旧模块。
- 说明对象所有权、扫码 worker 线程隔离、数字 LM 派发和日志转发。
- 说明 UI 看板只做输入展示，现有测试面板不属于主调度状态机。

### 任务 14：只注释变更验证

- 执行 `git diff --check`，不得有空白错误。
- 人工检查 diff，源码变化只能是注释或空白；文档变化只能是注释规范和阅读说明。
- 完整构建 `wh-robot-visual`，确认注释没有破坏预处理、MOC/UIC 或编译。
- 不执行 `git commit`，展示差异后等待用户明确确认。

### 后续任务通用注释模板（强制）

后续在本计划中追加任何任务时，必须同时填写：

**源码注释要求：**

- 列出新增/修改的类、接口、状态、变量、常量、宏和信号槽。
- 对每项注明业务含义、单位或取值范围、所有权/生命周期、线程边界、前置条件、副作用及错误处理。
- 对状态机修改说明进入条件、退出条件、正常下一步和失败路径。

**注释验收清单：**

- [ ] 新增声明和关键实现均有准确中文注释。
- [ ] 原有注释已随行为修改同步更新，无过期或矛盾描述。
- [ ] spec、plan、头文件和实现文件对同一接口/状态的描述一致。
- [ ] 现场临时值、单位、安全阈值和后续替换点均已明确标注。
- [ ] 完成代码审查与编译验证后，才允许将任务标记完成并请求提交。

### 后续任务文件范围确认模板（强制）

每个 Bug 修复或新功能任务必须在实施前填写：

**已确认文件范围：**

- 新增文件：逐项列出；没有则明确写“无”。
- 修改文件：逐项列出，并说明每个文件为何需要修改。
- 明确不修改的相邻模块：列出容易被误改但不在本任务范围内的文件或组件。

**范围扩大门禁：**

- [ ] 实施过程中没有新增计划外文件，也没有修改计划外文件。
- [ ] 如需扩大范围，已停止修改并向用户说明原因、文件清单、职责和影响。
- [ ] 已获得用户明确确认后才继续跨范围修改。
- [ ] 最终 `git diff --name-only` 与用户确认的文件范围一致。

### 后续任务 Git 提交门禁（强制）

Bug 修复或新功能完成并验证后，按以下顺序执行：

- [ ] 测试、完整编译和 `git diff --check` 均已通过。
- [ ] 已核对实际 diff 与用户确认的文件范围一致。
- [ ] 已向用户展示待提交文件、改动摘要、验证结果和建议 commit message。
- [ ] 已获得用户明确的提交确认。
- [ ] 仅在确认后执行本地 `git commit`。
- [ ] 未执行 `git push`；后续 push 由用户人工完成。

未满足任一门禁项时，代码可以保留在工作区继续检查，但不得提交，也不得声称已经完成十二工位现场验收。

---

## 20. 现场 Bug 修复

### 任务 15：Bug 1——默认禁止扫码失败后的 180 度旋转

**问题与根因：** `TaskExecutor::onScanFinished()` 在首轮扫码失败或空码时无条件进入 `RotateForScan`，存在碰撞人员和物体的风险。

**确认方案：**

- 在 `TaskExecutor` 集中常量区增加布尔代码开关，默认 `false`。
- 默认关闭时直接进入任务级失败收尾，不旋转、不进行第二轮扫码。
- 开关改为 `true` 后保留原有“旋转 180 度 → 第二轮扫码”流程。
- 不新增 UI，不使用 `QSettings`；开启需修改常量并重新编译。

**已确认文件范围：**

- 新增文件：无。
- 修改：`src/taskexecutor.h`、`src/taskexecutor.cpp`、唯一设计文档和本实施文档。
- 明确不修改：`huayanScheduler.*`、`linemanager.*`、`mainwindow.*`、`devicemanager.*`、扫码 SDK 和其他设备模块。

**实施与验证：**

1. 新增默认关闭的安全常量，并在首轮失败分支于旋转调用前判断。
2. 静态检查扫码成功、默认关闭失败、开启后旋转补救、第二轮失败四条路径。
3. 完整构建，执行 `git diff --check` 和文件范围检查。
4. 现场制造一次扫码失败，确认日志显示旋转补救关闭，机械臂不旋转并进入安全收姿态。

**源码注释要求：**

- 开关名称直接表达“扫码失败后是否允许旋转补救”。
- 注释说明默认 `false`、碰撞风险、开启前必须完成现场安全隔离、修改后需重新编译。
- 首轮失败分支说明关闭时直接任务失败属于安全策略。
- 更新 `m_scanAttempt`、`RotateForScan` 等相邻注释，使其只描述开关开启时的路径。

**注释验收清单：**

- [x] 新增常量和失败分支具有准确中文安全注释。
- [x] 相邻状态、字段和接口注释与“默认不旋转”一致。
- [x] spec、plan、头文件和实现文件的默认值及行为一致。
- [x] 无过期文案继续声称“首轮失败必须旋转”。

**范围扩大门禁：**

- [x] 实际差异仅包含上述四个已确认文件。
- [ ] 如需新增测试文件或修改构建文件，先暂停并获得用户确认。

**实施结果（2026-07-01）：**

- 已增加 `kEnableScanFailureRotationRecovery = false`，保护判断位于旋转状态切换和 `rotateToolRz180()` 调用之前。
- 默认关闭时记录明确安全日志，并调用 `beginCleanupAfterTaskFailure()` 进入收安全姿态流程。
- `cmake --build build --target wh-robot-visual -j$(nproc)` 通过。
- `ctest --test-dir build --output-on-failure`：2/2 通过，0 失败。
- 当前状态：代码和离线验证完成，待现场扫码失败复测；十二工位整体验收仍未完成。
