# 整线流程小规模可行性试测 实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 新建 `LineOrchestrator` 编排器，串起"AGV 取料站→机械臂取料→收姿态→AGV 倒料站→倒料→收姿态→AGV 回站1"的单次整线流程，工具栏开始/停止驱动。

**架构：** `LineOrchestrator` 是纯协调层（不碰 Modbus/SDK 原语），由 `DeviceManager` 持有并注入 `AgvController` + `HuayanScheduler`；AGV 移动调 `DeviceManager::dispatchAgv` 并等 `AgvController::arrived`，机械臂动作调 `HuayanScheduler` 新增的 `startStow`/`startUnload` 并等 `stageCompleted`。流程图改由编排器驱动。规格见 `docs/superpowers/specs/2026-06-12-line-flow-pilot-design.md`。

**技术栈：** Qt 6.8（Widgets/Network/SerialBus）、CMake、MinGW 13.1。

**测试说明：** 仓库无单元测试设施（纯 Qt GUI 工程），验证方式 = 每任务编译通过 + 末尾真机/离线流程验收。编译命令（已验证）：

```bash
cd /d/study/qtpro/robot-deploy/wh-robot-visual/build/Desktop_Qt_6_8_3_MinGW_64_bit-Debug && /d/study/qt/Tools/mingw1310_64/bin/mingw32-make.exe wh-robot-visual -j8 2>&1 | tail -5
```

预期 `[100%] Built target wh-robot-visual`。链接报 Permission denied 时先 `taskkill //F //IM wh-robot-visual.exe` 再重试。新增 .cpp 后 make 会自动重跑 cmake（CMakeLists 变更触发）。

**工作分支：** `feature/line-flow-pilot`（已基于最新 master 创建，设计文档已提交）。

**提交纪律：** 每个 commit 只 `git add` 本任务涉及的具体文件，**禁止 `git add -A`**——工作树有无关的第三方 SDK 二进制改动（`3rd/HuaYansdk/*`）和 `scripts/__pycache__/`，绝不能带入。

---

## 文件结构

| 文件 | 操作 | 职责 |
|------|------|------|
| `src/lineorchestrator.h` | 创建 | 整线状态机声明（LineState 枚举、接口、信号、控制器指针） |
| `src/lineorchestrator.cpp` | 创建 | 状态机实现（init 检查、AGV/机械臂步推进、错误中止） |
| `src/huayanScheduler.h` | 修改 | +Stage::Stow/Unload、+StageStep 三项、+startStow/startUnload、+三个 func 名成员 |
| `src/huayanScheduler.cpp` | 修改 | +startStow/startUnload 实现，proceedStage/advanceStep/executeCurrentStep 加 case |
| `src/workflowwidget.cpp` | 修改 | kDefaultSteps 改为整线 5 节点 |
| `src/devicemanager.h` | 修改 | +LineOrchestrator 成员 + getter |
| `src/devicemanager.cpp` | 修改 | 构造注入 + 转发 lineLog/lineError 到 logMessage |
| `src/mainwindow.h` | 修改 | onStart/onStop 改走编排器；+onLineStepChanged/onLineFinished/onLineStopped/onLineError 槽 |
| `src/mainwindow.cpp` | 修改 | 接线编排器信号，工具栏改驱动 LineOrchestrator |
| `CMakeLists.txt` | 修改 | 加入 lineorchestrator.{h,cpp} |
| `changelog/CHANGELOG.md` | 修改 | 追加条目 |

---

## 关键既有接口（实现时依赖，已核对）

- `AgvController`：`void sendToStation(int)`、信号 `void arrived()`（导航状态==Arrived 且站点匹配本次 sendToStation 目标时发）、信号 `void monitorUpdated(const AgvMonitorData &)`（含 `navStatus` 0-7、`curStation`）、信号 `void errorOccurred(const QString &)`
- `DeviceManager`：`void dispatchAgv(int workstation)`（slot，内部 resolveStation 后 sendToStation）、`int resolveStation(int)`、`AgvController *agvController()`、`HuayanScheduler *huayanScheduler()`
- `HuayanScheduler`：`void startStageOne()`、`void stop()`、信号 `void stageCompleted(const QString &)` / `void stageError(const QString &)` / `void logMessage(const QString &)`；私有 `bool executeRunFunc(const QString&, int timeoutMs=30000)`、`void startWaitForIdle(int)`、`void advanceStep()`、`void proceedStage()`、`void executeCurrentStep()`、成员 `Stage m_stage` / `StageStep m_stageStep`
- `WorkflowWidget`：`void setActiveStep(int)`、`const QList<WorkflowStep> &steps() const`
- `MainWindow`：`onStart()`（mainwindow.cpp:809）、`onStop()`（:819）、`onSdkStepChanged()`（:1027）、构造函数华沿信号区（:130 附近）、`m_btnStart`/`m_btnStop`/`m_lblCycle`/`m_lblStep`/`m_flow`/`m_devMgr`/`m_cycleCount`

---

## 任务 1：HuayanScheduler 新增收姿态与倒料阶段

**文件：**
- 修改：`src/huayanScheduler.h`
- 修改：`src/huayanScheduler.cpp`

- [ ] **步骤 1.1：huayanScheduler.h 扩展枚举与接口**

`enum class Stage`（huayanScheduler.h:15-20）改为：

```cpp
    enum class Stage {
        None,
        StageOne,
        StageTwo,
        StageThree,
        Stow,
        Unload
    };
```

`enum class StageStep`（私有，约 huayanScheduler.h:78-92）末尾 `ExecuteStackingFunction` 之后加三项：

```cpp
        ExecuteStackingFunction,
        StowArm,
        MoveToUnloadPoint,
        RunUnloadFunc
```

public 方法区 `void startStageThree();` 之后加：

```cpp
    void startStow();    // 收运行姿态（func_yun_xing_zhong）
    void startUnload();  // 倒料（func_daoliao_1_point → func_daoliao）
```

私有 func 名成员区（`m_releaseFuncName` 附近，约 huayanScheduler.h:165）加：

```cpp
    QString m_stowFuncName        = QStringLiteral("func_yun_xing_zhong");
    QString m_unloadPointFuncName = QStringLiteral("func_daoliao_1_point");
    QString m_unloadFuncName      = QStringLiteral("func_daoliao");
```

- [ ] **步骤 1.2：huayanScheduler.cpp 实现 startStow / startUnload**

在 `startStageThree()` 定义之后新增（结构对照 startStageOne：先确保连接 + 复位，再设阶段、发 stageStarted、proceedStage）：

```cpp
void HuayanScheduler::startStow()
{
    if (!ensureConnected())
        return;

    HRIF_GrpReset(m_boxID, m_rbtID);

    m_stage = Stage::Stow;
    m_stageStep = StageStep::StowArm;

    emit stageStarted(stageName(m_stage));
    proceedStage();
}

void HuayanScheduler::startUnload()
{
    if (!ensureConnected())
        return;

    HRIF_GrpReset(m_boxID, m_rbtID);

    m_stage = Stage::Unload;
    m_stageStep = StageStep::MoveToUnloadPoint;

    emit stageStarted(stageName(m_stage));
    proceedStage();
}
```

- [ ] **步骤 1.3：proceedStage 加新阶段分发**

`proceedStage()`（huayanScheduler.cpp:369）的 switch 加 case：

```cpp
    switch (m_stage) {
    case Stage::StageOne:
    case Stage::StageTwo:
    case Stage::StageThree:
    case Stage::Stow:
    case Stage::Unload:
        executeCurrentStep();
        break;
    default:
        break;
    }
```

- [ ] **步骤 1.4：advanceStep 加新阶段步进**

`advanceStep()`（huayanScheduler.cpp:383）的 switch 末尾 `default` 之前加：

```cpp
    case Stage::Stow:
        m_stageStep = StageStep::None;
        break;
    case Stage::Unload:
        if (m_stageStep == StageStep::MoveToUnloadPoint)
            m_stageStep = StageStep::RunUnloadFunc;
        else
            m_stageStep = StageStep::None;
        break;
```

- [ ] **步骤 1.5：executeCurrentStep 加新阶段执行**

`executeCurrentStep()`（huayanScheduler.cpp:438）的外层 switch（`case Stage::StageThree:` 块之后、`default:` 之前）加两个阶段块：

```cpp
    case Stage::Stow:
        switch (m_stageStep) {
        case StageStep::StowArm:
            emit logMessage(QStringLiteral("[收姿态] 调用 %1").arg(m_stowFuncName));
            executeRunFunc(m_stowFuncName);
            break;
        case StageStep::None:
            emit stageCompleted(stageName(m_stage));
            stop();
            break;
        default:
            break;
        }
        break;
    case Stage::Unload:
        switch (m_stageStep) {
        case StageStep::MoveToUnloadPoint:
            emit logMessage(QStringLiteral("[倒料] 移动到倒料点位 %1").arg(m_unloadPointFuncName));
            executeRunFunc(m_unloadPointFuncName);
            break;
        case StageStep::RunUnloadFunc:
            emit logMessage(QStringLiteral("[倒料] 执行倒料 %1").arg(m_unloadFuncName));
            executeRunFunc(m_unloadFuncName);
            break;
        case StageStep::None:
            emit stageCompleted(stageName(m_stage));
            stop();
            break;
        default:
            break;
        }
        break;
```

- [ ] **步骤 1.6：stageName 加新阶段名**

`stageName(Stage)` 自由函数（huayanScheduler.cpp:45 附近的匿名命名空间内 `QString stageName(HuayanScheduler::Stage stage)`）的 switch 加：

```cpp
    case HuayanScheduler::Stage::Stow:
        return QStringLiteral("收姿态");
    case HuayanScheduler::Stage::Unload:
        return QStringLiteral("倒料");
```

注意：`stepIndexFor`（huayanScheduler.cpp:420）**不改**——新阶段不驱动流程图（流程图改由编排器驱动，见任务 3）。新增的三个 StageStep 落入 `stepIndexFor` 的 `default: return -1`，不发 stepChanged，符合预期。

- [ ] **步骤 1.7：编译验证**

运行顶部编译命令。预期 Built target，无 error。

- [ ] **步骤 1.8：Commit**

```bash
git add src/huayanScheduler.h src/huayanScheduler.cpp
git commit -m "feat: HuayanScheduler 新增收姿态与倒料阶段"
```

---

## 任务 2：LineOrchestrator 整线编排器

**文件：**
- 创建：`src/lineorchestrator.h`
- 创建：`src/lineorchestrator.cpp`
- 修改：`CMakeLists.txt`

- [ ] **步骤 2.1：创建 lineorchestrator.h**

```cpp
#ifndef LINEORCHESTRATOR_H
#define LINEORCHESTRATOR_H

#include <QObject>
#include <QTimer>
#include "agvcontroller.h"   // AgvMonitorData 需完整定义

class HuayanScheduler;

/*!
 * \brief 整线流程编排器（AGV + 机械臂时序协调）
 *
 * 纯协调层，不调用任何 Modbus/SDK 原语；只调高层方法并监听完成信号。
 * 单次流程：初检 → AGV取料站 → 机械臂取料 → 收姿态 → AGV倒料站 →
 *           倒料 → 收姿态 → AGV回站1 → 完成。任一步出错就地停机。
 */
class LineOrchestrator : public QObject
{
    Q_OBJECT
public:
    explicit LineOrchestrator(AgvController *agv,
                              HuayanScheduler *arm,
                              QObject *parent = nullptr);

    /// 工位号（试测默认：待机1 / 取料3 / 倒料4），可经 setter 改
    void setStations(int home, int pickup, int unload);

    bool isRunning() const { return m_state != LineState::Idle; }

public slots:
    void start();  // 工具栏"开始"
    void stop();   // 工具栏"停止"（停机，不报错）

signals:
    void lineStarted();
    void lineFinished();                    // 一轮成功完成
    void lineStopped();                     // 手动停止
    void lineError(const QString &reason);  // 中止并报因
    void stepChanged(int stepIdx);          // 0-4 驱动流程图
    void lineLog(const QString &msg);

    /// 请求 AGV 前往某工位（DeviceManager 接此信号调 dispatchAgv）
    void agvDispatchRequested(int workstation);

private slots:
    void onAgvArrived();
    void onAgvMonitor(const AgvMonitorData &d);
    void onArmCompleted(const QString &stageName);
    void onArmError(const QString &msg);

private:
    enum class LineState {
        Idle,
        InitCheck,
        AgvToPickup,
        ArmPicking,
        StowAfterPick,
        AgvToUnload,
        ArmUnloading,
        StowAfterUnload,
        AgvReturnHome
    };

    void enterState(LineState s);
    void haltDevices();              // 取消 AGV 导航 + 停机械臂 + 回 Idle
    void abort(const QString &reason);
    void emitStepForState(LineState s);

    AgvController   *m_agv = nullptr;
    HuayanScheduler *m_arm = nullptr;

    LineState m_state = LineState::Idle;
    AgvMonitorData m_lastMonitor;    // 最近一次 AGV 监控快照（初检读 curStation）
    QTimer *m_agvTimeout = nullptr;  // AGV 单步超时保护

    int m_homeStation   = 1;
    int m_pickupStation = 3;
    int m_unloadStation = 4;

    static constexpr int kAgvTimeoutMs = 120000; // AGV 单步 120s 超时
};

#endif // LINEORCHESTRATOR_H
```

- [ ] **步骤 2.2：创建 lineorchestrator.cpp**

```cpp
/**
 * @file lineorchestrator.cpp
 * @brief LineOrchestrator 整线流程编排器实现
 *
 * 推进机制：
 *   AGV 移动步 — emit agvDispatchRequested(工位号) → 等 AgvController::arrived
 *   机械臂步   — 调 HuayanScheduler::startXxx → 等 stageCompleted
 *   出错       — abort()：停机 + lineError
 * 完成判定靠自身 m_state（任意时刻只有一个子动作在飞），不靠 stageName 字符串。
 */

#include "lineorchestrator.h"
#include "huayanScheduler.h"

LineOrchestrator::LineOrchestrator(AgvController *agv,
                                   HuayanScheduler *arm,
                                   QObject *parent)
    : QObject(parent), m_agv(agv), m_arm(arm)
{
    connect(m_agv, &AgvController::arrived,
            this, &LineOrchestrator::onAgvArrived);
    connect(m_agv, &AgvController::monitorUpdated,
            this, &LineOrchestrator::onAgvMonitor);
    connect(m_agv, &AgvController::errorOccurred, this, [this](const QString &msg) {
        if (m_state == LineState::AgvToPickup || m_state == LineState::AgvToUnload
            || m_state == LineState::AgvReturnHome)
            abort(QStringLiteral("AGV 通信错误：%1").arg(msg));
    });
    connect(m_arm, &HuayanScheduler::stageCompleted,
            this, &LineOrchestrator::onArmCompleted);
    connect(m_arm, &HuayanScheduler::stageError,
            this, &LineOrchestrator::onArmError);

    m_agvTimeout = new QTimer(this);
    m_agvTimeout->setSingleShot(true);
    m_agvTimeout->setInterval(kAgvTimeoutMs);
    connect(m_agvTimeout, &QTimer::timeout, this, [this]() {
        abort(QStringLiteral("AGV 单步超时（%1s 未到达）").arg(kAgvTimeoutMs / 1000));
    });
}

void LineOrchestrator::setStations(int home, int pickup, int unload)
{
    m_homeStation = home;
    m_pickupStation = pickup;
    m_unloadStation = unload;
}

void LineOrchestrator::start()
{
    if (m_state != LineState::Idle) return;
    emit lineStarted();
    emit lineLog(QStringLiteral("[整线] 流程启动，初始检查"));
    enterState(LineState::InitCheck);
}

void LineOrchestrator::stop()
{
    if (m_state == LineState::Idle) return;
    haltDevices();
    emit lineLog(QStringLiteral("[整线] 已手动停止"));
    emit lineStopped();
}

void LineOrchestrator::haltDevices()
{
    m_agvTimeout->stop();
    m_agv->cancelNavigation();
    m_arm->stop();
    m_state = LineState::Idle;
}

void LineOrchestrator::abort(const QString &reason)
{
    haltDevices();
    emit lineLog(QStringLiteral("[整线][错误] %1").arg(reason));
    emit lineError(reason);
}

/// 0-4 流程图节点：收姿态折进相邻 AGV 移动节点
void LineOrchestrator::emitStepForState(LineState s)
{
    int idx = -1;
    switch (s) {
    case LineState::AgvToPickup:     idx = 0; break;
    case LineState::ArmPicking:      idx = 1; break;
    case LineState::StowAfterPick:   idx = 2; break;
    case LineState::AgvToUnload:     idx = 2; break;
    case LineState::ArmUnloading:    idx = 3; break;
    case LineState::StowAfterUnload: idx = 4; break;
    case LineState::AgvReturnHome:   idx = 4; break;
    default:                         idx = -1; break;
    }
    if (idx >= 0)
        emit stepChanged(idx);
}

void LineOrchestrator::enterState(LineState s)
{
    m_state = s;
    emitStepForState(s);

    switch (s) {
    case LineState::InitCheck:
        // AGV 在站1（真检测）+ 强制机械臂收姿态
        if (m_lastMonitor.curStation != m_homeStation) {
            abort(QStringLiteral("初检失败：AGV 当前站 %1 ≠ 待机站 %2，请先归位")
                      .arg(m_lastMonitor.curStation).arg(m_homeStation));
            return;
        }
        emit lineLog(QStringLiteral("[整线] AGV 已在站%1，强制机械臂收姿态").arg(m_homeStation));
        m_arm->startStow();  // 完成 → onArmCompleted 推进到 AgvToPickup
        break;
    case LineState::AgvToPickup:
        emit lineLog(QStringLiteral("[整线] AGV 前往取料站 %1").arg(m_pickupStation));
        m_agvTimeout->start();
        emit agvDispatchRequested(m_pickupStation);
        break;
    case LineState::ArmPicking:
        emit lineLog(QStringLiteral("[整线] 机械臂取料"));
        m_arm->startStageOne();
        break;
    case LineState::StowAfterPick:
        emit lineLog(QStringLiteral("[整线] 取料后收姿态"));
        m_arm->startStow();
        break;
    case LineState::AgvToUnload:
        emit lineLog(QStringLiteral("[整线] AGV 前往倒料站 %1").arg(m_unloadStation));
        m_agvTimeout->start();
        emit agvDispatchRequested(m_unloadStation);
        break;
    case LineState::ArmUnloading:
        emit lineLog(QStringLiteral("[整线] 机械臂倒料"));
        m_arm->startUnload();
        break;
    case LineState::StowAfterUnload:
        emit lineLog(QStringLiteral("[整线] 倒料后收姿态"));
        m_arm->startStow();
        break;
    case LineState::AgvReturnHome:
        emit lineLog(QStringLiteral("[整线] AGV 返回待机站 %1").arg(m_homeStation));
        m_agvTimeout->start();
        emit agvDispatchRequested(m_homeStation);
        break;
    case LineState::Idle:
        break;
    }
}

void LineOrchestrator::onAgvArrived()
{
    m_agvTimeout->stop();
    switch (m_state) {
    case LineState::AgvToPickup:   enterState(LineState::ArmPicking);   break;
    case LineState::AgvToUnload:   enterState(LineState::ArmUnloading); break;
    case LineState::AgvReturnHome:
        emit lineLog(QStringLiteral("[整线] 回到待机站，流程完成"));
        m_state = LineState::Idle;
        emit lineFinished();
        break;
    default:
        break;  // 非等待 AGV 的状态，忽略
    }
}

void LineOrchestrator::onAgvMonitor(const AgvMonitorData &d)
{
    m_lastMonitor = d;
    // 等待 AGV 行走时，导航失败/取消/超时 → 中止
    if ((m_state == LineState::AgvToPickup || m_state == LineState::AgvToUnload
         || m_state == LineState::AgvReturnHome)
        && d.navStatus >= 5 && d.navStatus <= 7) {
        static const char *txt[] = {"", "", "", "", "", "失败", "取消", "超时"};
        abort(QStringLiteral("AGV 导航%1（状态 %2）")
                  .arg(QString::fromUtf8(txt[d.navStatus])).arg(d.navStatus));
    }
}

void LineOrchestrator::onArmCompleted(const QString &stageName)
{
    Q_UNUSED(stageName);  // 用自身状态判断，不靠字符串
    switch (m_state) {
    case LineState::InitCheck:       enterState(LineState::AgvToPickup);     break;
    case LineState::ArmPicking:      enterState(LineState::StowAfterPick);   break;
    case LineState::StowAfterPick:   enterState(LineState::AgvToUnload);     break;
    case LineState::ArmUnloading:    enterState(LineState::StowAfterUnload); break;
    case LineState::StowAfterUnload: enterState(LineState::AgvReturnHome);   break;
    default:
        break;
    }
}

void LineOrchestrator::onArmError(const QString &msg)
{
    if (m_state != LineState::Idle)
        abort(QStringLiteral("机械臂错误：%1").arg(msg));
}
```

- [ ] **步骤 2.3：CMakeLists.txt 加入新文件**

`PROJECT_SOURCES`（CMakeLists.txt:16-44）内 `src/huayanScheduler.h` 行之后加：

```cmake
        src/lineorchestrator.cpp
        src/lineorchestrator.h
```

- [ ] **步骤 2.4：编译验证**（同顶部命令）。注意：此任务后 LineOrchestrator 尚未被任何地方实例化，但应能独立编译通过。

- [ ] **步骤 2.5：Commit**

```bash
git add src/lineorchestrator.h src/lineorchestrator.cpp CMakeLists.txt
git commit -m "feat: 新增 LineOrchestrator 整线流程编排器"
```

---

## 任务 3：流程图节点改为整线 5 节点

**文件：**
- 修改：`src/workflowwidget.cpp:17-23`

- [ ] **步骤 3.1：替换 kDefaultSteps**

`kDefaultSteps`（workflowwidget.cpp:17）改为：

```cpp
// ── 默认步骤定义（整线流程节点，索引与 LineOrchestrator::emitStepForState 对应）──
// 此处仅用于 UI 展示，不含业务逻辑
static const QList<WorkflowStep> kDefaultSteps = {
    {QStringLiteral("AGV→取料站"), QStringLiteral("仙工 Modbus"), QColor(60,  180, 120)},
    {QStringLiteral("机械臂取料"), QStringLiteral("视觉夹取"),    QColor(220, 160,  50)},
    {QStringLiteral("AGV→倒料站"), QStringLiteral("收姿态+运输"), QColor(60,  180, 120)},
    {QStringLiteral("倒料"),       QStringLiteral("华沿 SDK"),    QColor(180, 110, 210)},
    {QStringLiteral("回站待机"),   QStringLiteral("收姿态+返回"), QColor(130, 130, 130)},
};
```

- [ ] **步骤 3.2：编译验证 + Commit**

```bash
git add src/workflowwidget.cpp
git commit -m "feat: 流程图节点改为整线流程视角"
```

---

## 任务 4：DeviceManager 持有并接线 LineOrchestrator

**文件：**
- 修改：`src/devicemanager.h`
- 修改：`src/devicemanager.cpp`

- [ ] **步骤 4.1：devicemanager.h 声明**

前向声明区（`class HuayanScheduler;` 附近）加：

```cpp
class LineOrchestrator;
```

getter 区（`huayanScheduler()` 之后）加：

```cpp
    LineOrchestrator *lineOrchestrator() const { return m_lineOrch; }
```

成员区（`m_huayanScheduler` 之后）加：

```cpp
    LineOrchestrator *m_lineOrch = nullptr;
```

- [ ] **步骤 4.2：devicemanager.cpp 构造注入与接线**

include 区加：

```cpp
#include "lineorchestrator.h"
```

构造函数末尾（`loadStationMap();` 之前或之后均可）加：

```cpp
    m_lineOrch = new LineOrchestrator(m_agvCtrl, m_huayanScheduler, this);
    // 编排器请求派单 → 经映射表解析后下发（复用 dispatchAgv）
    connect(m_lineOrch, &LineOrchestrator::agvDispatchRequested,
            this, &DeviceManager::dispatchAgv);
    connect(m_lineOrch, &LineOrchestrator::lineLog,
            this, &DeviceManager::logMessage);
    connect(m_lineOrch, &LineOrchestrator::lineError, this, [this](const QString &msg) {
        emit logMessage(QStringLiteral("[整线错误] %1").arg(msg));
    });
```

- [ ] **步骤 4.3：编译验证 + Commit**

```bash
git add src/devicemanager.h src/devicemanager.cpp
git commit -m "feat: DeviceManager 持有 LineOrchestrator 并接线派单"
```

---

## 任务 5：MainWindow 工具栏接入整线流程

**文件：**
- 修改：`src/mainwindow.h`
- 修改：`src/mainwindow.cpp`

- [ ] **步骤 5.1：mainwindow.h 槽声明**

private slots 区（`onSdkStepChanged` 附近）加：

```cpp
    void onLineStepChanged(int stepIdx);
    void onLineStarted();
    void onLineFinished();
    void onLineStopped();
    void onLineError(const QString &reason);
```

- [ ] **步骤 5.2：mainwindow.cpp include 与构造接线**

include 区加：

```cpp
#include "lineorchestrator.h"
```

构造函数华沿 SDK 信号连接块（mainwindow.cpp:130 的 `connect(hs, &HuayanScheduler::stepChanged, ...)` 之后）追加编排器接线。先取编排器指针：

```cpp
    LineOrchestrator *lo = m_devMgr->lineOrchestrator();
    connect(lo, &LineOrchestrator::stepChanged,
            this, &MainWindow::onLineStepChanged);
    connect(lo, &LineOrchestrator::lineStarted,
            this, &MainWindow::onLineStarted);
    connect(lo, &LineOrchestrator::lineFinished,
            this, &MainWindow::onLineFinished);
    connect(lo, &LineOrchestrator::lineStopped,
            this, &MainWindow::onLineStopped);
    connect(lo, &LineOrchestrator::lineError,
            this, &MainWindow::onLineError);
```

并断开流程图被 HuayanScheduler::stepChanged 驱动（避免双驱动）：把 mainwindow.cpp:130-131 的 `connect(hs, &HuayanScheduler::stepChanged, this, &MainWindow::onSdkStepChanged);` **删除**（机械臂细分步不再驱动整线流程图；其细节仍进日志）。

- [ ] **步骤 5.3：onStart / onStop 改走编排器**

`onStart()`（mainwindow.cpp:809）改为：

```cpp
void MainWindow::onStart()
{
    m_btnStart->setEnabled(false); // 连接为阻塞调用，先禁用防止窗口期重复点击
    DeviceManager::Config cfg;
    buildConfig(cfg);
    m_devMgr->setConfig(cfg);
    m_devMgr->applyConfig();                       // 重连 AGV Modbus + 更新视觉 URL
    m_devMgr->lineOrchestrator()->start();         // 整线流程
}
```

`onStop()`（mainwindow.cpp:819）改为：

```cpp
void MainWindow::onStop()
{
    m_devMgr->lineOrchestrator()->stop();          // 内部已含取消 AGV + 停机械臂
    m_flow->setActiveStep(-1);
    m_lblStep->setText(QStringLiteral("已停止"));
    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);
}
```

- [ ] **步骤 5.4：新增编排器响应槽**

在 `onSdkStepChanged`（mainwindow.cpp:1027）定义之后加：

```cpp
/// 整线步骤推进：高亮流程图节点并更新工具栏当前步骤标签
void MainWindow::onLineStepChanged(int stepIdx)
{
    m_flow->setActiveStep(stepIdx);
    if (stepIdx >= 0 && stepIdx < m_flow->steps().size())
        m_lblStep->setText(m_flow->steps()[stepIdx].name);
}

void MainWindow::onLineStarted()
{
    m_btnStart->setEnabled(false);
    m_btnStop->setEnabled(true);
}

void MainWindow::onLineFinished()
{
    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);
    m_flow->setActiveStep(-1);
    m_lblCycle->setText(QString::number(++m_cycleCount));
    m_lblStep->setText(QStringLiteral("完成"));
}

void MainWindow::onLineStopped()
{
    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);
    m_flow->setActiveStep(-1);
    m_lblStep->setText(QStringLiteral("已停止"));
}

void MainWindow::onLineError(const QString &reason)
{
    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);
    m_flow->setActiveStep(-1);
    m_lblStep->setText(QStringLiteral("错误"));
    log(QStringLiteral("⚠ 整线流程中止：%1").arg(reason));
}
```

注意 `onSdkStepChanged` 函数保留（华沿 SDK 调试面板的"启动取料"按钮仍可能用到它驱动流程图）——但由于步骤 5.2 删除了它的 connect，它现在无人调用。为避免留死函数：保留 `onSdkStepChanged` 实现但确认无连接，或在本步将其声明与定义一并删除。**采用删除**：删 mainwindow.h 中 `onSdkStepChanged` 声明、删 mainwindow.cpp:1027 的函数定义（华沿调试面板的单独取料不驱动整线流程图，符合职责分离）。

- [ ] **步骤 5.5：编译验证**

运行顶部命令。预期 Built target，无 error。若报 `onSdkStepChanged` 未定义/未声明，确认 5.4 的删除是声明与定义成对完成。

- [ ] **步骤 5.6：Commit**

```bash
git add src/mainwindow.h src/mainwindow.cpp
git commit -m "feat: 工具栏开始/停止改驱动整线流程编排器"
```

---

## 任务 6：流程验证

**文件：** 无代码修改。

- [ ] **步骤 6.1：离线接线验证（机械臂/AGV 均未连）**

启动 `build/Desktop_Qt_6_8_3_MinGW_64_bit-Debug/wh-robot-visual.exe`，不连任何设备，点工具栏"开始"。预期日志：`[整线] 流程启动，初始检查` → `初检失败：AGV 当前站 0 ≠ 待机站 1`（因无监控数据 curStation=0），按钮恢复、状态显"错误"。验证编排器启动→初检→中止→按钮恢复链路正确。

- [ ] **步骤 6.2：mock AGV 半程验证（AGV 部分）**

```bash
python scripts/mock_agv_server.py 1502 &
```

临时把 `DeviceManager::Config::agvPort` 改 1502（**验证后还原 502，不提交此改动**），改 mock 让其 curStation 初值=1（编辑 mock_agv_server.py 的 `input_regs[33]=1`，临时）。连 AGV(127.0.0.1)，点开始：预期初检过 → 机械臂 startStow 因未连 SDK 同步 emit stageError → 整线 abort 报"机械臂错误"。验证初检通过分支 + AGV 监控接入正确。**验证后还原 agvPort 与 mock 改动。**

- [ ] **步骤 6.3：真机整线验收（主，由用户执行）**

4 站点现场，机械臂 SDK + AGV 均连。确保示教器已示教 `func_yun_xing_zhong` / `func_daoliao_1_point` / `func_daoliao`，AGV 停在站1。点开始，观察：
1. 流程图依次高亮 AGV→取料站 / 机械臂取料 / AGV→倒料站 / 倒料 / 回站待机
2. 日志依次出现 0-7 步
3. 跑完回站1，循环计数 +1，按钮恢复
4. 测一次中途"停止"（AGV 行走中点停，确认 AGV 停车、机械臂停、按钮恢复）
5. 测一次故障（如开始时 AGV 不在站1，确认初检报错就地停）

- [ ] **步骤 6.4：确认无残留临时改动**

`git status` 确认 agvPort=502、mock 未改、无未预期改动。

---

## 任务 7：CHANGELOG 与收尾

**文件：**
- 修改：`changelog/CHANGELOG.md`

- [ ] **步骤 7.1：CHANGELOG 头部追加**

```markdown
## 2026-06-12 | 整线流程小规模可行性试测（feature/line-flow-pilot）

### 新增
- `LineOrchestrator`：整线流程编排器，单次串起 AGV取料站→机械臂取料→收姿态→AGV倒料站→倒料→收姿态→AGV回站1；纯协调层，AGV 移动调 `dispatchAgv` 等 `arrived`，机械臂调 `startStageOne`/`startStow`/`startUnload` 等 `stageCompleted`；任一步出错就地停机（取消 AGV + 停机械臂）
- `HuayanScheduler`：新增 `Stage::Stow`（func_yun_xing_zhong 收姿态）、`Stage::Unload`（func_daoliao_1_point → func_daoliao），`startStow`/`startUnload` 公开方法
- `DeviceManager`：持有 `LineOrchestrator`，接线 `agvDispatchRequested → dispatchAgv`

### 变更
- 工具栏开始/停止改驱动整线流程编排器（原为单独机械臂取料 startStageOne）
- 流程图节点改为整线视角：AGV→取料站 / 机械臂取料 / AGV→倒料站 / 倒料 / 回站待机
- 初始检查：AGV 须在待机站1（真检测 curStation）、机械臂强制收姿态

### 已知限制
- 单次单站点试测，无连续循环、无多站点；初检不过不自动归位（人工处理）
- 机械臂姿态用"强制收姿态"替代真实检测
```

- [ ] **步骤 7.2：Commit + 推送**

```bash
git add changelog/CHANGELOG.md
git commit -m "docs: CHANGELOG 记录整线流程试测"
git push -u origin feature/line-flow-pilot
```

PR（gh 未安装，手动）：https://github.com/ssrxiaoge2/robot_visual/pull/new/feature/line-flow-pilot

---

## 自检记录

- **规格覆盖度**：§流程→任务1+2；§1 LineOrchestrator→任务2；§2 HuayanScheduler新方法→任务1；§3 接线→任务4+5；§4 流程图节点→任务3；§5 错误处理→任务2（abort/haltDevices）、测试→任务6；Git→任务7。无遗漏。
- **占位符扫描**：无 TODO/待定；所有代码步骤含完整代码。`requestAgvDispatch` 是 header 声明的占位 slot（实际派单走 `agvDispatchRequested` 信号→DeviceManager），已在 cpp 注明，非缺陷。
- **类型一致性**：`LineState` 9 态在 enterState/onArmCompleted/onAgvArrived/emitStepForState 一致使用；`AgvMonitorData.curStation`/`.navStatus` 与 agvcontroller.h 定义一致；`startStow`/`startUnload`/`stageCompleted`/`stageError`/`arrived`/`monitorUpdated`/`dispatchAgv` 均与既有或任务1新增签名一致；`emitStepForState` 的 0-4 与任务3 kDefaultSteps 五节点顺序一致。
- **一处设计权衡**：`LineOrchestrator` 通过 `agvDispatchRequested` 信号请求派单（而非直接持 DeviceManager 指针），避免编排器反向依赖 DeviceManager，保持单向依赖；DeviceManager 在任务4 接此信号。自检时已移除冗余的 `requestAgvDispatch` 占位 slot。
