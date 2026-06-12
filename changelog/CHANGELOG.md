# 代码修改记录

> 每次提交或重要改动后，在此文件追加一条记录。
> 格式：日期 · 版本标签 · 改动摘要 + 文件清单。

---

## 2026-06-12 | 整线流程小规模可行性试测（feature/line-flow-pilot）

### 新增
- `LineOrchestrator`：整线流程编排器，单次串起 AGV取料站→机械臂取料→收姿态→AGV倒料站→倒料→收姿态→AGV回站1；纯协调层，AGV 移动调 `dispatchAgv` 经 `monitorUpdated` 判到达，机械臂调 `startStageOne`/`startStow`/`startUnload` 等 `stageCompleted`；任一步出错就地停机（取消 AGV + 停机械臂）
- `HuayanScheduler`：新增 `Stage::Stow`（Func_yun_xing_zhong 收姿态）、`Stage::Unload`（Func_daoliao_1_point → Func_daoliao），`startStow`/`startUnload` 公开方法
- `DeviceManager`：持有 `LineOrchestrator`，接线 `agvDispatchRequested → dispatchAgv`

### 变更
- 工具栏开始/停止改驱动整线流程编排器（原为单独机械臂取料 startStageOne）
- 流程图节点改为整线视角：AGV→取料站 / 机械臂取料 / AGV→倒料站 / 倒料 / 回站待机
- 初始检查：AGV 须在待机站1（真检测 curStation）、机械臂强制收姿态
- `VisionHttpClient` 多目标抓取改为显式优先级：深度最小（离相机最近/堆叠最上层）优先，深度差小于同层容差（`kSameLayerTolMm`=20mm）时视为并排平放，改取离图像中心最近者；原为直接取数组第一个

### 修复（真机试跑后）
- 收姿态/倒料函数名首字母改为大写（`Func_yun_xing_zhong`/`Func_daoliao_1_point`/`Func_daoliao`），与机械臂控制器程序中已示教的函数名一致，修复 `func_yun_xing_zhong` 报错 20601（No such function in the program）
- 阶段间快速衔接（取料→收姿态、倒料→收姿态）调用收姿态函数报错 20018（command prohibited in current robot state, ProgramStopped）：上一阶段完成时 `stop()` 的 `GrpStop` 使机器人进入 ProgramStopped 态，而 `GrpReset` 退出该态是异步的，复位后立即 `RunFunc` 会撞此态。`startStageOne`/`startStow`/`startUnload` 改用 `resetAndProceed`，复位后延时 `kResetSettleMs`(500ms) 再下发首条指令（机器人空闲启动无此问题，故仅延时无副作用）
- 取料闭环 Rz 旋转方向取反（`addMove(5, -rz)`）：眼在手上闭环时按 `+rz` 旋转会使下帧视觉角同向变大、机械臂持续旋转累积至 180°；联机验证方向相反（X 直接、Y 与 Rz 取反）
- 编排器到达判定的工位/站点号空间不一致：派单用工位号经映射表转物理站点号（如工位3→站点5），但 AGV 监控回报的是物理站点号，原先 `m_expectedStation` 存的是工位号，导致到达取料站永不命中（拍照函数不触发），且物理站点号偶然等于工位号时误判到达。新增 `setStationResolver` 注入 `DeviceManager::resolveStation`，到达判定与初检 `curStation` 比较前统一解析为物理站点号
- `LineOrchestrator::onAgvMonitor` 到达判定增加 `curStation == m_expectedStation` 校验：`navStatus`/`navStation` 仅反映导航任务自身状态，人工干预导航后可能残留"已到达"但 AGV 实际未到目标站，导致流程提前推进到下一步

### 已知限制
- 单次单站点试测，无连续循环、无多站点；初检不过不自动归位（人工处理）
- 机械臂姿态用"强制收姿态"替代真实检测

---

## 2026-06-11 | 界面清理与 AGV 调试面板（feature/ui-sdk-cleanup-agv-panel）

### 变更
- 移除机械臂 Modbus 路径：删除 `RobotController`、`WorkflowEngine` 及"机械臂 Modbus 控制"调试面板、机器人端口输入框、工具栏单步/步进延时
- 工具栏开始/停止与右侧流程图改由华沿 SDK（`HuayanScheduler` 新增 `stepChanged` 信号）驱动，流程图节点更新为：视觉定位/SDK 取料/翻转卸料/AGV 运输(占位)/码垛复位
- 顶部"机械臂"指示灯改为反映 SDK 连接状态；配置面板"机器人 IP"改"机械臂 IP"，测试按钮改 ping 10003
- 停止时同时取消 AGV 导航，避免空跑

### 新增
- AGV 调试面板：工位派单（出发/取消/暂停/继续）、工位→站点 id 映射表（QSettings 持久化，未配置回退 id=工位号）、实时监控区（导航/站点/定位/电量/控制权，1s 轮询）
- `AgvController`：`pauseNavigation`/`resumeNavigation`（[0x]00004/00005）、监控轮询 `monitorUpdated(AgvMonitorData)`、在途请求守卫
- `DeviceManager`：`resolveStation`/`setStationMap` 与 AGV 转发槽 `dispatchAgv`/`cancelAgvNav`/`pauseAgvNav`/`resumeAgvNav`

### 已知限制
- AGV 自动并入 SDK 调度流程未实现（流程图 AGV 节点为占位，后续整线流程补齐）
- `VisionHttpClient::transformToRegisters` 寄存器路径成为死代码，待后续清理

---

## 2026-06-11 | AGV 接入仙工真实 Modbus 寄存器映射（feature/agv-seer-modbus-dispatch）

### 变更
- `AgvController`：寄存器映射由占位假设（Holding 1000 / Input 1001）改为仙工官方 Modbus 寄存器表：
  - 下发站点：FC06 写 [4x]00001（AGV 收到后自动清 0 并开始路径导航）
  - 状态轮询：FC04 一次读 [3x]00007-00009（当前导航站点 / 定位状态 / 导航状态）
  - 文档地址位以 00001 为起始，请求时经 `pdu()` 统一减 1 转为 0 基 PDU 地址
- `AgvController`：`AgvStatus`（0-3 四态）替换为 `NavStatus`（0=无 1=等待 2=执行中 3=暂停 4=到达 5=失败 6=取消 7=超时）
- `AgvController::statusRead` 信号携带导航状态 + 当前导航站点 id；`arrived()` 判定改为"状态==4 且站点与本次下发目标一致"，避免上一单残留的"到达"误判
- `WorkflowEngine::onAgvStatusRead`：Failed/Canceled/Timeout 统一走故障分支（停定时器 → agvFaultDetected → stop() 复位机械臂）

### 新增
- `AgvController::cancelNavigation()`：FC05 写 [0x]00006=1 取消导航；`WorkflowEngine::stop()` 在 Step2 行走中停止时自动调用，避免 AGV 空跑
- `AgvController::readControlOwnership()`：FC04 读 [3x]00043 控制权状态，连接成功后自动读取一次；`DeviceManager` 将结果输出到日志（联调自检）
- `scripts/agv_modbus_debug.py`：零依赖联调脚本（selfcheck 地址偏移自检 / status 状态解码 / send 派单轮询 / cancel / pause / resume / confirmloc / watch），已对本地模拟 AGV 验证帧格式
- `scripts/mock_agv_server.py`：本地模拟仙工 AGV（默认 127.0.0.1:1502），模拟派单→执行中→到达状态流转及取消/暂停/继续，供无真机时测试联调脚本和 Qt 上位机

### 已知限制
- AGV 站点 id 为 uint16 数字，地图中的站点须配置数字编号（如 LM1 → 1）
- 连续两单目标站点相同时，"残留到达"防护可能在 AGV 尚未刷新状态的极短窗口内误判（300ms 轮询周期内）

### 真机验证（2026-06-11，AGV: AMB-01 叉车型 @ 192.168.1.100）
- `agv_modbus_debug.py` 真机测试通过：地址减 1 偏移（selfcheck 读 IP 寄存器一致）、状态读取、派单与到达轮询均正常
- Qt 上位机 Step2 流程待真机验证

---

## 2026-06-08 | 视觉引导取料功能（feature/vision-guided-pickup）

### 新增
- `VisionHttpClient`：新增 `rawCoordinatesReady(double x, double y, double z, double rz)` 信号，输出手眼变换后的工具坐标系 mm 值（供华研 SDK 路径使用）
- `VisionHttpClient`：提取 `transformToMm()` 私有方法，`transformToRegisters()` 复用它，消除重复矩阵运算
- `HuayanScheduler`：StageOne 新增三步 `MoveToSurvey → WaitForVision → MoveToGrab`，实现视觉引导抓取流程
- `HuayanScheduler`：新增 `surveyReady()` 信号（通知外部触发视觉推理）、`setGrabOffset()` 槽（接收视觉坐标）、`setSurveyPose()` 方法
- `HuayanScheduler`：`executeMoveJ()` 支持 `ucsName` 参数，`MoveToGrab` 步骤使用 UCS="TCP"
- `DeviceManager`：新增 `HuayanScheduler` 成员，构造时连线 `surveyReady→fetchInference→rawCoordinatesReady→setGrabOffset` 完整信号链路
- `DeviceManager::Config`：新增 `huayanIP`（默认 192.168.10.10）和 `huayanPort`（默认 10003）字段

### 修复
- `VisionHttpClient::parseInferenceReply`：消除重复手眼变换计算（原先 emit 两个信号时矩阵乘法执行两次）
- `VisionHttpClient::checkStatus`：修复 fps 格式化字符串 `%.1f`（printf 语法），改为 Qt 正确用法 `.arg(fps, 0, 'f', 1)`
- `HuayanScheduler::setGrabOffset`：守卫条件改为正向断言 `m_stage != StageOne`，防止未来状态扩展时静默漏过合法调用

### 已知限制
- `WaitForVision` 步骤无超时机制：若视觉服务返回 `noObjectDetected` 或 `errorOccurred`，状态机将永久停留在该步骤（后续 PR 补充超时/错误退出路径）
- `m_surveyPose`、`m_pickupLiftPose` 需联机示教后通过 `setSurveyPose()` 等接口注入实际坐标

---

## 2026-06-05 | 修正奥比 SDK 位置，新增华研 SDK 与接口

### 变更
- 修正 `CMakeLists.txt` 中 Orbbec SDK 库路径
- 新增华研 SDK 头文件 `3rd/HuaYansdk/.../include/HR_Pro.h`
- 新增华研调度接口 `src/huayanScheduler.h` / `src/huayanScheduler.cpp`
- 更新 `.gitignore`（忽略 `docs/`、`.vscode/` 等）


## 2026-05-26 | 补光灯初始状态修复

### 修复

#### `src/devicemanager.h`
- `m_lightOn` 初始值由 `true` 改为 `false`，补光灯默认关闭

#### `src/mainwindow.cpp`
- `initConfigPanel()` 中按钮初始文本由"● 关闭补光灯"改为"○ 开启补光灯"
- 按钮初始样式由橙色（亮灯态）改为灰色（灭灯态）
- 新增 `m_indLight->setStatus(false, "已关闭")` 同步指示灯初始状态

---

## 2026-05-26 | 目录结构整理 — 引入 src/ scripts/ tools/ 分层

### 变更

#### 目录结构
- 所有 C++ 源文件（`.h` / `.cpp` / `.ui`）从根目录移入 `src/`
- Python 调试脚本（`camera_stream.py`、`camera_test.py`）移入 `scripts/`
- Linux GPIO 控制程序（`fill_light`）移入 `tools/`
- 全部使用 `git mv` 操作，完整保留文件 git 历史

#### `CMakeLists.txt`
- `PROJECT_SOURCES` 中所有文件路径加 `src/` 前缀
- 新增 `target_include_directories(wh-robot-visual PRIVATE src/)`，保证 `#include "xxx.h"` 无需修改路径
- 按功能重新排序：入口 → UI → 业务核心 → 设备控制层

---

## 2026-05-26 | 架构修复 — MainWindow 调试面板不再直接访问 RobotController

### 变更

#### `devicemanager.h`
- 新增 `#include <QList>`
- 新增 public slots：`debugReadRobotRegisters(int addr, int count)`、`debugWriteRobotRegister(int addr, quint16 value)`
- 新增 signal：`debugRegistersRead(int startAddr, const QList<quint16> &values)`

#### `devicemanager.cpp`
- 构造函数中新增 `RobotController::registersRead` → `DeviceManager::debugRegistersRead` 转发连接
- 新增 `debugReadRobotRegisters()` / `debugWriteRobotRegister()` 实现；写入操作同时 emit `logMessage`

#### `mainwindow.cpp`
- 移除 `#include "robotcontroller.h"`、`#include "agvcontroller.h"`、`#include "visionclient.h"`（三者均为 MainWindow 不应直接依赖的硬件层头文件）
- 读取按钮 lambda 改为调用 `m_devMgr->debugReadRobotRegisters()`
- 写入按钮 lambda 改为调用 `m_devMgr->debugWriteRobotRegister()`，并移除 lambda 中的 `log()` 调用（日志已在 DeviceManager 内 emit logMessage）
- 构造函数信号连接改为 `m_devMgr, &DeviceManager::debugRegistersRead`，不再直接访问 `robotController()`

---

## 2026-05-25 | 会话 #3 — AgvController + 机器人真实 Modbus 时序

### 背景
前两轮会话完成了项目架构拆分（WorkflowEngine / DeviceManager / ThemeSwitch）和跨平台适配。
本轮目标：把"纯定时器仿真"的工作流替换为真实 Modbus 硬件驱动，并新增 AGV 控制器。

---

### 新建文件

#### `agvcontroller.h`
- 声明 `AgvController : QObject`，镜像 `RobotController` 接口风格
- 枚举 `AgvStatus`（Idle / Moving / Arrived / Fault），映射 AGV Input 寄存器状态字
- Modbus 寄存器常量：
  - `REG_TARGET_STATION = 1000`（Holding，上位机写入目标工位）
  - `REG_STATUS = 1001`（Input，读取 AGV 当前状态）
- 公开接口：`connectToHost()` / `disconnectFromHost()` / `isConnected()`
- 控制接口：`sendToStation(int)` / `readStatusRegister()`
- 信号：`connected()` / `disconnected()` / `errorOccurred(QString)` / `statusRead(AgvStatus)` / `arrived()`

#### `agvcontroller.cpp`
- 实现 QModbusTcpClient 连接管理，5s 自动重连定时器
- `sendToStation()`：FC16 写 Holding 1000
- `readStatusRegister()`：FC04 读 Input 1001，收到 Arrived 时额外 emit `arrived()`
- `onStateChanged()`：连接成功停止重连计时；断线时若 IP 非空则重启重连

---

### 修改文件

#### `robotcontroller.h` / `robotcontroller.cpp`
新增两个 public 方法及一个新信号：

| 新增项 | 说明 |
|--------|------|
| `writeRegisters(int startAddr, QList<quint16>)` | FC16 批量写 Holding（用于一次写坐标寄存器 901-906） |
| `readInputRegisters(int startAddr, int count)` | FC04 读 Input Register（用于轮询机器人状态字 Input 1132） |
| 信号 `inputRegistersRead(int, QList<quint16>)` | 与 `registersRead`（FC03）分开，避免地址冲突 |

---

#### `workflowengine.h`
- 包含 `agvcontroller.h`（需要完整类型以使用 `AgvController::AgvStatus`）
- 新增 Modbus 寄存器 / 命令常量（`constexpr`，改一处全部生效）：

  ```
  REG_CMD_ID=900, REG_COORD_START=901, REG_ROBOT_STATUS=1132
  CMD_GRAB=8, CMD_FLIP=9, CMD_RESET=0
  STATUS_IDLE=0, STATUS_REQ_PHOTO=1, STATUS_GRAB_DONE=2, STATUS_UNLOAD_DONE=3
  ```

- 新增公开接口：
  - `setRobotController(RobotController*)` / `setAgvController(AgvController*)`
  - slot `setCoordinates(QList<quint16>)` — 相机系统回调，注入 6 个坐标寄存器值
  - signal `requestCameraCapture()` — 通知相机系统开始拍照
  - signal `stepLog(QString)` / `engineError(QString)` — 引擎内部日志/错误

- 内部新增私有枚举 `EngineState`（Idle / Step0_Camera / Step1_WriteCoords / Step1_Grabbing / Step2_AgvMoving / Step3_Unloading / Step4_Resetting）
- 内部新增 `PendingCmd` 枚举，保护"先写坐标再写 CmdID"的写入顺序

---

#### `workflowengine.cpp`
**完全重写**为真实硬件状态机，原定时器仿真逻辑保留为"无硬件"回退：

| 步骤 | 真实模式（硬件已连接） | 仿真模式（硬件离线） |
|------|------------------------|----------------------|
| Step 0 相机拍照 | emit `requestCameraCapture()`，等待 `setCoordinates()` 回调 | `simIntervalMs` 后用零坐标自动推进 |
| Step 1 机器人抓取 | 写 Holding901-906 → `writeCompleted` 回调后写 Holding900=8 → 轮询 Input1132 == GRAB_DONE | `simIntervalMs` 后自动推进 |
| Step 2 AGV 行走 | 写 AGV Holding1000=工位号 → 轮询 AGV Input1001 == ARRIVED | `simIntervalMs` 后自动推进 |
| Step 3 机器人放料 | 写 Holding900=9（翻转） → 轮询 Input1132 == UNLOAD_DONE | `simIntervalMs` 后自动推进 |
| Step 4 复位等待 | 写 Holding900=0（复位） → 轮询 Input1132 == IDLE | `simIntervalMs` 后自动推进 |

关键设计：
- `m_pollTimer`（300ms）：定期向硬件发起状态轮询
- `m_stepTimer`（单次，simIntervalMs）：仿真自动推进 / 无硬件超时
- `m_timeoutTimer`（30s）：真实模式步骤保护，超时则 emit `engineError` 并 `stop()`
- `onRobotWriteCompleted()`：通过 `PendingCmd` 枚举实现顺序写保护（坐标先于 CmdID）
- `stop()`：停所有定时器，向机器人写 CmdID=0 复位

---

#### `devicemanager.h` / `devicemanager.cpp`
- `Config` 结构体新增 `agvPort = 502` 字段
- 新增 `agvController()` getter（返回 `AgvController*`）
- `applyConfig()` 扩展：同时重连 AGV Modbus（原来只重连机械臂）
- `testAgv()` 改用 `m_cfg.agvPort`（原来硬编码 502）
- 新增透传信号：`agvModbusConnected()` / `agvModbusDisconnected()` / `agvModbusError(QString)`
- 构造函数：创建 `AgvController`，连接其三个状态信号

---

#### `mainwindow.cpp`
- 新增 `#include "agvcontroller.h"`
- 构造函数注入控制器到引擎（在 `initUI()` 之前）：
  ```cpp
  m_engine->setRobotController(m_devMgr->robotController());
  m_engine->setAgvController(m_devMgr->agvController());
  ```
- 新增引擎日志/错误信号连接：`stepLog` / `engineError` → 转发到 `log()`
- 新增 AGV Modbus 信号连接：`agvModbusConnected/Disconnected/Error` → 更新 `m_indAGV` 指示灯 + 日志
- `robotModbusConnected` 现在同时更新 `m_indRobotConn` 和 `m_indRobot`

---

#### `CMakeLists.txt`
- `PROJECT_SOURCES` 列表新增 `agvcontroller.cpp` / `agvcontroller.h`

---

### 注意事项 / 待确认

| 项目 | 说明 |
|------|------|
| `CMD_FLIP = 9` | 翻转卸料命令字，需对照华沿机器人操作手册确认实际值 |
| AGV 寄存器 1000/1001 | 需对照仙工 AGV Modbus 接口手册验证地址 |
| `setCoordinates()` 调用方 | 相机坐标→寄存器数据通路尚未实现，当前触发相机后会超时用零坐标继续 |
| 单步模式硬件行为 | `singleStep()` 在硬件已连接时会触发真实 Modbus 指令并持续轮询（原为纯 UI 推进） |

---

---

## 2026-05-26 | 会话 #4 — 全文件中文注释增强

### 背景
会话 #3 完成了 AgvController + 真实 Modbus 状态机后，代码逻辑已成型但注释稀少，可读性不足。
本轮目标：为全部 15 个源文件添加详细中文注释，覆盖文件职责、类/结构体、每个方法、关键逻辑行。

---

### 修改文件（注释增强，无逻辑变更）

| 文件 | 主要新增注释内容 |
|------|----------------|
| `main.cpp` | 文件职责注释，`QApplication::exec()` 说明 |
| `workflowstep.h` | 结构体职责、每个字段含义（name/desc/icon/active） |
| `deviceindicator.h` | 类职责 + 用法示例，构造/setStatus 参数说明 |
| `deviceindicator.cpp` | LED 圆心像素位置计算注释，文字绘制逻辑 |
| `workflowwidget.h` | 类职责 + ASCII 布局示意图，所有公开方法参数说明 |
| `workflowwidget.cpp` | 布局参数计算，步骤框激活/非激活色，跨步箭头 vs 回环箭头，工位栏绘制 |
| `camerawindow.h` | 类职责 + 通信协议说明（Python 子进程 + 本地 TCP 帧格式），成员变量说明 |
| `camerawindow.cpp` | 帧解析两阶段（帧头→帧体），findPython 优先级，10 MB 安全截断，子进程生命周期 |
| `themeswitch.cpp` | ASCII 布局示意，slidePos 范围，lerp 插值，图标激活阈值（0.5）说明 |
| `robotcontroller.cpp` | Modbus 功能码对应表（FC01/03/04/05/06/16），顺序写保护说明（坐标先于 CmdID） |
| `agvcontroller.cpp` | 寄存器差异说明（与 RobotController 对比），自动重连逻辑，arrived() 快捷信号用途 |
| `workflowengine.cpp` | 文件级状态转换 ASCII 图，setRobotController/setAgvController 解耦模式，enterStep0-4 逻辑，三定时器（pollTimer/stepTimer/timeoutTimer）职责，PendingCmd 顺序写保护，completeCycle 站号轮转 |
| `devicemanager.cpp` | 职责分区（①生命周期 ②TCP Ping ③补光灯 ④信号透传），tcpPing 阻塞警告，toggleLight 跨平台差异 |
| `mainwindow.cpp` | 架构角色注释，信号连接顺序注释，initUI 布局图，onLightChanged 反直觉状态说明（success=false 时 indicator 为 red 而不是 yellow） |

---

### 注意事项
- 本轮**未修改任何业务逻辑**，所有改动仅为注释。
- `workflowengine.h` / `agvcontroller.h` / `robotcontroller.h` 头文件注释已在会话 #3 中随实现同步添加，本轮不再重复。

---

## 2026-05-26 | 会话 #5 — UI 网格布局 + 手眼矩阵动态加载

### 背景
会话 #4 完成了全文件中文注释增强。本轮目标：
1. 将左侧状态栏设备状态从纵列改为 2×2 网格布局
2. 新增手眼矩阵动态加载功能（向导式对话框）
3. 修复补光灯初始状态与代码不一致问题（m_lightOn=false 但 UI 显示为"关闭补光灯"）

---

### 新建文件

#### `src/handeyedialog.h` / `src/handeyedialog.cpp`
- `HandEyeDialog : QDialog`，三步向导（QStackedWidget）：
  - Page 0 说明页：介绍手眼矩阵用途、文件格式、操作流程
  - Page 1 加载页：文件路径输入 + 浏览按钮 + 解析按钮 + 4×4 可编辑表格（支持手动填写）
  - Page 2 确认页：只读 4×4 矩阵预览 + 警告文字 + "立即应用"按钮
- 解析逻辑：跳过 `#` 注释行，读取 16 个浮点数填入行主序 4×4 矩阵
- 信号：`matrixConfirmed(const float m[16])`

---

### 修改文件

#### `src/devicemanager.h`
- 新增 slot：`applyHandEyeMatrix(const float m[16])`
- 新增 signal：`handEyeMatrixApplied()`

#### `src/devicemanager.cpp`
- 新增 `applyHandEyeMatrix()` 实现：调用 `m_visionClient->setHandEyeMatrix(m)`，emit 日志和 `handEyeMatrixApplied()`

#### `src/mainwindow.h`
- 新增前向声明：`class HandEyeDialog;`
- 新增 slot：`onHandEyeCalib()`
- 新增成员：`QPushButton *m_btnHandEye`

#### `src/mainwindow.cpp`
- 新增 include：`<QGridLayout>`、`"handeyedialog.h"`
- 设备状态 GroupBox 布局从 `QVBoxLayout` 改为 `QGridLayout`（2×2：相机/机器人上行，AGV/补光灯下行）
- 补光灯按钮初始状态修正：文字改为"○ 开启补光灯"，样式改为灰色（与 `m_lightOn=false` 一致），并同步设置 `m_indLight->setStatus(false, "已关闭")`
- `initCameraPanel()` 新增"手眼标定矩阵"按钮（紫色），连接到 `onHandEyeCalib()`
- 新增 `onHandEyeCalib()` 实现：创建 `HandEyeDialog`，连接 `matrixConfirmed` → `DeviceManager::applyHandEyeMatrix`

#### `CMakeLists.txt`
- `PROJECT_SOURCES` 新增 `src/handeyedialog.cpp` / `src/handeyedialog.h`

---

### 注意事项
- `HandEyeDialog` 默认路径指向 `../hand-eye/calibration_output/handeye_matrix.txt`，
  Linux 部署时需确认相对路径与可执行文件位置的关系
- `matrixConfirmed` 信号传递裸指针 `const float m[16]`，Qt 跨线程传递时需注意；
  当前 Dialog 和 DeviceManager 均在主线程，无线程安全问题

---

## 2026-05-28 | 会话 #6 — WorkflowEngine 状态机重构 + 日志持久化

### 背景
会话 #5 完成了手眼矩阵加载功能。本轮目标：
1. 修复 WorkflowEngine Step0/Step1 时序 Bug：旧代码先触发相机再写 CmdID=8，
   实际硬件要求先写 CmdID=8 → 机器人运动到拍照位 → Input1132=1 → 再触发相机
2. 新增日志持久化：每条日志同步写入 `Log/<日期> log.txt`，追加模式，启动即生效

---

### 修改文件

#### `src/workflowengine.h`
- 类文档注释更新 Step 0 描述（写 CmdID=8 → 轮询 Input1132=1 → 触发视觉 → 等待坐标）
- `EngineState` 枚举：移除 `Step0_Camera`，新增 `Step0_SendCmd`、`Step0_WaitVision`
- `PendingCmd` 枚举：移除 `Grab`、`Flip`、`Reset`（后两个从未使用），新增 `WriteCoords`

#### `src/workflowengine.cpp`
- 文件头状态转换图更新，反映新两阶段 Step0
- `enterStep0()`：
  - 旧：直接 `fetchInference()` 或 emit `requestCameraCapture()`
  - 新：先写 `CmdID=8`（硬件模式）或启动 simTimer（仿真模式），等待机器人到位
- `setCoordinates()`：
  - 状态检查从 `Step0_Camera` 改为 `Step0_WaitVision`
  - 推进调用从 `enterStep1()` 改为 `enterStep(1)`，确保发出 `stepActivated(1)` 更新 UI
- `enterStep1()`：
  - `m_pendingCmd` 从 `PendingCmd::Grab` 改为 `PendingCmd::WriteCoords`
  - 文档注释去除"再写 CmdID=8"步骤（已在 Step0 写入）
- `onPollTick()`：新增 `Step0_SendCmd` case，每 300ms 读 Input1132 等待 status=1
- `onStepTimeout()`：
  - 移除 `Step0_Camera` case
  - 新增 `Step0_SendCmd` case（仿真：切换 Step0_WaitVision，调用 fetchInference 或 emit requestCameraCapture）
  - 新增 `Step0_WaitVision` case（视觉超时：使用零坐标继续）
- `onRobotWriteCompleted()`：
  - 检测 `PendingCmd::WriteCoords`（原 `Grab`），写完坐标仅切换到 `Step1_Grabbing`，不再写 CmdID=8
- `onRobotInputRegistersRead()`：
  - 新增 `Step0_SendCmd` case：检测 `status==STATUS_REQ_PHOTO(1)`，停定时器，切换 `Step0_WaitVision`，触发视觉推理

#### `src/mainwindow.h`
- 新增 include：`<QFile>`、`<QTextStream>`
- 新增成员：`QFile *m_logFile = nullptr`、`QTextStream *m_logStream = nullptr`

#### `src/mainwindow.cpp`
- 新增 include：`<QCoreApplication>`、`<QDate>`、`<QDir>`、`<QFile>`、`<QTextStream>`
- 构造函数起始处（`setupUi` 之后，业务层构造之前）：
  - `mkpath("applicationDir/Log")`
  - 以追加模式打开 `<日期> log.txt`，创建 `QTextStream`（UTF-8 编码）
- `log()` 方法：同步将带时间戳的行写入 `m_logStream` 并 `flush()`
- 析构函数：关闭 `m_logFile`

---

### 注意事项
- 日志文件路径：可执行文件所在目录下的 `Log/` 子目录，文件名格式 `2026-05-28 log.txt`
- 日志按"天"切换：程序跨天不重启则继续写当天文件（构造时确定路径）
- 仿真模式 Step0 时序：`Step0_SendCmd` simTimer → `Step0_WaitVision` simTimer → `setCoordinates(零)` 推进，
  全程无硬件写操作，`enterStep0()` 中写 CmdID=8 仅在 `robotReady()` 时执行

---

## 2026-05-28 | 会话 #7 — AGV 故障自动停机 + README 重写

### 新增

#### `src/workflowengine.h`
- 新增 signal `agvFaultDetected()`：AGV Input1001=3 时发出，供 UI 更新指示灯

#### `src/workflowengine.cpp`
- `onAgvStatusRead()` Fault 分支：
  - 显式停止 pollTimer / stepTimer / timeoutTimer
  - emit `agvFaultDetected()` 通知 UI
  - 调用 `stop()`（内部写 CmdID=0 复位机械臂，防止悬臂悬空）

#### `src/mainwindow.cpp`
- 新增 `agvFaultDetected` 信号连接：更新 AGV 指示灯为"故障"状态（区别于正常停止的"待机"）

### 变更

#### `README.md`
- 完整重写：补充硬件配置表、项目结构（src/scripts/tools 分层）、正确的五步时序（Step0 先写 CmdID=8）
- 新增"下一步工作"章节，按优先级详细列出所有 TODO 及条件

---

## 模板（复制此块追加下一次记录）

```
## YYYY-MM-DD | 会话 #N — 标题

### 背景


### 新建文件


### 修改文件


### 注意事项 / 待确认

```
