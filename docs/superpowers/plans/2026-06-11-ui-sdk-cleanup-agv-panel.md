# 界面清理 + AGV 调试面板 实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 删除机械臂 Modbus（界面+底层），主流程改由华沿 SDK 驱动流程图，新增 AGV 调试面板（派单 + 工位→站点映射表 + 实时监控）。

**架构：** MainWindow 纯 UI 层不变；HuayanScheduler 新增 `stepChanged` 信号驱动 WorkflowWidget；AgvController 新增监控轮询与暂停/继续；DeviceManager 持有工位→站点映射（QSettings 持久化）并提供 AGV 转发槽。规格见 `docs/superpowers/specs/2026-06-11-ui-sdk-cleanup-agv-panel-design.md`。

**技术栈：** Qt 6.8 (Widgets/Network/SerialBus)、CMake、MinGW 13.1。

**测试说明：** 仓库无单元测试基础设施（纯 Qt GUI 工程），本计划以"每任务编译通过 + 末尾 mock AGV 功能验证清单"作为验证手段。编译命令（已验证可用）：

```bash
cd /d/study/qtpro/robot-deploy/wh-robot-visual/build/Desktop_Qt_6_8_3_MinGW_64_bit-Debug && /d/study/qt/Tools/mingw1310_64/bin/mingw32-make.exe wh-robot-visual -j8
```

预期：`[100%] Built target wh-robot-visual`，无 error。（CMakeLists 变更后 make 会自动重跑 cmake。）

**工作分支：** `feature/ui-sdk-cleanup-agv-panel`（已存在，含规格文档两个 commit）。

**与规格的一处偏差（已确认合理）：** 规格 §2 写"步骤元数据定义在 mainwindow.cpp"。实际 `WorkflowWidget` 已自有 `kDefaultSteps`（workflowwidget.cpp:17），为避免两份定义（DRY），改为：更新 widget 内的 `kDefaultSteps` 并暴露 `steps()` 只读访问器，MainWindow 经访问器取步骤名。

---

## 文件结构

| 文件 | 操作 | 职责变化 |
|------|------|---------|
| `src/agvcontroller.{h,cpp}` | 修改 | +暂停/继续导航、+监控轮询（AgvMonitorData/monitorUpdated） |
| `src/huayanScheduler.{h,cpp}` | 修改 | +stepChanged(int) 信号 + StageStep→流程图索引映射 |
| `src/workflowwidget.{h,cpp}` | 修改 | kDefaultSteps 换为 SDK 阶段；+steps() 访问器 |
| `src/devicemanager.{h,cpp}` | 修改 | +站点映射(QSettings) +AGV 转发槽；−RobotController 全部 −Config::robotPort |
| `src/mainwindow.{h,cpp}` | 修改 | −WorkflowEngine/Modbus 面板/单步/延时；工具栏接 SDK；+initAgvPanel |
| `src/robotcontroller.{h,cpp}` | 删除 | — |
| `src/workflowengine.{h,cpp}` | 删除 | — |
| `CMakeLists.txt` | 修改 | 移除已删文件 |
| `changelog/CHANGELOG.md` | 修改 | 追加条目 |

---

### 任务 1：AgvController 扩展（暂停/继续 + 监控轮询）

**文件：**
- 修改：`src/agvcontroller.h`
- 修改：`src/agvcontroller.cpp`

- [ ] **步骤 1.1：agvcontroller.h 添加监控数据结构与新接口**

在 `#include <QTimer>` 之后、`class AgvController` 之前插入：

```cpp
/// AGV 实时监控快照（监控轮询每周期发布一次）
struct AgvMonitorData {
    int     navStation = 0;     ///< [3x]00007 当前导航站点
    quint16 locStatus  = 0;     ///< [3x]00008 定位状态
    quint16 navStatus  = 0;     ///< [3x]00009 导航状态
    quint16 battery    = 0;     ///< [3x]00013 电池电量 0-100
    int     curStation = 0;     ///< [3x]00034 当前所在站点（严格判定）
    bool    ctrlSeized = false; ///< [3x]00043 控制权被外部抢占
};
Q_DECLARE_METATYPE(AgvMonitorData)
```

寄存器常量区追加两行（在 `REG_CTRL_SEIZED` 之后）：

```cpp
    static constexpr int REG_BATTERY        = 13; ///< [3x] 电池电量
    static constexpr int REG_CUR_STATION    = 34; ///< [3x] 当前所在站点
    static constexpr int COIL_PAUSE_NAV     = 4;  ///< [0x] 暂停导航
    static constexpr int COIL_RESUME_NAV    = 5;  ///< [0x] 继续导航
```

public 方法区（`cancelNavigation()` 声明之后）追加：

```cpp
    /// 写 [0x]00004=1 暂停当前导航
    void pauseNavigation();
    /// 写 [0x]00005=1 继续导航
    void resumeNavigation();
    /// 启动监控轮询（连接成功后自动调用）
    void startMonitor(int intervalMs = 1000);
    void stopMonitor();
```

signals 区追加：

```cpp
    /// 监控轮询完成，发布最新快照
    void monitorUpdated(const AgvMonitorData &data);
```

private 区追加：

```cpp
    void pollMonitor();

    QTimer        *m_monitorTimer = nullptr;
    AgvMonitorData m_monitorData;
```

- [ ] **步骤 1.2：agvcontroller.cpp 实现**

构造函数末尾（`m_reconnectTimer` 初始化之后）追加：

```cpp
    m_monitorTimer = new QTimer(this);
    m_monitorTimer->setInterval(1000);
    connect(m_monitorTimer, &QTimer::timeout, this, &AgvController::pollMonitor);
```

`cancelNavigation()` 之后新增三个方法（pause/resume 与 cancel 结构相同，仅地址与文案不同）：

```cpp
void AgvController::pauseNavigation()
{
    if (!isConnected()) return;
    QModbusDataUnit unit(QModbusDataUnit::Coils, pdu(COIL_PAUSE_NAV), 1);
    unit.setValue(0, 1);
    auto *reply = m_client->sendWriteRequest(unit, 1);
    if (!reply) return;
    connect(reply, &QModbusReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QModbusDevice::NoError)
            emit errorOccurred(QString("AGV 暂停导航失败: %1").arg(reply->errorString()));
    });
}

void AgvController::resumeNavigation()
{
    if (!isConnected()) return;
    QModbusDataUnit unit(QModbusDataUnit::Coils, pdu(COIL_RESUME_NAV), 1);
    unit.setValue(0, 1);
    auto *reply = m_client->sendWriteRequest(unit, 1);
    if (!reply) return;
    connect(reply, &QModbusReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QModbusDevice::NoError)
            emit errorOccurred(QString("AGV 继续导航失败: %1").arg(reply->errorString()));
    });
}

void AgvController::startMonitor(int intervalMs)
{
    m_monitorTimer->setInterval(intervalMs);
    m_monitorTimer->start();
}

void AgvController::stopMonitor()
{
    m_monitorTimer->stop();
}

/**
 * @brief 监控轮询：两次 FC04 读取后发布快照
 *
 * 第一读 [3x]00007-00013（7 个寄存器，覆盖导航站点/定位/导航状态/电量），
 * 第二读 [3x]00034-00043（10 个寄存器，首尾分别是当前站点和控制权）。
 * 任一读失败则跳过本轮（下轮重试），不刷错误日志避免轮询噪音。
 */
void AgvController::pollMonitor()
{
    if (!isConnected()) return;

    auto *r1 = m_client->sendReadRequest(
        QModbusDataUnit(QModbusDataUnit::InputRegisters, pdu(REG_NAV_STATION), 7), 1);
    if (!r1) return;
    connect(r1, &QModbusReply::finished, this, [this, r1]() {
        r1->deleteLater();
        if (r1->error() != QModbusDevice::NoError) return;
        const QModbusDataUnit u = r1->result();
        m_monitorData.navStation = static_cast<qint16>(u.value(0));
        m_monitorData.locStatus  = u.value(1);
        m_monitorData.navStatus  = u.value(2);
        m_monitorData.battery    = u.value(6);

        auto *r2 = m_client->sendReadRequest(
            QModbusDataUnit(QModbusDataUnit::InputRegisters, pdu(REG_CUR_STATION), 10), 1);
        if (!r2) return;
        connect(r2, &QModbusReply::finished, this, [this, r2]() {
            r2->deleteLater();
            if (r2->error() != QModbusDevice::NoError) return;
            m_monitorData.curStation = static_cast<qint16>(r2->result().value(0));
            m_monitorData.ctrlSeized = (r2->result().value(9) == 1);
            emit monitorUpdated(m_monitorData);
        });
    });
}
```

`onStateChanged` 中接入自动启停：`ConnectedState` 分支在 `readControlOwnership();` 之后加 `startMonitor();`；`UnconnectedState` 分支在 `emit disconnected();` 之前加 `stopMonitor();`。

- [ ] **步骤 1.3：编译验证**

运行顶部编译命令。预期：Built target，无 error。

- [ ] **步骤 1.4：Commit**

```bash
git add src/agvcontroller.h src/agvcontroller.cpp
git commit -m "feat: AgvController 新增暂停/继续导航与监控轮询"
```

---

### 任务 2：HuayanScheduler 新增 stepChanged 信号

**文件：**
- 修改：`src/huayanScheduler.h`
- 修改：`src/huayanScheduler.cpp`

- [ ] **步骤 2.1：huayanScheduler.h 声明**

signals 区（`surveyReady();` 之后）追加：

```cpp
    /// 流程图步骤索引变化（0-4，对应 WorkflowWidget 节点），供 UI 高亮
    void stepChanged(int stepIdx);
```

private 区（`stageName` 声明附近）追加：

```cpp
    static int stepIndexFor(StageStep step);
```

- [ ] **步骤 2.2：huayanScheduler.cpp 实现映射并在执行入口发射**

在 `executeCurrentStep()` 定义之前新增：

```cpp
/**
 * @brief StageStep → 流程图节点索引（WorkflowWidget kDefaultSteps）
 *
 * 0=视觉定位 1=SDK取料 2=翻转卸料 3=AGV运输(本期不发射) 4=码垛复位
 */
int HuayanScheduler::stepIndexFor(StageStep step)
{
    switch (step) {
    case StageStep::MoveToSurvey:
    case StageStep::WaitForVision:           return 0;
    case StageStep::MoveToGrab:
    case StageStep::DescendZ:
    case StageStep::CloseGripper:
    case StageStep::LiftLoad:                return 1;
    case StageStep::MoveToUnload:
    case StageStep::FlipUnload:
    case StageStep::ReleaseLoad:             return 2;
    case StageStep::MoveEmptyBox:
    case StageStep::ExecuteStackingFunction: return 4;
    default:                                 return -1;
    }
}
```

`executeCurrentStep()`（huayanScheduler.cpp:414）函数体最顶部插入：

```cpp
    const int stepIdx = stepIndexFor(m_stageStep);
    if (stepIdx >= 0)
        emit stepChanged(stepIdx);
```

- [ ] **步骤 2.3：编译验证**（同任务 1）

- [ ] **步骤 2.4：Commit**

```bash
git add src/huayanScheduler.h src/huayanScheduler.cpp
git commit -m "feat: HuayanScheduler 新增 stepChanged 信号驱动流程图高亮"
```

---

### 任务 3：WorkflowWidget 步骤元数据更新

**文件：**
- 修改：`src/workflowwidget.h`
- 修改：`src/workflowwidget.cpp:15-23`

- [ ] **步骤 3.1：替换 kDefaultSteps（workflowwidget.cpp:17）**

```cpp
// ── 默认步骤定义（SDK 调度阶段，索引与 HuayanScheduler::stepIndexFor 对应）──
// 此处仅用于 UI 展示，不含业务逻辑
static const QList<WorkflowStep> kDefaultSteps = {
    {QStringLiteral("视觉定位"),  QStringLiteral("Orbbec+SAM2"), QColor(65,  130, 220)},
    {QStringLiteral("SDK 取料"),  QStringLiteral("华沿 SDK"),    QColor(220, 160,  50)},
    {QStringLiteral("翻转卸料"),  QStringLiteral("华沿 SDK"),    QColor(180, 110, 210)},
    {QStringLiteral("AGV 运输"),  QStringLiteral("仙工 Modbus"), QColor(60,  180, 120)},
    {QStringLiteral("码垛复位"),  QStringLiteral("下一轮"),      QColor(130, 130, 130)},
};
```

- [ ] **步骤 3.2：workflowwidget.h 添加只读访问器**（`setStationMode` 声明之后）

```cpp
    /// 步骤元数据（MainWindow 用于读取步骤名显示在工具栏标签）
    const QList<WorkflowStep> &steps() const { return m_steps; }
```

- [ ] **步骤 3.3：编译验证 + Commit**

```bash
git add src/workflowwidget.h src/workflowwidget.cpp
git commit -m "feat: 流程图步骤元数据改为 SDK 调度阶段"
```

---

### 任务 4：DeviceManager 站点映射 + AGV 转发槽

**文件：**
- 修改：`src/devicemanager.h`
- 修改：`src/devicemanager.cpp`

- [ ] **步骤 4.1：devicemanager.h 声明**

头部 include 区加 `#include <QHash>`。public 区（`setConfig` 之后）追加：

```cpp
    /// 工位号 → AGV 站点 id（未配置回退 id=工位号）
    int resolveStation(int workstation) const;
    QHash<int, int> stationMap() const { return m_stationMap; }
    void setStationMap(const QHash<int, int> &map);
```

public slots 区追加：

```cpp
    void dispatchAgv(int workstation);
    void cancelAgvNav();
    void pauseAgvNav();
    void resumeAgvNav();
```

private 区追加：

```cpp
    void loadStationMap();
    void saveStationMap() const;
    QHash<int, int> m_stationMap;
```

- [ ] **步骤 4.2：devicemanager.cpp 实现**

include 区加 `#include <QSettings>`。文件顶部（include 之后）加常量：

```cpp
// QSettings 组织/应用名与映射键（值格式 "工位:站点,工位:站点"，如 "2:3,5:7"）
static const char *kSettingsOrg  = "wh-robot";
static const char *kSettingsApp  = "robot-visual";
static const char *kStationMapKey = "agv/stationMap";
```

构造函数末尾加 `loadStationMap();`。文件末尾新增：

```cpp
void DeviceManager::loadStationMap()
{
    m_stationMap.clear();
    QSettings settings(kSettingsOrg, kSettingsApp);
    const QString raw = settings.value(kStationMapKey).toString();
    for (const QString &entry : raw.split(',', Qt::SkipEmptyParts)) {
        const QStringList kv = entry.split(':');
        if (kv.size() != 2) continue;
        bool okW = false, okS = false;
        const int w = kv[0].toInt(&okW);
        const int s = kv[1].toInt(&okS);
        if (okW && okS && w > 0 && s > 0 && w <= 65535 && s <= 65535)
            m_stationMap.insert(w, s);
    }
}

void DeviceManager::saveStationMap() const
{
    QStringList entries;
    for (auto it = m_stationMap.constBegin(); it != m_stationMap.constEnd(); ++it)
        entries << QString("%1:%2").arg(it.key()).arg(it.value());
    QSettings settings(kSettingsOrg, kSettingsApp);
    settings.setValue(kStationMapKey, entries.join(','));
}

void DeviceManager::setStationMap(const QHash<int, int> &map)
{
    m_stationMap = map;
    saveStationMap();
}

int DeviceManager::resolveStation(int workstation) const
{
    return m_stationMap.value(workstation, workstation);
}

void DeviceManager::dispatchAgv(int workstation)
{
    const int station = resolveStation(workstation);
    emit logMessage(QString("[AGV] 派单：工位 %1 → 站点 %2").arg(workstation).arg(station));
    m_agvCtrl->sendToStation(station);
}

void DeviceManager::cancelAgvNav()
{
    m_agvCtrl->cancelNavigation();
    emit logMessage(QStringLiteral("[AGV] 已发送取消导航"));
}

void DeviceManager::pauseAgvNav()
{
    m_agvCtrl->pauseNavigation();
    emit logMessage(QStringLiteral("[AGV] 已发送暂停导航"));
}

void DeviceManager::resumeAgvNav()
{
    m_agvCtrl->resumeNavigation();
    emit logMessage(QStringLiteral("[AGV] 已发送继续导航"));
}
```

- [ ] **步骤 4.3：编译验证 + Commit**

```bash
git add src/devicemanager.h src/devicemanager.cpp
git commit -m "feat: DeviceManager 新增工位站点映射与 AGV 转发槽"
```

---

### 任务 5：机械臂 Modbus 全移除

**文件：**
- 删除：`src/robotcontroller.h` `src/robotcontroller.cpp` `src/workflowengine.h` `src/workflowengine.cpp`
- 修改：`CMakeLists.txt:21-22,28-29`、`src/devicemanager.h`、`src/devicemanager.cpp`、`src/mainwindow.h`、`src/mainwindow.cpp`

> 本任务步骤间相互依赖，编译验证只在最后一步做一次。

- [ ] **步骤 5.1：CMakeLists.txt 移除四行**

从 `PROJECT_SOURCES` 删除：
`src/workflowengine.cpp`、`src/workflowengine.h`、`src/robotcontroller.cpp`、`src/robotcontroller.h`

- [ ] **步骤 5.2：git rm 删除源文件**

```bash
git rm src/robotcontroller.h src/robotcontroller.cpp src/workflowengine.h src/workflowengine.cpp
```

- [ ] **步骤 5.3：devicemanager.h 清理**

- 删 `class RobotController;` 前向声明
- 删 `RobotController *robotController() const ...` getter
- 删 `m_robotCtrl` 成员
- 删槽 `debugReadRobotRegisters` / `debugWriteRobotRegister`
- 删信号 `robotModbusConnected` / `robotModbusDisconnected` / `robotModbusError` / `debugRegistersRead`
- `Config` 删 `int robotPort = 502;` 字段
- `configApplied` 信号改为 `void configApplied(const QString &robotIP, const QString &agvIP);`

- [ ] **步骤 5.4：devicemanager.cpp 清理**

- 删 `#include "robotcontroller.h"`
- 构造函数删 `m_robotCtrl = new RobotController(this);` 及其 4 个 connect（devicemanager.cpp:15-23）
- `applyConfig()`：删机器人 Modbus 重连两行（`m_robotCtrl->disconnectFromHost/connectToHost`）；日志行改为
  `emit logMessage(QString("配置已更新 → 机械臂 %1  AGV %2:%3  视觉 %4:%5").arg(m_cfg.robotIP).arg(m_cfg.agvIP).arg(m_cfg.agvPort).arg(m_cfg.cameraIP).arg(m_cfg.cameraPort));`
  emit 改为 `emit configApplied(m_cfg.robotIP, m_cfg.agvIP);`
- `testRobot()`（devicemanager.cpp:99-118）：端口全部由 `m_cfg.robotPort` 改为 `m_cfg.huayanPort`，日志文案"机器人"改"机械臂 SDK"
- 删 `debugReadRobotRegisters` / `debugWriteRobotRegister` 函数体

- [ ] **步骤 5.5：mainwindow.h 清理**

- 删 `class WorkflowEngine;` 前向声明、`m_engine` 成员
- 删槽：`onStep`、`onStepActivated`、`onEngineStarted`、`onEngineStopped`
- `onConfigApplied` 签名改 `(const QString &robotIP, const QString &agvIP)`
- 删成员：`m_btnStep`、`m_spinDelay`、`m_spinRobotPort`、`m_indRobotConn`、`m_editRegAddr`、`m_spinRegCount`、`m_btnReadReg`、`m_editRegValue`、`m_btnWriteReg`、`m_regView`
- 删 `initRobotPanel` 声明

- [ ] **步骤 5.6：mainwindow.cpp 清理**

- 删 `#include "workflowengine.h"`
- 构造函数：删 `m_engine = new WorkflowEngine(this);`、三行 `m_engine->setXxx(...)` 注入、第 4 节全部 WorkflowEngine connect（mainwindow.cpp:92-112）、第 6 节机械臂 Modbus 状态 connect（:137-149）、第 8 节 debugRegistersRead connect（:165-175）
- `initUI()`：删 `m_btnStep`/`m_spinDelay` 的创建、布局加入和 `m_btnStep->setObjectName`；删 toolbar 中"步进延时"标签两行；删 `connect(m_btnStep, ...)` 与 `connect(m_spinDelay, ...)`；删 `initRobotPanel(leftPanel);` 调用
- 删 `initRobotPanel()` 整个函数定义（mainwindow.cpp:511-573）
- 删 `onStep()`、`onStepActivated()`、`onEngineStarted()`、`onEngineStopped()` 函数体
- `onStart()` 改为：

```cpp
void MainWindow::onStart()
{
    DeviceManager::Config cfg;
    buildConfig(cfg);
    m_devMgr->setConfig(cfg);
    m_devMgr->applyConfig();                       // 重连 AGV Modbus + 更新视觉 URL
    m_devMgr->huayanScheduler()->startStageOne();  // SDK 取料流程
}
```

- `onStop()` 改为：

```cpp
void MainWindow::onStop()
{
    m_devMgr->huayanScheduler()->stop();
    m_flow->setActiveStep(-1);
    m_lblStep->setText(QStringLiteral("已停止"));
}
```

- `buildConfig()`：删 `cfg.robotPort = m_spinRobotPort->value();` 行
- `onConfigApplied` 改签名并保持两行指示灯更新（去掉 port 参数）
- `initConfigPanel()`：删 `m_spinRobotPort` 创建与布局（robotRow 只留 IP 输入框 + 测试按钮），"机器人 IP:" 标签改 `QStringLiteral("机械臂 IP:")`
- `applyTheme()`：删 `m_regView->setStyleSheet(...)` 块；删 QSS 中 `#btnStep` 两条规则（深浅各两行）
- 工具栏的 AGV 故障 connect（:109-112，引用 m_engine）一并删除（任务 7 用 monitorUpdated 重建故障显示）

- [ ] **步骤 5.7：全文检索确认无残留**

```bash
grep -rn "WorkflowEngine\|RobotController\|robotModbus\|m_engine\|m_regView\|m_spinDelay\|m_btnStep\|robotPort" src/
```

预期：仅 `workflowstep.h`/`workflowwidget.cpp` 注释中的历史提及（顺手把这两处注释里的 "WorkflowEngine" 字样改为 "HuayanScheduler"），无代码引用。

- [ ] **步骤 5.8：编译验证**（命令同顶部，预期 Built target）

- [ ] **步骤 5.9：Commit**

```bash
git add -A
git commit -m "refactor: 移除机械臂 Modbus 路径（RobotController/WorkflowEngine 及界面）"
```

---

### 任务 6：工具栏与流程图接入 SDK 调度

**文件：**
- 修改：`src/mainwindow.h`
- 修改：`src/mainwindow.cpp`

- [ ] **步骤 6.1：mainwindow.h 添加成员与槽**

private 成员区加 `int m_cycleCount = 0;`；private slots 区加 `void onSdkStepChanged(int stepIdx);`

- [ ] **步骤 6.2：mainwindow.cpp 构造函数华沿信号区追加**

第 9 节（华沿 SDK 信号连接块）内追加：

```cpp
        connect(hs, &HuayanScheduler::stepChanged,
                this, &MainWindow::onSdkStepChanged);
        // 顶部"机械臂"指示灯跟随 SDK 连接状态
        connect(hs, &HuayanScheduler::connected, this, [this]() {
            m_indRobot->setStatus(true, QStringLiteral("SDK 已连接"));
        });
        connect(hs, &HuayanScheduler::disconnected, this, [this]() {
            m_indRobot->setStatus(false, QStringLiteral("离线"));
        });
```

- [ ] **步骤 6.3：onSdkStepChanged 实现**（放在 onHuayanStageStarted 之前）

```cpp
/// SDK 调度步骤推进：高亮流程图节点并更新工具栏当前步骤标签
void MainWindow::onSdkStepChanged(int stepIdx)
{
    m_flow->setActiveStep(stepIdx);
    if (stepIdx >= 0 && stepIdx < m_flow->steps().size())
        m_lblStep->setText(m_flow->steps()[stepIdx].name);
}
```

- [ ] **步骤 6.4：阶段槽接管工具栏按钮与计数**

`onHuayanStageStarted` 追加两行：

```cpp
    m_btnStart->setEnabled(false);
    m_btnStop->setEnabled(true);
```

`onHuayanStageCompleted` 追加：

```cpp
    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);
    m_flow->setActiveStep(-1);
    if (stageName.startsWith(QStringLiteral("阶段一")))
        m_lblCycle->setText(QString::number(++m_cycleCount));
    m_lblStep->setText(QStringLiteral("完成"));
```

`onHuayanStageError` 追加：

```cpp
    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);
    m_flow->setActiveStep(-1);
    m_lblStep->setText(QStringLiteral("错误"));
```

- [ ] **步骤 6.5：设备状态指示灯标签改文案**

`initUI()` 中 `m_indRobot = new DeviceIndicator(QStringLiteral("华沿机器人"));` 改为 `QStringLiteral("机械臂 SDK")`。

- [ ] **步骤 6.6：编译验证 + Commit**

```bash
git add src/mainwindow.h src/mainwindow.cpp
git commit -m "feat: 工具栏与流程图改由华沿 SDK 调度驱动"
```

---

### 任务 7：AGV 调试面板

**文件：**
- 修改：`src/mainwindow.h`
- 修改：`src/mainwindow.cpp`

- [ ] **步骤 7.1：mainwindow.h 声明**

include 区加 `#include <QTableWidget>`，`#include "agvcontroller.h"`（AgvMonitorData 需要完整定义）。
private 函数区加：

```cpp
    void initAgvPanel(QVBoxLayout *leftPanel);
    void loadStationMapToTable();
    void rebuildStationMapFromTable();
    void refreshResolvedLabel();
    void updateAgvMonitor(const AgvMonitorData &d);
```

成员区加（原"机械臂 Modbus 控制面板"成员注释块位置）：

```cpp
    // ── AGV 调试面板 ────────────────────────────────────────
    QSpinBox     *m_spinWorkstation = nullptr;
    QLabel       *m_lblResolved     = nullptr;
    QPushButton  *m_btnAgvGo        = nullptr;
    QPushButton  *m_btnAgvCancel    = nullptr;
    QPushButton  *m_btnAgvPause     = nullptr;
    QPushButton  *m_btnAgvResume    = nullptr;
    QTableWidget *m_tblStationMap   = nullptr;
    QPushButton  *m_btnMapAdd       = nullptr;
    QPushButton  *m_btnMapDel       = nullptr;
    QLabel       *m_lblAgvNav       = nullptr;
    QLabel       *m_lblAgvStation   = nullptr;
    QLabel       *m_lblAgvLoc       = nullptr;
    QLabel       *m_lblAgvBattery   = nullptr;
    QLabel       *m_lblAgvCtrl      = nullptr;
    bool          m_darkTheme       = true;
```

private slots 区加 `void onAgvMonitorUpdated(const AgvMonitorData &data);`（直接声明为普通函数 `updateAgvMonitor` 亦可——这里统一用上面 private 函数 + lambda 连接，不加新槽，删除本行。最终以 7.3 的连接代码为准。）

- [ ] **步骤 7.2：initAgvPanel 实现**（放在原 initRobotPanel 位置，`initUI()` 中原 `initRobotPanel(leftPanel)` 调用点改为 `initAgvPanel(leftPanel);`）

```cpp
/**
 * @brief 初始化"AGV 调试"面板
 *
 * 三块：派单行（工位号→站点解析+四按钮）、工位→站点映射表（编辑即存 QSettings）、
 * 实时监控区（AgvController 1s 轮询经 monitorUpdated 刷新）。
 */
void MainWindow::initAgvPanel(QVBoxLayout *leftPanel)
{
    auto *gb   = new QGroupBox(QStringLiteral("AGV 调试"));
    auto *vbox = new QVBoxLayout(gb);

    // ── 派单行 ───────────────────────────────────────────────
    auto *dispatchRow = new QHBoxLayout();
    m_spinWorkstation = new QSpinBox();
    m_spinWorkstation->setRange(1, 65535);
    m_spinWorkstation->setFixedWidth(70);
    m_lblResolved = new QLabel();
    m_btnAgvGo = new QPushButton(QStringLiteral("出发"));
    m_btnAgvGo->setFixedSize(56, 24);
    dispatchRow->addWidget(new QLabel(QStringLiteral("工位:")));
    dispatchRow->addWidget(m_spinWorkstation);
    dispatchRow->addWidget(m_lblResolved);
    dispatchRow->addStretch();
    dispatchRow->addWidget(m_btnAgvGo);
    vbox->addLayout(dispatchRow);

    auto *navRow = new QHBoxLayout();
    m_btnAgvCancel = new QPushButton(QStringLiteral("取消"));
    m_btnAgvPause  = new QPushButton(QStringLiteral("暂停"));
    m_btnAgvResume = new QPushButton(QStringLiteral("继续"));
    for (auto *b : {m_btnAgvCancel, m_btnAgvPause, m_btnAgvResume})
        b->setFixedHeight(24);
    navRow->addWidget(m_btnAgvCancel);
    navRow->addWidget(m_btnAgvPause);
    navRow->addWidget(m_btnAgvResume);
    navRow->addStretch();
    vbox->addLayout(navRow);

    // ── 工位 → 站点映射表 ───────────────────────────────────
    m_tblStationMap = new QTableWidget(0, 2);
    m_tblStationMap->setHorizontalHeaderLabels(
        {QStringLiteral("工位号"), QStringLiteral("站点 id")});
    m_tblStationMap->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_tblStationMap->verticalHeader()->setVisible(false);
    m_tblStationMap->setFixedHeight(110);
    vbox->addWidget(m_tblStationMap);

    auto *mapBtnRow = new QHBoxLayout();
    m_btnMapAdd = new QPushButton(QStringLiteral("+ 添加"));
    m_btnMapDel = new QPushButton(QStringLiteral("- 删除"));
    m_btnMapAdd->setFixedSize(56, 22);
    m_btnMapDel->setFixedSize(56, 22);
    mapBtnRow->addWidget(m_btnMapAdd);
    mapBtnRow->addWidget(m_btnMapDel);
    mapBtnRow->addStretch();
    vbox->addLayout(mapBtnRow);

    // ── 实时监控区 ───────────────────────────────────────────
    auto *monForm = new QFormLayout();
    m_lblAgvNav     = new QLabel(QStringLiteral("-"));
    m_lblAgvStation = new QLabel(QStringLiteral("-"));
    m_lblAgvLoc     = new QLabel(QStringLiteral("-"));
    m_lblAgvBattery = new QLabel(QStringLiteral("-"));
    m_lblAgvCtrl    = new QLabel(QStringLiteral("-"));
    monForm->addRow(QStringLiteral("导航状态:"), m_lblAgvNav);
    monForm->addRow(QStringLiteral("当前站点:"), m_lblAgvStation);
    monForm->addRow(QStringLiteral("定位状态:"), m_lblAgvLoc);
    monForm->addRow(QStringLiteral("电池电量:"), m_lblAgvBattery);
    monForm->addRow(QStringLiteral("控制权:"),   m_lblAgvCtrl);
    vbox->addLayout(monForm);

    leftPanel->addWidget(gb);

    // ── 信号连接 ─────────────────────────────────────────────
    connect(m_btnAgvGo, &QPushButton::clicked, this, [this]() {
        const int ws = m_spinWorkstation->value();
        m_devMgr->dispatchAgv(ws);
        m_flow->setStationHighlight(ws);
    });
    connect(m_btnAgvCancel, &QPushButton::clicked, m_devMgr, &DeviceManager::cancelAgvNav);
    connect(m_btnAgvPause,  &QPushButton::clicked, m_devMgr, &DeviceManager::pauseAgvNav);
    connect(m_btnAgvResume, &QPushButton::clicked, m_devMgr, &DeviceManager::resumeAgvNav);
    connect(m_spinWorkstation, &QSpinBox::valueChanged,
            this, [this](int) { refreshResolvedLabel(); });

    connect(m_btnMapAdd, &QPushButton::clicked, this, [this]() {
        const int row = m_tblStationMap->rowCount();
        m_tblStationMap->insertRow(row);
        m_tblStationMap->setItem(row, 0, new QTableWidgetItem(QStringLiteral("1")));
        m_tblStationMap->setItem(row, 1, new QTableWidgetItem(QStringLiteral("1")));
    });
    connect(m_btnMapDel, &QPushButton::clicked, this, [this]() {
        const int row = m_tblStationMap->currentRow();
        if (row >= 0) m_tblStationMap->removeRow(row);
        rebuildStationMapFromTable();
    });
    connect(m_tblStationMap, &QTableWidget::cellChanged,
            this, [this](int, int) { rebuildStationMapFromTable(); });

    connect(m_devMgr->agvController(), &AgvController::monitorUpdated,
            this, [this](const AgvMonitorData &d) { updateAgvMonitor(d); });

    loadStationMapToTable();
    refreshResolvedLabel();
}
```

注意 `#include <QHeaderView>` 加入 mainwindow.cpp include 区。

- [ ] **步骤 7.3：表格与监控辅助函数实现**（放在 initAgvPanel 之后）

```cpp
/// QSettings 中的映射 → 填充表格（QSignalBlocker 防止 cellChanged 递归回写）
void MainWindow::loadStationMapToTable()
{
    const QSignalBlocker blocker(m_tblStationMap);
    const QHash<int, int> map = m_devMgr->stationMap();
    QList<int> keys = map.keys();
    std::sort(keys.begin(), keys.end());
    m_tblStationMap->setRowCount(0);
    for (int w : keys) {
        const int row = m_tblStationMap->rowCount();
        m_tblStationMap->insertRow(row);
        m_tblStationMap->setItem(row, 0, new QTableWidgetItem(QString::number(w)));
        m_tblStationMap->setItem(row, 1, new QTableWidgetItem(QString::number(map[w])));
    }
}

/// 表格 → 映射哈希并持久化；非法行（非 1-65535 整数）跳过
void MainWindow::rebuildStationMapFromTable()
{
    QHash<int, int> map;
    for (int row = 0; row < m_tblStationMap->rowCount(); ++row) {
        const auto *itemW = m_tblStationMap->item(row, 0);
        const auto *itemS = m_tblStationMap->item(row, 1);
        if (!itemW || !itemS) continue;
        bool okW = false, okS = false;
        const int w = itemW->text().toInt(&okW);
        const int s = itemS->text().toInt(&okS);
        if (okW && okS && w > 0 && s > 0 && w <= 65535 && s <= 65535)
            map.insert(w, s);
    }
    m_devMgr->setStationMap(map);
    refreshResolvedLabel();
}

void MainWindow::refreshResolvedLabel()
{
    const int ws = m_spinWorkstation->value();
    m_lblResolved->setText(QString("→ 站点 %1").arg(m_devMgr->resolveStation(ws)));
}

/// 监控快照 → 五行标签；导航状态着色：到达=绿 失败/取消/超时=红
void MainWindow::updateAgvMonitor(const AgvMonitorData &d)
{
    static const QStringList kNavText = {
        QStringLiteral("无"), QStringLiteral("等待"), QStringLiteral("执行中"),
        QStringLiteral("暂停"), QStringLiteral("到达"), QStringLiteral("失败"),
        QStringLiteral("取消"), QStringLiteral("超时")};
    static const QStringList kLocText = {
        QStringLiteral("失败"), QStringLiteral("正确"),
        QStringLiteral("重定位中"), QStringLiteral("完成(需确认)")};

    const QString nav = d.navStatus < kNavText.size()
        ? kNavText[d.navStatus] : QString::number(d.navStatus);
    m_lblAgvNav->setText(QString("%1 (%2)").arg(nav).arg(d.navStatus));

    QString color = m_darkTheme ? "#ccc" : "#333";
    if (d.navStatus == 4) color = m_darkTheme ? "#5cb85c" : "#107c10";
    else if (d.navStatus >= 5 && d.navStatus <= 7) color = m_darkTheme ? "#d9534f" : "#c42b1c";
    m_lblAgvNav->setStyleSheet(QString("color:%1; font-weight:bold;").arg(color));

    m_lblAgvStation->setText(QString("%1（导航目标 %2）")
                                 .arg(d.curStation).arg(d.navStation));
    m_lblAgvLoc->setText(d.locStatus < kLocText.size()
        ? kLocText[d.locStatus] : QString::number(d.locStatus));
    m_lblAgvBattery->setText(QString("%1%").arg(d.battery));
    m_lblAgvCtrl->setText(d.ctrlSeized ? QStringLiteral("被外部抢占")
                                       : QStringLiteral("正常"));

    if (d.navStatus >= 5 && d.navStatus <= 7)
        m_indAGV->setStatus(false, QStringLiteral("导航异常"));
}
```

- [ ] **步骤 7.4：applyTheme 适配**

函数体开头加 `m_darkTheme = dark;`。kDark QSS 追加（kLight 同理换浅色值）：

```
"QTableWidget { background:#3a3d44; color:#ddd; gridline-color:#555;"
"               border:1px solid #555; font-size:12px; }"
"QHeaderView::section { background:#2f3138; color:#ccc; border:none; padding:2px 4px; }"
```

kLight 对应：

```
"QTableWidget { background:#ffffff; color:#222; gridline-color:#ccc;"
"               border:1px solid #bbb; font-size:12px; }"
"QHeaderView::section { background:#e8e8e8; color:#333; border:none; padding:2px 4px; }"
```

- [ ] **步骤 7.5：编译验证 + Commit**

```bash
git add src/mainwindow.h src/mainwindow.cpp
git commit -m "feat: 新增 AGV 调试面板（派单/映射表/实时监控）"
```

---

### 任务 8：功能验证（mock AGV）

**文件：** 无代码修改。

- [ ] **步骤 8.1：启动模拟 AGV 并运行程序**

```bash
python scripts/mock_agv_server.py 1502 &
```

临时验证用：把 `DeviceManager::Config::agvPort` 默认值改 1502 运行（**验证完改回 502 再 commit，本步骤不留改动**），或管理员权限 `python scripts/mock_agv_server.py 502`。启动 `build/Desktop_Qt_6_8_3_MinGW_64_bit-Debug/wh-robot-visual.exe`，AGV IP 填 127.0.0.1，点"应用配置"。

- [ ] **步骤 8.2：核对验证清单**

1. 日志出现"AGV Modbus 已连接"与"[AGV] 控制权未被外部抢占"
2. 监控区 1s 内开始刷新（电量 88%、定位"正确"）
3. 映射表添加一行 `2 → 3`，工位 SpinBox 调到 2，解析标签显示"→ 站点 3"
4. 点"出发"：日志打印"派单：工位 2 → 站点 3"，监控区导航状态 执行中→到达（绿色），mock 控制台收到站点 3
5. 重启程序：映射表自动恢复 `2 → 3`
6. 再派一单并点"取消"：状态变"取消"（红色），AGV 指示灯"导航异常"
7. 工具栏只剩 开始/停止/循环/当前/主题开关；左侧无"机械臂 Modbus 控制"面板
8. 主题切换深/浅各一遍：表格、监控标签、面板按钮无白底黑字穿帮
9. （SDK 部分，无真机时可选）流程图 5 节点文案为 视觉定位/SDK 取料/翻转卸料/AGV 运输/码垛复位

- [ ] **步骤 8.3：清理验证现场**

杀掉 mock 进程；`git status` 确认无未预期改动（agvPort 已还原）。

---

### 任务 9：CHANGELOG 与收尾

**文件：**
- 修改：`changelog/CHANGELOG.md`

- [ ] **步骤 9.1：CHANGELOG 头部追加**

```markdown
## 2026-06-11 | 界面清理与 AGV 调试面板（feature/ui-sdk-cleanup-agv-panel）

### 变更
- 移除机械臂 Modbus 路径：删除 `RobotController`、`WorkflowEngine` 及"机械臂 Modbus 控制"调试面板、机器人端口输入框、工具栏单步/步进延时
- 工具栏开始/停止与右侧流程图改由华沿 SDK（`HuayanScheduler` 新增 `stepChanged` 信号）驱动，流程图节点更新为：视觉定位/SDK 取料/翻转卸料/AGV 运输(占位)/码垛复位
- 顶部"机械臂"指示灯改为反映 SDK 连接状态；配置面板"机器人 IP"改"机械臂 IP"，测试按钮改 ping 10003

### 新增
- AGV 调试面板：工位派单（出发/取消/暂停/继续）、工位→站点 id 映射表（QSettings 持久化，未配置回退 id=工位号）、实时监控区（导航/站点/定位/电量/控制权，1s 轮询）
- `AgvController`：`pauseNavigation`/`resumeNavigation`（[0x]00004/00005）、监控轮询 `monitorUpdated(AgvMonitorData)`
- `DeviceManager`：`resolveStation`/`setStationMap` 与 AGV 转发槽 `dispatchAgv`/`cancelAgvNav`/`pauseAgvNav`/`resumeAgvNav`

### 已知限制
- AGV 自动并入 SDK 调度流程未实现（流程图 AGV 节点为占位）
- `VisionHttpClient::transformToRegisters` 寄存器路径成为死代码，待后续清理
```

- [ ] **步骤 9.2：Commit + 推送**

```bash
git add changelog/CHANGELOG.md
git commit -m "docs: CHANGELOG 记录界面清理与 AGV 调试面板"
git push -u origin feature/ui-sdk-cleanup-agv-panel
```

PR 创建（gh 未安装，手动）：https://github.com/ssrxiaoge2/robot_visual/pull/new/feature/ui-sdk-cleanup-agv-panel

---

## 自检记录

- **规格覆盖度**：§1 删除清单→任务 5；§2 SDK 驱动→任务 2/3/6；§3 AGV 面板（3.1-3.6）→任务 1/4/7；§4 配置面板→任务 5（5.4/5.6）；§5 验证→任务 8；Git→任务 9。无遗漏。
- **占位符扫描**：无"待定/TODO/类似任务N"；所有代码步骤含完整代码。
- **类型一致性**：`AgvMonitorData` 字段（navStation/locStatus/navStatus/battery/curStation/ctrlSeized）在任务 1 定义、任务 7 消费一致；`stepIndexFor` 索引与任务 3 kDefaultSteps 顺序一致（0/1/2/4，3=AGV 占位不发射）；`dispatchAgv` 等槽名任务 4 定义、任务 7 连接一致。
