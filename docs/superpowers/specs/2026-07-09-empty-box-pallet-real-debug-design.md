# 空箱码垛真实单步调试设计说明

## 目标

把空箱码垛从“视觉检测 + UI 模拟”调整为“人工确认 + 配置页真实单步机械臂调试 + 主流程复用同一动作链”。现场先在独立码垛配置界面把单次空箱码垛动作调通，再用同一套配置、点位算法和机械臂动作接口调通主流程。

## 背景

空箱码垛时，物料箱在夹爪上方或夹爪附近，相机当前无法可靠检测码垛区占用状态。因此本版本取消码垛区视觉检测、视觉连续空区计数和视觉自动清零。码垛区是否已经搬空、当前已放数量是否正确，均由人工在配置页确认并修正。

当前代码已有以下基础：

- `PalletScheduler`：保存大箱/小箱两套配置，计算容量、下一相对偏移、仿真点位和已放数量。
- `PalletParamDialog`：提供空箱码垛配置界面，保留人工修正、人工清零和不改缓存的点位预览，并已接入真实单步码垛调试、运行中关闭保护和停止收尾。
- `TaskExecutor`：标准码垛主流程当前为 `PreparePalletPoint -> ArmPalletPlace -> CommitPallet -> task success`；标准动作内已回运行安全位，不再重复进入 `StowAfterPallet`。
- `HuayanScheduler::startPalletPlace(const PalletPose &targetOffset, double releaseZOffsetMm, double robotBaseHeightFromGroundMm)`：已执行统一码垛动作链，并通过专用 `schedulerStopped` 信号给配置页做停止收尾。
- `lineconfig.h`：提供 `PalletAreaTaskConfig`，其中 `palletBaseFunc` 和 `releaseFunc` 是主流程码垛函数来源。

## 范围

本设计包含：

- 去掉配置页所有码垛视觉检测入口和状态展示。
- 保留人工清零和人工修正已放数量。
- 新增或调整配置项，使 `releaseZOffset` 表示“目标层上方释放高度”。
- 在配置页新增“执行一次码垛”真实调试入口，每次只执行一个空箱。
- 复用主流程 `lineconfig.h` 的 `palletBaseFunc` 和 `releaseFunc`。
- 让配置页和主流程共用同一条机械臂标准码垛动作链。
- 增加独立 `tests/` 下的 C++ 自动化测试，验证点位、释放高度、满载、失败不提交等核心行为。
- 同步更新代码注释、函数名、接口说明、变量说明和文档。

本设计不包含：

- 不新增配置页停止按钮，现场停止统一使用已有华研面板停止按钮。
- 不做连续循环码垛；每次码垛都必须人工点击一次。
- 不新增视觉检测替代方案。
- 不引入新的 UI 框架，不拆大规模无关模块。

## 产线标准动作链

配置页单步调试和主流程必须使用同一条标准动作链。一次成功的空箱码垛动作定义为：

1. 机械臂当前处于运行安全位或现场确认的安全位。
2. 调用夹紧函数 `Func_jiajin`，模拟倒料后空箱仍在夹爪上。
3. 调用当前码垛区 `palletBaseFunc`，运动到码垛区上方基准点。
4. 读取 `PalletScheduler::nextRelativeOffset(area)`，得到目标格 XY/Rz 偏移和目标层表面离地高度。
5. `palletBaseFunc` 到位后，华研 SDK 读取当前 TCP 位姿，特别是当前基准点 `Z_basePoint`。
6. 先执行 XY 平面相对移动，到目标列/行上方。
7. 按 `deltaZ = (目标层表面离地高度 + releaseZOffset) - robotBaseHeightFromGround + PALLET_GRIPPER_RELEASE_Z_OFFSET_MM - Z_basePoint` 计算 Z 相对移动量，再执行 Z 下降；该值通常为负数。
8. 调用当前码垛区 `releaseFunc`，松开夹爪，使空箱自由落到目标层。
9. 先按相反方向 Z 抬升，回到码垛区上方安全高度。
10. 调用 `Func_yun_xing_zhong`，回运行安全位。
11. 只有步骤 2-10 全部成功，才发出单次码垛成功信号，并由上层调用 `PalletScheduler::commitPlaced(area)`。

任何一步失败、人工停止、华研面板停止、配置无效、满载，都不能提交已放数量。

## 释放高度和车载基座 Z 语义

机械臂安装在车上，示教器点位 Z 属于机器人基座坐标系，不属于地面坐标系。托盘固定在地面，托盘面高度是地面坐标；因此代码不能直接把车高叠加到示教点 Z 上，而是使用一个可配置的基座离地高度做坐标换算。

`PalletConfig` 中相关字段语义如下：

```text
palletSize.z = 托盘面离地高度
boxSize.z = 空箱高度
releaseZOffset = 目标层上方释放高度，必须 >= 0
robotBaseHeightFromGround = 机器人基座原点离地高度，当前先按实测 850mm 配置，现场可补偿
PALLET_GRIPPER_RELEASE_Z_OFFSET_MM = 夹爪释放参考点相对 TCP/法兰的高度补偿，当前宏定义为 420mm
Z_basePoint = palletBaseFunc 到位后 SDK 读取的当前 TCP Z
```

真实下降量公式：

```text
目标层表面离地高度 = palletSize.z + layerIndex * boxSize.z
释放点地面高度 = 目标层表面离地高度 + releaseZOffset
释放点基座 Z = 释放点地面高度 - robotBaseHeightFromGround
目标 TCP Z = 释放点基座 Z + PALLET_GRIPPER_RELEASE_Z_OFFSET_MM
Z 相对移动量 = 目标 TCP Z - Z_basePoint
```

说明：

- `releaseZOffset` 仍是正数或 0，表示松爪点高于目标层的安全余量。
- 最终 `Z 相对移动量` 允许为负值，现场从码垛上方基准点向下运动通常就是负值。
- 机器人基座离地高度默认值先填 850mm，后续通过配置页人工补偿。
- 夹爪释放参考点补偿按当前实测 420mm 用宏 `PALLET_GRIPPER_RELEASE_Z_OFFSET_MM` 固定；如果现场更换夹爪或 TCP 标定到夹爪释放点，需要同步调整该宏或移除补偿。
- `palletBaseFunc` 的绝对 X/Y/Z/Rx/Ry/Rz 仍由示教器管理，上位机到位后只读取当前 TCP Z 参与下降量计算。
- 如果后续测得机器人基座原点高于车板表面，应直接调整 `robotBaseHeightFromGround`，不要在代码里额外加车高。

## 配置页行为

`PalletParamDialog` 保留大箱码垛和小箱码垛两个页签。每个页签提供：

- 码垛区域尺寸、空箱尺寸、箱间距、边缘预留、最大层数。
- `releaseZOffset`，标签改为“目标层上方释放高度”。
- `robotBaseHeightFromGround`，标签为“机器人基座离地高度”，默认 850mm，现场可补偿。
- 最高安全 Z、方向反转、机械臂初始点位预览。
- 已放数量展示、人工修正、人工清零。
- 容量、下一点、点位表、校验结果。
- “执行一次码垛”按钮。

删除以下视觉相关 UI：

- “视觉检测”分组。
- “检测当前区域”按钮。
- 视觉检测结果标签。
- 连续空计数标签。
- 所有 `VisionHttpClient::fetchPalletOccupancy()` 相关连接和 pending request 状态。

“执行一次码垛”按钮规则：

- 点击前先保存或读取当前页配置并校验。
- 如果当前区域已满，拒绝执行并提示“码垛区已满，请人工搬运并清零”。
- 如果华研机械臂实例未就绪，配置页直接拒绝执行；如果机械臂未连接、正忙或函数未配置，由 `HuayanScheduler::startPalletPlace()` 返回失败信号并提示原因。
- 执行期间按钮禁用，避免并发启动。
- 调试执行期间窗口 `closeEvent` 必须拒绝关闭，避免现场误关窗口后丢失停止/失败提示。
- 成功后提交已放数量并刷新下一点。
- 失败或被华研面板停止后不提交数量，提示人工检查现场状态。
- 配置页不新增停止按钮；停止统一使用华研面板停止按钮，`PalletParamDialog` 通过 `HuayanScheduler::schedulerStopped` 做按钮恢复和状态提示。

## 主流程行为

`TaskExecutor` 的主流程接口语义保持不变：

- `PreparePalletPoint`：校验当前码垛区配置，读取下一相对偏移，不推进状态。
- `ArmPalletPlace`：调用机械臂标准码垛动作链。
- `CommitPallet`：仅在机械臂报告完整成功后调用 `commitPlaced()`。
- 标准主流程成功路径：`PreparePalletPoint -> ArmPalletPlace -> CommitPallet -> task success`。
- `StowAfterPallet`：可作为遗留/显式状态保留在代码中，但标准码垛主流程不再进入该状态；因为标准动作链内部已经调用 `Func_yun_xing_zhong` 回运行安全位。

配置页和主流程必须复用同一个 `PalletScheduler` 实例、同一个 `PalletAreaTaskConfig` 函数来源、同一个机械臂码垛动作接口。

## 满载和人工清零

满载定义为 `placedCount >= totalCapacity(area)`。满载后：

- 配置页“执行一次码垛”拒绝启动。
- 主流程进入错误或等待人工处理，不允许继续下发码垛动作。
- UI 提示“码垛区已满，请人工搬运并清零”。
- 人工搬走所有空箱后，必须点击“清零当前区域”，再允许从第一点重新码垛。

人工数量修正继续保留，用于现场状态和缓存不一致时纠偏。

## 注释和命名要求

代码和文档必须补充中文说明，便于现场维护人员知道每个改动的目的：

- 新增函数要写中文注释说明调用方、成功条件和失败语义。
- 新增接口名要体现真实含义，例如 `startSinglePalletDebug()`、`startPalletPlaceSequence()`、`palletPlaceSequenceCompleted()`。
- 新增变量名要体现单位和语义，例如 `releaseZOffsetMm`、`robotBaseHeightFromGroundMm`、`palletTargetOffset`、`palletReleaseOffset`。
- 修改已有接口时，同步更新头文件注释、调用方注释和计划文档。
- 如果实现过程中新增功能或调整行为，必须同步更新本 spec 和 plan，保证文档与代码一致。

## 测试要求

测试代码必须放在 `robot_visual20260625/robot_visual/tests/`，不能混入 `src/`。CMake 要保持 Qt Creator 中 `tests` 目录独立可见。

至少增加以下 C++ 测试：

- `PalletScheduler` 默认容量和点位计算。
- `releaseZOffset`、`robotBaseHeightFromGround` 和 `PALLET_GRIPPER_RELEASE_Z_OFFSET_MM` 共同参与真实释放点 Z 计算，最终 Z 相对移动量允许为负值。
- 满载时不能继续计算或执行。
- 人工清零后重新从第一点开始。
- 机械臂码垛动作规划顺序为：夹紧、基准点、XY、Z下降、松爪、Z抬升、回安全位。

建议新增纯规划 helper 测试动作顺序，避免单元测试直接依赖真实华研 SDK。

## 现场待分析问题

2026-07-09 现场实测发现：空垛调试时，下面两层肉眼观察下降高度基本合理，但执行到第 5 层附近时，机械臂读取/运动到的 Z 值异常偏高，接近或达到机械臂奇异点并导致动作挂起。该现象说明当前 Z 计算模型仍可能存在未覆盖因素，暂不继续盲目修复代码，需次日重新头脑风暴确认。

待重点分析：

- `placedCount`、层号、行列计算是否在连续单步执行中与现场层数一致。
- `palletSize.z` 当前作为“托盘面离地高度”是否和现场配置值一致。
- `releaseZOffset` 是否仍偏大，导致高层释放点地面高度过高。
- `PALLET_GRIPPER_RELEASE_Z_OFFSET_MM=420.0` 是否应理解为 TCP/法兰到夹爪释放参考点的正向补偿，方向是否正确。
- `HRIF_ReadActTcpPos()` 读取的 TCP Z 是否对应当前使用的 TCP；若示教器 TCP 与夹爪实际释放参考点不一致，宏补偿可能会重复或反向。
- `palletBaseFunc` 到位后的基准点 Z 是否每层都应固定；若执行中存在额外抬升/下降动作，应从华研日志确认每一步实际 Z。
- 是否需要把每次码垛的 `层/行/列、目标层表面离地高度、释放地面高度、当前 TCP Z、计算 deltaZ、动作后 TCP Z` 写入更完整的调试日志。

明日讨论前，现场应保留第 1 层到第 5 层每次执行时的码垛日志，尤其是：

```text
[码垛] 基准TCP Z=...，释放地面Z=...，机器人基座离地=...，夹爪释放补偿=...，本次Z相对移动=...
```

## 构建和验证命令

配置命令：

```bash
/opt/Qt/qt6.8/6.8.3/gcc_64/bin/qt-cmake -S robot_visual20260625/robot_visual -B robot_visual20260625/robot_visual/build -DQt6_DIR=/opt/Qt/qt6.8/6.8.3/gcc_64/lib/cmake/Qt6 -DBUILD_TESTING=ON
```

构建命令：

```bash
cmake --build robot_visual20260625/robot_visual/build -j
```

测试命令：

```bash
ctest --test-dir robot_visual20260625/robot_visual/build --output-on-failure
```

## 手动验证方法

1. 打开空箱码垛配置页。
2. 选择大箱或小箱码垛区。
3. 校验配置，确认容量、下一点和点位表符合现场预期。
4. 人工确认码垛区为空，点击“清零当前区域”。
5. 设置“目标层上方释放高度”，先使用保守高度。
6. 人工确认夹爪和现场安全，点击“执行一次码垛”。
7. 观察动作顺序：夹紧、基准点、XY 到位、Z 下降、松爪、Z 抬升、回 `Func_yun_xing_zhong`。
8. 确认动作成功后，已放数量加 1，下一点更新。
9. 再执行 2-3 次，确认列、行、层变化正确。
10. 执行过程中使用华研面板停止，确认已放数量不增加。
11. 手动把已放数量修正到满载，确认配置页拒绝继续执行并提示人工搬运清零。
12. 配置页单步稳定后，再启动主流程，确认 `PreparePalletPoint -> ArmPalletPlace -> CommitPallet` 使用同一套配置和动作。
