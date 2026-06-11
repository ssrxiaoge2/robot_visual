# 界面清理（机械臂 Modbus 移除）+ AGV 调试面板 设计文档

日期：2026-06-11
分支：`feature/ui-sdk-cleanup-agv-panel`（基于 `feature/agv-seer-modbus-dispatch`，依赖其中的 AgvController 仙工寄存器映射）

## 背景

机械臂控制已全面转向华沿 SDK（`HuayanScheduler`，端口 10003），机械臂 Modbus 路径
（`RobotController` + `WorkflowEngine` 五步状态机）废弃。AGV 仍走 Modbus TCP
（`AgvController`，协议层已真机验证）。本次改造：

1. 删除机械臂 Modbus 的界面与底层代码
2. 主流程（工具栏 + 流程图）改由 HuayanScheduler 驱动
3. 新增 AGV 调试面板，含工位→站点 id 映射表

## 1. 删除清单

### 整文件删除（同步更新 CMakeLists.txt）

- `src/robotcontroller.h` / `src/robotcontroller.cpp`
- `src/workflowengine.h` / `src/workflowengine.cpp`

保留：`src/workflowstep.h`、`src/workflowwidget.{h,cpp}`（流程图控件继续使用）。

### MainWindow 删除

- `initRobotPanel()` 整个"机械臂 Modbus 控制"面板（连接指示灯、寄存器读/写行、`m_regView`）
  及成员 `m_indRobotConn/m_editRegAddr/m_spinRegCount/m_btnReadReg/m_editRegValue/m_btnWriteReg/m_regView`
- 配置面板的机器人端口 SpinBox（`m_spinRobotPort`）
- 工具栏"单步"按钮（`m_btnStep`、`onStep`）和仿真间隔 SpinBox（`m_spinDelay`）
- `WorkflowEngine` 相关的所有 include、成员、信号连接

### DeviceManager 删除

- `m_robotCtrl` 成员及全部 `RobotController` 信号连接
- 槽：`debugReadRobotRegisters` / `debugWriteRobotRegister`
- 信号：`robotModbusConnected` / `robotModbusDisconnected` / `robotModbusError` / `debugRegistersRead`
- `Config::robotPort` 字段

### 保留不动

- `VisionHttpClient` 整体不动（`transformToRegisters` 寄存器路径成为死代码，
  本期不清理，在 CLAUDE.md TODO 记录待后续移除）

## 2. 主流程改造（SDK 驱动）

- "开始"按钮 → `HuayanScheduler::startStageOne()`；"停止"按钮 → `HuayanScheduler::stop()`
- 流程图步骤列表换为 5 个 SDK 阶段节点（元数据 `QList<WorkflowStep>` 定义在
  `mainwindow.cpp`，UI 层持有，不进 HuayanScheduler）：

  | 序号 | 节点 | 驱动来源 |
  |------|------|---------|
  | 0 | 视觉定位 | StageStep::MoveToSurvey / WaitForVision |
  | 1 | SDK 取料 | StageStep::MoveToGrab ~ LiftLoad |
  | 2 | 翻转卸料 | StageStep::MoveToUnload ~ ReleaseLoad |
  | 3 | AGV 运输（本期占位） | 本期不自动触发，节点常灰 |
  | 4 | 码垛复位 | StageStep::MoveEmptyBox / ExecuteStackingFunction |

- `HuayanScheduler` 新增信号 `stepChanged(int stepIdx)`：在内部 `StageStep` 推进处
  按上表映射发出（映射表为 scheduler 内静态函数 `stepIndexFor(StageStep)`，
  返回 -1 表示不更新高亮）。现有 `stageStarted/stageCompleted/stageError` 信号不动。
- 周期计数标签保留：MainWindow 收到 `stageCompleted` 且阶段名为 StageOne 时 +1
- 顶部"机器人"指示灯（`m_indRobot`）改为反映华沿 SDK 连接状态
  （连接 `HuayanScheduler::connected/disconnected`），标签文字改"机械臂 SDK"
- AGV 运输节点本期仅作占位显示，自动并入 SDK 调度流程留待下期

## 3. AGV 调试面板（新增）

放在左侧面板原"机械臂 Modbus 控制"的位置。自上而下三块：

### 3.1 派单行

- 工位号 QSpinBox（1-65535）+ 解析结果标签（实时显示 `→ 站点 N`）
- 按钮：出发 / 取消 / 暂停 / 继续

### 3.2 工位→站点映射表

- QTableWidget 两列：工位号 | 站点 id；下方"+ 添加行 / - 删除行"按钮
- 编辑即保存（`cellChanged` → 重建映射 → 写 QSettings）
- 背景：AGV 地图站点 id 因历史删除不连续，上位机统一用逻辑工位号，
  发 Modbus 前查表转换

### 3.3 实时监控区

5 行只读标签，1s 轮询刷新：导航状态、当前/目标站点、定位状态、电池电量、控制权。
导航状态颜色：到达=绿、失败/取消/超时=红、其余中性色（深浅主题各一套，
在 `applyTheme(bool dark)` 同步更新）。

### 3.4 映射逻辑归属

- `DeviceManager` 持有 `QHash<int,int> m_stationMap`，提供
  `int resolveStation(int workstation) const`（**未配置回退 id=工位号**）、
  `void setStationMap(const QHash<int,int>&)`
- QSettings 持久化（构造时加载，setStationMap 时保存），键 `agv/stationMap`，
  值为字符串，格式 `工位:站点id` 逗号分隔（如 `"2:3,5:7"`），解析失败的条目跳过
- `AgvController` 不感知映射，只接收最终站点 id（协议层纯净）；
  将来 SDK 自动调度同样经 `resolveStation()` 走同一张表

### 3.5 AgvController 扩展

- 新增 `pauseNavigation()` / `resumeNavigation()`：FC05 写线圈 [0x]00004 / [0x]00005
- 新增监控轮询：`startMonitor(int intervalMs = 1000)` / `stopMonitor()`，
  内部 QTimer 周期读取：
  - FC04 读 [3x]00007-00013（7 个寄存器：导航站点/定位状态/导航状态/导航类型/
    置信度×2/电池电量，一次请求）
  - FC04 读 [3x]00034（当前所在站点）、[3x]00043（控制权）
  - 汇总后 emit `monitorUpdated(AgvMonitorData)`（struct：navStation, locStatus,
    navStatus, battery, curStation, ctrlSeized；Q_DECLARE_METATYPE 注册）
- 连接成功自动 `startMonitor()`，断开自动 `stopMonitor()`
- 现有 `sendToStation / cancelNavigation / statusRead / arrived /
  readControlOwnership` 复用不动

### 3.6 派单链路

遵守"MainWindow 不含 Modbus 逻辑"原则，`DeviceManager` 新增四个转发槽供面板按钮调用：

- `dispatchAgv(int workstation)`：`resolveStation()` 转换后调
  `AgvController::sendToStation(站点id)`，日志同时打印工位号与站点 id
- `cancelAgvNav()` / `pauseAgvNav()` / `resumeAgvNav()`：直接转发到 AgvController 对应方法

到达判定按站点 id 匹配（现有逻辑不动）。监控数据 `monitorUpdated` 信号由
MainWindow 直接连接 `m_devMgr->agvController()`（与现有指示灯连接方式一致，只读不控制）。

## 4. 配置面板与 DeviceManager 调整

- "机器人 IP" 标签改"机械臂 IP"，端口框删除
- 该行"测试"按钮改为 tcpPing `ip:10003`（华沿 SDK 端口）
- `Config`：删 `robotPort`；`huayanIP = robotIP` 复用关系保持
- `applyConfig()`：仅重连 AGV Modbus + 更新视觉 URL
- `configApplied` 信号简化为 `(robotIP, agvIP)`

## 5. 错误处理与验证

### 错误处理

- AGV 导航失败/取消/超时：监控区状态变红 + 日志输出 + 顶部 AGV 指示灯置故障
  （复用 `errorOccurred` / 监控数据链路）
- SDK 阶段错误：维持现有 `stageError` → 日志 + 指示灯
- 映射表输入校验：工位号/站点 id 必须为 1-65535 正整数，非法输入恢复原值

### 验证清单

1. 全量编译（MinGW Debug）零警告
2. `scripts/mock_agv_server.py` 跑通：出发/取消/暂停/继续四按钮 + 监控区 1s 刷新 +
   映射表（配置 工位2→站点3，派单工位 2 确认 Modbus 写入值为 3）
3. 映射表持久化：编辑后重启程序确认恢复
4. 主题切换深/浅各检查一遍新控件样式
5. SDK 流程图高亮：验证 `stepChanged` 信号连接（真机或手动 emit）

### Git

- 分支 `feature/ui-sdk-cleanup-agv-panel`（基于 feature/agv-seer-modbus-dispatch）
- 更新 `changelog/CHANGELOG.md`，创建 PR，不自行合并

## 范围外（明确不做）

- AGV 自动并入 SDK 调度流程（下期）
- 货叉控制（叉车型 AGV 的举升/到位寄存器）
- VisionHttpClient 死代码清理
- AGV 端口输入框（mock 测试用管理员权限跑 502 端口代替）
