# 12工位连续补料调度系统 设计文档

日期：2026-06-25
状态：开发联调中，尚未完成十二工位现场实测验收
范围：`LineManager`、`TaskQueue`、`TaskExecutor`、`HuayanScheduler`、`MainWindow` 调度监控区

## 背景

现有 `LineOrchestrator` 已在单工位场景跑通“初始站点 → 取料 → 倒料 → 回初始站点”的整线流程，并验证了 AGV、华研机械臂、视觉闭环等关键链路。但现场最终需要支持 12 工位连续补料：

- 单 AGV + 单华研机械臂，设备动作严格串行。
- 缺料任务可连续到达，并按先来后到进入 FIFO 队列。
- 当前 v1 使用 UI 模拟缺料；客户系统暂不自动派单。
- 主流程必须覆盖：取料、视觉搜索与闭环、夹取前扫码预留、倒料、空箱码垛、空闲回 LM1。
- 现有 AGV、机械臂、视觉、扫码、客户系统、码垛测试面板全部保留，不做禁用或权限互锁。

本设计新增调度层，不继续扩展 `LineOrchestrator`。`LineOrchestrator` 保留为单工位参考和调试资产。

## 1. 总体架构

```
MainWindow
  └── DeviceManager
        ├── AgvController
        ├── HuayanScheduler
        ├── VisionHttpClient
        ├── NScanScheduler
        ├── CustomSysScheduler
        ├── PalletScheduler
        └── LineManager
              ├── TaskQueue
              └── TaskExecutor
```

职责边界：

- `MainWindow`：只做 UI 展示和按钮输入。新增调度监控区；现有测试面板不动。
- `DeviceManager`：继续持有所有设备对象；新增持有 `LineManager`，并提供设备依赖注入。
- `LineManager`：管理系统状态、Start/Stop/Reset、FIFO 队列、空闲回 LM1、系统级 ERROR 清队列。
- `TaskQueue`：保存任务实例，按 FIFO 取任务，不按工位号排序，不做同工位去重。
- `TaskExecutor`：执行一个任务的大流程，协调 AGV、机械臂、扫码、码垛。
- `HuayanScheduler`：负责机械臂动作细节，包含视觉搜索、视觉闭环、扫码暂停点、旋转 180 度、倒料、码垛动作。
- `PalletScheduler`：继续负责码垛参数、容量、下一点偏移、已放数量缓存；不直接控制机械臂。

## 2. 任务模型与队列规则

每一次缺料信号生成一个独立任务。当前 v1 的缺料信号只来自 UI 模拟按钮，客户系统暂不参与自动派单。

```cpp
enum class TaskState {
    Pending,
    Running,
    Succeeded,
    Failed,
    Canceled
};

enum class TaskSource {
    UiMock,
    CustomerSystem
};

struct Task {
    quint64 taskId = 0;
    int stationId = 0;
    TaskSource source = TaskSource::UiMock;
    TaskState state = TaskState::Pending;
    int stepIndex = 0;
    QDateTime createdAt;
    QString statusText;
    QString lastError;
};
```

队列规则：

- FIFO，先来后到，不按工位号排序。
- 同一工位允许多次入队；一次任务只补一个料箱。
- `Idle` 状态允许入队，但不自动执行，点击 Start 后执行。
- `Running` 状态允许入队，若当前空闲则自动执行。
- `ReturningHome` 状态收到新任务时，取消回 LM1，直接执行新任务。
- `Error` 状态不允许入队，点击缺料按钮只提示“系统报警中，请先复位”。

客户系统未来接入方向：

- 客户系统将根据产量、12 工位消耗比例、安全阈值推算缺料。
- 当前 v1 不实现该算法，保留 `CustomSysScheduler` 通信测试面板。

## 3. 系统状态机

```
Idle
  Start
    有 Pending 任务 -> Running，直接执行队首任务
    无 Pending 任务 -> Running/WaitingForTask，并要求 AGV 回 LM1

Running
  任务完成或任务级失败收尾成功
    有 Pending 任务 -> 直接执行下一任务，不回 LM1
    无 Pending 任务 -> ReturningHome
  Stop -> Error

ReturningHome
  AGV 到 LM1 -> Running/WaitingForTask
  回站途中收到新任务 -> 主动取消回站，执行新任务
  回站失败/非主动取消/超时 -> Error

Error
  清空 Pending 队列
  不接收新任务
  Reset Error -> Idle
```

LM1 语义：

- LM1 是空闲待机点。
- 任务之间有下一任务时，不回 LM1。
- 队列为空时，AGV 必须在 LM1 或正在回 LM1。
- Start 时如果已有 Pending 任务，不要求 AGV 先在 LM1，直接派发到队首任务取料点。

Stop 语义：

- Stop 按人工急停报警处理。
- 立即 cancel AGV、stop 机械臂。
- 当前任务标记 `Canceled`。
- 所有 Pending 任务全部取消/清空。
- 系统进入 `Error`，必须人工 Reset，之后回 `Idle`。

## 4. 工位与码垛配置

调度层使用数字 LM 点位，直接发给 AGV Modbus。文档和日志可显示 `LM5`，代码中保存为 `5`。

```cpp
struct StationTaskConfig {
    int stationId;
    int pickupLm;
    int unloadLm;
    PalletArea palletArea;
    QString captureFunc;
    QString unloadPointFunc;
    QString unloadFunc;
};

struct PalletAreaTaskConfig {
    PalletArea area;
    int palletLm;
    QString palletBaseFunc;
    QString releaseFunc;
};
```

工位映射：

| 工位 | 取料 LM | 倒料 LM | 码垛区 |
|------|---------|---------|--------|
| 1 | 3 | 3 | 共享码垛区 |
| 2 | 4 | 4 | 共享码垛区 |
| 3 | 5 | 9 | 共享码垛区 |
| 4 | 5 | 9 | 共享码垛区 |
| 5 | 5 | 10 | 共享码垛区 |
| 6 | 6 | 10 | 共享码垛区 |
| 7 | 6 | 11 | 共享码垛区 |
| 8 | 6 | 11 | 共享码垛区 |
| 9 | 7 | 12 | 共享码垛区 |
| 10 | 7 | 12 | 共享码垛区 |
| 11 | 7 | 7 | 共享码垛区 |
| 12 | 15 | 15 | 工位12独立码垛区 |

码垛区映射：

- `PalletArea::LargeBox` = 共享码垛区（工位 1-11）。
- `PalletArea::SmallBox` = 工位12独立码垛区。
- 共享码垛区 `palletLm = 16`，当前为模拟占位值，现场替换。
- 工位12独立码垛区 `palletLm = 17`，当前为模拟占位值，现场替换。

默认机械臂函数命名：

- `captureFunc = Func_capture_Sxx`
- `unloadPointFunc = Func_daoliao_point_Sxx`
- `unloadFunc = Func_daoliao_Sxx`
- 共享码垛基准函数：`Func_pallet_shared_base`
- 工位12独立码垛基准函数：`Func_pallet_s12_base`
- 松爪函数：`Func_songzhua`
- 收安全姿态函数：`Func_yun_xing_zhong`

v1 使用集中代码配置表，不新增配置 UI。后续现场函数名或 LM 点位变化时，优先修改集中配置表。

## 5. 单任务主流程

```
TaskStart
  读取 StationTaskConfig
  读取 PalletAreaTaskConfig

AgvToPickup
  AGV -> pickupLm

ArmPickup
  机械臂调用 captureFunc
  视觉搜索 A
  视觉闭环 B
  Z 下降到扫码/夹取前位置
  发出 preGripScanRequested

PreGripScan
  扫码成功且 barcode 非空 -> 默认比对通过，记录 barcode
  扫码失败或空码，且扫码补救旋转开关关闭（默认） -> 当前任务 Failed，进入 CleanupStow
  扫码失败或空码，且扫码补救旋转开关开启 -> 机械臂 Rz 相对旋转 180 度，再扫码
  第二轮仍失败 -> 当前任务 Failed，进入 CleanupStow

ContinueGrip
  机械臂继续夹紧、抬升

StowAfterPickup
  机械臂收安全姿态

AgvToUnload
  AGV -> unloadLm

ArmUnload
  机械臂调用 unloadPointFunc
  机械臂调用 unloadFunc 翻转倒料
  倒料后机械臂仍夹持空箱

StowAfterUnload
  机械臂收安全姿态

AgvToPallet
  根据 palletArea 找 palletLm
  AGV -> palletLm

PreparePalletPoint
  PalletScheduler::validateConfig
  PalletScheduler::nextRelativeOffset

ArmPalletPlace
  机械臂调用 palletBaseFunc 到码垛基准点
  按 offset 分轴相对 MoveRelL
  调用 releaseFunc 松爪放空箱

CommitPallet
  PalletScheduler::commitPlaced

StowAfterPallet
  机械臂收安全姿态

TaskSucceeded
  UI 显示“工位X送料完成”
  有下一任务 -> 直接执行下一任务
  无下一任务 -> AGV 回 LM1 待机
```

“送料完成”的业务定义：

- AGV 到达倒料点不算完成。
- 必须完成倒料、空箱码垛、`commitPlaced`、机械臂收安全姿态，才显示“送料完成”。

## 6. HuayanScheduler 增强

新增接口只扩展能力，不破坏现有测试面板可用性。

### 6.1 工位函数注入

```cpp
struct StationArmFunctions {
    QString captureFunc;
    QString unloadPointFunc;
    QString unloadFunc;
};

void setStationFunctions(const StationArmFunctions &funcs);
```

任务开始前由 `TaskExecutor` 注入当前工位函数。现有默认函数保留，用于单独测试。

### 6.2 视觉搜索 A 与视觉闭环 B

视觉阶段包含两个循环，均放在 `HuayanScheduler` 内部。

搜索 A：解决算法初始拍照位可能识别不到目标的问题。

- 调用 `captureFunc` 到拍照/搜索初始位。
- `noObjectDetected`：每次 Z 向下搜索 `20mm`，等待视觉稳定后重试。
- 最大搜索下移 `80mm`，最多 4 次；仍无目标则任务失败。
- 视觉 HTTP 错误、JSON 解析错误、视觉服务不可达不做下移，直接任务失败。

闭环 B：识别到目标后对准。

- 根据视觉返回 x/y/rz 做工具坐标微调。
- 误差阈值沿用：XY `2mm`，Rz `1deg`。
- 最大闭环次数沿用 `6` 次。
- 超限后任务失败。

### 6.3 夹取前扫码暂停点

```cpp
void setPreGripScanEnabled(bool enabled);
void continueAfterPreGripScan();

signals:
void preGripScanRequested();
```

Z 下降到扫码/夹取前位置后，如果启用扫码暂停，`HuayanScheduler` 发出 `preGripScanRequested()` 并暂停。`TaskExecutor` 扫码通过后调用 `continueAfterPreGripScan()`，机械臂再夹紧、抬升。

### 6.4 扫码失败旋转

```cpp
void rotateToolRz180();

signals:
void toolRotationCompleted();
void toolRotationError(QString reason);
```

扫码补救旋转属于可能碰撞现场人员或物体的危险动作，必须由 `TaskExecutor` 中的集中代码常量控制。该开关默认关闭，不新增 UI，也不通过 `QSettings` 保存；默认状态下首轮扫码失败或空码直接判定当前任务失败并进入安全收姿态，不得调用 `rotateToolRz180()`。

后续确需恢复旋转补救时，现场负责人须先确认旋转空间和人员隔离，再将 `TaskExecutor` 中对应布尔常量改为 `true` 并重新编译。开关开启时，首轮扫码失败或空码后才允许调用旋转接口并执行第二轮扫码。

### 6.5 码垛动作

```cpp
struct PalletArmFunctions {
    QString palletBaseFunc;
    QString releaseFunc;
};

void setPalletFunctions(const PalletArmFunctions &funcs);
void startPalletPlace(const PalletPose &offset);

signals:
void palletPlaceCompleted();
void palletPlaceError(QString reason);
```

码垛动作由 `HuayanScheduler` 执行：

1. 调用 `palletBaseFunc` 到码垛区基准点。
2. 根据 `PalletScheduler::nextRelativeOffset()` 返回的 offset，按 X/Y/Z/Rz 分轴相对 MoveRelL。
3. 调用 `releaseFunc` 松爪。
4. 完成后通知 `TaskExecutor` 调用 `commitPlaced()`。

## 7. 扫码与比对策略

调度流程使用默认扫码配置，不读取 NScan 调试面板输入框作为运行参数：

- IP：使用 `DeviceManager::Config::scannerIP`
- port：`30000`
- timeoutMs：`2000`
- maxAttempts：`3`
- retryIntervalMs：`500`

策略：

- 每轮扫码最多 3 次。
- 第一轮失败或空码后，机械臂 Rz 相对旋转 180 度，再扫码一轮。
- SDK 返回 Success 但 barcode 为空，按失败处理。
- v1 没有客户比对标准，非空 barcode 默认通过，并记录日志。

预留接口：

```cpp
ScanValidationResult validateBarcode(const Task &task, const QString &barcode);
```

v1 实现为“非空即通过”。客户提供比对规则后，只替换 validator，不改主状态机。

## 8. 码垛策略

`PalletScheduler` 接入主流程，负责：

- `validateConfig(area)`
- `nextRelativeOffset(area, &offset, &error)`
- `commitPlaced(area, &error)`
- 容量与已放数量缓存

执行规则：

- 工位 1-11 使用 `PalletArea::LargeBox`，即共享码垛区。
- 工位 12 使用 `PalletArea::SmallBox`，即工位12独立码垛区。
- 只有机械臂到达放置点并松爪成功后，才调用 `commitPlaced()`。
- `commitPlaced()` 失败进入系统级 ERROR。
- 码垛区满、配置无效、无法计算下一放置点时，弹窗警告、日志记录、系统进入 ERROR，不继续下一任务。

倒料后默认机械臂仍夹持空箱，不再做视觉或扫码，直接进入空箱码垛段。

## 9. 错误处理

### 9.1 任务级失败

任务级失败包括：

- 视觉 HTTP/解析/服务错误。
- 视觉搜索超限。
- 视觉闭环超限。
- 扫码两轮失败或空码。
- 普通机械臂阶段失败。
- 机械臂码垛动作失败。

处理：

```
当前任务 Failed
尝试 HuayanScheduler::startStow()
  收姿态成功：
    有下一任务 -> 继续下一任务
    无下一任务 -> AGV 回 LM1 待机
  收姿态失败：
    升级系统级 ERROR
```

机械臂错误 v1 默认先按任务级失败处理，尝试收姿态；后续再增加 SDK 错误码分级。

### 9.2 系统级 ERROR

系统级 ERROR 包括：

- AGV 任务导航失败、超时、异常取消。
- AGV 通信错误。
- 用户 Stop。
- 失败收尾时收安全姿态失败。
- 空闲回 LM1 失败、非主动取消、超时。
- 码垛区满、码垛配置无效、无法计算下一点、`commitPlaced()` 失败。

处理：

```
当前任务 Failed 或 Canceled
所有 Pending 任务全部取消/清空
系统进入 Error
UI 报警
不接收新任务
必须人工 Reset Error
Reset 后回 Idle
```

AGV 导航类型区分：

- 任务导航：去取料点、倒料点、码垛点。失败/取消/超时均为系统级 ERROR。
- 空闲回站导航：回 LM1。因新任务主动取消属于正常切换，不报警；其他失败为系统级 ERROR。

AGV 到站判定沿用现有严格逻辑：

- 必须先观察到导航进入 Waiting/Running。
- 必须同时满足 `navStatus == Arrived`、`navStation == expectedLm`、`curStation == expectedLm`。
- 单步超时沿用 `120s`。

## 10. UI 设计方向

新增一个全局调度监控区，现有测试面板全部保留且不禁用。现场人员人为保证调度运行中不误点测试按钮，后续版本再做权限和互锁。

调度监控区最小内容：

- 系统状态：未启动、等待缺料、正在送料、回待机点、系统报警。
- 当前任务：工位、业务状态、错误原因。
- 待送料数量。
- 12 个工位“模拟缺料”按钮。
- Start、Stop、Reset Error。
- FIFO 队列表：任务号、工位、状态、入队时间。只显示 Pending + Running。
- 最近完成、最近失败、报警信息。

客户友好文案：

- 工位X已加入送料队列。
- 工位X开始送料。
- 工位X正在取料。
- 工位X正在送料。
- 工位X正在倒料。
- 工位X正在放空箱。
- 工位X送料完成。
- 工位X送料失败：原因。
- 系统报警：原因。

`WorkflowWidget` v1 不改。客户不关心内部流程图，后续可弱化或移除。

## 11. 日志与可追溯

v1 必须增强日志定位能力。每条任务日志带：

```
[T#任务号 S工位][阶段] 内容
```

示例：

```
[T#18 S05][AGV] 前往取料点 LM5
[T#18 S05][VISION] 未识别，搜索下移 20mm
[T#18 S05][SCAN] 首轮扫码失败，旋转补救已关闭，任务失败
[T#18 S05][PALLET] 共享码垛区下一点 offset x=...
```

任务历史不在队列表长期保留，依靠日志追溯。队列表只显示 Pending + Running，防止长时间运行 UI 表格无限增长。

未来任务：

- 增加 `task_queue_snapshot.json` 或 SQLite。
- 保存 Pending/Running 队列快照。
- 程序启动时提示是否恢复上次任务。
- 当前 v1 不实现断电恢复，但 `Task` 保留 `taskId`、`stepIndex`、`createdAt` 等可序列化字段。

## 12. 线程模型

调度不使用 `while` 死循环，不阻塞 UI 线程。

主线程 / Qt 事件循环：

- `LineManager`
- `TaskQueue`
- `TaskExecutor`
- `AgvController`
- `HuayanScheduler`
- `VisionHttpClient`
- UI

扫码工作线程：

- 复用现有 `NScanScheduler::scan()` worker 线程。

推进方式：

```
任务开始
  -> 发 AGV dispatch
  -> 等 monitorUpdated 到站
  -> 调 HuayanScheduler 阶段
  -> 等 stageCompleted / preGripScanRequested / stageError
  -> 发扫码 worker
  -> 等 scan result
  -> 推进下一状态
```

“持续运行”由 `LineManager` 的 `Running` 状态和 `tryStartNext()` 驱动，不是后台阻塞循环。

## 13. 范围外与后续任务

v1 不做：

- 客户系统自动缺料算法接入。
- 客户系统完成结果 HTTP 回传。
- 断电恢复。
- 现有测试面板禁用/权限互锁。
- 全设备 Mock 框架。
- 每工位拆分模块测试 UI。
- 机械臂 SDK 错误码精细分级。
- 码垛 LM 和机械臂函数名的现场 UI 配置。

后续任务：

- 客户系统基于产量、工位消耗比例、安全阈值生成任务。
- 扫码 barcode 与客户标准比对。
- 工位/码垛配置迁移到 JSON、QSettings 或配置 UI。
- 任务队列快照和恢复。
- 调度运行中禁用危险测试按钮。
- 根据现场调试调整视觉搜索下移参数、码垛 offset 方向、码垛区真实 LM。

## 14. 验证场景

队列与系统状态：

1. Idle 下连续点击多个工位，点击 Start 后按 FIFO 执行。
2. Running 且队列为空时，AGV 回 LM1 等待。
3. 回 LM1 途中点击工位，主动取消回站并执行新任务，不报警。
4. Error 状态点击工位，不入队并提示先复位。
5. Stop 后当前任务 Canceled，Pending 全部清空，Reset 后回 Idle。

正常主流程：

1. 工位 1-11 完成取料、扫码、倒料、共享码垛区放空箱、回 LM1 或下一任务。
2. 工位 12 完成取料、扫码、倒料、工位12独立码垛区放空箱、回 LM1 或下一任务。
3. 有连续任务时，任务间不回 LM1，直接去下一任务取料点。

异常流程：

1. AGV 去取料/倒料/码垛点失败，系统 ERROR，队列清空。
2. 视觉无目标搜索 80mm 后仍失败，当前任务 Failed，收姿态后继续下一任务。
3. 视觉服务不可达，当前任务 Failed，收姿态后继续下一任务。
4. 默认配置下扫码空码/失败不得旋转，当前任务直接 Failed 并安全收姿态。
5. 仅在代码常量明确开启且现场完成安全隔离时，首轮扫码失败才旋转 180 度后重试；第二轮仍失败则当前任务 Failed。
6. 收姿态失败，系统 ERROR，队列清空。
7. 码垛区满或 `nextRelativeOffset()` 失败，弹窗+日志，系统 ERROR，队列清空。
8. `commitPlaced()` 失败，系统 ERROR，队列清空。

## 15. 评审关注点

评审时重点确认：

- 12 工位 pickup/unload 数字 LM 表是否正确。
- `PalletArea::LargeBox` 映射为共享区、`SmallBox` 映射为工位12独立区是否持续成立。
- `palletLm = 16/17` 是否仅作为临时模拟值。
- 默认机械臂函数命名是否方便现场替换。
- 视觉搜索默认 `20mm * 4 = 80mm` 是否足够保守。
- 系统级 ERROR 清空队列的策略是否符合现场恢复习惯。

## 16. 源码注释与阅读规范

本任务相关源码必须以中文注释解释业务含义和安全边界，但不得用逐行复述代码的方式制造噪音。

注释覆盖要求：

- 类和核心结构体：说明职责、所有权、线程归属及明确不负责的事项。
- 枚举：逐项说明业务状态、进入条件和退出方向；状态机枚举必须能脱离实现代码阅读。
- 配置与常量：说明单位、现场来源、默认值、修改风险和适用范围。
- 成员变量：说明生命周期、不变量，以及它属于系统状态、单任务状态还是设备快照。
- 函数接口：说明输入、输出、前置条件、副作用、成功/失败信号及调用线程。
- 状态跳转：解释“为什么允许推进”和“为什么必须等待”，尤其覆盖 AGV 到站、扫码暂停、失败收姿态和码垛提交。
- 设备接线：说明信号来源、接收者、同步/异步边界，以及为什么不能与测试操作串线。
- UI 调度区：说明控件只负责展示/输入，业务推进由 `LineManager` 完成。

重点文件阅读顺序：

1. `lineconfig.h`：任务模型、工位配置和显示辅助函数。
2. `taskqueue.h`：FIFO 任务容器。
3. `taskexecutor.h/.cpp`：单任务状态机。
4. `linemanager.h/.cpp`：整线状态机和连续任务调度。
5. `huayanScheduler.h/.cpp`：机械臂动作状态机。
6. `devicemanager.h/.cpp`：设备所有权、线程和信号接线。
7. `mainwindow.h/.cpp`：调度看板输入与展示。

注释维护规则：后续任何行为修改必须先同步本设计和实现计划，并同时更新受影响接口、变量和状态跳转注释。注释不得描述尚未实现的行为；现场临时值必须明确标注，不得伪装成最终配置。

后续新增任务的强制要求：

- 每个新增任务的 spec/plan 必须单列“源码注释要求”和“注释验收清单”，不得只描述功能实现。
- 新增类、结构体、枚举、状态、函数接口、成员变量、常量、宏定义及设备信号槽必须添加详尽中文注释。
- 注释必须说明业务含义、单位/取值、所有权、线程归属、前置条件、副作用、错误边界和状态跳转；不适用的项目可以省略，但不得用无意义模板填充。
- 修改现有行为时，必须同步检查并更新原注释，禁止保留与实现不一致的过期说明。
- 任务验收必须包含“注释与实现一致性检查”；缺少必要注释或文档未同步时，任务不得标记完成、不得申请提交。

文件范围变更确认门禁：

- Bug 修复或新功能开始前，必须在 spec/plan 中列出预计新增和修改的文件范围。
- 若实施过程中发现需要新增文件，或修改原计划之外的任何文件，必须立即停止代码修改。
- 继续实施前必须向用户说明：变更原因、拟新增/修改文件清单、每个文件的职责、对现有模块和测试的影响，以及不扩大范围的替代方案（如有）。
- 只有获得用户明确确认后，才能新增文件或修改原范围之外的文件；不得以“顺手重构”“编译需要”或“更规范”为由自行扩大范围。
- 纯只读排查不受此限制，但排查结果不得自动转化为跨范围代码修改授权。

## 17. 唯一文档与交付门禁

本设计文档与 `docs/superpowers/plans/2026-06-25-12-station-continuous-replenishment.md` 是十二工位连续补料调度目标的唯一 spec/plan 文档。当前整套流程涉及任务队列、AGV、机械臂、视觉、扫码、倒料、码垛、UI 和错误恢复，尚未通过完整现场实测，因此不得将本任务标记为完成。

唯一文档规则：

- 后续所有 Bug 修复、新功能、诊断日志、现场参数调整和架构改进，都必须以实现本设计定义的十二工位连续调度目标为前提。
- 不允许为同一十二工位调度目标创建新的 spec 或 plan；必须持续修订本设计文档和对应实现计划，保证跨模块上下文集中、连续且可追溯。
- 每次代码修改前先更新这两份文档中的需求、文件范围、状态机、接口、风险和验收方法；实现结果与现场结论也必须回填到同一文档。
- 只有完整现场流程通过并获得用户明确确认后，才能把文档状态改为“现场验收通过”。编译成功、单元测试通过或单次局部动作成功均不能替代现场验收。

Git 交付门禁：

- Bug 修复或新功能完成后，必须先执行与风险相匹配的测试、完整编译和差异检查。
- 验证通过后如需提交，必须先向用户展示待提交文件、改动摘要、验证结果和建议 commit message，并获得用户明确确认。
- 未获得用户明确确认时，不得执行 `git commit`，即使代码和测试已经完成。
- 获得确认后只允许执行本地 `git commit`；禁止执行 `git push`、创建远端分支或发起 PR。
- 所有 push 均由用户人工执行，任何任务描述中的“完成”“提交”都不构成 push 授权。

## 18. 现场问题记录

### Bug 1：扫码失败时机械臂旋转存在碰撞风险

现场现象：首轮扫码失败或返回空码后，当前实现无条件驱动工具坐标系 Rz 相对旋转 180 度，可能撞击旁边的人员或物体。

根因：`TaskExecutor::onScanFinished()` 的首轮失败分支直接进入 `RotateForScan` 并调用 `HuayanScheduler::rotateToolRz180()`，缺少安全开关。

确认设计：

- 使用 `TaskExecutor` 内的集中代码布尔常量控制扫码补救旋转，默认 `false`。
- 默认情况下首轮扫码失败或空码直接进入任务失败收尾，绝不下发旋转动作。
- 不新增 UI，不使用 `QSettings`，不修改 `HuayanScheduler` 的底层旋转能力。
- 后续仅在确认现场旋转空间安全后，将该常量改为 `true` 并重新编译。

预计修改：`src/taskexecutor.h`、`src/taskexecutor.cpp`、本设计文档及唯一实施文档；不修改机械臂底层、UI、扫码 SDK 或其他设备模块。

实施结果（2026-07-01）：已增加 `kEnableScanFailureRotationRecovery = false`，首轮扫码失败或空码时会在进入旋转状态及下发旋转命令前直接转入任务失败安全收尾。完整构建和现有策略测试已通过；尚待现场制造扫码失败并确认机械臂无旋转动作。
