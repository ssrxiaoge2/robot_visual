# 整线流程小规模可行性试测 设计文档

日期：2026-06-12
分支：`feature/line-flow-pilot`（基于 master，已含 AGV 调试面板 + 仙工 Modbus + Rz 闭环）

## 背景

AGV 调试面板与机械臂 SDK 调度（`HuayanScheduler`）目前各自独立，无组件把"AGV 行走 → 到位 → 机械臂动作 → 完成 → AGV 再走"编排成一条线。本次新增整线编排器，串起一条 4 站点的小规模可行性流程，在真机上跑通取料→运输→倒料→返回的完整闭环。

站点语义（试测阶段已录入 4 个）：
- 站点 1 = 待机位置
- 站点 2 = 路线航点（无动作，AGV 自动途经）
- 站点 3 = 第一个取料位置
- 站点 4 = 第一个倒料位置

## 流程（单次，跑完停站 1，手动重跑）

```
0. 初始检查      AGV 当前站==1（真检测）+ 调 func_yun_xing_zhong 强制机械臂运行姿态
1. AGV→取料站(3)  站1 直发站3，AGV 自动途经站2，等到达
2. 机械臂取料     startStageOne（到 Func_capture 取料位 → 视觉闭环夹取 → 抬升），现有逻辑不动
3. 收姿态        func_yun_xing_zhong，机械臂收到运行姿态
4. AGV→倒料站(4)  等到达
5. 倒料          到 func_daoliao_1_point 点位 → 跑 func_daoliao 倒料
6. 收姿态        func_yun_xing_zhong
7. AGV→回站1     等到达待机
→ 完成，停在站 1，按钮恢复，手动重跑
```

任一步出错 → 就地停机报警（见第 5 节）。

## 1. 组件：LineOrchestrator（新增）

新文件 `src/lineorchestrator.{h,cpp}`，类 `LineOrchestrator : QObject`。职责：按上述时序协调 AGV 与机械臂，自身不调用任何 Modbus/SDK 原语，只调高层方法并监听完成信号。

- 由 `DeviceManager` 持有并注入 `AgvController*` + `HuayanScheduler*`（与"DeviceManager 持有所有控制器"模式一致）
- 内部状态机枚举 `LineState`：`Idle / InitCheck / AgvToPickup / ArmPicking / StowAfterPick / AgvToUnload / ArmUnloading / StowAfterUnload / AgvReturnHome`
- 工位号成员（试测默认常量，留 setter）：`m_homeStation = 1`、`m_pickupStation = 3`、`m_unloadStation = 4`
- 缓存最近一次 `AgvController::monitorUpdated` 的 `AgvMonitorData`，供初始检查读 `curStation`

### 公开接口

```cpp
void start();   // 工具栏"开始"触发：从 InitCheck 进入流程
void stop();    // 工具栏"停止"触发：中止（取消 AGV + 停机械臂），不报错
```

### 信号

```cpp
void lineStarted();
void lineFinished();                    // 一轮成功完成（MainWindow 据此 ++循环计数）
void lineStopped();                     // 手动停止（停机但不计数、不报错）
void lineError(const QString &reason);  // 中止并报因
void stepChanged(int stepIdx);          // 0-4，驱动流程图（见第 4 节）
void lineLog(const QString &msg);       // 转发到日志区
```

### 推进机制

- AGV 移动步：调 `m_devMgr->dispatchAgv(工位号)`（内部经映射表解析为站点 id），监听 `AgvController::arrived` 推进；监听 `monitorUpdated`，导航状态 ∈ {5 失败, 6 取消, 7 超时} 时中止
- 机械臂动作步：调 `HuayanScheduler` 的 `startStageOne()`/`startStow()`/`startUnload()`，监听 `stageCompleted` 推进、`stageError` 中止
- 完成判定不靠字符串匹配：编排器任意时刻只有一个子动作在飞，用**自身 `LineState`** 判断刚完成的是哪一步（规避前期踩过的 stageName 前缀脆弱问题）

## 2. HuayanScheduler 新增方法

机械臂示教函数统一经 `HRIF_RunFunc` 调用（与现有 `Func_capture`/`Func_jiajin` 同机制），复用现有 `executeRunFunc` + `startWaitForIdle` + `onPollTick` 轮询基础设施。

### 新增成员

```cpp
QString m_stowFuncName        = QStringLiteral("func_yun_xing_zhong"); // 收运行姿态
QString m_unloadPointFuncName = QStringLiteral("func_daoliao_1_point");// 倒料点位
QString m_unloadFuncName      = QStringLiteral("func_daoliao");        // 倒料动作
```

### 新增阶段与公开方法

- `Stage::Stow`：单步 `StageStep::StowArm` → 跑 `m_stowFuncName` → idle 后 `stageCompleted`。公开 `void startStow();`
- `Stage::Unload`：两步 `StageStep::MoveToUnloadPoint`（跑 `m_unloadPointFuncName`）→ `StageStep::RunUnloadFunc`（跑 `m_unloadFuncName`）→ idle 后 `stageCompleted`。公开 `void startUnload();`

接入点：`proceedStage` / `advanceStep` / `executeCurrentStep` 各加对应 `case`；`startStow`/`startUnload` 结构对照 `startStageOne`（先 `ensureConnected` + `HRIF_GrpReset`，再设阶段、发 `stageStarted`、`proceedStage`）。

`startStageOne()` 与现有取料逻辑**不动**。旧 `Stage::StageTwo`（翻转卸料→开爪→搬空箱）保留但本流程不用。

## 3. 接线（DeviceManager + MainWindow）

- `DeviceManager`：新增 `LineOrchestrator *m_lineOrch`，构造时 `new` 并注入 `m_agvCtrl` + `m_huayanScheduler`，提供 `lineOrchestrator()` getter；转发 `lineLog`/`lineError` 到 `logMessage`
- `MainWindow`：
  - 工具栏"开始" → `m_devMgr->lineOrchestrator()->start()`（替换原 `huayanScheduler()->startStageOne()`）
  - 工具栏"停止" → `m_devMgr->lineOrchestrator()->stop()`（内部已含取消 AGV + 停机械臂）
  - 连 `lineStarted`（禁开始/启停止）、`lineFinished`/`lineStopped`/`lineError`（启开始/禁停止）管理按钮 enable；仅 `lineFinished` 时 `++m_cycleCount`
  - 连 `LineOrchestrator::stepChanged` → 流程图（替换原 `HuayanScheduler::stepChanged` 对流程图的驱动；机械臂细分步仅进日志）
  - 华沿 SDK 调试面板的"启动取料（阶段一）"等按钮保留，单独调试机械臂动作不受影响

## 4. 流程图节点（WorkflowWidget）

`kDefaultSteps` 改为整线 5 节点视角，由 `LineOrchestrator` 作为流程图唯一驱动：

| 节点 | 名称 | 描述 | 覆盖编排步 |
|------|------|------|-----------|
| 0 | AGV→取料站 | 仙工 Modbus | 步 1 |
| 1 | 机械臂取料 | 视觉夹取 | 步 2 |
| 2 | AGV→倒料站 | 收姿态+运输 | 步 3+4 |
| 3 | 倒料 | 华沿 SDK | 步 5 |
| 4 | 回站待机 | 收姿态+返回 | 步 6+7 |

`LineOrchestrator` 在进入每个阶段时 emit 对应 `stepChanged(0..4)`（收姿态折进相邻 AGV 移动节点）。`HuayanScheduler::stepIndexFor` 不再驱动流程图（其 `stepChanged` 连接从 MainWindow 移除或忽略）。

## 5. 错误处理与测试

### 错误处理

- 私有停机动作 `haltDevices()`：`m_agvCtrl->cancelNavigation()` + `m_huayanScheduler->stop()` + `m_state=Idle`
- `abort(reason)`：`haltDevices()` + emit `lineError(reason)`。触发源：初始检查 AGV 不在站 1、AGV 导航状态 ∈ {5,6,7}、`AgvController::errorOccurred`（移动步内）、`HuayanScheduler::stageError`
- `stop()`（手动）：`haltDevices()` + emit `lineStopped`（不报错、不计数）
- 单步超时：AGV 移动步设上层超时保护（如 120s 未 `arrived` 则 `abort`）；机械臂动作的超时由 `HuayanScheduler` 现有 `onStepTimeout` 负责（发 `stageError`）

### 测试

1. 全量编译（MinGW Debug）零警告
2. 离线接线验证：机械臂离线时点"开始"→ 看初始检查/派单/超时日志链路是否正确串起（不触达真机动作）
3. **真机验收（主）**：在 4 站点现场跑一轮整线，观察 0-7 步顺序、流程图节点高亮、日志；并测一次中途"停止"与一次人为故障（如取消 AGV 导航）触发就地停机
4. mock AGV 可验证 AGV 移动与到达推进部分，但机械臂动作需真机，故整线只能真机完整验收

### Git

- 分支 `feature/line-flow-pilot`（基于 master）
- 更新 `changelog/CHANGELOG.md`，创建 PR，不自行合并

## 范围外（明确不做）

- 连续循环 / 多站点扩展（本次单次单站点试测）
- 自动归位（初检不过时不自动移动 AGV，报错由人工处理）
- 机械臂姿态的真实检测（用"强制收姿态"替代）
- 旧 `Stage::StageTwo` 清理、`VisionHttpClient` 寄存器死代码清理
