/**
 * @file workflowengine.cpp
 * @brief WorkflowEngine 工作流状态机实现
 *
 * ── 状态转换图 ────────────────────────────────────────────────
 *
 *   start()
 *     │
 *     ▼
 *   Step0_SendCmd  （写 CmdID=8，轮询 Input1132）
 *     │ 硬件：Input1132==1 (REQ_PHOTO)
 *     │ 仿真：simTimer 超时
 *     ▼
 *   Step0_WaitVision（触发 fetchInference，等待 setCoordinates 回调）
 *     │ 收到坐标 / 视觉超时（用零坐标）
 *     ▼
 *   Step1_WriteCoords（写 Holding 901-906）
 *     │ writeCompleted()
 *     ▼
 *   Step1_Grabbing （轮询 Input 1132）
 *                                                                   │ status==GRAB_DONE
 *                                                                   ▼
 *                                                             Step2_AgvMoving
 *                                                             （写 AGV [4x]00001）
 *                                                             （轮询 AGV [3x]00007-00009）
 *                                                                   │ status==ARRIVED
 *                                                                   ▼
 *                                                             Step3_Unloading
 *                                                             （写 Holding 900=FLIP）
 *                                                             （轮询 Input 1132）
 *                                                                   │ status==UNLOAD_DONE
 *                                                                   ▼
 *                                                             Step4_Resetting
 *                                                             （写 Holding 900=RESET）
 *                                                             （轮询 Input 1132）
 *                                                                   │ status==IDLE
 *                                                                   ▼
 *                                                             completeCycle()
 *                                                             cycleCount++, station++
 *                                                                   │ m_running==true
 *                                                                   └──────► Step0_Camera
 *
 * ── 双模式运行 ────────────────────────────────────────────────
 *   硬件模式（robot/agv 已连接）：
 *     m_pollTimer（300ms）定期轮询 Modbus 状态，状态满足后推进
 *     m_timeoutTimer（30s）超时保护，超时则停止并报错
 *
 *   仿真模式（硬件未连接）：
 *     m_stepTimer（simIntervalMs，单次）模拟步骤耗时后自动推进
 *     m_pollTimer 仍运行但 onPollTick() 内判断未连接故无实际操作
 */

#include "workflowengine.h"
#include "robotcontroller.h"
#include "agvcontroller.h"
#include "visionclient.h"

// ── 步骤元数据 ──────────────────────────────────────────────

const QList<WorkflowStep> WorkflowEngine::kSteps = {
    {QStringLiteral("相机拍照"),   QStringLiteral("Orbbec 3D"),  QColor(65,  130, 220)},
    {QStringLiteral("机器人抓取"), QStringLiteral("华沿 Modbus"), QColor(220, 160,  50)},
    {QStringLiteral("AGV行走"),    QStringLiteral("仙工 Modbus"), QColor(60,  180, 120)},
    {QStringLiteral("机器人放料"), QStringLiteral("翻转卸料"),    QColor(180, 110, 210)},
    {QStringLiteral("复位等待"),   QStringLiteral("下一轮"),      QColor(130, 130, 130)},
};

// ── 构造 ─────────────────────────────────────────────────────

WorkflowEngine::WorkflowEngine(QObject *parent)
    : QObject(parent)
{
    // ── 轮询定时器（300ms 周期）────────────────────────────
    // 硬件模式下：定期向 Robot/AGV 发起 Modbus 读请求，检查状态字
    // 仿真模式下：tick 内判断未连接，什么都不做（仿真依赖 m_stepTimer）
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(300);
    connect(m_pollTimer, &QTimer::timeout, this, &WorkflowEngine::onPollTick);

    // ── 仿真/超时步骤定时器（单次触发）───────────────────
    // 仿真模式：在步骤开始时以 simIntervalMs 启动，超时则自动推进
    // 硬件模式：不启动（推进依靠硬件状态回调）
    m_stepTimer = new QTimer(this);
    m_stepTimer->setSingleShot(true);
    connect(m_stepTimer, &QTimer::timeout, this, &WorkflowEngine::onStepTimeout);

    // ── 步骤超时保护（30s，硬件模式专用）──────────────────
    // 若某步骤 30s 内硬件无响应（卡死、断线），则停止流程并报错
    m_timeoutTimer = new QTimer(this);
    m_timeoutTimer->setSingleShot(true);
    m_timeoutTimer->setInterval(30000);
    connect(m_timeoutTimer, &QTimer::timeout, this, [this]() {
        emit engineError(QString("[超时] 步骤 %1 等待硬件响应超过 30 秒，停止运行")
                             .arg(m_currentStep));
        stop();
    });
}

// ── 硬件控制器注入 ───────────────────────────────────────────

/**
 * @brief 注入机械臂控制器
 *
 * 断开旧控制器的所有连接，再连接新控制器的信号。
 * 在 start() 之前调用，MainWindow 构造时执行一次即可。
 */
void WorkflowEngine::setRobotController(RobotController *ctrl)
{
    if (m_robotCtrl)
        disconnect(m_robotCtrl, nullptr, this, nullptr); // 断开旧连接，防止僵尸回调

    m_robotCtrl = ctrl;
    if (ctrl) {
        // Input 寄存器读取完成 → 检查状态字（FC04，读取机器人状态 Input 1132）
        connect(ctrl, &RobotController::inputRegistersRead,
                this, &WorkflowEngine::onRobotInputRegistersRead);
        // 寄存器写入完成 → 处理顺序写（先坐标后命令字）
        connect(ctrl, &RobotController::writeCompleted,
                this, &WorkflowEngine::onRobotWriteCompleted);
    }
}

/**
 * @brief 注入 AGV 控制器
 *
 * 逻辑同 setRobotController。
 */
void WorkflowEngine::setAgvController(AgvController *ctrl)
{
    if (m_agvCtrl)
        disconnect(m_agvCtrl, nullptr, this, nullptr);

    m_agvCtrl = ctrl;
    if (ctrl) {
        // AGV 状态读取完成 → 检查是否已到达目标站点（[3x]00009）
        connect(ctrl, &AgvController::statusRead,
                this, &WorkflowEngine::onAgvStatusRead);
    }
}

/**
 * @brief 注入视觉 HTTP 客户端
 *
 * 连接两个核心信号：
 *   coordinatesReady → setCoordinates()：有目标时注入坐标并推进 Step1
 *   noObjectDetected → 用零坐标继续（记录警告日志，不停止工作流）
 *
 * 断开旧客户端连接，避免重复注入时信号堆叠。
 */
void WorkflowEngine::setVisionClient(VisionHttpClient *client)
{
    if (m_visionClient)
        disconnect(m_visionClient, nullptr, this, nullptr);

    m_visionClient = client;
    if (client) {
        // 视觉推理成功 → 注入坐标（触发 Step0→Step1 转换）
        connect(client, &VisionHttpClient::coordinatesReady,
                this, &WorkflowEngine::setCoordinates);

        // 未检测到目标 → 警告日志 + 用零坐标继续
        connect(client, &VisionHttpClient::noObjectDetected, this, [this]() {
            emit stepLog(QStringLiteral("[Step0] ⚠ 未检测到目标，用零坐标继续"));
            setCoordinates(QList<quint16>(6, 0));
        });

        // 视觉服务网络错误 → 记录错误日志 + 用零坐标继续（不停止工作流）
        connect(client, &VisionHttpClient::errorOccurred, this, [this](const QString &msg) {
            emit engineError(msg);
            setCoordinates(QList<quint16>(6, 0));
        });
    }
}

// ── 公开控制接口 ─────────────────────────────────────────────

/**
 * @brief 启动自动循环
 * @param simIntervalMs 仿真模式下每个步骤的最长等待时间（ms）
 *
 * 重置所有计数器，从工位1第0步开始。
 */
void WorkflowEngine::start(int simIntervalMs)
{
    m_simIntervalMs = simIntervalMs;
    m_running       = true;
    m_cycleCount    = 0;
    m_station       = 1;
    m_currentStep   = -1;

    emit started();   // 通知 UI 更新按钮状态
    enterStep(0);     // 立即进入第0步
}

/**
 * @brief 停止自动循环
 *
 * 停止所有定时器，清理状态，若机器人已连接则发送复位命令。
 */
void WorkflowEngine::stop()
{
    const bool agvMoving = (m_state == EngineState::Step2_AgvMoving);

    m_running     = false;
    m_state       = EngineState::Idle;
    m_currentStep = -1;
    m_pendingCmd  = PendingCmd::None;
    m_hasCoords   = false;

    m_pollTimer->stop();
    m_stepTimer->stop();
    m_timeoutTimer->stop();

    // 若停止时 AGV 正在行走，取消其导航任务（[0x]00006=1），避免空跑
    if (agvMoving && agvReady())
        m_agvCtrl->cancelNavigation();

    // 向机器人发送复位命令（CmdID=0），让机械臂回到原点等待位置
    if (robotReady())
        m_robotCtrl->writeRegister(REG_CMD_ID, CMD_RESET);

    emit stopped();
}

/**
 * @brief 手动单步推进（仅在非运行状态下有效）
 *
 * 首次调用：从步骤0开始，工位递增
 * 再次调用：推进到下一步骤（跳过硬件等待，直接进入下一步）
 */
void WorkflowEngine::singleStep()
{
    if (m_running) return; // 自动循环运行中，单步不可用

    if (m_currentStep < 0) {
        // 尚未开始或已完成一轮：重置到步骤0，工位递增
        m_cycleCount++;
        m_station = (m_station % 12) + 1;
        enterStep(0);
    } else {
        // 跳过当前步骤的硬件等待，强制推进到下一步
        int next = m_currentStep + 1;
        if (next >= kSteps.size())
            completeCycle(); // 最后一步之后进入循环结算
        else
            enterStep(next);
    }
}

void WorkflowEngine::setInterval(int ms)
{
    m_simIntervalMs = ms; // 运行中也可动态调整仿真步进间隔
}

/**
 * @brief 接收相机坐标数据（由外部相机系统调用）
 * @param coords 6 个 quint16 值，对应 Holding 901-906（X/Y/Z/Rx/Ry/Rz）
 *
 * 若当前正处于 Step0_Camera 等待状态，收到数据后立即推进到 Step1。
 */
void WorkflowEngine::setCoordinates(const QList<quint16> &coords)
{
    if (coords.size() != 6) {
        emit engineError(QStringLiteral("[相机] 坐标数据错误：期望 6 个寄存器值"));
        return;
    }
    m_coords    = coords;
    m_hasCoords = true;

    if (m_state == EngineState::Step0_WaitVision) {
        m_stepTimer->stop();
        m_timeoutTimer->stop();
        emit stepLog(QStringLiteral("[视觉] 坐标数据已就绪，写入机器人寄存器 901-906"));
        enterStep(1);  // 通过 enterStep() 发出 stepActivated(1) 更新 UI
    }
    // 其他状态收到（延迟回调）：存储坐标，下次循环使用
}

// ── 步骤入口（统一分发）─────────────────────────────────────

/**
 * @brief 进入指定步骤
 *
 * 更新 m_currentStep，发出 stepActivated 信号（UI 更新），再分发到具体步骤函数。
 */
void WorkflowEngine::enterStep(int idx)
{
    m_currentStep = idx;
    emit stepActivated(idx, m_station, m_cycleCount); // 通知 UI 高亮对应步骤
    switch (idx) {
    case 0: enterStep0(); break;
    case 1: enterStep1(); break;
    case 2: enterStep2(); break;
    case 3: enterStep3(); break;
    case 4: enterStep4(); break;
    }
}

// ── 各步骤逻辑 ────────────────────────────────────────────────

/**
 * @brief Step 0 — 写抓取指令，等待机器人运动到拍照位，再触发视觉识别
 *
 * 真实模式（机器人已连接）：
 *   1. 写 Holding 900=CMD_GRAB(8)：机器人收到指令后运动到预设拍照等待位
 *   2. 300ms 轮询 Input 1132，直到 status==STATUS_REQ_PHOTO(1)
 *   3. 检测到 status=1 后切换到 Step0_WaitVision，触发视觉推理
 *   4. 视觉服务返回坐标后 setCoordinates() 被调用，推进到 Step1
 *
 * 仿真模式（机器人未连接）：
 *   stepTimer 超时后模拟机器人到达拍照位，切换到 Step0_WaitVision，
 *   再次超时后用零坐标推进（或由视觉服务异步返回坐标）
 */
void WorkflowEngine::enterStep0()
{
    m_state     = EngineState::Step0_SendCmd;
    m_hasCoords = false;
    m_coords.clear();

    emit stepLog(QString("[步骤0] 工位%1 发送抓取指令，等待机器人运动到拍照位").arg(m_station));

    if (robotReady()) {
        m_robotCtrl->writeRegister(REG_CMD_ID, CMD_GRAB);
        startPollAndTimeout();
        emit stepLog(QStringLiteral("[步骤0] 已写 CmdID=8，轮询 Input1132 等待 status=1"));
    } else {
        m_stepTimer->start(m_simIntervalMs);
        emit stepLog(QStringLiteral("[步骤0][仿真] 机器人未连接，模拟运动到拍照位"));
    }
}

/**
 * @brief Step 1 — 写坐标并等待机器人抓取完成
 *
 * 硬件模式：
 *   1. 写 Holding 901-906（坐标）→ 等待 writeCompleted()
 *   2. 坐标写完后进入 Step1_Grabbing，轮询 Input 1132 等待 status==GRAB_DONE(2)
 *   （注：CmdID=8 已在 Step0 写入，机器人收到坐标后自动完成抓取）
 *
 * 仿真模式：simTimer 超时后直接推进 Step 2
 */
void WorkflowEngine::enterStep1()
{
    emit stepLog(QStringLiteral("[步骤1] 写坐标到机器人寄存器 901-906"));

    if (robotReady()) {
        m_state      = EngineState::Step1_WriteCoords;
        m_pendingCmd = PendingCmd::WriteCoords;

        // 用相机提供的坐标，若无则用零坐标（仿真/测试场景）
        QList<quint16> coords = m_hasCoords ? m_coords : QList<quint16>(6, 0);
        m_robotCtrl->writeRegisters(REG_COORD_START, coords);
        startPollAndTimeout(); // 启动轮询 + 30s 超时保护
    } else {
        // 仿真模式
        m_state = EngineState::Step1_Grabbing;
        emit stepLog(QStringLiteral("[步骤1][仿真] 机器人未连接，模拟抓取等待"));
        m_stepTimer->start(m_simIntervalMs);
        startPollAndTimeout();
    }
}

/**
 * @brief Step 2 — AGV 行走
 *
 * 硬件模式：写目标站点 id 到 AGV [4x]00001，轮询 [3x]00009 直到 Arrived(4)
 * 仿真模式：simTimer 超时后直接推进 Step 3
 */
void WorkflowEngine::enterStep2()
{
    m_state = EngineState::Step2_AgvMoving;
    emit stepLog(QString("[步骤2] AGV 前往站点 %1").arg(m_station));

    if (agvReady()) {
        m_agvCtrl->sendToStation(m_station); // 写目标站点，触发路径导航
        startPollAndTimeout();               // 开始轮询 AGV 导航状态
    } else {
        emit stepLog(QStringLiteral("[步骤2][仿真] AGV 未连接，模拟行走等待"));
        m_stepTimer->start(m_simIntervalMs);
    }
}

/**
 * @brief Step 3 — 机器人放料（翻转卸料）
 *
 * 硬件模式：写 Holding 900=CMD_FLIP，轮询 Input 1132 直到 UNLOAD_DONE
 * 仿真模式：simTimer 超时后直接推进 Step 4
 */
void WorkflowEngine::enterStep3()
{
    m_state      = EngineState::Step3_Unloading;
    m_pendingCmd = PendingCmd::None; // 本步骤无顺序写，清空标记
    emit stepLog(QStringLiteral("[步骤3] 机器人翻转卸料"));

    if (robotReady()) {
        m_robotCtrl->writeRegister(REG_CMD_ID, CMD_FLIP);
        startPollAndTimeout();
    } else {
        emit stepLog(QStringLiteral("[步骤3][仿真] 机器人未连接，模拟卸料等待"));
        m_stepTimer->start(m_simIntervalMs);
    }
}

/**
 * @brief Step 4 — 复位等待
 *
 * 硬件模式：写 Holding 900=CMD_RESET，轮询 Input 1132 直到 IDLE
 * 仿真模式：simTimer 超时后调用 completeCycle()
 */
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

/**
 * @brief 一轮结束处理
 *
 * 更新循环计数和工位编号（12 工位循环），
 * 若引擎仍在运行则立即进入下一轮 Step0，否则进入 Idle。
 */
void WorkflowEngine::completeCycle()
{
    m_pollTimer->stop();
    m_stepTimer->stop();
    m_timeoutTimer->stop();

    m_cycleCount++;                            // 完成轮数 +1
    m_station = (m_station % 12) + 1;         // 工位 1→2→...→12→1 循环

    emit stepLog(QString("[完成] 第 %1 轮结束，下一工位 %2")
                     .arg(m_cycleCount).arg(m_station));

    if (m_running)
        enterStep(0);  // 自动循环：立即开始下一轮
    else
        m_state = EngineState::Idle; // 单步模式：回到空闲
}

// ── 辅助函数 ─────────────────────────────────────────────────

/// 机械臂就绪：控制器非空且 Modbus 已连接
bool WorkflowEngine::robotReady() const
{
    return m_robotCtrl && m_robotCtrl->isConnected();
}

/// AGV 就绪：控制器非空且 Modbus 已连接
bool WorkflowEngine::agvReady() const
{
    return m_agvCtrl && m_agvCtrl->isConnected();
}

/// 同时启动轮询定时器和 30s 超时保护定时器
void WorkflowEngine::startPollAndTimeout()
{
    m_pollTimer->start();    // 300ms 周期轮询
    m_timeoutTimer->start(); // 30s 超时保护
}

// ── 定时器槽 ─────────────────────────────────────────────────

/**
 * @brief 轮询定时器回调（每 300ms）
 *
 * 根据当前状态向相应设备发起 Modbus 读请求。
 * 硬件未连接时 readXxx() 内部直接 return，无实际操作。
 */
void WorkflowEngine::onPollTick()
{
    switch (m_state) {
    case EngineState::Step0_SendCmd:   // 等待机器人到达拍照位（Input1132==1）
    case EngineState::Step1_Grabbing:  // 等待抓取完成（Input1132==2）
    case EngineState::Step3_Unloading: // 等待卸料完成（Input1132==3）
    case EngineState::Step4_Resetting: // 等待复位完成（Input1132==0）
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

/**
 * @brief 步骤超时定时器回调
 *
 * 仿真模式：充当每步的"虚拟完成"信号，自动推进到下一步。
 * 真实模式：视觉等待阶段（Step0_WaitVision）超时时也会触发，用零坐标继续。
 */
void WorkflowEngine::onStepTimeout()
{
    switch (m_state) {
    case EngineState::Step0_SendCmd:
        // 仿真：机器人"已运动到拍照位"，切换到视觉等待阶段
        emit stepLog(QStringLiteral("[步骤0][仿真] 机器人到达拍照位，触发视觉识别"));
        m_state = EngineState::Step0_WaitVision;
        if (m_visionClient && m_visionClient->isConfigured()) {
            m_visionClient->fetchInference();
        } else {
            emit requestCameraCapture();
        }
        m_stepTimer->start(m_simIntervalMs); // 视觉响应超时备用
        break;
    case EngineState::Step0_WaitVision:
        // 视觉服务超时（仿真降级或真实网络超时）：用零坐标继续
        emit stepLog(QStringLiteral("[步骤0] 视觉超时，使用零坐标继续"));
        setCoordinates(QList<quint16>(6, 0));
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

// ── 硬件回调槽 ───────────────────────────────────────────────

/**
 * @brief 机械臂寄存器写入完成回调
 *
 * Step1 进入时写坐标（FC16，Holding 901-906），设 m_pendingCmd=WriteCoords；
 * 写完后切换到 Step1_Grabbing，由轮询等待 Input1132==2。
 * 注：CmdID=8 已在 Step0 写入，机器人拿到坐标后自行完成抓取，无需重复写命令字。
 */
void WorkflowEngine::onRobotWriteCompleted()
{
    if (m_pendingCmd == PendingCmd::WriteCoords && robotReady()) {
        m_pendingCmd = PendingCmd::None;
        m_state      = EngineState::Step1_Grabbing;
        emit stepLog(QStringLiteral("[步骤1] 坐标已写入寄存器 901-906，轮询等待抓取完成"));
    }
}

/**
 * @brief 机械臂 Input 寄存器读取完成回调
 *
 * 过滤非 1132 地址的结果（debug 面板可能也触发 inputRegistersRead），
 * 根据当前状态检查状态字是否达到期望值，满足则推进到下一步。
 */
void WorkflowEngine::onRobotInputRegistersRead(int startAddr, const QList<quint16> &values)
{
    // 只处理机器人状态字寄存器的读取结果
    if (startAddr != REG_ROBOT_STATUS || values.isEmpty()) return;

    const quint16 status = values[0];

    switch (m_state) {
    case EngineState::Step0_SendCmd:
        if (status == STATUS_REQ_PHOTO) {   // 状态字 == 1：机器人到达拍照位
            emit stepLog(QStringLiteral("[步骤0] Input1132=1 机器人到达拍照位，触发视觉识别"));
            m_pollTimer->stop();
            m_timeoutTimer->stop();
            m_state = EngineState::Step0_WaitVision;
            if (m_visionClient && m_visionClient->isConfigured()) {
                m_visionClient->fetchInference();
                m_timeoutTimer->start(); // 重启 30s，等待视觉响应
            } else {
                emit requestCameraCapture();
                m_stepTimer->start(m_simIntervalMs);
            }
        }
        break;
    case EngineState::Step1_Grabbing:
        if (status == STATUS_GRAB_DONE) {   // 状态字 == 2：抓取完成
            emit stepLog(QStringLiteral("[步骤1] Input1132=2 抓取完成 ✓"));
            m_pollTimer->stop();
            m_stepTimer->stop();
            m_timeoutTimer->stop();
            enterStep(2);
        }
        break;
    case EngineState::Step3_Unloading:
        if (status == STATUS_UNLOAD_DONE) { // 状态字 == 3：卸料完成
            emit stepLog(QStringLiteral("[步骤3] Input1132=3 卸料完成 ✓"));
            m_pollTimer->stop();
            m_stepTimer->stop();
            m_timeoutTimer->stop();
            enterStep(4);
        }
        break;
    case EngineState::Step4_Resetting:
        if (status == STATUS_IDLE) {        // 状态字 == 0：空闲就绪
            emit stepLog(QStringLiteral("[步骤4] Input1132=0 复位完成 ✓"));
            m_pollTimer->stop();
            m_stepTimer->stop();
            m_timeoutTimer->stop();
            completeCycle();
        }
        break;
    default:
        break; // 其他状态收到读取结果，忽略
    }
}

/**
 * @brief AGV 状态读取完成回调（[3x]00009 导航状态 + [3x]00007 当前导航站点）
 *
 * 仅在 Step2_AgvMoving 状态下处理。
 * Arrived(4) 且站点匹配 → 推进到 Step3
 * Failed(5)/Canceled(6)/Timeout(7) → 立即停止所有定时器，
 *              发出 agvFaultDetected() 供 UI 更新指示灯，
 *              调用 stop()（内部写 CmdID=0 复位机械臂，防止悬臂悬停）
 */
void WorkflowEngine::onAgvStatusRead(AgvController::NavStatus status, int navStation)
{
    if (m_state != EngineState::Step2_AgvMoving) return;

    switch (status) {
    case AgvController::NavStatus::Arrived:
        // 站点不匹配说明读到的是上一单残留的"到达"，等待本单状态刷新
        if (navStation != m_station) break;
        emit stepLog(QString("[步骤2] AGV 到达站点 %1 ✓").arg(m_station));
        m_pollTimer->stop();
        m_stepTimer->stop();
        m_timeoutTimer->stop();
        enterStep(3);
        break;
    case AgvController::NavStatus::Failed:
    case AgvController::NavStatus::Canceled:
    case AgvController::NavStatus::Timeout:
        m_pollTimer->stop();
        m_stepTimer->stop();
        m_timeoutTimer->stop();
        emit engineError(QString("[步骤2] AGV 导航异常（[3x]00009=%1），停止流程并复位机器人")
                             .arg(static_cast<quint16>(status)));
        emit agvFaultDetected();  // UI 更新 AGV 指示灯为故障状态
        stop();                   // 写 CmdID=0 复位机械臂，防止悬臂悬空
        break;
    default:
        break; // None/Waiting/Running/Paused：继续等待，下次轮询再判断
    }
}
