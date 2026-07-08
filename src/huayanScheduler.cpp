/**
 * @file huayanScheduler.cpp
 * @brief HuayanScheduler 华研机器人调度实现
 */

#include "huayanScheduler.h"
#include "HR_Pro.h"
#include "palletscheduler.h"

#include <QTimer>
#include <vector>

namespace {
// 机器人运动和位姿常量，参考原有取料 / 卸料场景坐标
// 默认位姿仍保留示例值，真实场景下可通过 set*Pose 接口注入

// 运动参数：现场调速优先使用 speed override；未核对负载和安全空间前不要放大基础值。
static constexpr double kMoveVelocity = 50.0;
static constexpr double kMoveAcceleration = 100.0;
static constexpr double kMoveRadius = 0.0;

// 闭环视觉矫正参数
static constexpr double kGrabTolerance     = 2.0;    // XY 偏移收敛阈值(mm)
static constexpr double kRzTolerance       = 1.0;    // Rz 旋转收敛阈值(度)
static constexpr int    kMaxGrabIterations = 10;      // 最大矫正迭代次数（防死循环）
static constexpr int    kVisionSettleMs    = 2000;   // 移动后等视觉出新帧(ms)
static constexpr double kSearchDescendStep = 20.0; // 未识别目标时每轮搜索下移量(mm)
static constexpr double kMaxSearchDescend  = 80.0; // 搜索累计安全上限(mm)
// GrpReset 退出 ProgramStopped 态是异步的；阶段间快速衔接时（取料→收姿态、倒料→收姿态）
// 复位后立即下发 RunFunc 会撞 20018，须等控制器状态切换。机器人空闲时无此延迟需求。
static constexpr int    kResetSettleMs     = 1000;

// Z 下探参数：下探量 = 视觉深度 - grabZClearance，受 kMaxDescend 上限约束
// grabZClearance 标定法：固定一物体，记视觉深度 D 和能夹到的下探量 H，则 = D - H
//   （本例 D=1048, H=640 → 408）。此值对不同深度通用，视觉深度变化时下探量自动适应。
static constexpr double kMaxDescend     = 1078.0;  // 下探安全上限(mm)，正常不应触发截断
static constexpr bool   kZDescendInvert = false;   // Z 下探方向；若实际朝反方向，改 true
static constexpr double kOffsetIgnoreDistance = 0.5; // 码垛平移死区(mm)
static constexpr double kOffsetIgnoreAngle = 0.5;    // 码垛旋转死区(deg)
static constexpr double kRotateToolAngle = 180.0;    // 扫码补救的工具系 Rz 角(deg)
static constexpr double kLargeRzJumpThreshold = 80.0; // Rz 大角度跳变确认阈值(deg)
static constexpr double kLargeRzJumpDeltaTolerance = 15.0; // 连续两帧幅值接近阈值(deg)

// 华研机器人默认 TCP/UCS 名称
static const QString kTcpName = QStringLiteral("TCP");
static const QString kFlipFuncName = QStringLiteral("FlipUnload");

QString describeError(unsigned int boxID, int code)
{
    string msg;
    if (HRIF_GetErrorCodeStr(boxID, code, msg) == 0 && !msg.empty())
        return QString::fromStdString(msg);
    return QString();
}

QString stageName(HuayanScheduler::Stage stage)
{
    switch (stage) {
    case HuayanScheduler::Stage::StageOne:
        return QStringLiteral("阶段一：取料");
    case HuayanScheduler::Stage::StageTwo:
        return QStringLiteral("阶段二：卸料");
    case HuayanScheduler::Stage::StageThree:
        return QStringLiteral("阶段三：码垛");
    case HuayanScheduler::Stage::Stow:
        return QStringLiteral("收姿态");
    case HuayanScheduler::Stage::Unload:
        return QStringLiteral("倒料");
    default:
        return QStringLiteral("未知阶段");
    }
}
}

QString HuayanScheduler::stageName(Stage stage) const
{
    return ::stageName(stage);
}

HuayanScheduler::HuayanScheduler(QObject *parent)
    : QObject(parent)
{
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(100);
    connect(m_pollTimer, &QTimer::timeout, this, &HuayanScheduler::onPollTick);

    m_timeoutTimer = new QTimer(this);
    m_timeoutTimer->setSingleShot(true);
    connect(m_timeoutTimer, &QTimer::timeout, this, &HuayanScheduler::onStepTimeout);

    m_commandReadyTimer = new QTimer(this);
    m_commandReadyTimer->setInterval(100);
    connect(m_commandReadyTimer, &QTimer::timeout, this, &HuayanScheduler::pollCommandReady);

    // 阶段一改为调用示教器函数（Func_capture / Func_jiajin），
    // 拍照位/抓取/抬升的姿态与轨迹全部由示教器保证，不再硬编码坐标

    // 阶段二位姿（卸料/空箱），联机后示教覆写
    m_unloadPose.x = 2000.0; m_unloadPose.y = 1000.0; m_unloadPose.z = 1500.0;
    m_emptyBoxPose.x = 2100.0; m_emptyBoxPose.y = 1100.0; m_emptyBoxPose.z = 1200.0;
}

HuayanScheduler::~HuayanScheduler()
{
    disconnectRobot();
}

// --- Pose setter/getter implementations ---
void HuayanScheduler::setPickupPose(const Pose &p)
{
    m_pickupPose = p;
}

HuayanScheduler::Pose HuayanScheduler::pickupPose() const
{
    return m_pickupPose;
}

void HuayanScheduler::setPickupLiftPose(const Pose &p)
{
    m_pickupLiftPose = p;
}

HuayanScheduler::Pose HuayanScheduler::pickupLiftPose() const
{
    return m_pickupLiftPose;
}

void HuayanScheduler::setUnloadPose(const Pose &p)
{
    m_unloadPose = p;
}

HuayanScheduler::Pose HuayanScheduler::unloadPose() const
{
    return m_unloadPose;
}

void HuayanScheduler::setEmptyBoxPose(const Pose &p)
{
    m_emptyBoxPose = p;
}

HuayanScheduler::Pose HuayanScheduler::emptyBoxPose() const
{
    return m_emptyBoxPose;
}

bool HuayanScheduler::startRobotScript()
{
    if (!ensureConnected())
        return false;

    int nRet = HRIF_StartScript(m_boxID);
    if (nRet != 0) {
        emit stageError(QStringLiteral("启动机器人脚本失败：%1").arg(nRet));
        return false;
    }

    emit logMessage(QStringLiteral("已请求启动机器人脚本"));
    return true;
}

bool HuayanScheduler::stopRobotScript()
{
    if (!ensureConnected())
        return false;

    int nRet = HRIF_StopScript(m_boxID);
    if (nRet != 0) {
        emit stageError(QStringLiteral("停止机器人脚本失败：%1").arg(nRet));
        return false;
    }

    emit logMessage(QStringLiteral("已请求停止机器人脚本"));
    return true;
}

void HuayanScheduler::setConnectionParams(const QString &ip,
                                          unsigned short port,
                                          unsigned int boxID,
                                          unsigned int rbtID)
{
    m_ip = ip;
    m_port = port;
    m_boxID = boxID;
    m_rbtID = rbtID;
}

bool HuayanScheduler::connectRobot()
{
    if (m_ip.isEmpty()) {
        emitOperationError(QStringLiteral("华研机器人连接参数未配置"));
        return false;
    }

    // 连接华研 CPS，默认控制盒 boxID / 机器人 ID
    int nRet = HRIF_Connect(m_boxID, m_ip.toStdString().c_str(), m_port);
    if (nRet != 0) {
        emitOperationError(QStringLiteral("连接华研机器人失败：%1").arg(nRet));
        return false;
    }

    int nSimulateRobot = 0;
    nRet = HRIF_IsSimulateRobot(m_boxID, nSimulateRobot);
    if (nRet == 0) {
        const double overrideValue = (nSimulateRobot == 1) ? 1.0 : (m_speedPercent / 100.0);
        HRIF_SetOverride(m_boxID, m_rbtID, overrideValue);
    }

    nRet = HRIF_XToStandby(m_boxID, m_rbtID);
    if (nRet != 0) {
        emitOperationError(QStringLiteral("华研机器人进入 Standby 失败：%1").arg(nRet));
        return false;
    }

    m_connected = true;

    // 仅在当前应用脚本不是目标脚本时才切换：SwitchScript 会让脚本回到"未编译"
    // 状态，每次连接都切换会导致每次启动都提示需重新编译
    string curScript;
    int nReadRet = HRIF_ReadDefaultScript(m_boxID, m_rbtID, curScript);
    const QString cur = QString::fromStdString(curScript);
    emit logMessage(QStringLiteral("当前应用脚本：%1").arg(cur.isEmpty() ? QStringLiteral("(空)") : cur));
    if (nReadRet != 0 || (cur != m_scriptName && cur != m_scriptName + QStringLiteral(".json"))) {
        nRet = HRIF_SwitchScript(m_boxID, m_rbtID, m_scriptName.toStdString());
        if (nRet != 0)
            emit logMessage(QStringLiteral("[警告] 切换脚本 %1 失败：%2").arg(m_scriptName).arg(nRet));
        else
            emit logMessage(QStringLiteral("已切换到脚本 %1（请确认已在示教器编译）").arg(m_scriptName));
    }

    emit logMessage(QStringLiteral("已连接华研机器人：%1:%2").arg(m_ip).arg(m_port));
    emit connected();
    return true;
}

bool HuayanScheduler::disconnectRobot()
{
    if (!m_connected)
        return true;

    int nRet = HRIF_DisConnect(m_boxID);
    if (nRet != 0) {
        emit stageError(QStringLiteral("断开华研机器人连接失败：%1").arg(nRet));
        return false;
    }

    m_connected = false;
    emit logMessage(QStringLiteral("已断开华研机器人连接"));
    emit disconnected();
    return true;
}

bool HuayanScheduler::isConnected() const
{
    return m_connected && HRIF_IsConnected(m_boxID);
}

void HuayanScheduler::setStackingFunction(const QString &funcName,
                                          const QStringList &params)
{
    m_stackingFuncName = funcName;
    m_stackingParams = params;
}

void HuayanScheduler::setStationFunctions(const StationArmFunctions &funcs)
{
    // 只覆盖非空字段，保留默认函数名，保证现有测试面板仍可单独调试。
    if (!funcs.captureFunc.isEmpty())
        m_captureFuncName = funcs.captureFunc;
    // 夹后策略和 Z 余量来自 lineconfig 的当前工位配置。
    // 这两个值必须随任务注入，不能用全局固定值，否则工位12和带过渡点工位会复用错误路径。
    m_afterGripMode = funcs.afterGripMode;
    m_afterGripFuncName = funcs.afterGripFunc;
    m_grabZClearance = funcs.grabZClearance;
    if (!funcs.unloadPointFunc.isEmpty())
        m_unloadPointFuncName = funcs.unloadPointFunc;
    if (!funcs.unloadFunc.isEmpty())
        m_unloadFuncName = funcs.unloadFunc;
}

void HuayanScheduler::setPalletFunctions(const PalletArmFunctions &funcs)
{
    if (!funcs.palletBaseFunc.isEmpty())
        m_palletBaseFuncName = funcs.palletBaseFunc;
    if (!funcs.releaseFunc.isEmpty())
        m_releaseFuncName = funcs.releaseFunc;
}

void HuayanScheduler::setPreGripScanEnabled(bool enabled)
{
    m_preGripScanEnabled = enabled;
    if (!enabled)
        m_waitingPreGripScan = false;
}

void HuayanScheduler::continueAfterPreGripScan()
{
    // 扫码结果可能在 Stop/状态切换后迟到，必须同时核对阶段、步骤和等待标志。
    if (m_action != Action::None)
        return;
    if (m_stage != Stage::StageOne || m_stageStep != StageStep::WaitPreGripScan || !m_waitingPreGripScan)
        return;

    m_waitingPreGripScan = false;
    m_stageStep = StageStep::CloseGripper;
    emit logMessage(QStringLiteral("[阶段一] 扫码通过，继续夹紧并抬升"));
    proceedStage();
}

void HuayanScheduler::rotateToolRz180()
{
    if (m_action != Action::None) {
        emit toolRotationError(QStringLiteral("当前已有独立动作执行中"));
        return;
    }
    if (m_stage != Stage::None
        && !(m_stage == Stage::StageOne
             && m_stageStep == StageStep::WaitPreGripScan
             && m_waitingPreGripScan)) {
        emit toolRotationError(QStringLiteral("当前阶段不允许执行工具旋转"));
        return;
    }

    // 用于扫码枪扫不到有码面时，让箱体侧面二维码对准扫码枪。
    // Action 与 Stage 分离，旋转结束后仍保留 StageOne 的扫码暂停上下文。
    m_action = Action::RotateTool;
    m_actionStep = ActionStep::RotateTool;
    if (!ensureConnected())
        return;
    emit logMessage(QStringLiteral("[独立动作] 工具坐标系 Rz 相对旋转 180 度"));
    proceedAction();
}

void HuayanScheduler::movePreGripScanSearchTo(double targetYOffsetMm)
{
    if (m_action != Action::None) {
        emit preGripScanSearchMoveError(QStringLiteral("当前已有独立动作执行中"));
        return;
    }
    if (m_stage != Stage::StageOne || m_stageStep != StageStep::WaitPreGripScan || !m_waitingPreGripScan) {
        emit preGripScanSearchMoveError(QStringLiteral("当前阶段不允许执行夹紧前扫码搜索移动"));
        return;
    }

    m_preGripScanSearchTargetY = targetYOffsetMm;
    m_action = Action::PreGripScanSearchMove;
    m_actionStep = ActionStep::MovePreGripScanSearchY;
    if (!ensureConnected())
        return;
    emit logMessage(QStringLiteral("[扫码搜索] 移动到工具系 Y=%1mm 搜码位置")
                        .arg(targetYOffsetMm, 0, 'f', 1));
    proceedAction();
}

void HuayanScheduler::returnToCaptureForScanFailure()
{
    if (m_action != Action::None) {
        emit preGripScanCaptureReturnError(QStringLiteral("当前已有独立动作执行中"));
        return;
    }
    if (m_stage != Stage::StageOne || m_stageStep != StageStep::WaitPreGripScan || !m_waitingPreGripScan) {
        emit preGripScanCaptureReturnError(QStringLiteral("当前阶段不允许回拍照位"));
        return;
    }

    m_action = Action::ReturnToCaptureForScanFailure;
    m_actionStep = ActionStep::RunCaptureForScanFailure;
    if (!ensureConnected())
        return;
    emit logMessage(QStringLiteral("[扫码搜索] 全部失败，调用拍照位函数 %1 返回拍照位")
                        .arg(m_captureFuncName));
    proceedAction();
}

void HuayanScheduler::startPalletPlace(const PalletPose &offset)
{
    if (m_action != Action::None) {
        emit palletPlaceError(QStringLiteral("当前已有独立动作执行中"));
        return;
    }
    if (m_stage != Stage::None) {
        emit palletPlaceError(QStringLiteral("当前阶段忙碌，不能开始码垛放置动作"));
        return;
    }
    if (m_palletBaseFuncName.isEmpty()) {
        emit palletPlaceError(QStringLiteral("未配置码垛基准点函数"));
        return;
    }
    if (m_releaseFuncName.isEmpty()) {
        emit palletPlaceError(QStringLiteral("未配置松爪函数"));
        return;
    }

    m_palletMoves.clear();
    m_palletMoveIdx = 0;
    auto addMove = [this](int poseId, double value, double ignoreThreshold) {
        if (qAbs(value) < ignoreThreshold)
            return;
        m_palletMoves.append({ poseId, value >= 0 ? 1 : 0, qAbs(value) });
    };
    addMove(0, offset.x, kOffsetIgnoreDistance);
    addMove(1, offset.y, kOffsetIgnoreDistance);
    addMove(2, offset.z, kOffsetIgnoreDistance);
    addMove(5, offset.rz, kOffsetIgnoreAngle);

    // 码垛 offset 来自 PalletScheduler::nextRelativeOffset()，此处只执行机械臂动作，
    // 不推进码垛缓存。
    m_action = Action::PalletPlace;
    m_actionStep = ActionStep::RunPalletBase;
    if (!ensureConnected())
        return;
    emit logMessage(QStringLiteral("[码垛] 开始放置 offset X=%1 Y=%2 Z=%3 Rz=%4")
                        .arg(offset.x, 0, 'f', 1)
                        .arg(offset.y, 0, 'f', 1)
                        .arg(offset.z, 0, 'f', 1)
                        .arg(offset.rz, 0, 'f', 1));
    proceedAction();
}

bool HuayanScheduler::rejectStageStartWhileActionRunning(const QString &stageName)
{
    if (m_action != Action::None) {
        const QString msg = QStringLiteral("%1启动失败：当前独立动作执行中").arg(stageName);
        emit logMessage(msg);
        emit stageError(msg);
        return true;
    }

    const bool hasPendingCommand = m_pendingCommand.kind != PendingCommandKind::None;
    if (!canQueuePendingCommand(hasPendingCommand, hasActiveRobotCommand())) {
        const QString msg = QStringLiteral("%1启动失败：机械臂仍有命令执行中").arg(stageName);
        emit logMessage(msg);
        emit stageError(msg);
        return true;
    }

    return false;
}

void HuayanScheduler::startStageOne()
{
    if (rejectStageStartWhileActionRunning(stageName(Stage::StageOne)))
        return;
    if (!ensureConnected())
        return;

    // 手动松爪等非阶段命令也会启动等待轮询；开启新阶段前先清掉遗留轮询/门控状态，
    // 避免旧命令完成回调误推进新阶段。
    stopPollingAndTimers();
    clearActionState();
    m_stage = Stage::StageOne;
    m_stageStep = StageStep::MoveToSurvey;
    m_grabIterations = 0;
    m_searchDescendCount = 0;
    m_searchDescendedMm = 0.0;
    m_pendingLargeRzConfirmation = false;
    m_pendingLargeRz = 0.0;
    m_waitingPreGripScan = false;
    m_preGripScanSearchCurrentY = 0.0;
    m_preGripScanSearchTargetY = 0.0;

    emit stageStarted(stageName(m_stage));
    resetAndProceed();
}

void HuayanScheduler::startStageTwo()
{
    if (rejectStageStartWhileActionRunning(stageName(Stage::StageTwo)))
        return;
    if (!ensureConnected())
        return;

    // 新阶段启动前先切断旧命令的轮询尾巴，避免跨阶段误推进。
    stopPollingAndTimers();
    clearActionState();
    m_stage = Stage::StageTwo;
    m_stageStep = StageStep::MoveToUnload;
    m_waitingPreGripScan = false;

    emit stageStarted(stageName(m_stage));
    proceedStage();
}

void HuayanScheduler::startStageThree()
{
    if (rejectStageStartWhileActionRunning(stageName(Stage::StageThree)))
        return;
    if (!ensureConnected())
        return;

    if (m_stackingFuncName.isEmpty()) {
        emit stageError(QStringLiteral("启动阶段三失败：未配置码垛脚本函数"));
        return;
    }

    // 新阶段启动前先切断旧命令的轮询尾巴，避免跨阶段误推进。
    stopPollingAndTimers();
    clearActionState();
    m_stage = Stage::StageThree;
    m_stageStep = StageStep::ExecuteStackingFunction;
    m_waitingPreGripScan = false;

    emit stageStarted(stageName(m_stage));
    proceedStage();
}

void HuayanScheduler::startStow()
{
    if (rejectStageStartWhileActionRunning(stageName(Stage::Stow)))
        return;
    if (!ensureConnected())
        return;

    // 新阶段启动前先切断旧命令的轮询尾巴，避免跨阶段误推进。
    stopPollingAndTimers();
    clearActionState();
    m_stage = Stage::Stow;
    m_stageStep = StageStep::StowArm;
    m_waitingPreGripScan = false;

    emit stageStarted(stageName(m_stage));
    resetAndProceed();
}

void HuayanScheduler::startUnload()
{
    if (rejectStageStartWhileActionRunning(stageName(Stage::Unload)))
        return;
    if (!ensureConnected())
        return;

    // 新阶段启动前先切断旧命令的轮询尾巴，避免跨阶段误推进。
    stopPollingAndTimers();
    clearActionState();
    m_stage = Stage::Unload;
    m_stageStep = StageStep::MoveToUnloadPoint;
    m_waitingPreGripScan = false;

    emit stageStarted(stageName(m_stage));
    resetAndProceed();
}

void HuayanScheduler::resetAndProceed()
{
    // GrpReset 退出 ProgramStopped 是异步的，复位后延时再下发首条指令；
    // 等待期间若被 stop() 打断（m_stage 置 None）则不再继续
    const quint64 seq = nextCallbackSeq();
    const int resetRet = HRIF_GrpReset(m_boxID, m_rbtID);
    if (resetRet != 0)
        emit logMessage(QStringLiteral("[警告] resetAndProceed 调用 GrpReset 失败：%1").arg(resetRet));
    QTimer::singleShot(kResetSettleMs, this, [this, seq] {
        if (seq == m_commandSeq && m_stage != Stage::None)
            proceedStage();
    });
}

void HuayanScheduler::completeStage()
{
    // 完成信号直连编排器，会在 emit 内同步启动下一阶段；必须先 stop() 清理本阶段，
    // 否则 emit 返回后的 stop() 会把刚启动的下一阶段重置（m_stage 复位 None）
    const QString done = stageName(m_stage);
    stop(false);
    emit stageCompleted(done);
}

void HuayanScheduler::stop(bool emitStoppedLog)
{
    stopPollingAndTimers();
    requestRobotStop();
    ++m_commandSeq; // 让已经排队的 singleShot 回调全部失效，避免旧阶段推进新阶段。
    clearActionState();
    m_searchDescendCount = 0;
    m_searchDescendedMm = 0.0;
    m_pendingLargeRzConfirmation = false;
    m_pendingLargeRz = 0.0;
    m_waitingPreGripScan = false;
    m_preGripScanSearchCurrentY = 0.0;
    m_preGripScanSearchTargetY = 0.0;
    m_stage = Stage::None;
    m_stageStep = StageStep::None;
    if (emitStoppedLog)
        emit logMessage(QStringLiteral("调度已停止"));
}

void HuayanScheduler::startWaitForIdle(int timeoutMs)
{
    m_pollCount = 0;
    m_hasSeenMoving = false;
    m_timeoutTimer->start(timeoutMs);
    m_pollTimer->start();
}

void HuayanScheduler::onPollTick()
{
    // SDK 无统一的单动作完成回调，因此轮询运动状态推进步骤；必须先观察到运动
    // 再观察到空闲，避免命令启动延迟被误判为已经完成。
    int nMovingState, nEnableState, nErrorState, nErrorCode, nErrorAxis, nBreaking, nPause, nBlendingDone;
    int nRet = HRIF_ReadRobotFlags(m_boxID, m_rbtID,
                                    nMovingState, nEnableState, nErrorState,
                                    nErrorCode, nErrorAxis, nBreaking,
                                    nPause, nBlendingDone);
    if (nRet != 0) {
        emitOperationError(QStringLiteral("读取机器人状态失败，错误码：%1").arg(nRet));
        return;
    }
    if (nErrorState != 0) {
        const QString detail = describeError(m_boxID, nErrorCode);
        emitOperationError(detail.isEmpty()
            ? QStringLiteral("机器人报错，错误码：%1").arg(nErrorCode)
            : QStringLiteral("机器人报错，错误码：%1（%2）").arg(nErrorCode).arg(detail));
        return;
    }
    if (nMovingState != 0)
        m_hasSeenMoving = true;
    m_pollCount++;
    // 必须先观察到运动真正开始(nMovingState!=0)再判结束，避免指令启动延迟被误判完成；
    // 兜底：极短运动可能采样不到运动态，超过约 3 秒(pollCount>=30 @100ms)也判完成
    if ((m_hasSeenMoving || m_pollCount >= 30) && nMovingState == 0) {
        m_pollTimer->stop();
        m_timeoutTimer->stop();
        if (m_action == Action::PalletPlace && m_actionStep == ActionStep::MovePalletOffset) {
            m_palletMoveIdx++;
            const quint64 seq = nextCallbackSeq();
            QTimer::singleShot(300, this, [this, seq] {
                if (seq == m_commandSeq
                    && m_action == Action::PalletPlace
                    && m_actionStep == ActionStep::MovePalletOffset)
                    executeNextPalletMove();
            });
            return;
        }
        if (m_action != Action::None) {
            advanceActionStep();
            proceedAction();
            return;
        }
        if (m_stage == Stage::StageOne && m_stageStep == StageStep::SearchDescend) {
            emit logMessage(QStringLiteral("[阶段一] 搜索下移完成，等待视觉稳定后重新检测"));
            m_stageStep = StageStep::WaitForVision;
            const quint64 seq = nextCallbackSeq();
            QTimer::singleShot(kVisionSettleMs, this, [this, seq] {
                if (seq == m_commandSeq
                    && m_stage == Stage::StageOne
                    && m_stageStep == StageStep::WaitForVision)
                    proceedStage();
            });
            return;
        }
        // MoveToGrab 是多次 MoveRelL 串联，单次到位后继续下一个偏移分量
        if (m_stage == Stage::StageOne && m_stageStep == StageStep::MoveToGrab) {
            m_grabMoveIdx++;
            // 运动结束后机器人状态切换有滞后(nMovingState=0 但仍 RobotInMoving)，
            // 高速下稍等再发下一轴，避免 20018 RobotInMoving
            const quint64 seq = nextCallbackSeq();
            QTimer::singleShot(300, this, [this, seq] {
                if (seq == m_commandSeq
                    && m_stage == Stage::StageOne
                    && m_stageStep == StageStep::MoveToGrab)
                    executeNextGrabMove();
            });
        } else {
            advanceStep();
            proceedStage();
        }
    }
}

void HuayanScheduler::onStepTimeout()
{
    m_pollTimer->stop();
    emitOperationError(QStringLiteral("步骤超时，机器人未在预期时间内完成动作"));
}

void HuayanScheduler::proceedStage()
{
    // 根据当前阶段和当前步骤继续执行下一步动作
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
}

void HuayanScheduler::advanceStep()
{
    // Stage 状态机唯一的顺序定义；新增步骤时须同步更新 executeCurrentStep()。
    switch (m_stage) {
    case Stage::StageOne:
        switch (m_stageStep) {
        case StageStep::MoveToSurvey:  m_stageStep = StageStep::WaitForVision; break;
        case StageStep::DescendZ:
            m_stageStep = m_preGripScanEnabled ? StageStep::WaitPreGripScan : StageStep::CloseGripper;
            break;
        case StageStep::WaitPreGripScan: m_stageStep = StageStep::CloseGripper;  break;
        case StageStep::CloseGripper:  m_stageStep = StageStep::LiftLoad;      break;
        case StageStep::LiftLoad:      m_stageStep = StageStep::None;          break;
        default:                       m_stageStep = StageStep::None;          break;
        }
        break;
    case Stage::StageTwo:
        if (m_stageStep == StageStep::MoveToUnload)
            m_stageStep = StageStep::FlipUnload;
        else if (m_stageStep == StageStep::FlipUnload)
            m_stageStep = StageStep::ReleaseLoad;
        else if (m_stageStep == StageStep::ReleaseLoad)
            m_stageStep = StageStep::MoveEmptyBox;
        else
            m_stageStep = StageStep::None;
        break;
    case Stage::StageThree:
        m_stageStep = StageStep::None;
        break;
    case Stage::Stow:
        m_stageStep = StageStep::None;
        break;
    case Stage::Unload:
        if (m_stageStep == StageStep::MoveToUnloadPoint)
            m_stageStep = StageStep::RunUnloadFunc;
        else
            m_stageStep = StageStep::None;
        break;
    default:
        m_stageStep = StageStep::None;
        break;
    }
}

/**
 * @brief StageStep → 流程图节点索引（WorkflowWidget kDefaultSteps）
 *
 * 0=视觉定位 1=SDK取料 2=翻转卸料 3=AGV运输(本期不发射) 4=码垛复位
 */
int HuayanScheduler::stepIndexFor(StageStep step)
{
    switch (step) {
    case StageStep::MoveToSurvey:
    case StageStep::WaitForVision:
    case StageStep::SearchDescend:           return 0;
    case StageStep::MoveToGrab:
    case StageStep::DescendZ:
    case StageStep::WaitPreGripScan:
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

void HuayanScheduler::executeCurrentStep()
{
    // 每个分支只下发一条动作或进入一个等待点，严禁阻塞 UI 线程等待设备完成。
    const int stepIdx = stepIndexFor(m_stageStep);
    if (stepIdx >= 0)
        emit stepChanged(stepIdx);

    switch (m_stage) {
    case Stage::StageOne:
        switch (m_stageStep) {
        case StageStep::MoveToSurvey:
            emit logMessage(QStringLiteral("[阶段一] 调用拍照位函数 %1").arg(m_captureFuncName));
            executeRunFunc(m_captureFuncName);
            break;
        case StageStep::WaitForVision:
            emit logMessage(QStringLiteral("[阶段一] 已到拍照位，等待视觉推理结果"));
            emit surveyReady();
            m_timeoutTimer->start(10000);
            break;
        case StageStep::SearchDescend:
            break;
        case StageStep::MoveToGrab:
            executeNextGrabMove();
            break;
        case StageStep::DescendZ: {
            const double descend = calculateGrabDescend(m_grabOffset.z, m_grabZClearance, kMaxDescend);
            if (descend < 1.0) {
                emit logMessage(QStringLiteral("[阶段一] 无需 Z 下探，已到扫码/夹取前位置"));
                m_stageStep = m_preGripScanEnabled ? StageStep::WaitPreGripScan : StageStep::CloseGripper;
                proceedStage();
                break;
            }
            emit logMessage(QStringLiteral("[阶段一] Z 下探 %1mm（视觉深度 %2 - 余量 %3，上限 %4）")
                                .arg(descend, 0, 'f', 1).arg(m_grabOffset.z, 0, 'f', 1)
                                .arg(m_grabZClearance, 0, 'f', 1).arg(kMaxDescend, 0, 'f', 1));
            PendingCommand cmd;
            cmd.kind = PendingCommandKind::MoveRelTool;
            cmd.label = QStringLiteral("Z 下探");
            cmd.poseId = 2;
            cmd.direction = kZDescendInvert ? 0 : 1;
            cmd.distance = descend;
            beginCommandWhenReady(cmd);
            break;
        }
        case StageStep::WaitPreGripScan:
            // 这是主流程插入扫码比对的安全暂停点，不能被普通阶段完成自动跳过。
            if (!m_preGripScanEnabled) {
                m_stageStep = StageStep::CloseGripper;
                proceedStage();
                break;
            }
            m_waitingPreGripScan = true;
            emit logMessage(QStringLiteral("[阶段一] 已到扫码/夹取前位置，等待扫码结果"));
            emit preGripScanRequested();
            break;
        case StageStep::CloseGripper:
            emit logMessage(QStringLiteral("[阶段一] 调用夹紧函数 %1").arg(m_gripFuncName));
            executeGripFunc();
            break;
        case StageStep::LiftLoad: {
            // 夹紧后的离开路径不能再隐式等同拍照路径。
            // 工位12的 Func_capture12 包含“过渡点→拍照点”，夹后复用会导致去倒料前多绕路；
            // 其他工位未来也可能增加过渡点，因此这里按 lineconfig 的 afterGripMode 执行。
            bool shouldRunAfterGrip = false;
            const QString afterGripFunc = resolveAfterGripFunction(m_afterGripMode,
                                                                    m_captureFuncName,
                                                                    m_afterGripFuncName,
                                                                    &shouldRunAfterGrip);
            if (!shouldRunAfterGrip) {
                emit logMessage(QStringLiteral("[阶段一] 夹紧后配置为不回安全位，直接完成取料阶段"));
                m_stageStep = StageStep::None;
                proceedStage();
                break;
            }
            if (afterGripFunc.isEmpty()) {
                emitOperationError(QStringLiteral("[阶段一] 夹后策略需要函数，但函数名为空"));
                break;
            }
            emit logMessage(m_afterGripMode == AfterGripMode::CustomFunc
                ? QStringLiteral("[阶段一] 夹紧后调用安全离开函数 %1").arg(afterGripFunc)
                : QStringLiteral("[阶段一] 夹紧后复用拍照位函数 %1 回安全高度").arg(afterGripFunc));
            executeRunFunc(afterGripFunc);
            break;
        }
        case StageStep::None:
            completeStage();
            break;
        default:
            break;
        }
        break;
    case Stage::StageTwo:
        switch (m_stageStep) {
        case StageStep::MoveToUnload:
            emit logMessage(QStringLiteral("[阶段二] 机械臂移动到卸料框坐标"));
            executeMoveJ(m_unloadPose.x, m_unloadPose.y, m_unloadPose.z,
                         m_unloadPose.rx, m_unloadPose.ry, m_unloadPose.rz);
            break;
        case StageStep::FlipUnload:
            emit logMessage(QStringLiteral("[阶段二] 执行翻转卸料脚本"));
            executeFlipUnload();
            break;
        case StageStep::ReleaseLoad:
            emit logMessage(QStringLiteral("[阶段二] 夹爪打开，释放物料"));
            setGripper(true); // 1 = open
            break;
        case StageStep::MoveEmptyBox:
            emit logMessage(QStringLiteral("[阶段二] 移动空箱至 AGV 工位"));
            executeMoveJ(m_emptyBoxPose.x, m_emptyBoxPose.y, m_emptyBoxPose.z,
                         m_emptyBoxPose.rx, m_emptyBoxPose.ry, m_emptyBoxPose.rz);
            break;
        case StageStep::None:
            completeStage();
            break;
        default:
            break;
        }
        break;
    case Stage::StageThree:
        switch (m_stageStep) {
        case StageStep::ExecuteStackingFunction:
            emit logMessage(QStringLiteral("[阶段三] 执行码垛脚本函数"));
            executeStackingFunction();
            break;
        case StageStep::None:
            completeStage();
            break;
        default:
            break;
        }
        break;
    case Stage::Stow:
        switch (m_stageStep) {
        case StageStep::StowArm:
            emit logMessage(QStringLiteral("[收姿态] 调用 %1").arg(m_stowFuncName));
            executeRunFunc(m_stowFuncName);
            break;
        case StageStep::None:
            completeStage();
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
            completeStage();
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }
}

void HuayanScheduler::proceedAction()
{
    switch (m_action) {
    case Action::RotateTool:
        if (m_actionStep == ActionStep::None) {
            finishAction();
            return;
        }
        {
            PendingCommand cmd;
            cmd.kind = PendingCommandKind::MoveRelTool;
            cmd.label = QStringLiteral("工具旋转");
            cmd.poseId = 5;
            cmd.direction = 1;
            cmd.distance = kRotateToolAngle;
            beginCommandWhenReady(cmd);
        }
        break;
    case Action::PalletPlace:
        switch (m_actionStep) {
        case ActionStep::RunPalletBase:
            emit logMessage(QStringLiteral("[码垛] 调用基准点函数 %1").arg(m_palletBaseFuncName));
            executeRunFunc(m_palletBaseFuncName, 120000);
            break;
        case ActionStep::MovePalletOffset:
            executeNextPalletMove();
            break;
        case ActionStep::ReleasePallet:
            emit logMessage(QStringLiteral("[码垛] 调用松爪函数 %1").arg(m_releaseFuncName));
            executeRunFunc(m_releaseFuncName, 30000);
            break;
        case ActionStep::None:
            finishAction();
            break;
        default:
            break;
        }
        break;
    case Action::PreGripScanSearchMove:
        switch (m_actionStep) {
        case ActionStep::MovePreGripScanSearchY: {
            const double delta = m_preGripScanSearchTargetY - m_preGripScanSearchCurrentY;
            if (qAbs(delta) < 0.1) {
                finishAction();
                return;
            }

            const int direction = delta >= 0.0 ? 1 : 0;
            PendingCommand cmd;
            cmd.kind = PendingCommandKind::MoveRelTool;
            cmd.label = QStringLiteral("扫码搜索 Y 轴移动");
            cmd.poseId = 1;
            cmd.direction = direction;
            cmd.distance = qAbs(delta);
            beginCommandWhenReady(cmd);
            break;
        }
        case ActionStep::None:
            finishAction();
            break;
        default:
            break;
        }
        break;
    case Action::ReturnToCaptureForScanFailure:
        switch (m_actionStep) {
        case ActionStep::RunCaptureForScanFailure:
            executeRunFunc(m_captureFuncName);
            break;
        case ActionStep::None:
            finishAction();
            break;
        default:
            break;
        }
        break;
    case Action::None:
        break;
    }
}

void HuayanScheduler::advanceActionStep()
{
    switch (m_action) {
    case Action::RotateTool:
        m_actionStep = ActionStep::None;
        break;
    case Action::PalletPlace:
        if (m_actionStep == ActionStep::RunPalletBase)
            m_actionStep = ActionStep::MovePalletOffset;
        else if (m_actionStep == ActionStep::ReleasePallet)
            m_actionStep = ActionStep::None;
        break;
    case Action::PreGripScanSearchMove:
    case Action::ReturnToCaptureForScanFailure:
        m_actionStep = ActionStep::None;
        break;
    case Action::None:
        break;
    }
}

bool HuayanScheduler::executeNextPalletMove()
{
    if (m_palletMoveIdx >= m_palletMoves.size()) {
        emit logMessage(QStringLiteral("[码垛] offset 分轴移动完成，准备松爪"));
        m_actionStep = ActionStep::ReleasePallet;
        proceedAction();
        return true;
    }

    if (!ensureConnected())
        return false;

    static const QString axisName[] = {
        QStringLiteral("X"),  QStringLiteral("Y"),  QStringLiteral("Z"),
        QStringLiteral("Rx"), QStringLiteral("Ry"), QStringLiteral("Rz")
    };
    const RelMove &mv = m_palletMoves.at(m_palletMoveIdx);
    emit logMessage(QStringLiteral("[码垛] 基坐标系相对移动 %1 %2 %3")
                        .arg(axisName[mv.poseId])
                        .arg(mv.direction ? QStringLiteral("正向") : QStringLiteral("负向"))
                        .arg(mv.distance, 0, 'f', 1));
    PendingCommand cmd;
    cmd.kind = PendingCommandKind::MoveRelBase;
    cmd.label = QStringLiteral("码垛相对移动");
    cmd.poseId = mv.poseId;
    cmd.direction = mv.direction;
    cmd.distance = mv.distance;
    return beginCommandWhenReady(cmd);
}

void HuayanScheduler::clearActionState()
{
    m_action = Action::None;
    m_actionStep = ActionStep::None;
    m_palletMoves.clear();
    m_palletMoveIdx = 0;
}

void HuayanScheduler::finishAction()
{
    const Action action = m_action;
    const double targetY = m_preGripScanSearchTargetY;
    clearActionState();
    if (action == Action::RotateTool)
        emit toolRotationCompleted();
    else if (action == Action::PreGripScanSearchMove) {
        m_preGripScanSearchCurrentY = targetY;
        emit preGripScanSearchMoveCompleted(m_preGripScanSearchCurrentY);
    } else if (action == Action::ReturnToCaptureForScanFailure) {
        m_preGripScanSearchCurrentY = 0.0;
        m_preGripScanSearchTargetY = 0.0;
        emit preGripScanCaptureReturnCompleted();
    }
    else if (action == Action::PalletPlace)
        emit palletPlaceCompleted();
}

void HuayanScheduler::actionError(const QString &msg, bool stopRobot)
{
    const Action action = m_action;
    stopPollingAndTimers();
    if (stopRobot)
        requestRobotStop();
    clearActionState();

    if (action == Action::RotateTool)
        emit toolRotationError(msg);
    else if (action == Action::PreGripScanSearchMove)
        emit preGripScanSearchMoveError(msg);
    else if (action == Action::ReturnToCaptureForScanFailure)
        emit preGripScanCaptureReturnError(msg);
    else if (action == Action::PalletPlace)
        emit palletPlaceError(msg);
    else
        emit stageError(msg);
}

void HuayanScheduler::stopPollingAndTimers()
{
    m_pollTimer->stop();
    m_timeoutTimer->stop();
    m_commandReadyTimer->stop();
    m_pollCount = 0;
    m_hasSeenMoving = false;
    m_pendingCommand = PendingCommand();
    m_commandReadyElapsedMs = 0;
    m_commandResetIssued = false;
    ++m_commandSeq;
}

void HuayanScheduler::requestRobotStop()
{
    // 软件状态机之外，必要时仍向控制器发停止指令让机器人真正减速停下。
    if (m_connected) {
        int nRet = HRIF_GrpStop(m_boxID, m_rbtID);
        if (nRet != 0)
            emit logMessage(QStringLiteral("[警告] 停止指令未成功下发：%1").arg(nRet));
    }
}

void HuayanScheduler::emitOperationError(const QString &msg)
{
    if (m_action != Action::None) {
        actionError(msg);
        return;
    }

    emit stageError(msg);
    if (m_stage != Stage::None || m_waitingPreGripScan || m_pollTimer->isActive() || m_timeoutTimer->isActive())
        stop();
}

bool HuayanScheduler::ensureConnected()
{
    if (isConnected())
        return true;

    return connectRobot();
}

bool HuayanScheduler::executeMoveJ(double x, double y, double z,
                                  double rx, double ry, double rz,
                                  const QString &cmdId,
                                  const QString &ucsName)
{
    if (!ensureConnected())
        return false;

    PendingCommand cmd;
    cmd.kind = PendingCommandKind::MoveJ;
    cmd.label = QStringLiteral("MoveJ %1").arg(ucsName);
    cmd.targetPose = Pose{x, y, z, rx, ry, rz};
    cmd.cmdId = cmdId;
    cmd.ucsName = ucsName;
    return beginCommandWhenReady(cmd);
}

void HuayanScheduler::setGrabOffset(double x, double y, double z, double rz)
{
    if (m_stage != Stage::StageOne || m_stageStep != StageStep::WaitForVision)
        return;

    stopVisionWaitTimeout();

    auto sameDirection = [](double a, double b) {
        return (a >= 0.0 && b >= 0.0) || (a < 0.0 && b < 0.0);
    };

    const bool isLargeRzJump = qAbs(rz) >= kLargeRzJumpThreshold;
    bool suppressLargeRzRotation = false;
    double effectiveRz = rz;
    if (isLargeRzJump) {
        if (!m_pendingLargeRzConfirmation
            || !sameDirection(m_pendingLargeRz, rz)
            || qAbs(qAbs(m_pendingLargeRz) - qAbs(rz)) > kLargeRzJumpDeltaTolerance) {
            m_pendingLargeRzConfirmation = true;
            m_pendingLargeRz = rz;
            suppressLargeRzRotation = true;
            effectiveRz = 0.0;
            emit logMessage(QStringLiteral("[阶段一] 检测到疑似 Rz 大角度跳变 Rz=%1，等待下一帧确认，本轮不执行 Rz 旋转")
                                .arg(rz, 0, 'f', 1));
        } else {
            emit logMessage(QStringLiteral("[阶段一] Rz 大角度跳变已连续确认 Rz=%1，允许执行旋转")
                                .arg(rz, 0, 'f', 1));
            m_pendingLargeRzConfirmation = false;
            m_pendingLargeRz = 0.0;
        }
    } else {
        m_pendingLargeRzConfirmation = false;
        m_pendingLargeRz = 0.0;
    }

    m_grabOffset = { x, y, z, 0.0, 0.0, effectiveRz };
    emit logMessage(QStringLiteral("[阶段一] 视觉偏移(工具系) X=%1 Y=%2 Z=%3 Rz=%4 (迭代 %5)")
                        .arg(x, 0, 'f', 1).arg(y, 0, 'f', 1)
                        .arg(z, 0, 'f', 1).arg(effectiveRz, 0, 'f', 1)
                        .arg(m_grabIterations));

    const bool aligned = !suppressLargeRzRotation
        && qAbs(x) < kGrabTolerance
        && qAbs(y) < kGrabTolerance
        && qAbs(effectiveRz) < kRzTolerance;

    // 闭环收敛：只有 XY 平移与 Rz 旋转都达标才进入 Z 下探。
    if (aligned) {
        emit logMessage(QStringLiteral("[阶段一] 视觉对准完成（X=%1 Y=%2 Rz=%3），开始 Z 下探")
                            .arg(x, 0, 'f', 1).arg(y, 0, 'f', 1).arg(effectiveRz, 0, 'f', 1));
        m_stageStep = StageStep::DescendZ;
        proceedStage();
        return;
    }

    if (m_grabIterations >= kMaxGrabIterations) {
        emitOperationError(QStringLiteral("[阶段一] 视觉闭环对准超限：迭代 %1 次后仍未收敛（X=%2 Y=%3 Rz=%4，阈值 XY<%5mm/Rz<%6°）")
                               .arg(m_grabIterations)
                               .arg(x, 0, 'f', 1)
                               .arg(y, 0, 'f', 1)
                               .arg(effectiveRz, 0, 'f', 1)
                               .arg(kGrabTolerance, 0, 'f', 1)
                               .arg(kRzTolerance, 0, 'f', 1));
        return;
    }

    // 未收敛：工具坐标系 XY 平面微调，移动完成后回 WaitForVision 重新拍照
    m_grabIterations++;
    m_grabMoves.clear();
    auto addMove = [this](int poseId, double v) {
        if (qAbs(v) < kOffsetIgnoreDistance) return;   // 忽略 <0.5mm 的微小偏移
        m_grabMoves.append({ poseId, v >= 0 ? 1 : 0, qAbs(v) });
    };
    // 方向修正（联机实测）：X 方向一致直接施加，Y 与 Rz 方向相反需取反
    // Rz 取反：眼在手上闭环时，按 +rz 旋转会使下帧视觉角同向变大、机械臂持续旋转累积至 180°
    addMove(0, x);    // X
    addMove(1, -y);   // Y
    addMove(5, -effectiveRz);  // Rz 旋转

    m_grabMoveIdx = 0;
    m_stageStep = StageStep::MoveToGrab;
    proceedStage();
}

void HuayanScheduler::onVisionNoObject()
{
    if (m_stage != Stage::StageOne || m_stageStep != StageStep::WaitForVision) {
        emit logMessage(QStringLiteral("[阶段一] 收到未检测到目标，但当前不在等待视觉阶段，忽略"));
        return;
    }

    stopVisionWaitTimeout();

    const double nextDescendMm = m_searchDescendedMm + kSearchDescendStep;
    if (nextDescendMm > kMaxSearchDescend) {
        // 80mm 是保守默认值，防止算法一直识别不到时机械臂持续下移触碰料箱。
        emitOperationError(QStringLiteral("[阶段一] 连续未检测到目标，搜索下移累计 %1mm 已达保守上限 %2mm，任务失败")
                               .arg(m_searchDescendedMm, 0, 'f', 1)
                               .arg(kMaxSearchDescend, 0, 'f', 1));
        return;
    }

    if (!ensureConnected())
        return;

    m_stageStep = StageStep::SearchDescend;
    m_searchDescendCount++;
    m_searchDescendedMm = nextDescendMm;
    // 这里的搜索下移是“找目标”的保护搜索，不是抓取阶段依据深度做的 Z 下探。
    emit logMessage(QStringLiteral("[阶段一] 未检测到目标，执行第 %1 次搜索下移 %2mm（累计 %3/%4mm）")
                        .arg(m_searchDescendCount)
                        .arg(kSearchDescendStep, 0, 'f', 1)
                        .arg(m_searchDescendedMm, 0, 'f', 1)
                        .arg(kMaxSearchDescend, 0, 'f', 1));

    PendingCommand cmd;
    cmd.kind = PendingCommandKind::MoveRelTool;
    cmd.label = QStringLiteral("搜索下移");
    cmd.poseId = 2;
    cmd.direction = kZDescendInvert ? 0 : 1;
    cmd.distance = kSearchDescendStep;
    if (!beginCommandWhenReady(cmd)) {
        if (m_stage == Stage::StageOne && m_stageStep == StageStep::SearchDescend) {
            m_stageStep = StageStep::WaitForVision;
            m_searchDescendCount--;
            m_searchDescendedMm -= kSearchDescendStep;
        }
    }
}

void HuayanScheduler::onVisionErrorForPickup(const QString &msg)
{
    if (m_stage != Stage::StageOne || m_stageStep != StageStep::WaitForVision) {
        emit logMessage(QStringLiteral("[阶段一] 收到视觉错误，但当前不在等待视觉阶段，忽略：%1").arg(msg));
        return;
    }

    stopVisionWaitTimeout();

    // 通信/解析错误不代表目标不在视野内，继续下移没有意义，应直接按视觉异常失败处理。
    emitOperationError(QStringLiteral("[阶段一] 视觉推理失败：%1").arg(msg));
}

bool HuayanScheduler::executeNextGrabMove()
{
    if (m_grabMoveIdx >= m_grabMoves.size()) {
        // 本次 XY 微调完成，回到拍照状态，等视觉出新帧后重新检测（闭环）
        emit logMessage(QStringLiteral("[阶段一] 本次微调完成，等待视觉更新后重新检测"));
        m_stageStep = StageStep::WaitForVision;
        const quint64 seq = nextCallbackSeq();
        QTimer::singleShot(kVisionSettleMs, this, [this, seq] {
            if (seq == m_commandSeq
                && m_stage == Stage::StageOne
                && m_stageStep == StageStep::WaitForVision)
                proceedStage();
        });
        return true;
    }

    if (!ensureConnected())
        return false;

    static const QString axisName[] = {
        QStringLiteral("X"),  QStringLiteral("Y"),  QStringLiteral("Z"),
        QStringLiteral("Rx"), QStringLiteral("Ry"), QStringLiteral("Rz")
    };
    const RelMove &mv = m_grabMoves.at(m_grabMoveIdx);
    emit logMessage(QStringLiteral("[阶段一] 工具系微调 %1 %2 %3")
                        .arg(axisName[mv.poseId])
                        .arg(mv.direction ? QStringLiteral("正向") : QStringLiteral("负向"))
                        .arg(mv.distance, 0, 'f', 1));

    PendingCommand cmd;
    cmd.kind = PendingCommandKind::MoveRelTool;
    cmd.label = QStringLiteral("相对运动 %1").arg(axisName[mv.poseId]);
    cmd.poseId = mv.poseId;
    cmd.direction = mv.direction;
    cmd.distance = mv.distance;
    return beginCommandWhenReady(cmd);
}

void HuayanScheduler::setSurveyPose(const HuayanScheduler::Pose &p)
{
    m_surveyPose = p;
}

void HuayanScheduler::releaseGripper()
{
    if (!ensureConnected())
        return;

    if (executeRunFunc(m_releaseFuncName, 30000))
        emit logMessage(QStringLiteral("已请求松开夹爪 %1").arg(m_releaseFuncName));
}

void HuayanScheduler::setSpeedOverride(int percent)
{
    m_speedPercent = qBound(1, percent, 100);
    if (m_connected)
        HRIF_SetOverride(m_boxID, m_rbtID, m_speedPercent / 100.0);
}

bool HuayanScheduler::setGripper(bool open)
{
    // 使用华研 SDK 控制末端夹爪开闭：0 闭合, 1 打开
    if (!ensureConnected())
        return false;

    int nRet = HRIF_SetToolMotion(m_boxID, m_rbtID, open ? 1 : 0);
    if (nRet != 0) {
        emitOperationError(QStringLiteral("夹爪控制失败：%1").arg(nRet));
        return false;
    }

    // 夹爪是 IO 动作，nMovingState 不反映其状态，固定等待 1.5s 让气动/伺服完成动作
    const quint64 seq = nextCallbackSeq();
    QTimer::singleShot(1500, this, [this, seq] {
        if (seq == m_commandSeq && m_stage != Stage::None) {
            advanceStep();
            proceedStage();
        }
    });
    return true;
}

quint64 HuayanScheduler::nextCallbackSeq()
{
    return ++m_commandSeq;
}

bool HuayanScheduler::executeRunFunc(const QString &funcName, int timeoutMs)
{
    PendingCommand cmd;
    cmd.kind = PendingCommandKind::RunFunc;
    cmd.label = QStringLiteral("RunFunc %1").arg(funcName);
    cmd.funcName = funcName;
    cmd.timeoutMs = timeoutMs;
    return beginCommandWhenReady(cmd);
}

bool HuayanScheduler::executeGripFunc()
{
    return executeRunFunc(m_gripFuncName, 5000);
}

bool HuayanScheduler::executeFlipUnload()
{
    return executeRunFunc(kFlipFuncName, 60000);
}

bool HuayanScheduler::executeStackingFunction()
{
    // 运行阶段三配置的码垛脚本函数，支持传参
    if (!ensureConnected())
        return false;

    if (m_stackingFuncName.isEmpty()) {
        emit stageError(QStringLiteral("未配置码垛脚本函数"));
        stop();
        return false;
    }

    PendingCommand cmd;
    cmd.kind = PendingCommandKind::RunFunc;
    cmd.label = QStringLiteral("RunFunc %1").arg(m_stackingFuncName);
    cmd.funcName = m_stackingFuncName;
    cmd.timeoutMs = 120000;
    cmd.params = m_stackingParams;
    return beginCommandWhenReady(cmd);
}

bool HuayanScheduler::hasActiveRobotCommand() const
{
    return m_pollTimer->isActive()
        || (m_timeoutTimer->isActive() && m_stageStep != StageStep::WaitForVision);
}

void HuayanScheduler::stopVisionWaitTimeout()
{
    if (m_stage == Stage::StageOne
        && m_stageStep == StageStep::WaitForVision
        && m_timeoutTimer->isActive()) {
        emit logMessage(QStringLiteral("[阶段一] 已收到视觉结果，停止 WaitForVision 超时定时器"));
        m_timeoutTimer->stop();
    }
}

bool HuayanScheduler::beginCommandWhenReady(const PendingCommand &cmd)
{
    if (!ensureConnected())
        return false;

    const bool hasPendingCommand = m_pendingCommand.kind != PendingCommandKind::None;
    if (!canQueuePendingCommand(hasPendingCommand, false)) {
        const QString msg = QStringLiteral("待下发命令仍未执行，拒绝覆盖：old=%1 new=%2")
                                .arg(m_pendingCommand.label, cmd.label);
        emitOperationError(msg);
        return false;
    }
    if (!canQueuePendingCommand(false, hasActiveRobotCommand())) {
        const QString msg = QStringLiteral("机械臂仍有命令执行中，拒绝插入新命令：%1").arg(cmd.label);
        emitOperationError(msg);
        return false;
    }

    // 每次命令下发前都重新检查控制器状态。
    // 20018 的现场根因是串行命令之间只判断“不运动”，没有确认控制器已允许下一条命令。
    ++m_commandSeq;
    m_pendingCommand = cmd;
    m_commandReadyElapsedMs = 0;
    m_commandResetIssued = false;
    return pollCommandReady();
}

bool HuayanScheduler::pollCommandReady()
{
    if (m_pendingCommand.kind == PendingCommandKind::None)
        return false;

    int nMovingState = 0;
    int nEnableState = 0;
    int nErrorState = 0;
    int nErrorCode = 0;
    int nErrorAxis = 0;
    int nBreaking = 0;
    int nPause = 0;
    int nBlendingDone = 0;
    int nRet = HRIF_ReadRobotFlags(m_boxID, m_rbtID,
                                   nMovingState, nEnableState, nErrorState,
                                   nErrorCode, nErrorAxis, nBreaking,
                                   nPause, nBlendingDone);
    if (nRet != 0) {
        const QString msg = QStringLiteral("命令前读取机器人状态失败：%1").arg(nRet);
        m_pendingCommand = PendingCommand();
        m_commandReadyTimer->stop();
        emitOperationError(msg);
        return false;
    }

    if (nErrorState != 0) {
        const QString detail = describeError(m_boxID, nErrorCode);
        const QString msg = detail.isEmpty()
            ? QStringLiteral("命令前机器人报错，错误码：%1").arg(nErrorCode)
            : QStringLiteral("命令前机器人报错，错误码：%1（%2）").arg(nErrorCode).arg(detail);
        m_pendingCommand = PendingCommand();
        m_commandReadyTimer->stop();
        emitOperationError(msg);
        return false;
    }

    int nCurFSM = 0;
    string strCurFSM;
    const int fsmRet = HRIF_ReadCurFSM(m_boxID, m_rbtID, nCurFSM, strCurFSM);
    const QString fsmText = fsmRet == 0 ? QString::fromStdString(strCurFSM) : QStringLiteral("unknown");
    const CommandReadiness readiness = evaluateCommandReadiness(nMovingState, nPause, fsmRet, fsmText);
    if (readiness == CommandReadiness::Wait) {
        m_commandReadyElapsedMs += m_commandReadyTimer->interval();
        if (fsmRet == 0
            && fsmText.contains(QStringLiteral("ProgramStopped"), Qt::CaseInsensitive)
            && !m_commandResetIssued) {
            emit logMessage(QStringLiteral("[华沿] 命令前检测到 ProgramStopped，执行 GrpReset 后等待可执行状态：%1")
                                .arg(m_pendingCommand.label));
            const int resetRet = HRIF_GrpReset(m_boxID, m_rbtID);
            if (resetRet != 0)
                emit logMessage(QStringLiteral("[警告] 命令前 GrpReset 失败：%1").arg(resetRet));
            m_commandResetIssued = true;
        }
    } else if (readiness == CommandReadiness::Error) {
        const QString msg = QStringLiteral("命令前读取 FSM 失败：ret=%1，label=%2").arg(fsmRet).arg(m_pendingCommand.label);
        m_pendingCommand = PendingCommand();
        m_commandReadyTimer->stop();
        emitOperationError(msg);
        return false;
    } else {
        PendingCommand cmd = m_pendingCommand;
        m_pendingCommand = PendingCommand();
        m_commandReadyTimer->stop();
        return dispatchReadyCommand(cmd);
    }

    static constexpr int kCommandReadyTimeoutMs = 8000;
    if (m_commandReadyElapsedMs >= kCommandReadyTimeoutMs) {
        const QString msg = QStringLiteral("命令前等待机器人可执行状态超时：%1（moving=%2 pause=%3 fsmRet=%4 fsm=%5/%6）")
            .arg(m_pendingCommand.label)
            .arg(nMovingState)
            .arg(nPause)
            .arg(fsmRet)
            .arg(nCurFSM)
            .arg(fsmText);
        m_pendingCommand = PendingCommand();
        m_commandReadyTimer->stop();
        emitOperationError(msg);
        return false;
    }

    if (!m_commandReadyTimer->isActive())
        m_commandReadyTimer->start();
    return true;
}

bool HuayanScheduler::dispatchReadyCommand(const PendingCommand &cmd)
{
    if (cmd.kind == PendingCommandKind::RunFunc) {
        std::vector<string> params;
        for (const QString &entry : cmd.params)
            params.push_back(entry.toStdString());
        int nRet = HRIF_RunFunc(m_boxID, cmd.funcName.toStdString(), params);
        if (nRet != 0) {
            const QString detail = describeError(m_boxID, nRet);
            emitOperationError(detail.isEmpty()
                ? QStringLiteral("调用函数 %1 失败：%2").arg(cmd.funcName).arg(nRet)
                : QStringLiteral("调用函数 %1 失败：%2（%3）").arg(cmd.funcName).arg(nRet).arg(detail));
            return false;
        }
        startWaitForIdle(cmd.timeoutMs);
        return true;
    }

    if (cmd.kind == PendingCommandKind::MoveRelTool || cmd.kind == PendingCommandKind::MoveRelBase) {
        const int toolMotion = cmd.kind == PendingCommandKind::MoveRelTool ? 1 : 0;
        int nRet = HRIF_MoveRelL(m_boxID, m_rbtID, cmd.poseId, cmd.direction, cmd.distance, toolMotion);
        if (nRet != 0) {
            const QString detail = describeError(m_boxID, nRet);
            emitOperationError(detail.isEmpty()
                ? QStringLiteral("%1失败：%2").arg(cmd.label).arg(nRet)
                : QStringLiteral("%1失败：%2（%3）").arg(cmd.label).arg(nRet).arg(detail));
            return false;
        }
        startWaitForIdle(cmd.timeoutMs);
        return true;
    }

    if (cmd.kind == PendingCommandKind::MoveJ) {
        int nRet = HRIF_MoveJ(m_boxID, m_rbtID,
                              cmd.targetPose.x, cmd.targetPose.y, cmd.targetPose.z,
                              cmd.targetPose.rx, cmd.targetPose.ry, cmd.targetPose.rz,
                              0, 0, 0, 0, 0, 0,
                              kTcpName.toStdString(), cmd.ucsName.toStdString(),
                              kMoveVelocity, kMoveAcceleration, kMoveRadius,
                              0, 0, 0, 0,
                              cmd.cmdId.toStdString());
        if (nRet != 0) {
            const QString detail = describeError(m_boxID, nRet);
            emitOperationError(detail.isEmpty()
                ? QStringLiteral("%1失败：%2").arg(cmd.label).arg(nRet)
                : QStringLiteral("%1失败：%2（%3）").arg(cmd.label).arg(nRet).arg(detail));
            return false;
        }
        startWaitForIdle(cmd.timeoutMs);
        return true;
    }

    return false;
}

void HuayanScheduler::resetArm()
{
    if (!ensureConnected())
        return;

    emit logMessage(QStringLiteral("[复位] 调用复位函数 Func_fuwei"));
    executeRunFunc(QStringLiteral("Func_fuwei"), 30000);
}
