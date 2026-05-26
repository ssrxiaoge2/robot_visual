# 代码修改记录

> 每次提交或重要改动后，在此文件追加一条记录。
> 格式：日期 · 版本标签 · 改动摘要 + 文件清单。

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

## 模板（复制此块追加下一次记录）

```
## YYYY-MM-DD | 会话 #N — 标题

### 背景


### 新建文件


### 修改文件


### 注意事项 / 待确认

```
