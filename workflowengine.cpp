#include "workflowengine.h"
#include "robotcontroller.h"
#include "agvcontroller.h"

// ── 步骤元数据 ──────────────────────────────────────────────
const QList<WorkflowStep> WorkflowEngine::kSteps = {
    {QStringLiteral("相机拍照"),   QStringLiteral("Orbbec 3D"),  QColor(65,  130, 220)},
    {QStringLiteral("机器人抓取"), QStringLiteral("华沿 Modbus"), QColor(220, 160,  50)},
    {QStringLiteral("AGV行走"),    QStringLiteral("仙工 Modbus"), QColor(60,  180, 120)},
    {QStringLiteral("机器人放料"), QStringLiteral("翻转卸料"),    QColor(180, 110, 210)},
    {QStringLiteral("复位等待"),   QStringLiteral("下一轮"),      QColor(130, 130, 130)},
};

// ── 构造 ────────────────────────────────────────────────────

WorkflowEngine::WorkflowEngine(QObject *parent)
    : QObject(parent)
{
    // 轮询定时器：300 ms，向硬件读取状态字
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(300);
    connect(m_pollTimer, &QTimer::timeout, this, &WorkflowEngine::onPollTick);

    // 仿真/超时定时器：单次触发
    m_stepTimer = new QTimer(this);
    m_stepTimer->setSingleShot(true);
    connect(m_stepTimer, &QTimer::timeout, this, &WorkflowEngine::onStepTimeout);

    // 步骤超时保护（真实模式下用）
    m_timeoutTimer = new QTimer(this);
    m_timeoutTimer->setSingleShot(true);
    m_timeoutTimer->setInterval(30000); // 30 s
    connect(m_timeoutTimer, &QTimer::timeout, this, [this]() {
        emit engineError(QString("[超时] 步骤 %1 等待硬件响应超过 30 秒，停止运行")
                             .arg(m_currentStep));
        stop();
    });
}

// ── 硬件控制器注入 ───────────────────────────────────────────

void WorkflowEngine::setRobotController(RobotController *ctrl)
{
    if (m_robotCtrl) {
        disconnect(m_robotCtrl, nullptr, this, nullptr);
    }
    m_robotCtrl = ctrl;
    if (ctrl) {
        connect(ctrl, &RobotController::inputRegistersRead,
                this, &WorkflowEngine::onRobotInputRegistersRead);
        connect(ctrl, &RobotController::writeCompleted,
                this, &WorkflowEngine::onRobotWriteCompleted);
    }
}

void WorkflowEngine::setAgvController(AgvController *ctrl)
{
    if (m_agvCtrl) {
        disconnect(m_agvCtrl, nullptr, this, nullptr);
    }
    m_agvCtrl = ctrl;
    if (ctrl) {
        connect(ctrl, &AgvController::statusRead,
                this, &WorkflowEngine::onAgvStatusRead);
    }
}

// ── 公开控制接口 ────────────────────────────────────────────

void WorkflowEngine::start(int simIntervalMs)
{
    m_simIntervalMs = simIntervalMs;
    m_running       = true;
    m_cycleCount    = 0;
    m_station       = 1;
    m_currentStep   = -1;

    emit started();
    enterStep(0);
}

void WorkflowEngine::stop()
{
    m_running = false;
    m_state   = EngineState::Idle;
    m_currentStep = -1;
    m_pendingCmd  = PendingCmd::None;
    m_hasCoords   = false;

    m_pollTimer->stop();
    m_stepTimer->stop();
    m_timeoutTimer->stop();

    // 向机器人发复位命令（若已连接）
    if (robotReady())
        m_robotCtrl->writeRegister(REG_CMD_ID, CMD_RESET);

    emit stopped();
}

void WorkflowEngine::singleStep()
{
    if (m_running) return;

    if (m_currentStep < 0) {
        m_cycleCount++;
        m_station = (m_station % 12) + 1;
        enterStep(0);
    } else {
        // 当前步骤已激活，手动推进到下一步
        int next = m_currentStep + 1;
        if (next >= kSteps.size())
            completeCycle();
        else
            enterStep(next);
    }
}

void WorkflowEngine::setInterval(int ms)
{
    m_simIntervalMs = ms;
}

void WorkflowEngine::setCoordinates(const QList<quint16> &coords)
{
    if (coords.size() != 6) {
        emit engineError(QStringLiteral("[相机] 坐标数据错误：期望 6 个寄存器值"));
        return;
    }
    m_coords    = coords;
    m_hasCoords = true;

    // 若当前正在等待相机数据，立即进入步骤 1
    if (m_state == EngineState::Step0_Camera) {
        m_stepTimer->stop();
        emit stepLog(QStringLiteral("[相机] 坐标数据已就绪，开始机器人抓取"));
        enterStep1();
    }
}

// ── 步骤入口 ────────────────────────────────────────────────

void WorkflowEngine::enterStep(int idx)
{
    m_currentStep = idx;
    emit stepActivated(idx, m_station, m_cycleCount);

    switch (idx) {
    case 0: enterStep0(); break;
    case 1: enterStep1(); break;
    case 2: enterStep2(); break;
    case 3: enterStep3(); break;
    case 4: enterStep4(); break;
    }
}

void WorkflowEngine::enterStep0()
{
    m_state     = EngineState::Step0_Camera;
    m_hasCoords = false;
    m_coords.clear();

    emit stepLog(QString("[步骤0] 工位%1 触发相机拍照").arg(m_station));
    emit requestCameraCapture();

    // 仿真：等待 m_simIntervalMs 后自动用零坐标推进
    m_stepTimer->start(m_simIntervalMs);
}

void WorkflowEngine::enterStep1()
{
    emit stepLog(QStringLiteral("[步骤1] 写坐标到机器人寄存器 901-906"));

    if (robotReady()) {
        // 真实模式：先写 6 个坐标寄存器，writeCompleted 后再写 CmdID=8
        m_state      = EngineState::Step1_WriteCoords;
        m_pendingCmd = PendingCmd::Grab;

        // 确保坐标列表长度为 6（相机未给数据则用 0）
        QList<quint16> coords = m_hasCoords ? m_coords : QList<quint16>(6, 0);
        m_robotCtrl->writeRegisters(REG_COORD_START, coords);

        startPollAndTimeout();
    } else {
        // 仿真模式
        m_state = EngineState::Step1_Grabbing;
        emit stepLog(QStringLiteral("[步骤1][仿真] 机器人未连接，模拟抓取等待"));
        m_stepTimer->start(m_simIntervalMs);
        startPollAndTimeout();
    }
}

void WorkflowEngine::enterStep2()
{
    m_state = EngineState::Step2_AgvMoving;
    emit stepLog(QString("[步骤2] AGV 前往工位 %1").arg(m_station));

    if (agvReady()) {
        m_agvCtrl->sendToStation(m_station);
        startPollAndTimeout();
    } else {
        emit stepLog(QStringLiteral("[步骤2][仿真] AGV 未连接，模拟行走等待"));
        m_stepTimer->start(m_simIntervalMs);
    }
}

void WorkflowEngine::enterStep3()
{
    m_state      = EngineState::Step3_Unloading;
    m_pendingCmd = PendingCmd::None;
    emit stepLog(QStringLiteral("[步骤3] 机器人翻转卸料"));

    if (robotReady()) {
        m_robotCtrl->writeRegister(REG_CMD_ID, CMD_FLIP);
        startPollAndTimeout();
    } else {
        emit stepLog(QStringLiteral("[步骤3][仿真] 机器人未连接，模拟卸料等待"));
        m_stepTimer->start(m_simIntervalMs);
    }
}

void WorkflowEngine::enterStep4()
{
    m_state      = EngineState::Step4_Resetting;
    m_pendingCmd = PendingCmd::None;
    emit stepLog(QStringLiteral("[步骤4] 机器人复位回原点"));

    if (robotReady()) {
        m_robotCtrl->writeRegister(REG_CMD_ID, CMD_RESET);
        startPollAndTimeout();
    } else {
        emit stepLog(QStringLiteral("[步骤4][仿真] 机器人未连接，模拟复位等待"));
        m_stepTimer->start(m_simIntervalMs);
    }
}

void WorkflowEngine::completeCycle()
{
    m_pollTimer->stop();
    m_stepTimer->stop();
    m_timeoutTimer->stop();

    m_cycleCount++;
    m_station = (m_station % 12) + 1;

    emit stepLog(QString("[完成] 第 %1 轮结束，下一工位 %2")
                     .arg(m_cycleCount).arg(m_station));

    if (m_running)
        enterStep(0);
    else
        m_state = EngineState::Idle;
}

// ── 辅助函数 ────────────────────────────────────────────────

bool WorkflowEngine::robotReady() const
{
    return m_robotCtrl && m_robotCtrl->isConnected();
}

bool WorkflowEngine::agvReady() const
{
    return m_agvCtrl && m_agvCtrl->isConnected();
}

void WorkflowEngine::startPollAndTimeout()
{
    m_pollTimer->start();
    m_timeoutTimer->start();
}

// ── 定时器槽 ────────────────────────────────────────────────

void WorkflowEngine::onPollTick()
{
    // 向硬件发起状态轮询（仅在硬件连接时有效；仿真路径走 onStepTimeout）
    switch (m_state) {
    case EngineState::Step1_Grabbing:
    case EngineState::Step3_Unloading:
    case EngineState::Step4_Resetting:
        if (robotReady())
            m_robotCtrl->readInputRegisters(REG_ROBOT_STATUS, 1);
        break;
    case EngineState::Step2_AgvMoving:
        if (agvReady())
            m_agvCtrl->readStatusRegister();
        break;
    default:
        break;
    }
}

void WorkflowEngine::onStepTimeout()
{
    // 仿真模式下的自动推进逻辑（硬件连接时此定时器被停止，不会触发）
    switch (m_state) {
    case EngineState::Step0_Camera:
        emit stepLog(QStringLiteral("[步骤0][仿真] 相机超时，使用零坐标继续"));
        m_coords    = QList<quint16>(6, 0);
        m_hasCoords = true;
        enterStep1();
        break;
    case EngineState::Step1_Grabbing:
        emit stepLog(QStringLiteral("[步骤1][仿真] 抓取完成（模拟）"));
        m_pollTimer->stop();
        m_timeoutTimer->stop();
        enterStep(2);
        break;
    case EngineState::Step2_AgvMoving:
        emit stepLog(QStringLiteral("[步骤2][仿真] AGV 到位（模拟）"));
        enterStep(3);
        break;
    case EngineState::Step3_Unloading:
        emit stepLog(QStringLiteral("[步骤3][仿真] 卸料完成（模拟）"));
        m_pollTimer->stop();
        m_timeoutTimer->stop();
        enterStep(4);
        break;
    case EngineState::Step4_Resetting:
        emit stepLog(QStringLiteral("[步骤4][仿真] 复位完成（模拟）"));
        m_pollTimer->stop();
        m_timeoutTimer->stop();
        completeCycle();
        break;
    default:
        break;
    }
}

// ── 硬件回调槽 ──────────────────────────────────────────────

void WorkflowEngine::onRobotWriteCompleted()
{
    // 坐标写入完成后，发送 CmdID 命令（顺序写保护）
    if (m_pendingCmd == PendingCmd::Grab && robotReady()) {
        m_pendingCmd = PendingCmd::None;
        m_state      = EngineState::Step1_Grabbing;
        emit stepLog(QString("[步骤1] 坐标已写入 → 发送 CmdID=%1 启动抓取").arg(CMD_GRAB));
        m_robotCtrl->writeRegister(REG_CMD_ID, CMD_GRAB);
    }
}

void WorkflowEngine::onRobotInputRegistersRead(int startAddr, const QList<quint16> &values)
{
    if (startAddr != REG_ROBOT_STATUS || values.isEmpty()) return;

    const quint16 status = values[0];

    switch (m_state) {
    case EngineState::Step1_Grabbing:
        if (status == STATUS_GRAB_DONE) {
            emit stepLog(QStringLiteral("[步骤1] Input1132 = 抓取完成 ✓"));
            m_pollTimer->stop();
            m_stepTimer->stop();
            m_timeoutTimer->stop();
            enterStep(2);
        }
        break;
    case EngineState::Step3_Unloading:
        if (status == STATUS_UNLOAD_DONE) {
            emit stepLog(QStringLiteral("[步骤3] Input1132 = 卸料完成 ✓"));
            m_pollTimer->stop();
            m_stepTimer->stop();
            m_timeoutTimer->stop();
            enterStep(4);
        }
        break;
    case EngineState::Step4_Resetting:
        if (status == STATUS_IDLE) {
            emit stepLog(QStringLiteral("[步骤4] Input1132 = 空闲就绪 ✓"));
            m_pollTimer->stop();
            m_stepTimer->stop();
            m_timeoutTimer->stop();
            completeCycle();
        }
        break;
    default:
        break;
    }
}

void WorkflowEngine::onAgvStatusRead(AgvController::AgvStatus status)
{
    if (m_state != EngineState::Step2_AgvMoving) return;

    if (status == AgvController::AgvStatus::Arrived) {
        emit stepLog(QString("[步骤2] AGV 到位（工位 %1）✓").arg(m_station));
        m_pollTimer->stop();
        m_stepTimer->stop();
        m_timeoutTimer->stop();
        enterStep(3);
    } else if (status == AgvController::AgvStatus::Fault) {
        emit engineError(QStringLiteral("[步骤2] AGV 报告故障，停止运行"));
        stop();
    }
}
