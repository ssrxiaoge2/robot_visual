# 机械臂收姿态、扫码搜索与 Cleanup 修复设计

## 背景

2026-07-08 现场日志暴露了三个需要拆开处理的问题：

1. 工位 3 倒料完成后，从倒料点回运行安全位不能继续使用全局 `Func_yun_xing_zhong`。现场示教器需要一个新函数，函数内部包含“三号倒料点 -> 过渡点 -> 安全点位”的完整路径。
2. 工位 7、6、3、4 等在扫码第 2 或第 3 轮成功后，夹紧并回安全高度，日志显示“阶段一：取料 已完成”，但任务不继续进入取料后收姿态、倒料或后续流程。
3. 工位 5 在码垛函数缺失后进入 Cleanup 收姿态，调用 `Func_yun_xing_zhong` 时出现 `20561（Command execution failed during program running）`。同类码垛函数缺失在工位 10、9、8、7、6、4、12 上多数 Cleanup 成功，因此不能把它判断为工位 5 专属配置问题。

本修复只处理上述三个 Bug。码垛基准函数未在示教器配置的问题仍按当前任务失败处理，不在本次实现中补齐码垛示教函数，也不改变码垛主流程语义。

## 约束

- 不改动原有正常流程逻辑：首轮扫码成功、取料后收姿态、倒料、码垛失败转任务失败等既有语义保持不变。
- 不使用 Python 作为验证测试；新增或修改的自动化验证使用 C++/CMake 或现有 CTest。
- 一个 Bug 修复对应一个代码 commit；文档 commit 单独提交；不 push。
- 不能硬编码工位 3 特例到状态机分支里，工位差异必须进入 `lineconfig.h` 的工位配置表。
- 问题 3 不能写成“工位 5 专属根因已完全证明”。应按 SDK demo 证据补强 RunFunc/FSM 诊断和等待。

## 问题 1：倒料后回安全位按工位配置

### 现状

`TaskExecutor::enterState(ExecState::StowAfterUnload)` 当前调用 `HuayanScheduler::startStow()`，而 `HuayanScheduler::startStow()` 固定执行成员变量 `m_stowFuncName`，默认值为 `Func_yun_xing_zhong`。这使所有工位的“倒料后收姿态”走同一个示教器函数。

### 根因

工位 3 的倒料点回安全位需要示教器函数内部多走一个过渡点。全局 `Func_yun_xing_zhong` 无法表达“工位 3 倒料点 -> 工位 3 过渡点 -> 安全点位”这条路径。

### 设计

在 `StationTaskConfig` 增加字段：

```cpp
QString stowAfterUnloadFunc;
```

12 个工位都显式配置该字段。默认值为 `Func_yun_xing_zhong`。工位 3 配置为现场新增的示教器函数名。若现场最终函数名与计划中的默认建议不同，只改配置表，不改状态机。

`HuayanScheduler::StationArmFunctions` 增加同名字段，`TaskExecutor::start()` 注入当前工位配置。`HuayanScheduler` 增加“下一次收姿态函数选择”能力，使 `StowAfterUnload` 调用当前工位的 `stowAfterUnloadFunc`，其他收姿态场景继续使用 `Func_yun_xing_zhong`。

### 验收

- 工位 3 倒料完成后日志显示调用工位 3 配置的倒料后收姿态函数。
- 工位 1、2、4-12 倒料完成后仍调用 `Func_yun_xing_zhong`，除非配置表显式修改。
- 取料后收姿态、码垛后收姿态、失败 Cleanup 收姿态不受该字段影响。

## 问题 2：扫码搜索成功后阶段完成不推进

### 现状

扫码失败后，`TaskExecutor` 会通过 `PreGripScanSearchMove` 移动到工具系 Y=20mm 或 Y=-20mm 搜码位置。扫码成功后进入 `PreGripScanSearchReturn`，机械臂回到原夹取位，然后调用 `continueAfterPreGripScan()` 继续夹紧和抬升。

日志显示：

- 工位 7：第 3 轮扫码成功，回原夹取位，夹紧后复用 `Func_capture7` 回安全高度，阶段一完成后不再推进。
- 工位 6、3、4：第 2 轮扫码成功路径有同类现象。

### 根因

`TaskExecutor::onArmStageCompleted()` 的取料完成分支只处理 `ArmPickup`、`PreGripScan`、`RotateForScan`。当扫码搜索成功后，任务状态仍可能是 `PreGripScanSearchReturn`，阶段一完成信号到达时没有匹配分支，因此不会进入 `StowAfterPickup` 或同 LM 直接倒料。

### 设计

把所有“阶段一完成后等价于取料完成”的状态集中成一个 helper，例如：

```cpp
bool isPickupCompletionState(ExecState state);
```

该 helper 返回 true 的状态包括：

- `ArmPickup`
- `PreGripScan`
- `PreGripScanSearchReturn`
- `RotateForScan`

`onArmStageCompleted()` 使用 helper 进入原有取料完成逻辑。不要改变扫码成功后的夹紧、回安全高度、同 LM 跳过 AGV 导航等既有分支。

### 验收

- 第 1 轮扫码成功路径继续保持现有行为。
- 第 2 或第 3 轮扫码成功后，日志中“阶段一：取料 已完成”后必须继续出现“启动收姿态”或“同一 LM，直接进入倒料准备点”。
- 不再需要人工 Stop 才能结束卡住的任务。

## 问题 3：Cleanup 收姿态偶发 20561

### 已证实内容

工位 5 的正常取料、取料后收姿态、倒料、倒料后收姿态均成功。异常发生在码垛阶段 `Func_pallet_shared_base` 缺失后，任务进入 Cleanup，再调用 `Func_yun_xing_zhong` 时返回 `20561（Command execution failed during program running）`。

同样的 `Func_pallet_shared_base` 缺失在其他工位上多数 Cleanup 成功。因此问题不是“码垛函数缺失必然导致 Cleanup 失败”，也不是“工位 5 倒料后收姿态配置错误”。

### SDK demo 证据

`/home/dh/project/robot/HuaYan_sdk/HuayanRobotSDKSample/src/Sample_Script.cpp` 的 `RunFunc()` 示例在 `HRIF_RunFunc()` 后循环读取 `HRIF_ReadCurFSMFromCPS()`，直到 `nCurFSM != 34`。注释说明 `34` 是脚本运行中。

`/home/dh/project/robot/HuaYan_sdk/HuayanRobotSDKSample/src/Sample_XToStandby.cpp` 中状态码包括：

- `25 = Moving`
- `33 = StandBy`
- `34 = ScriptRunning`
- `36 = ScriptHolding`

这说明 RunFunc 类型命令完成判定不能只看机器人运动状态，还应观察脚本 FSM 是否仍处于 `34 ScriptRunning`。

### 设计

在 `HuayanScheduler` 中补充 RunFunc/FSM 诊断和等待：

- 下发 RunFunc 前记录 `HRIF_ReadRobotFlags()` 和 `HRIF_ReadCurFSM()` 的状态，用于现场追踪。
- RunFunc 下发成功后，在轮询完成阶段加入 FSM 判定：若命令类型是 RunFunc 且 FSM 仍为 `34 ScriptRunning`，继续等待，不把该 RunFunc 视为完成。
- Cleanup 收姿态前复用同一套命令门控。若 FSM 显示不可执行状态，等待或执行已有 `GrpReset` 策略，不立即叠加下一条 RunFunc。
- 若 Cleanup 仍失败，日志必须包含失败前最近一次 FSM/flags 快照，便于判断是控制器状态、示教器函数、碰撞/急停还是其他问题。

### 验收

- 码垛函数缺失仍记录为任务级失败，不改成系统级错误。
- Cleanup 收姿态前后日志包含 `moving/pause/error/FSM` 信息。
- 若 RunFunc 后 FSM=34，状态机继续等待；离开 34 后再推进。
- 若 20561 再现，日志能看出失败前 FSM 状态。

## 修改范围

- `src/lineconfig.h`：扩展工位配置，增加倒料后收姿态函数。
- `src/huayanScheduler.h`：扩展 `StationArmFunctions`，增加 RunFunc/FSM 诊断所需的内部字段或 helper 声明。
- `src/huayanScheduler.cpp`：实现按场景选择收姿态函数；实现 RunFunc 后 FSM 等待和日志。
- `src/taskexecutor.h`：增加取料完成状态 helper 声明。
- `src/taskexecutor.cpp`：注入工位倒料后收姿态函数；修复扫码搜索成功后的阶段完成推进。
- `tests/test_station_pickup_config.cpp`：改为覆盖新增配置、扫码搜索状态修复和 RunFunc/FSM 静态回归检查。
- `tests/CMakeLists.txt`：如需新增 C++ 测试目标，在此登记。
- `README.md` 或 `changelog/CHANGELOG.md`：记录现场 Bug 修复摘要。

## 不在本次范围

- 不创建或修改示教器内的真实函数内容。
- 不补齐 `Func_pallet_shared_base`、`Func_pallet_s12_base` 等码垛函数。
- 不改变扫码 Y 轴搜索距离、次数、扫码枪参数。
- 不改变 AGV 调度、客户系统缺料逻辑、视觉闭环算法。
- 不使用 Python 新增验证脚本。

