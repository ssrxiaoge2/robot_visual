/**
 * @file huayanScheduler.cpp
 * @brief HuayanScheduler 华研机器人调度实现
 */

#include "huayanScheduler.h"
#include "HR_Pro.h"
#include "palletplacesequence.h"
#include "palletscheduler.h"

#include <QTimer>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
// 机器人运动和位姿常量，参考原有取料 / 卸料场景坐标
// 默认位姿仍保留示例值，真实场景下可通过 set*Pose 接口注入

// 运动参数：现场调速优先使用 speed override；未核对负载和安全空间前不要放大基础值。
static constexpr double kMoveVelocity = 50.0;
static constexpr double kMoveAcceleration = 100.0;
static constexpr double kMoveRadius = 0.0;

// 联合视觉对准窗口参数。XY/Rz 容差和精修次数来自运行时配置，不能在调度器中硬编码。
static constexpr int kStableZWindowFrames = 5; // 最近五个真实新帧去掉一高一低，以中间三帧抵抗偶发深度异常。
static constexpr qint64 kStableZMinElapsedMs = 4000; // 现场观察约 4 秒后深度才稳定；按时间而非易变 FPS 控制。
static constexpr qint64 kStableZMaxElapsedMs = 8000; // 8 秒仍无稳定窗口或新帧则安全失败，禁止盲目下探。
static constexpr double kStableZMaxRangeMm = 5.0; // 去掉一高一低后的中间三帧仍必须满足该核心极差(mm)。
static constexpr int kStableZPollIntervalMs = 100; // /inference 返回缓存，短轮询并用 frame_id 去重，不能按响应次数计帧。
static constexpr double kSearchDescendStep = 20.0; // 未识别目标时每轮搜索下移量(mm)
static constexpr double kMaxSearchDescend  = 80.0; // 搜索累计安全上限(mm)
// GrpReset 退出 ProgramStopped 态是异步的；阶段间快速衔接时（取料→收姿态、倒料→收姿态）
// 复位后立即下发 RunFunc 会撞 20018，须等控制器状态切换。机器人空闲时无此延迟需求。
static constexpr int    kResetSettleMs     = 1000;

// Z 下探参数：下探量 = 视觉深度 - grabZClearance，受 kMaxDescend 和现场硬保护上限共同约束
// grabZClearance 标定法：固定一物体，记视觉深度 D 和能夹到的下探量 H，则 = D - H
//   （本例 D=1048, H=640 → 408）。此值对不同深度通用，视觉深度变化时下探量自动适应。
static constexpr double kMaxDescend     = 1078.0;  // 下探安全上限(mm)，正常不应触发截断
static constexpr double HUAYAN_MAX_SINGLE_XY_ADJUST_MM = 250.0; // 阶段一单次 X/Y 微调硬上限(mm)，现场验证 250mm 可覆盖正常锁定目标微调；超过即 fail-closed 拒绝下发 MoveRelL。
static constexpr double HUAYAN_MAX_Z_DESCEND_MM = 1078.0; // 阶段一 Z 下探硬上限(mm)，不得因临时调试放大，超过即 fail-closed。
static constexpr int HUAYAN_STAGE_ONE_Z_DESCEND_TIMEOUT_MS = 120000; // 阶段一 Z 下探专用到位等待超时(ms)；1 米级下探可能超过 30s，普通 X/Y/Rz 微调仍使用默认超时。
static constexpr bool   kZDescendInvert = false;   // Z 下探方向；若实际朝反方向，改 true
static constexpr double kOffsetIgnoreDistance = 0.5; // 码垛平移死区(mm)
static constexpr double kOffsetIgnoreAngle = 0.5;    // 码垛旋转死区(deg)
static constexpr double kRotateToolAngle = 180.0;    // 扫码补救的工具系 Rz 角(deg)
static constexpr double kLargeRzJumpThreshold = 80.0; // Rz 大角度跳变确认阈值(deg)
static constexpr double kLargeRzJumpDeltaTolerance = 15.0; // 连续两帧幅值接近阈值(deg)
static constexpr int HUAYAN_STAGE_ONE_MAX_LARGE_RZ_EXECUTIONS = 1; // 同一阶段一锁定目标周期内允许实际执行的 Rz 大角度旋转次数；只统计 abs(Rz)>=kLargeRzJumpThreshold 的旋转，防止视觉旧帧导致 90° 重复累计。

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

void HuayanScheduler::applyRuntimeSettings(const RuntimeSettings &settings)
{
    Q_ASSERT(validateRuntimeSettings(settings).ok);
    Q_ASSERT(!isBusy());
    m_runtimeSettings = settings;
    m_pollTimer->setInterval(settings.safety.pollIntervalMs);
    m_commandReadyTimer->setInterval(settings.safety.pollIntervalMs);
    if (m_visionClient)
        m_visionClient->applyRuntimeSettings(settings);
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

bool HuayanScheduler::isBusy() const
{
    return m_stage != Stage::None
        || m_action != Action::None
        || m_pendingCommand.kind != PendingCommandKind::None
        || hasActiveRobotCommand();
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

void HuayanScheduler::setVisionClient(VisionHttpClient *client)
{
    // 非拥有指针：DeviceManager 管理 VisionHttpClient 生命周期，本调度器只在拍照前写入上下文。
    m_visionClient = client;
    if (m_visionClient)
        m_visionClient->applyRuntimeSettings(m_runtimeSettings);
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

void HuayanScheduler::startPalletPlace(const PalletPose &targetOffset,
                                       double releaseZOffsetMm,
                                       double robotBaseHeightFromGroundMm)
{
    startPalletPlaceInternal(targetOffset,
                             releaseZOffsetMm,
                             robotBaseHeightFromGroundMm,
                             true);
}

void HuayanScheduler::startPalletPlaceFromClampedSafety(const PalletPose &targetOffset,
                                                       double releaseZOffsetMm,
                                                       double robotBaseHeightFromGroundMm)
{
    startPalletPlaceInternal(targetOffset,
                             releaseZOffsetMm,
                             robotBaseHeightFromGroundMm,
                             false);
}

void HuayanScheduler::startPalletPlaceInternal(const PalletPose &targetOffset,
                                               double releaseZOffsetMm,
                                               double robotBaseHeightFromGroundMm,
                                               bool clampAtSafety)
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

    if (releaseZOffsetMm < 0.0 || robotBaseHeightFromGroundMm <= 0.0) {
        emit palletPlaceError(QStringLiteral("码垛释放高度或机器人基座离地高度无效，不能执行"));
        return;
    }

    m_palletMoves.clear();
    m_palletMoveIdx = 0;
    m_action = Action::PalletPlace;
    m_actionStep = clampAtSafety ? ActionStep::ClampPalletAtSafety : ActionStep::RunPalletBase;
    m_palletClampAtSafety = clampAtSafety;
    m_pendingPalletTargetOffset = targetOffset;
    m_pendingPalletReleaseZ = 0.0;
    m_pendingPalletReleaseHeightAboveLayer = releaseZOffsetMm;
    m_pendingRobotBaseHeightFromGround = robotBaseHeightFromGroundMm;
    if (!ensureConnected())
        return;
    emit logMessage(QStringLiteral("[码垛] 开始标准单次动作%1 offset X=%2 Y=%3 Z=%4 Rz=%5 releaseZ=%6")
                        .arg(clampAtSafety ? QStringLiteral("（先夹紧）")
                                           : QStringLiteral("（已夹紧，跳过重复夹紧）"))
                        .arg(targetOffset.x, 0, 'f', 1)
                        .arg(targetOffset.y, 0, 'f', 1)
                        .arg(targetOffset.z, 0, 'f', 1)
                        .arg(targetOffset.rz, 0, 'f', 1)
                        .arg(m_pendingPalletReleaseZ, 0, 'f', 1));
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
    // 无论本次启动随后是否因连接/互斥检查被拒绝，都先作废上一任务的推理请求；
    // 这样旧 reply 不可能借启动失败窗口进入下一次合法任务。
    if (m_visionClient)
        m_visionClient->invalidateInferenceRequests();

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
    // 每个新任务只允许一次首次联合 MoveJ，并从零开始统计真正到位的联合精修 MoveL。
    // 待确认修正必须同步清空，避免上一次停止/失败遗留量在新任务到位时误累计到锚点。
    m_initialVisionMoveCompleted = false;
    m_completedFineCorrectionCount = 0;
    m_pendingAlignmentCorrection = {};
    m_pendingLargeRzExecution = false;
    m_searchDescendCount = 0;
    m_searchDescendedMm = 0.0;
    resetStableZValidation();
    resetDepthDescentState();
    m_pendingLargeRzConfirmation = false;
    m_pendingLargeRz = 0.0;
    m_stageOneLargeRzExecutionCount = 0;
    m_waitingPreGripScan = false;
    m_preGripScanSearchCurrentY = 0.0;
    m_preGripScanSearchTargetY = 0.0;
    resetVisionAnchorTracking();

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
    if (rejectStageStartWhileActionRunning(stageName(Stage::Stow))) {
        m_nextStowFuncName.clear();
        return;
    }
    if (!ensureConnected()) {
        m_nextStowFuncName.clear();
        return;
    }

    const RobotStateSnapshot snapshot = readRobotStateSnapshot();
    // 所有收姿态都会记录状态，但不改变原有动作流程。
    emit logMessage(QStringLiteral("[收姿态] 启动前机器人状态：%1")
                        .arg(formatRobotStateSnapshot(snapshot)));

    // 新阶段启动前先切断旧命令的轮询尾巴，避免跨阶段误推进。
    stopPollingAndTimers();
    clearActionState();
    m_stage = Stage::Stow;
    m_stageStep = StageStep::StowArm;
    m_waitingPreGripScan = false;

    emit stageStarted(stageName(m_stage));
    resetAndProceed();
}

void HuayanScheduler::setNextStowFunction(const QString &funcName)
{
    // 只影响下一次收姿态；倒料后的工位定制路径不能污染其他收姿态场景。
    m_nextStowFuncName = funcName;
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
    // 必须在清理状态机前先推进视觉请求代际；abort 引起的旧请求 finished
    // 会被 VisionHttpClient 静默丢弃，不能再次进入本调度器。
    if (m_visionClient)
        m_visionClient->invalidateInferenceRequests();

    // 先按当前阶段状态停止视觉等待，随后统一停止运动轮询和门控计时器。
    // stopPollingAndTimers() 内部只作废一次命令序号，避免重复递增掩盖回调归属。
    stopVisionWaitTimeout();
    stopPollingAndTimers();
    requestRobotStop();
    clearActionState();
    m_searchDescendCount = 0;
    m_searchDescendedMm = 0.0;
    // 未确认到位的修正不得进入锚点累计；停止后也不能被旧运动完成回调再次消费。
    m_pendingAlignmentCorrection = {};
    m_pendingLargeRzExecution = false;
    resetStableZValidation();
    resetDepthDescentState();
    m_pendingLargeRzConfirmation = false;
    m_pendingLargeRz = 0.0;
    m_pendingLargeRzFrameId = -1;
    m_pendingLargeRzTimestampMs = -1;
    m_waitingPreGripScan = false;
    m_preGripScanSearchCurrentY = 0.0;
    m_preGripScanSearchTargetY = 0.0;
    m_initialVisionMoveCompleted = false;
    m_completedFineCorrectionCount = 0;
    // 错误路径已在 emitOperationError() 中输出工位和锁定目标；到这里再清理现场锚点，
    // 既保留故障诊断信息，又保证下一任务不会继承本轮目标。
    resetVisionAnchorTracking();
    m_stage = Stage::None;
    m_stageStep = StageStep::None;
    if (emitStoppedLog) {
        emit logMessage(QStringLiteral("调度已停止"));
        emit schedulerStopped();
    }
}

void HuayanScheduler::startWaitForIdle(int timeoutMs)
{
    m_pollCount = 0;
    m_hasSeenMoving = false;
    m_hasSeenBlendingNotDone = false;
    m_motionElapsedMs = 0;
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
    m_motionElapsedMs += m_pollTimer->interval();

    bool commandCompleted = false;
    if (isActiveVisionMotion()) {
        // ReadRobotFlags 的 blendingDone 可能是上一命令残留；视觉运动必须额外读取
        // 官方“路点运动是否到位”接口，并建立当前命令自己的 not-done→done 证据。
        bool blendingDone = false;
        const int blendingRet =
            HRIF_IsBlendingDone(m_boxID, m_rbtID, blendingDone);
        if (blendingRet == 0 && !blendingDone)
            m_hasSeenBlendingNotDone = true;

        VisionMotionCompletionInput completionInput;
        completionInput.moving = nMovingState != 0;
        completionInput.hasSeenMoving = m_hasSeenMoving;
        completionInput.blendingQuerySucceeded = blendingRet == 0;
        completionInput.blendingDone = blendingDone;
        completionInput.hasSeenBlendingNotDone =
            m_hasSeenBlendingNotDone;
        completionInput.elapsedMs = m_motionElapsedMs;
        completionInput.shortMotionFallbackMs =
            m_runtimeSettings.safety.shortMotionFallbackMs;
        completionInput.actualPoseAtTarget =
            m_motionElapsedMs
                >= m_runtimeSettings.safety.shortMotionFallbackMs
            && actualVisionTargetReached();
        commandCompleted =
            evaluateVisionMotionCompletion(completionInput)
            == VisionMotionCompletion::Complete;
    } else {
        // 非视觉旧动作保留“观察到 moving 或短动作兜底”的既有语义，但时间来自
        // 运行时 shortMotionFallbackMs，不再把 30 次轮询写死成隐式三秒。
        commandCompleted =
            nMovingState == 0
            && (m_hasSeenMoving
                || m_motionElapsedMs
                    >= m_runtimeSettings.safety.shortMotionFallbackMs);
    }

    if (commandCompleted) {
        if (m_activeCommandKind == PendingCommandKind::RunFunc) {
            int nCurFSM = 0;
            string strCurFSM;
            const int fsmRet = HRIF_ReadCurFSM(m_boxID, m_rbtID, nCurFSM, strCurFSM);
            if (fsmRet != 0) {
                emitOperationError(QStringLiteral("RunFunc 完成前读取 FSM 失败：ret=%1 label=%2")
                                       .arg(fsmRet)
                                       .arg(m_activeCommandLabel));
                return;
            }
            if (nCurFSM == 34) {
                // 华沿 SDK demo 中 34 表示 ScriptRunning；RunFunc 未结束前不能把阶段视为完成。
                if (!m_loggedRunFuncScriptRunning) {
                    emit logMessage(QStringLiteral("[华沿] RunFunc 仍处于 ScriptRunning，等待函数结束：label=%1 fsm=%2/%3")
                                        .arg(m_activeCommandLabel)
                                        .arg(nCurFSM)
                                        .arg(QString::fromStdString(strCurFSM)));
                    m_loggedRunFuncScriptRunning = true;
                }
                return;
            }
        }
        // 清空活动命令前保存本次真正到位的命令种类；阶段步骤与命令种类必须同时匹配，
        // 防止状态被外部停止/切换后把无关动作误记为视觉联合运动完成。
        const PendingCommandKind completedCommandKind = m_activeCommandKind;
        const bool completedGrabZDescend =
            m_stage == Stage::StageOne
            && m_stageStep == StageStep::DescendZ
            && m_activeCommandKind == PendingCommandKind::MoveRelTool
            && m_activeCommandLabel == QStringLiteral("Z 下探");
        const bool completedGrabXCompensation =
            m_stage == Stage::StageOne
            && m_stageStep == StageStep::DescendZ
            && m_activeCommandKind == PendingCommandKind::MoveRelTool
            && m_activeCommandLabel == QStringLiteral("X 补偿");
        const bool completedDepthDescent =
            m_stage == Stage::StageOne
            && m_stageStep == StageStep::DepthDescent
            && m_activeCommandKind == PendingCommandKind::MoveRelTool
            && m_activeCommandLabel == QStringLiteral("深度自动下探");
        m_activeCommandKind = PendingCommandKind::None;
        m_activeCommandLabel.clear();
        m_activeVisionTargetPoseValid = false;
        m_loggedRunFuncScriptRunning = false;
        m_pollTimer->stop();
        m_timeoutTimer->stop();
        if (m_stage == Stage::StageOne
            && (m_stageStep == StageStep::MoveToPregrasp
                || m_stageStep == StageStep::FineCorrectAlignment)
            && ((m_stageStep == StageStep::MoveToPregrasp
                 && completedCommandKind == PendingCommandKind::VisionPregraspMoveJ)
                || (m_stageStep == StageStep::FineCorrectAlignment
                    && completedCommandKind
                        == PendingCommandKind::VisionFineCorrectionMoveL))) {
            // 只有控制器轮询确认到位后才能累计锚点或精修次数；随后再开启新视觉窗口。
            // SDK 拒绝、命令门控失败、超时和 stop() 都不会进入此分支。
            recordCompletedVisionAlignmentMove();
            enterVisionAlignmentValidation();
            return;
        }
        if (m_action == Action::PalletPlace
            && (m_actionStep == ActionStep::MovePalletXY
                || m_actionStep == ActionStep::DescendPalletZ
                || m_actionStep == ActionStep::LiftAfterPalletRelease)) {
            m_palletMoveIdx++;
            const quint64 seq = nextCallbackSeq();
            QTimer::singleShot(300, this, [this, seq] {
                if (seq == m_commandSeq
                    && m_action == Action::PalletPlace
                    && (m_actionStep == ActionStep::MovePalletXY
                        || m_actionStep == ActionStep::DescendPalletZ
                        || m_actionStep == ActionStep::LiftAfterPalletRelease))
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
            QTimer::singleShot(m_runtimeSettings.vision.settleMs, this, [this, seq] {
                if (seq == m_commandSeq
                    && m_stage == Stage::StageOne
                    && m_stageStep == StageStep::WaitForVision)
                    proceedStage();
            });
            return;
        }
        if (completedDepthDescent) {
            m_depthDescentAccumulatedMm += m_pendingDepthDescentMm;
            m_pendingDepthDescentMm = 0.0;
            emit logMessage(QStringLiteral("[深度下探] 移动到位，累计下探 %1/%2mm，等待视觉稳定后重新检测")
                                .arg(m_depthDescentAccumulatedMm, 0, 'f', 1)
                                .arg(m_runtimeSettings.depthDescent.maxAccumulatedMm, 0, 'f', 1));
            m_stageStep = StageStep::WaitForVision;
            const quint64 seq = nextCallbackSeq();
            QTimer::singleShot(m_runtimeSettings.vision.settleMs, this, [this, seq] {
                if (seq == m_commandSeq
                    && m_stage == Stage::StageOne
                    && m_stageStep == StageStep::WaitForVision) {
                    if (m_visionClient)
                        m_visionClient->setTargetSelectionContext(makeVisionTargetSelectionContext());
                    emit surveyReady();
                    m_timeoutTimer->start(10000);
                }
            });
            return;
        }
        const double grabXCompensation =
            m_runtimeSettings.pickup.grabXCompensationMm;
        const double grabYCompensation =
            m_runtimeSettings.pickup.grabYCompensationMm;
        if (completedGrabZDescend && qAbs(grabXCompensation) >= kOffsetIgnoreDistance) {
            emit logMessage(QStringLiteral("[阶段一] Z 下探到位，执行 X 补偿 %1mm")
                                .arg(grabXCompensation, 0, 'f', 1));
            PendingCommand cmd;
            cmd.kind = PendingCommandKind::MoveRelTool;
            cmd.label = QStringLiteral("X 补偿");
            cmd.poseId = 0;
            cmd.direction = grabXCompensation >= 0.0 ? 1 : 0;
            cmd.distance = qAbs(grabXCompensation);
            beginCommandWhenReady(cmd);
            return;
        }
        if ((completedGrabZDescend || completedGrabXCompensation)
            && qAbs(grabYCompensation) >= kOffsetIgnoreDistance) {
            emit logMessage(QStringLiteral("[阶段一] 执行 Y 补偿 %1mm")
                                .arg(grabYCompensation, 0, 'f', 1));
            PendingCommand cmd;
            cmd.kind = PendingCommandKind::MoveRelTool;
            cmd.label = QStringLiteral("Y 补偿");
            cmd.poseId = 1;
            cmd.direction = grabYCompensation >= 0.0 ? 1 : 0;
            cmd.distance = qAbs(grabYCompensation);
            beginCommandWhenReady(cmd);
            return;
        }
        advanceStep();
        proceedStage();
    }
}

void HuayanScheduler::onStepTimeout()
{
    m_pollTimer->stop();
    if (m_stage == Stage::StageOne
        && m_stageStep == StageStep::ValidateVisionAlignment) {
        const VisionAlignment::Sample sample{
            m_grabOffset.x,
            m_grabOffset.y,
            m_grabOffset.z,
            m_grabOffset.rz,
            false};
        emitOperationError(formatVisionAlignmentFailure(
            sample,
            QStringLiteral("观察窗口达到8秒仍未获得可判定的真实新帧")));
        return;
    }
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
        case StageStep::MoveToPregrasp:
        case StageStep::FineCorrectAlignment:
            m_stageStep = StageStep::ValidateVisionAlignment;
            break;
        case StageStep::ValidateVisionAlignment:
            m_stageStep = StageStep::WaitForVision;
            break;
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
    case StageStep::DepthDescent:            return 0;
    case StageStep::ValidateVisionAlignment: return 0;
    case StageStep::MoveToPregrasp:
    case StageStep::FineCorrectAlignment:
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
            if (m_visionClient)
                m_visionClient->setTargetSelectionContext(makeVisionTargetSelectionContext());
            emit surveyReady();
            m_timeoutTimer->start(10000);
            break;
        case StageStep::ValidateVisionAlignment:
            // 下一次视觉请求由 enterVisionAlignmentValidation() 或上一帧的动作分支发起；
            // 此状态不下发额外机械臂运动，确保一个窗口内的所有样本来自同一静止位姿。
            break;
        case StageStep::SearchDescend:
        case StageStep::DepthDescent:
            break;
        case StageStep::MoveToPregrasp:
        case StageStep::FineCorrectAlignment:
            // 两种联合运动均由视觉回调完成命令组装并进入统一门控，此处只表示等待到位。
            break;
        case StageStep::DescendZ: {
            const double plannedDescend = m_grabOffset.z - m_grabZClearance;
            if (plannedDescend > HUAYAN_MAX_Z_DESCEND_MM) {
                // 先按未截断计划下探量(mm)做硬保护；超过即 fail-closed，禁止把异常深度截断后继续下发 MoveRelL。
                emitOperationError(QStringLiteral("[阶段一] 目标不可信：计划 Z 下探 %1mm 超过硬上限 %2mm，拒绝下发 MoveRelL")
                                       .arg(plannedDescend, 0, 'f', 1)
                                       .arg(HUAYAN_MAX_Z_DESCEND_MM, 0, 'f', 1));
                return;
            }
            const double descend = calculateGrabDescend(
                m_grabOffset.z,
                m_grabZClearance,
                qMin(kMaxDescend, HUAYAN_MAX_Z_DESCEND_MM));
            if (descend < 1.0) {
                emit logMessage(QStringLiteral("[阶段一] 无需 Z 下探，已到扫码/夹取前位置"));
                m_stageStep = m_preGripScanEnabled ? StageStep::WaitPreGripScan : StageStep::CloseGripper;
                proceedStage();
                break;
            }
            emit logMessage(QStringLiteral("[阶段一] Z 下探 %1mm（未截断计划 %2mm = 视觉深度 %3mm - 余量 %4mm，下发上限 %5mm，硬上限 %6mm，到位超时 %7ms）")
                                .arg(descend, 0, 'f', 1)
                                .arg(plannedDescend, 0, 'f', 1)
                                .arg(m_grabOffset.z, 0, 'f', 1)
                                .arg(m_grabZClearance, 0, 'f', 1)
                                .arg(qMin(kMaxDescend, HUAYAN_MAX_Z_DESCEND_MM), 0, 'f', 1)
                                .arg(HUAYAN_MAX_Z_DESCEND_MM, 0, 'f', 1)
                                .arg(HUAYAN_STAGE_ONE_Z_DESCEND_TIMEOUT_MS));
            PendingCommand cmd;
            cmd.kind = PendingCommandKind::MoveRelTool;
            cmd.label = QStringLiteral("Z 下探");
            cmd.poseId = 2;
            cmd.direction = kZDescendInvert ? 0 : 1;
            cmd.distance = descend;
            cmd.timeoutMs = HUAYAN_STAGE_ONE_Z_DESCEND_TIMEOUT_MS;
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
        {
            const QString stowFunc = m_nextStowFuncName.isEmpty()
                ? m_stowFuncName
                : m_nextStowFuncName;
            // 一次性覆盖使用后立即清空，避免取料后、码垛后或 Cleanup 误用倒料后路径。
            m_nextStowFuncName.clear();
            emit logMessage(QStringLiteral("[收姿态] 调用 %1").arg(stowFunc));
            executeRunFunc(stowFunc);
            break;
        }
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

void HuayanScheduler::resetVisionAnchorTracking()
{
    m_anchorAccumulatedToolX = 0.0;
    m_anchorAccumulatedToolY = 0.0;
    m_anchorHasPreviousTarget = false;
    m_anchorPreviousTargetX = 0.0;
    m_anchorPreviousTargetY = 0.0;
    m_anchorMissingFrames = 0;
    m_stageOneLargeRzExecutionCount = 0;
}

VisionHttpClient::TargetSelectionContext HuayanScheduler::makeVisionTargetSelectionContext() const
{
    VisionHttpClient::TargetSelectionContext context;
    context.anchorEnabled = m_stage == Stage::StageOne;
    context.lockEnabled = true;
    context.stationRoiHalfX = m_runtimeSettings.vision.stationRoiHalfXmm;
    context.stationRoiHalfY = m_runtimeSettings.vision.stationRoiHalfYmm;
    context.accumulatedToolX = m_anchorAccumulatedToolX;
    context.accumulatedToolY = m_anchorAccumulatedToolY;
    context.hasPreviousAnchorTarget = m_anchorHasPreviousTarget;
    context.previousAnchorX = m_anchorPreviousTargetX;
    context.previousAnchorY = m_anchorPreviousTargetY;
    context.lockMissingFrames = m_anchorMissingFrames;
    context.maxTrustX = m_runtimeSettings.vision.anchorMaxTrustXmm;
    context.maxTrustY = m_runtimeSettings.vision.anchorMaxTrustYmm;
    context.sameLayerZTol = m_runtimeSettings.vision.anchorSameLayerToleranceMm;
    context.maxSwitchDistance = m_runtimeSettings.vision.anchorSwitchMaxXyMm;
    context.maxLockMissingFrames = m_runtimeSettings.vision.lockMaxMissingFrames;
    context.lockTrackRadius = m_runtimeSettings.vision.lockTrackRadiusMm;
    context.lockSameLayerZTol = m_runtimeSettings.vision.lockSameLayerToleranceMm;
    return context;
}

void HuayanScheduler::recordCompletedVisionAlignmentMove()
{
    // pending 中保存的是统一方向换算后的实际工具系修正；只有运动轮询确认到位后，
    // 才能把 X/Y 写入拍照锚点。Rz 只改变姿态，不得污染锚点平面累计量。
    m_anchorAccumulatedToolX += m_pendingAlignmentCorrection.xMm;
    m_anchorAccumulatedToolY += m_pendingAlignmentCorrection.yMm;
    if (m_pendingLargeRzExecution) {
        // 大角度次数表达“机械臂已实际执行”，不能在视觉连续帧确认时提前消耗。
        // 否则统一窗口等待4秒期间，后续大角度帧会被旧保护逻辑置零并形成假对准。
        ++m_stageOneLargeRzExecutionCount;
    }

    QString completedKind = QStringLiteral("未知");
    if (m_stageStep == StageStep::MoveToPregrasp) {
        m_initialVisionMoveCompleted = true;
        completedKind = QStringLiteral("初始");
    } else if (m_stageStep == StageStep::FineCorrectAlignment) {
        ++m_completedFineCorrectionCount;
        completedKind = QStringLiteral("精修");
    }

    emit logMessage(
        QStringLiteral("[阶段一][联合运动完成] kind=%1 "
                       "accumulatedToolXY=(X=%2mm,Y=%3mm) completedFine=%4/%5")
            .arg(completedKind)
            .arg(m_anchorAccumulatedToolX, 0, 'f', 1)
            .arg(m_anchorAccumulatedToolY, 0, 'f', 1)
            .arg(m_completedFineCorrectionCount)
            .arg(m_runtimeSettings.vision.maxFineCorrectionCount));

    m_pendingAlignmentCorrection = {};
    m_pendingLargeRzExecution = false;
}

void HuayanScheduler::enterVisionAlignmentValidation()
{
    if (!m_visionClient
        || m_visionClient->lastInferenceFrameId() < 0
        || m_visionClient->lastInferenceTimestampMs() < 0) {
        const VisionAlignment::Sample sample{
            m_grabOffset.x,
            m_grabOffset.y,
            m_grabOffset.z,
            m_grabOffset.rz,
            false};
        emitOperationError(formatVisionAlignmentFailure(
            sample,
            QStringLiteral("算法响应缺少 frame_id 或 timestamp，无法建立运动后新帧基线")));
        return;
    }

    // 每次 MoveJ/MoveL 真正到位后都丢弃上一个窗口的全部 Z 样本，并把当前算法帧
    // 记录为基线。下一次仅接受 frame_id 与 timestamp 同时递增的帧，因而无需附加
    // settle 延迟或“再等一帧”，同时也不会把运动前缓存误算成运动后观测。
    resetStableZValidation();
    m_stableZLastFrameId = m_visionClient->lastInferenceFrameId();
    m_stableZLastTimestampMs = m_visionClient->lastInferenceTimestampMs();
    m_stableZElapsedTimer.start();
    m_stageStep = StageStep::ValidateVisionAlignment;
    requestNextStableZFrame();

    emit logMessage(
        QStringLiteral("[阶段一][联合对准] 联合运动已到位，立即开启4～8秒观察窗口：基线帧=%1，"
                       "已完成精修=%2/%3")
            .arg(m_stableZLastFrameId)
            .arg(m_completedFineCorrectionCount)
            .arg(m_runtimeSettings.vision.maxFineCorrectionCount));
}

QString HuayanScheduler::formatVisionAlignmentFailure(
    const VisionAlignment::Sample &sample,
    const QString &reason) const
{
    const qint64 elapsedMs = m_stableZElapsedTimer.isValid()
        ? m_stableZElapsedTimer.elapsed()
        : 0;

    // m_captureFuncName 是当前任务按工位注入的固定拍照位函数，可稳定定位现场工位；
    // anchorPreviousTarget 是视觉端最后一次确认的锁定目标坐标，便于排查串工位或跳目标。
    return QStringLiteral(
               "[阶段一][联合对准] 工位=%1，锁定目标锚点=(%2,%3)，"
               "当前=(X=%4,Y=%5,Z=%6,Rz=%7)，阈值=(XY≤%8mm,Rz≤%9°)，"
               "精修=%10/%11，窗口耗时=%12ms，真实新帧=%13，停止原因=%14")
        .arg(m_captureFuncName)
        .arg(m_anchorPreviousTargetX, 0, 'f', 1)
        .arg(m_anchorPreviousTargetY, 0, 'f', 1)
        .arg(sample.xMm, 0, 'f', 1)
        .arg(sample.yMm, 0, 'f', 1)
        .arg(sample.zMm, 0, 'f', 1)
        .arg(sample.rzDeg, 0, 'f', 1)
        .arg(m_runtimeSettings.vision.xyToleranceMm, 0, 'f', 1)
        .arg(m_runtimeSettings.vision.rzToleranceDeg, 0, 'f', 1)
        .arg(m_completedFineCorrectionCount)
        .arg(m_runtimeSettings.vision.maxFineCorrectionCount)
        .arg(elapsedMs)
        .arg(m_stableZUniqueFrames)
        .arg(reason);
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
        case ActionStep::ClampPalletAtSafety:
            emit logMessage(QStringLiteral("[码垛] 安全位夹紧 %1").arg(m_gripFuncName));
            executeRunFunc(m_gripFuncName, 30000);
            break;
        case ActionStep::RunPalletBase:
            emit logMessage(QStringLiteral("[码垛] 调用基准点函数 %1").arg(m_palletBaseFuncName));
            executeRunFunc(m_palletBaseFuncName, 120000);
            break;
        case ActionStep::MovePalletXY: {
            PalletPose basePose;
            QString readError;
            if (!readActualTcpPose(&basePose, &readError)) {
                actionError(QStringLiteral("读取码垛基准点 TCP 位姿失败：%1").arg(readError));
                break;
            }
            QString sequenceError;
            const QList<PalletPlaceStep> steps = buildPalletPlaceSequence(
                m_pendingPalletTargetOffset,
                m_pendingPalletReleaseHeightAboveLayer,
                m_pendingRobotBaseHeightFromGround,
                basePose.z,
                &sequenceError);
            if (steps.isEmpty()) {
                actionError(QStringLiteral("码垛 Z 计算失败：%1").arg(sequenceError));
                break;
            }
            const double releaseGroundZ = PalletScheduler::releaseGroundZ(
                m_pendingPalletTargetOffset, m_pendingPalletReleaseHeightAboveLayer);
            const double targetTcpZ = PalletScheduler::releaseTcpZ(
                m_pendingPalletTargetOffset,
                m_pendingPalletReleaseHeightAboveLayer,
                m_pendingRobotBaseHeightFromGround);
            const double calculatedPalletReleaseZ = targetTcpZ - basePose.z;
            m_pendingPalletReleaseZ = steps.at(3).offset.z;
            emit logMessage(QStringLiteral("[码垛] 基准TCP Z=%1，目标TCP Z=%2，释放地面Z=%3，机器人基座离地=%4，夹爪释放补偿=%5，本次Z相对移动=%6")
                                .arg(basePose.z, 0, 'f', 1)
                                .arg(targetTcpZ, 0, 'f', 1)
                                .arg(releaseGroundZ, 0, 'f', 1)
                                .arg(m_pendingRobotBaseHeightFromGround, 0, 'f', 1)
                                .arg(PALLET_GRIPPER_RELEASE_Z_OFFSET_MM, 0, 'f', 1)
                                .arg(m_pendingPalletReleaseZ, 0, 'f', 1));
            Q_ASSERT(qFuzzyCompare(calculatedPalletReleaseZ + 1.0, m_pendingPalletReleaseZ + 1.0));
            auto addMove = [this](int poseId, double value, double ignoreThreshold) {
                if (qAbs(value) < ignoreThreshold)
                    return;
                m_palletMoves.append({ poseId, value >= 0 ? 1 : 0, qAbs(value) });
            };
            m_palletMoves.clear();
            m_palletMoveIdx = 0;
            addMove(0, m_pendingPalletTargetOffset.x, kOffsetIgnoreDistance);
            addMove(1, m_pendingPalletTargetOffset.y, kOffsetIgnoreDistance);
            addMove(5, m_pendingPalletTargetOffset.rz, kOffsetIgnoreAngle);
            executeNextPalletMove();
            break;
        }
        case ActionStep::DescendPalletZ:
            m_palletMoves = {
                {2, m_pendingPalletReleaseZ >= 0.0 ? 1 : 0, qAbs(m_pendingPalletReleaseZ)},
            };
            m_palletMoveIdx = 0;
            executeNextPalletMove();
            break;
        case ActionStep::ReleasePallet:
            emit logMessage(QStringLiteral("[码垛] 调用松爪函数 %1").arg(m_releaseFuncName));
            executeRunFunc(m_releaseFuncName, 30000);
            break;
        case ActionStep::LiftAfterPalletRelease:
            m_palletMoves = {
                {2, m_pendingPalletReleaseZ >= 0.0 ? 0 : 1, qAbs(m_pendingPalletReleaseZ)},
            };
            m_palletMoveIdx = 0;
            executeNextPalletMove();
            break;
        case ActionStep::StowAfterPalletRelease:
            emit logMessage(QStringLiteral("[码垛] 回运行安全位 %1").arg(m_stowFuncName));
            executeRunFunc(m_stowFuncName, 120000);
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
        if (m_actionStep == ActionStep::ClampPalletAtSafety)
            m_actionStep = ActionStep::RunPalletBase;
        else if (m_actionStep == ActionStep::RunPalletBase)
            m_actionStep = ActionStep::MovePalletXY;
        else if (m_actionStep == ActionStep::MovePalletXY)
            m_actionStep = ActionStep::DescendPalletZ;
        else if (m_actionStep == ActionStep::DescendPalletZ)
            m_actionStep = ActionStep::ReleasePallet;
        else if (m_actionStep == ActionStep::ReleasePallet)
            m_actionStep = ActionStep::LiftAfterPalletRelease;
        else if (m_actionStep == ActionStep::LiftAfterPalletRelease)
            m_actionStep = ActionStep::StowAfterPalletRelease;
        else if (m_actionStep == ActionStep::StowAfterPalletRelease)
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
        switch (m_actionStep) {
        case ActionStep::MovePalletXY:
            emit logMessage(QStringLiteral("[码垛] 目标上方平移完成，准备执行释放高度 Z 动作"));
            break;
        case ActionStep::DescendPalletZ:
            emit logMessage(QStringLiteral("[码垛] 已到释放高度，准备松爪"));
            break;
        case ActionStep::LiftAfterPalletRelease:
            emit logMessage(QStringLiteral("[码垛] 松爪后抬升完成，准备回运行安全位"));
            break;
        default:
            emit logMessage(QStringLiteral("[码垛] 相对移动完成"));
            break;
        }
        advanceActionStep();
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
    m_pendingPalletTargetOffset = PalletPose();
    m_pendingPalletReleaseZ = 0.0;
    m_pendingPalletReleaseHeightAboveLayer = 0.0;
    m_pendingRobotBaseHeightFromGround = 850.0;
    m_palletClampAtSafety = true;
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
    m_hasSeenBlendingNotDone = false;
    m_motionElapsedMs = 0;
    m_pendingCommand = PendingCommand();
    m_activeCommandKind = PendingCommandKind::None;
    m_activeCommandLabel.clear();
    m_activeVisionTargetPoseValid = false;
    m_loggedRunFuncScriptRunning = false;
    m_commandReadyElapsedMs = 0;
    m_commandResetIssued = false;
    // 所有延时回调都携带命令序号；统一递增一次即可让旧 singleShot 安全失效。
    nextCallbackSeq();
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

    const bool directVisionAlignmentFailure =
        isVisionAlignmentFailureStep();
    if (m_stage == Stage::StageOne) {
        // 独立 Action 已在上方按原语义返回；只有真正进入 stageError/stop 的阶段一
        // 错误才能记录“安全停止”。日志必须在 stop() 清理锚点和窗口前输出。
        emit logMessage(
            QStringLiteral("[阶段一][安全停止] station=%1 "
                           "anchor=(X=%2mm,Y=%3mm) reason=%4")
                .arg(m_captureFuncName)
                .arg(m_anchorPreviousTargetX, 0, 'f', 1)
                .arg(m_anchorPreviousTargetY, 0, 'f', 1)
                .arg(msg));
    }

    if (directVisionAlignmentFailure) {
        // Qt AutoConnection 在同线程中是同步调用。必须先 stop() 作废视觉代际、定时器、
        // pending 命令和机器人运动，再通知 TaskExecutor；否则上层同步启动 CleanupStow
        // 后，本函数尾部的 stop() 会把刚启动的收姿态再次清掉。
        stop();
        emit visionAlignmentFailed(msg);
        return;
    }

    emit stageError(msg);
    if (m_stage != Stage::None || m_waitingPreGripScan
        || m_pollTimer->isActive() || m_timeoutTimer->isActive()) {
        stop();
    }
}

bool HuayanScheduler::isVisionAlignmentFailureStep() const
{
    if (m_stage != Stage::StageOne)
        return false;

    switch (m_stageStep) {
    case StageStep::WaitForVision:
    case StageStep::MoveToPregrasp:
    case StageStep::ValidateVisionAlignment:
    case StageStep::FineCorrectAlignment:
        return true;
    default:
        return false;
    }
}

bool HuayanScheduler::ensureConnected()
{
    if (isConnected())
        return true;

    return connectRobot();
}

bool HuayanScheduler::executeMoveJ(double x, double y, double z,
                                  double rx, double ry, double rz)
{
    // Qt5 的 QStringLiteral 默认实参会在部分 GCC/assembler 组合下生成重复局部符号。
    // 默认命令号和坐标系放在实现文件中转发，避免头文件默认参数参与每个翻译单元展开。
    return executeMoveJ(x, y, z, rx, ry, rz,
                        QStringLiteral("0"), QStringLiteral("Base"));
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

void HuayanScheduler::resetDepthDescentState()
{
    m_depthDescentAccumulatedMm = 0.0;
    m_pendingDepthDescentMm = 0.0;
}

void HuayanScheduler::resetStableZValidation()
{
    m_stableZSamples.clear();
    m_stableZLastFrameId = -1;
    m_stableZLastTimestampMs = -1;
    m_stableZElapsedTimer.invalidate();
    m_stableZUniqueFrames = 0;
}

void HuayanScheduler::requestNextStableZFrame()
{
    // /inference 返回后台最新缓存，不代表每次 HTTP 响应都有新相机帧。
    // 以 100ms 短轮询查询并在回调中用 frame_id 去重，兼顾当前约 2 FPS 和后续帧率提升。
    const quint64 seq = nextCallbackSeq();
    QTimer::singleShot(kStableZPollIntervalMs, this, [this, seq] {
        if (seq != m_commandSeq
            || m_stage != Stage::StageOne
            || m_stageStep != StageStep::ValidateVisionAlignment) {
            return;
        }

        if (m_stableZElapsedTimer.isValid()
            && m_stableZElapsedTimer.elapsed() >= kStableZMaxElapsedMs) {
            const VisionAlignment::Sample sample{
                m_grabOffset.x,
                m_grabOffset.y,
                m_grabOffset.z,
                m_grabOffset.rz,
                false};
            emitOperationError(formatVisionAlignmentFailure(
                sample,
                QStringLiteral("观察窗口达到8秒，真实新帧不足或尚未同时满足XY/Rz与Z稳定")));
            return;
        }

        if (m_visionClient)
            m_visionClient->setTargetSelectionContext(makeVisionTargetSelectionContext());
        // 单次 HTTP 请求的等待保护不能越过本窗口 8 秒硬上限。视觉客户端自身仍保留
        // 5 秒传输超时；这里取窗口剩余时间，确保低帧率或丢帧时不会额外等待。
        const qint64 elapsedMs = m_stableZElapsedTimer.elapsed();
        const int remainingWindowMs = static_cast<int>(
            qMax<qint64>(1, kStableZMaxElapsedMs - elapsedMs));
        m_timeoutTimer->start(remainingWindowMs);
        emit surveyReady();
    });
}

bool HuayanScheduler::handleExcessiveVisionDepth(double depthMm)
{
    const DepthDescentDecision decision =
        decideDepthDescent(depthMm, m_depthDescentAccumulatedMm,
                           m_runtimeSettings.depthDescent);
    if (decision.action == DepthDescentDecision::Action::ContinuePickup)
        return false;

    if (decision.action == DepthDescentDecision::Action::FailLimitReached) {
        emitOperationError(
            QStringLiteral("[深度下探] depth=%1mm 仍大于阈值 %2mm，累计下探已达上限 %3mm，任务失败")
                .arg(depthMm, 0, 'f', 1)
                .arg(m_runtimeSettings.depthDescent.triggerDepthMm, 0, 'f', 1)
                .arg(m_runtimeSettings.depthDescent.maxAccumulatedMm, 0, 'f', 1));
        return true;
    }

    const double totalAfterMove =
        m_depthDescentAccumulatedMm + decision.moveMm;
    if (decision.moveMm <= 0.0
        || totalAfterMove > m_runtimeSettings.safety.maxZDescendMm) {
        emitOperationError(
            QStringLiteral("[深度下探] 计划累计下探 %1mm 超过 Z 安全上限 %2mm，拒绝执行")
                .arg(totalAfterMove, 0, 'f', 1)
                .arg(m_runtimeSettings.safety.maxZDescendMm, 0, 'f', 1));
        return true;
    }

    emit logMessage(
        QStringLiteral("[深度下探] depth=%1mm > threshold=%2mm，本次下探 %3mm，累计 %4/%5mm")
            .arg(depthMm, 0, 'f', 1)
            .arg(m_runtimeSettings.depthDescent.triggerDepthMm, 0, 'f', 1)
            .arg(decision.moveMm, 0, 'f', 1)
            .arg(totalAfterMove, 0, 'f', 1)
            .arg(m_runtimeSettings.depthDescent.maxAccumulatedMm, 0, 'f', 1));
    executeDepthDescent(decision.moveMm);
    return true;
}

void HuayanScheduler::executeDepthDescent(double moveMm)
{
    if (!ensureConnected())
        return;

    m_stageStep = StageStep::DepthDescent;
    PendingCommand cmd;
    cmd.kind = PendingCommandKind::MoveRelTool;
    cmd.label = QStringLiteral("深度自动下探");
    cmd.poseId = 2;
    cmd.direction = m_runtimeSettings.pickup.zDescendInvert ? 0 : 1;
    cmd.distance = moveMm;
    cmd.timeoutMs = m_runtimeSettings.safety.longZMotionTimeoutMs;
    if (beginCommandWhenReady(cmd)) {
        m_pendingDepthDescentMm = moveMm;
    } else if (m_stage == Stage::StageOne
               && m_stageStep == StageStep::DepthDescent) {
        m_stageStep = StageStep::WaitForVision;
    }
}

void HuayanScheduler::setGrabOffset(double x, double y, double z, double rz)
{
    const bool validatingVisionAlignment =
        m_stageStep == StageStep::ValidateVisionAlignment;
    if (m_stage != Stage::StageOne
        || (m_stageStep != StageStep::WaitForVision
            && !validatingVisionAlignment)) {
        return;
    }

    stopVisionWaitTimeout();

    qint64 frameId =
        m_visionClient ? m_visionClient->lastInferenceFrameId() : -1;
    qint64 timestampMs =
        m_visionClient ? m_visionClient->lastInferenceTimestampMs() : -1;
    if (validatingVisionAlignment) {
        if (!m_visionClient) {
            const VisionAlignment::Sample invalidSample{x, y, z, rz, false};
            emitOperationError(formatVisionAlignmentFailure(
                invalidSample,
                QStringLiteral("未注入视觉客户端，无法校验真实新帧")));
            return;
        }

        if (frameId < 0 || timestampMs < 0) {
            const VisionAlignment::Sample invalidSample{x, y, z, rz, false};
            emitOperationError(formatVisionAlignmentFailure(
                invalidSample,
                QStringLiteral("算法响应缺少 frame_id 或 timestamp，无法排除缓存重复帧")));
            return;
        }

        if (frameId <= m_stableZLastFrameId) {
            if (frameId < m_stableZLastFrameId) {
                const VisionAlignment::Sample invalidSample{x, y, z, rz, false};
                emitOperationError(formatVisionAlignmentFailure(
                    invalidSample,
                    QStringLiteral("frame_id 从 %1 回退到 %2，疑似视觉服务重启")
                        .arg(m_stableZLastFrameId)
                        .arg(frameId)));
                return;
            }

            const qint64 elapsedMs = m_stableZElapsedTimer.elapsed();
            if (elapsedMs >= kStableZMaxElapsedMs) {
                const VisionAlignment::Sample invalidSample{x, y, z, rz, false};
                emitOperationError(formatVisionAlignmentFailure(
                    invalidSample,
                    QStringLiteral("观察窗口达到8秒，重复缓存帧不能形成新的联合判定样本")));
                return;
            }

            emit logMessage(QStringLiteral("[阶段一][联合对准] frame_id=%1 为重复缓存帧，不计入样本；真实新帧=%2，窗口耗时=%3ms")
                                .arg(frameId)
                                .arg(m_stableZUniqueFrames)
                                .arg(elapsedMs));
            requestNextStableZFrame();
            return;
        }

        if (timestampMs <= m_stableZLastTimestampMs) {
            const VisionAlignment::Sample invalidSample{x, y, z, rz, false};
            emitOperationError(formatVisionAlignmentFailure(
                invalidSample,
                QStringLiteral("新 frame_id=%1 的 timestamp=%2 未大于上一帧 %3")
                    .arg(frameId)
                    .arg(timestampMs)
                    .arg(m_stableZLastTimestampMs)));
            return;
        }

        m_stableZLastFrameId = frameId;
        m_stableZLastTimestampMs = timestampMs;
    }

    // 深度自动下探只属于首次绝对预抓取前的原流程。进入联合验证窗口后，
    // 当前 Z 仅作为最终抓取深度样本，不能绕开统一判定另起一条运动路径。
    if (!validatingVisionAlignment && handleExcessiveVisionDepth(z))
        return;

    if (!validatingVisionAlignment
        && qAbs(rz) >= m_runtimeSettings.vision.largeRzJumpThresholdDeg
        && (frameId < 0 || timestampMs < 0)) {
        const VisionAlignment::Sample invalidSample{x, y, z, rz, false};
        emitOperationError(formatVisionAlignmentFailure(
            invalidSample,
            QStringLiteral("Rz大角度初次确认缺少frame_id或timestamp，无法证明两个真实新帧")));
        return;
    }

    const LargeRzConfirmationState largeRzState{
        m_pendingLargeRzConfirmation,
        m_pendingLargeRz,
        m_pendingLargeRzFrameId,
        m_pendingLargeRzTimestampMs};
    const LargeRzEvaluation largeRz = evaluateLargeRzConfirmation(
        largeRzState,
        rz,
        frameId,
        timestampMs,
        m_runtimeSettings.vision.largeRzJumpThresholdDeg,
        m_runtimeSettings.vision.largeRzDeltaToleranceDeg,
        m_stageOneLargeRzExecutionCount,
        m_runtimeSettings.vision.maxLargeRzExecutions);

    // measuredRz 始终保留真实视觉残差；motionRz 只表达本帧是否获准旋转。
    // 这两个语义绝不能再复用一个 effectiveRz，否则达到旋转上限时会把未收敛伪装成 0。
    const double measuredRz = largeRz.measuredRzDeg;
    const double motionRz = largeRz.motionRzDeg;
    const bool suppressLargeRzRotation =
        largeRz.action == LargeRzAction::WaitForNewFrame;
    const bool largeRzExecutionLimitReached =
        largeRz.action == LargeRzAction::ExecutionLimitReached;
    m_pendingLargeRzConfirmation = largeRz.nextState.pending;
    m_pendingLargeRz = largeRz.nextState.rzDeg;
    m_pendingLargeRzFrameId = largeRz.nextState.frameId;
    m_pendingLargeRzTimestampMs = largeRz.nextState.timestampMs;

    if (suppressLargeRzRotation) {
        emit logMessage(QStringLiteral(
            "[阶段一] Rz大角度候选等待真实新帧确认：Rz=%1 frame=%2 timestamp=%3，"
            "候选frame=%4 timestamp=%5，本帧禁止授权联合运动")
                            .arg(rz, 0, 'f', 1)
                            .arg(frameId)
                            .arg(timestampMs)
                            .arg(m_pendingLargeRzFrameId)
                            .arg(m_pendingLargeRzTimestampMs));
    } else if (largeRz.action == LargeRzAction::AllowMotion) {
        emit logMessage(QStringLiteral(
            "[阶段一] Rz大角度已由两个严格递增真实帧确认：Rz=%1，"
            "允许纳入联合运动（已执行=%2/%3）")
                            .arg(rz, 0, 'f', 1)
                            .arg(m_stageOneLargeRzExecutionCount)
                            .arg(m_runtimeSettings.vision.maxLargeRzExecutions));
    } else if (largeRzExecutionLimitReached) {
        emit logMessage(QStringLiteral(
            "[阶段一] Rz大角度执行次数已达上限：测量残差=%1°，允许运动量=0°，"
            "继续按未收敛处理并禁止下探（已执行=%2/%3）")
                            .arg(rz, 0, 'f', 1)
                            .arg(m_stageOneLargeRzExecutionCount)
                            .arg(m_runtimeSettings.vision.maxLargeRzExecutions));
    }

    emit logMessage(
        QStringLiteral("[阶段一][视觉输入] frame=%1 "
                       "anchor=(X=%2mm,Y=%3mm) "
                       "raw=(X=%4mm,Y=%5mm,Z=%6mm,Rz=%7°) "
                       "normalizedRz=%8°")
            .arg(frameId)
            .arg(m_anchorPreviousTargetX, 0, 'f', 1)
            .arg(m_anchorPreviousTargetY, 0, 'f', 1)
            .arg(x, 0, 'f', 1)
            .arg(y, 0, 'f', 1)
            .arg(z, 0, 'f', 1)
            .arg(rz, 0, 'f', 1)
            .arg(measuredRz, 0, 'f', 1));

    m_grabOffset = { x, y, z, 0.0, 0.0, measuredRz };
    emit logMessage(QStringLiteral("[阶段一] 视觉偏移(工具系) X=%1 Y=%2 Z=%3 Rz=%4（首次MoveJ=%5，已完成精修=%6/%7）")
                        .arg(x, 0, 'f', 1).arg(y, 0, 'f', 1)
                        .arg(z, 0, 'f', 1).arg(measuredRz, 0, 'f', 1)
                        .arg(m_initialVisionMoveCompleted
                                 ? QStringLiteral("已完成")
                                 : QStringLiteral("待执行"))
                        .arg(m_completedFineCorrectionCount)
                        .arg(m_runtimeSettings.vision.maxFineCorrectionCount));

    // Rz 大角度的第一帧只允许登记候选值，不能让 X/Y 先行运动后消耗唯一一次首次 MoveJ。
    // 等下一真实帧通过原有方向和幅值确认后，再把完整 X/Y/Rz 作为一条联合命令下发。
    if (suppressLargeRzRotation && !validatingVisionAlignment) {
        const quint64 seq = nextCallbackSeq();
        QTimer::singleShot(m_runtimeSettings.vision.settleMs, this, [this, seq] {
            if (seq == m_commandSeq
                && m_stage == Stage::StageOne
                && m_stageStep == StageStep::WaitForVision) {
                proceedStage();
            }
        });
        return;
    }

    const VisionAlignment::Sample measuredSample{x, y, z, measuredRz, true};
    const VisionAlignment::Sample motionSample{x, y, z, motionRz, true};

    if (!validatingVisionAlignment) {
        // 首次可信目标无论是否已经落在容差内，都必须走同一条绝对预抓取 MoveJ。
        // 这保证首次运动固定且仅固定一次，并且不会从初始帧直接绕过到 Z 下探。
        if (!m_initialVisionMoveCompleted) {
            if (!queueInitialVisionPregrasp(motionSample))
                return;

            m_stageStep = StageStep::MoveToPregrasp;
            emit logMessage(QStringLiteral("[阶段一] 首次可信目标已排队为单条联合 MoveJ，等待控制器确认到位"));
            return;
        }

        // WaitForVision 在首次 MoveJ 完成后不再承担闭环判定；若外部状态迁移错误地回到这里，
        // 直接失败比复用旧逐轴路径更安全，也防止绕过每次运动后的统一4～8秒窗口。
        emitOperationError(formatVisionAlignmentFailure(
            measuredSample,
            QStringLiteral("首次联合MoveJ已完成，但视觉回调不在联合验证状态")));
        return;
    }

    if (validatingVisionAlignment) {
        // frame_id 与 timestamp 均已通过严格递增检查，本帧才有资格同时更新
        // 平面对准偏差和最近三帧 Z 窗口；HTTP 重复缓存响应不会增加计数。
        ++m_stableZUniqueFrames;
        m_stableZSamples.append(z);
        if (m_stableZSamples.size() > kStableZWindowFrames)
            m_stableZSamples.removeFirst();

        QStringList sampleTexts;
        for (double zSample : m_stableZSamples)
            sampleTexts.append(QString::number(zSample, 'f', 1));

        const VisionAlignment::StableDepthEvaluation stableDepth =
            VisionAlignment::evaluateStableDepth(
                m_stableZSamples,
                kStableZWindowFrames,
                kStableZMaxRangeMm);
        const bool zStable = stableDepth.stable;
        const qint64 elapsedMs = m_stableZElapsedTimer.elapsed();

        VisionAlignment::WindowPolicy policy;
        policy.xyToleranceMm = m_runtimeSettings.vision.xyToleranceMm;
        policy.rzToleranceDeg = m_runtimeSettings.vision.rzToleranceDeg;
        policy.maxFineCorrectionCount = m_runtimeSettings.vision.maxFineCorrectionCount;
        policy.minElapsedMs = kStableZMinElapsedMs;
        policy.maxElapsedMs = kStableZMaxElapsedMs;

        VisionAlignment::WindowInput input;
        input.latest = measuredSample;
        input.zStable = zStable;
        input.elapsedMs = elapsedMs;
        input.completedFineCorrectionCount = m_completedFineCorrectionCount;
        VisionAlignment::WindowDecision decision =
            VisionAlignment::decideWindow(input, policy);

        // Rz 大角度候选的首帧只建立确认状态，不能因 effectiveRz 被临时抑制为 0
        // 而误判为已对准并下探。8 秒 Stop 仍保持最高安全约束，不会被该保护延长。
        if ((suppressLargeRzRotation || largeRzExecutionLimitReached)
            && decision.action != VisionAlignment::WindowAction::Stop) {
            decision.action = VisionAlignment::WindowAction::ContinueObserving;
            decision.reason = largeRzExecutionLimitReached
                ? QStringLiteral("Rz大角度残差仍超阈值且旋转次数已达上限，禁止下探")
                : QStringLiteral("等待Rz大角度两个真实新帧确认");
        }

        QString actionText;
        switch (decision.action) {
        case VisionAlignment::WindowAction::ContinueObserving:
            actionText = QStringLiteral("继续观察");
            break;
        case VisionAlignment::WindowAction::Descend:
            actionText = QStringLiteral("Z下探");
            break;
        case VisionAlignment::WindowAction::FineCorrect:
            actionText = QStringLiteral("联合精修");
            break;
        case VisionAlignment::WindowAction::Stop:
            actionText = QStringLiteral("安全停止");
            break;
        }

        emit logMessage(
            QStringLiteral("[阶段一][统一窗口] elapsed=%1ms uniqueFrames=%2 "
                           "XY/Rz=(X=%3mm,Y=%4mm,Rz=%5°) "
                           "ZCoreRange=%6mm ZMedian=%7mm "
                           "action=%8 reason=%9 frame=%10 ZSamples=[%11]")
                .arg(elapsedMs)
                .arg(m_stableZUniqueFrames)
                .arg(measuredSample.xMm, 0, 'f', 1)
                .arg(measuredSample.yMm, 0, 'f', 1)
                .arg(measuredSample.rzDeg, 0, 'f', 1)
                .arg(stableDepth.coreRangeMm, 0, 'f', 1)
                .arg(stableDepth.filteredZMm, 0, 'f', 1)
                .arg(actionText)
                .arg(decision.reason)
                .arg(frameId)
                .arg(sampleTexts.join(QStringLiteral(", "))));

        switch (decision.action) {
        case VisionAlignment::WindowAction::ContinueObserving:
            requestNextStableZFrame();
            return;
        case VisionAlignment::WindowAction::Descend:
            // decideWindow() 已同时确认达到最短4秒、XY/Rz对准和五帧稳健Z稳定。
            // 下探使用五帧中值而不是最新单帧；工具Z方向和既有硬安全上限公式保持不变。
            m_grabOffset.z = stableDepth.filteredZMm;
            resetStableZValidation();
            m_stageStep = StageStep::DescendZ;
            proceedStage();
            return;
        case VisionAlignment::WindowAction::FineCorrect:
            // 当前窗口证据在排队前立即作废；命令排队失败会由统一错误出口停止，
            // 排队成功后必须等 onPollTick() 确认 MoveL 到位，才能开启新窗口。
            resetStableZValidation();
            queueVisionFineCorrection(decision.correction);
            return;
        case VisionAlignment::WindowAction::Stop:
            emitOperationError(
                formatVisionAlignmentFailure(measuredSample, decision.reason));
            return;
        }
    }

}

void HuayanScheduler::setGrabOffset(double x, double y, double z, double rz,
                                    double contextSelectedAnchorX, double contextSelectedAnchorY)
{
    if (m_stage != Stage::StageOne
        || (m_stageStep != StageStep::WaitForVision
            && m_stageStep != StageStep::ValidateVisionAlignment)) {
        return;
    }

    // 视觉端只会在锚点模式选中可信目标时带回 anchor；记录值用于下一帧跳变保护。
    m_anchorHasPreviousTarget = true;
    m_anchorPreviousTargetX = contextSelectedAnchorX;
    m_anchorPreviousTargetY = contextSelectedAnchorY;
    m_anchorMissingFrames = 0;
    setGrabOffset(x, y, z, rz);
}

void HuayanScheduler::onVisionNoObject()
{
    if (m_stage == Stage::StageOne
        && m_stageStep == StageStep::ValidateVisionAlignment) {
        stopVisionWaitTimeout();
        const VisionAlignment::Sample invalidSample{
            m_grabOffset.x,
            m_grabOffset.y,
            m_grabOffset.z,
            m_grabOffset.rz,
            false};
        emitOperationError(formatVisionAlignmentFailure(
            invalidSample,
            QStringLiteral("验证期间未检测到锁定目标，拒绝搜索下移")));
        return;
    }

    if (m_stage != Stage::StageOne || m_stageStep != StageStep::WaitForVision) {
        emit logMessage(QStringLiteral("[阶段一] 收到未检测到目标，但当前不在等待视觉阶段，忽略"));
        return;
    }

    stopVisionWaitTimeout();

    const double searchStepMm = m_runtimeSettings.search.descendStepMm;
    const double nextDescendMm = m_searchDescendedMm + searchStepMm;
    if (nextDescendMm > m_runtimeSettings.search.maxAccumulatedMm) {
        // 80mm 是保守默认值，防止算法一直识别不到时机械臂持续下移触碰料箱。
        emitOperationError(QStringLiteral("[阶段一] 连续未检测到目标，搜索下移累计 %1mm 已达保守上限 %2mm，任务失败")
                               .arg(m_searchDescendedMm, 0, 'f', 1)
                               .arg(m_runtimeSettings.search.maxAccumulatedMm, 0, 'f', 1));
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
                        .arg(searchStepMm, 0, 'f', 1)
                        .arg(m_searchDescendedMm, 0, 'f', 1)
                        .arg(m_runtimeSettings.search.maxAccumulatedMm, 0, 'f', 1));

    PendingCommand cmd;
    cmd.kind = PendingCommandKind::MoveRelTool;
    cmd.label = QStringLiteral("搜索下移");
    cmd.poseId = 2;
    cmd.direction = kZDescendInvert ? 0 : 1;
    cmd.distance = searchStepMm;
    if (!beginCommandWhenReady(cmd)) {
        if (m_stage == Stage::StageOne && m_stageStep == StageStep::SearchDescend) {
            m_stageStep = StageStep::WaitForVision;
            m_searchDescendCount--;
            m_searchDescendedMm -= searchStepMm;
        }
    }
}

void HuayanScheduler::onVisionTargetRejectedForPickup(VisionHttpClient::TargetSelectionReason reason,
                                                      const QString &msg)
{
    if (m_stage != Stage::StageOne
        || (m_stageStep != StageStep::WaitForVision
            && m_stageStep != StageStep::ValidateVisionAlignment)) {
        emit logMessage(QStringLiteral("[阶段一] 收到锚点可信拒绝，但当前不在等待视觉阶段，忽略：%1").arg(msg));
        return;
    }

    const bool validatingVisionAlignment =
        m_stageStep == StageStep::ValidateVisionAlignment;
    stopVisionWaitTimeout();
    if (validatingVisionAlignment) {
        const VisionAlignment::Sample invalidSample{
            m_grabOffset.x,
            m_grabOffset.y,
            m_grabOffset.z,
            m_grabOffset.rz,
            false};
        emitOperationError(formatVisionAlignmentFailure(
            invalidSample,
            QStringLiteral("锁定目标被可信规则拒绝：%1，拒绝搜索下移")
                .arg(msg)));
        return;
    }

    QString reasonText;
    using Reason = VisionHttpClient::TargetSelectionReason;
    switch (reason) {
    case Reason::AnchorDistanceTooFar:
        reasonText = QStringLiteral("最高目标超出拍照锚点矩形可信范围");
        break;
    case Reason::AnchorTargetJumpTooFar:
        reasonText = QStringLiteral("闭环目标相对上一帧跳变过大");
        break;
    case Reason::LockTargetMissing:
    case Reason::LockTargetLost: {
        const int nextMissingFrames = m_anchorMissingFrames + 1;
        m_anchorMissingFrames = nextMissingFrames;
        if (reason == Reason::LockTargetLost
            || nextMissingFrames >= VISION_LOCK_MAX_MISSING_FRAMES) {
            emitOperationError(QStringLiteral("[阶段一] 锁定目标连续丢失 %1/%2 帧：%3，锁定=(%4,%5)，拒绝切换旁边工位目标")
                                   .arg(nextMissingFrames)
                                   .arg(VISION_LOCK_MAX_MISSING_FRAMES)
                                   .arg(msg)
                                   .arg(m_anchorPreviousTargetX, 0, 'f', 1)
                                   .arg(m_anchorPreviousTargetY, 0, 'f', 1));
            return;
        }

        emit logMessage(QStringLiteral("[阶段一] 锁定目标暂时丢失 %1/%2 帧：%3，锁定=(%4,%5)，拒绝切换旁边工位目标，本帧不下发 MoveRelL")
                            .arg(nextMissingFrames)
                            .arg(VISION_LOCK_MAX_MISSING_FRAMES)
                            .arg(msg)
                            .arg(m_anchorPreviousTargetX, 0, 'f', 1)
                            .arg(m_anchorPreviousTargetY, 0, 'f', 1));
        const quint64 seq = nextCallbackSeq();
        QTimer::singleShot(m_runtimeSettings.vision.settleMs, this, [this, seq] {
            if (seq == m_commandSeq
                && m_stage == Stage::StageOne
                && m_stageStep == StageStep::WaitForVision)
                proceedStage();
        });
        return;
    }
    default:
        reasonText = QStringLiteral("锚点可信规则拒绝");
        break;
    }

    // 这不是“未检测到目标”，继续搜索下移会把旁边工位/跳变目标风险放大，因此直接阶段失败。
    emitOperationError(QStringLiteral("[阶段一] 视觉目标不可信：%1，%2，拒绝进入搜索下移")
                           .arg(reasonText, msg));
}

void HuayanScheduler::onVisionErrorForPickup(const QString &msg)
{
    if (m_stage != Stage::StageOne
        || (m_stageStep != StageStep::WaitForVision
            && m_stageStep != StageStep::ValidateVisionAlignment)) {
        emit logMessage(QStringLiteral("[阶段一] 收到视觉错误，但当前不在等待视觉阶段，忽略：%1").arg(msg));
        return;
    }

    stopVisionWaitTimeout();

    // 通信/解析错误不代表目标不在视野内，继续下移没有意义，应直接按视觉异常失败处理。
    if (m_stageStep == StageStep::ValidateVisionAlignment) {
        const VisionAlignment::Sample invalidSample{
            m_grabOffset.x,
            m_grabOffset.y,
            m_grabOffset.z,
            m_grabOffset.rz,
            false};
        emitOperationError(formatVisionAlignmentFailure(
            invalidSample,
            QStringLiteral("视觉推理失败：%1").arg(msg)));
        return;
    }
    emitOperationError(QStringLiteral("[阶段一] 视觉推理失败：%1").arg(msg));
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
        || (m_timeoutTimer->isActive()
            && m_stageStep != StageStep::WaitForVision
            && m_stageStep != StageStep::ValidateVisionAlignment);
}

bool HuayanScheduler::isActiveVisionMotion() const
{
    return m_activeCommandKind == PendingCommandKind::VisionPregraspMoveJ
        || m_activeCommandKind
            == PendingCommandKind::VisionFineCorrectionMoveL;
}

bool HuayanScheduler::actualVisionTargetReached() const
{
    if (!m_activeVisionTargetPoseValid)
        return false;

    Pose actual;
    const int ret = HRIF_ReadActTcpPos(
        m_boxID, m_rbtID,
        actual.x, actual.y, actual.z,
        actual.rx, actual.ry, actual.rz);
    if (ret != 0)
        return false;

    static constexpr double kPositionToleranceMm = 1.0;
    static constexpr double kAngleToleranceDeg = 1.0;
    const auto angleDistance = [](double a, double b) {
        return qAbs(std::remainder(a - b, 360.0));
    };
    return qAbs(actual.x - m_activeVisionTargetPose.x)
            <= kPositionToleranceMm
        && qAbs(actual.y - m_activeVisionTargetPose.y)
            <= kPositionToleranceMm
        && qAbs(actual.z - m_activeVisionTargetPose.z)
            <= kPositionToleranceMm
        && angleDistance(actual.rx, m_activeVisionTargetPose.rx)
            <= kAngleToleranceDeg
        && angleDistance(actual.ry, m_activeVisionTargetPose.ry)
            <= kAngleToleranceDeg
        && angleDistance(actual.rz, m_activeVisionTargetPose.rz)
            <= kAngleToleranceDeg;
}

// 同时读取 flags 和 FSM，是为了定位 Cleanup 20561 前控制器是否仍在脚本运行态。
HuayanScheduler::RobotStateSnapshot HuayanScheduler::readRobotStateSnapshot() const
{
    RobotStateSnapshot snapshot;
    int nEnableState = 0;
    int nErrorAxis = 0;
    int nBreaking = 0;
    int nBlendingDone = 0;
    snapshot.flagsRet = HRIF_ReadRobotFlags(m_boxID, m_rbtID,
                                            snapshot.movingState,
                                            nEnableState,
                                            snapshot.errorState,
                                            snapshot.errorCode,
                                            nErrorAxis,
                                            nBreaking,
                                            snapshot.pauseState,
                                            nBlendingDone);
    string fsmText;
    snapshot.fsmRet = HRIF_ReadCurFSM(m_boxID, m_rbtID, snapshot.nCurFSM, fsmText);
    snapshot.strCurFSM = snapshot.fsmRet == 0
        ? QString::fromStdString(fsmText)
        : QStringLiteral("未知");
    snapshot.valid = snapshot.flagsRet == 0 && snapshot.fsmRet == 0;
    return snapshot;
}

HuayanScheduler::MotionDiagnosticSnapshot
HuayanScheduler::readMotionDiagnosticSnapshot(bool readPoseAndJoints,
                                              bool readAxisErrors) const
{
    MotionDiagnosticSnapshot snapshot;
    snapshot.robotState = readRobotStateSnapshot();

    if (readPoseAndJoints) {
        snapshot.actualTcpRet = HRIF_ReadActTcpPos(
            m_boxID, m_rbtID,
            snapshot.actualTcp.x, snapshot.actualTcp.y, snapshot.actualTcp.z,
            snapshot.actualTcp.rx, snapshot.actualTcp.ry, snapshot.actualTcp.rz);
        snapshot.commandedTcpRet = HRIF_ReadCmdTcpPos(
            m_boxID, m_rbtID,
            snapshot.commandedTcp.x, snapshot.commandedTcp.y, snapshot.commandedTcp.z,
            snapshot.commandedTcp.rx, snapshot.commandedTcp.ry, snapshot.commandedTcp.rz);
        snapshot.jointsRet = HRIF_ReadActJointPos(
            m_boxID, m_rbtID,
            snapshot.joints[0], snapshot.joints[1], snapshot.joints[2],
            snapshot.joints[3], snapshot.joints[4], snapshot.joints[5]);
    }

    if (readAxisErrors) {
        snapshot.axisErrorsRet = HRIF_ReadAxisErrorCode(
            m_boxID, m_rbtID, snapshot.axisErrorCode,
            snapshot.axisErrors[0], snapshot.axisErrors[1], snapshot.axisErrors[2],
            snapshot.axisErrors[3], snapshot.axisErrors[4], snapshot.axisErrors[5]);
    }
    return snapshot;
}

bool HuayanScheduler::readActualPoseAndJoints(RobotPoseAndJoints *snapshot, QString *error) const
{
    if (!snapshot) {
        if (error)
            *error = QStringLiteral("实际位姿与关节快照输出参数为空");
        return false;
    }

    // 必须先完整读取实际 TCP，再读取同一调度时刻的 J1～J6。任何一步失败都立即
    // fail-closed，避免把局部有效的旧关节或位姿用于后续逆解参考和运动下发。
    const int tcpRet = HRIF_ReadActTcpPos(
        m_boxID, m_rbtID,
        snapshot->actualTcp.x, snapshot->actualTcp.y, snapshot->actualTcp.z,
        snapshot->actualTcp.rx, snapshot->actualTcp.ry, snapshot->actualTcp.rz);
    if (tcpRet != 0) {
        const QString detail = describeError(m_boxID, tcpRet);
        if (error) {
            *error = detail.isEmpty()
                ? QStringLiteral("读取实际 TCP 失败：ret=%1").arg(tcpRet)
                : QStringLiteral("读取实际 TCP 失败：ret=%1（%2）")
                      .arg(tcpRet)
                      .arg(detail);
        }
        return false;
    }

    const int jointsRet = HRIF_ReadActJointPos(
        m_boxID, m_rbtID,
        snapshot->actualJoints[0], snapshot->actualJoints[1],
        snapshot->actualJoints[2], snapshot->actualJoints[3],
        snapshot->actualJoints[4], snapshot->actualJoints[5]);
    if (jointsRet != 0) {
        const QString detail = describeError(m_boxID, jointsRet);
        if (error) {
            *error = detail.isEmpty()
                ? QStringLiteral("读取实际 J1～J6 失败：ret=%1").arg(jointsRet)
                : QStringLiteral("读取实际 J1～J6 失败：ret=%1（%2）")
                      .arg(jointsRet)
                      .arg(detail);
        }
        return false;
    }

    if (error)
        error->clear();
    return true;
}

bool HuayanScheduler::composeVisionPregraspPose(
    const Pose &capturePose,
    const VisionAlignment::ToolCorrection &correction,
    Pose *pregraspPose,
    QString *error) const
{
    if (!pregraspPose) {
        if (error)
            *error = QStringLiteral("绝对预抓取位姿输出参数为空");
        return false;
    }

    // 绝对目标必须由 SDK 做完整刚体位姿连乘：
    // T_base_pregrasp = T_base_capture × Trans(toolX, toolY, 0) × RotZ(toolRz)。
    // correction 已由 toToolCorrection() 完成 X 同向、Y/Rz 取反，禁止再做符号变换，
    // 也禁止把欧拉角或平移量直接与 capturePose 的六个分量相加。
    const int ret = HRIF_PoseTrans(
        m_boxID, m_rbtID,
        capturePose.x, capturePose.y, capturePose.z,
        capturePose.rx, capturePose.ry, capturePose.rz,
        correction.xMm, correction.yMm, 0,
        0, 0, correction.rzDeg,
        pregraspPose->x, pregraspPose->y, pregraspPose->z,
        pregraspPose->rx, pregraspPose->ry, pregraspPose->rz);
    if (ret != 0) {
        const QString detail = describeError(m_boxID, ret);
        if (error) {
            *error = detail.isEmpty()
                ? QStringLiteral("组合绝对预抓取位姿失败：ret=%1").arg(ret)
                : QStringLiteral("组合绝对预抓取位姿失败：ret=%1（%2）")
                      .arg(ret)
                      .arg(detail);
        }
        return false;
    }

    if (error)
        error->clear();
    return true;
}

bool HuayanScheduler::queueInitialVisionPregrasp(
    const VisionAlignment::Sample &sample)
{
    RobotPoseAndJoints snapshot;
    QString error;
    if (!readActualPoseAndJoints(&snapshot, &error)) {
        emitOperationError(QStringLiteral("[阶段一][初始联合MoveJ] %1").arg(error));
        return false;
    }

    // 工具修正的唯一方向换算入口：视觉 X 同向，视觉 Y/Rz 取反。
    // Z/Rx/Ry 不属于首次平面对准，composeVisionPregraspPose() 固定传入 0。
    const VisionAlignment::ToolCorrection correction =
        VisionAlignment::toToolCorrection(sample);
    Pose pregraspPose;
    if (!composeVisionPregraspPose(
            snapshot.actualTcp, correction, &pregraspPose, &error)) {
        emitOperationError(QStringLiteral("[阶段一][初始联合MoveJ] %1").arg(error));
        return false;
    }

    emit logMessage(
        QStringLiteral("[阶段一][位姿组合] "
                       "captureBase=(X=%1mm,Y=%2mm,Z=%3mm,Rx=%4°,Ry=%5°,Rz=%6°) "
                       "toolDelta=(X=%7mm,Y=%8mm,Z=0mm,Rx=0°,Ry=0°,Rz=%9°) "
                       "pregraspBase=(X=%10mm,Y=%11mm,Z=%12mm,Rx=%13°,Ry=%14°,Rz=%15°)")
            .arg(snapshot.actualTcp.x, 0, 'f', 3)
            .arg(snapshot.actualTcp.y, 0, 'f', 3)
            .arg(snapshot.actualTcp.z, 0, 'f', 3)
            .arg(snapshot.actualTcp.rx, 0, 'f', 3)
            .arg(snapshot.actualTcp.ry, 0, 'f', 3)
            .arg(snapshot.actualTcp.rz, 0, 'f', 3)
            .arg(correction.xMm, 0, 'f', 3)
            .arg(correction.yMm, 0, 'f', 3)
            .arg(correction.rzDeg, 0, 'f', 3)
            .arg(pregraspPose.x, 0, 'f', 3)
            .arg(pregraspPose.y, 0, 'f', 3)
            .arg(pregraspPose.z, 0, 'f', 3)
            .arg(pregraspPose.rx, 0, 'f', 3)
            .arg(pregraspPose.ry, 0, 'f', 3)
            .arg(pregraspPose.rz, 0, 'f', 3));

    PendingCommand cmd;
    cmd.kind = PendingCommandKind::VisionPregraspMoveJ;
    cmd.label = QStringLiteral("视觉初始绝对预抓取");
    cmd.targetPose = pregraspPose;
    // 这里保存的是读取绝对目标时的当前实际 J1～J6，只作为笛卡尔逆解参考，
    // dispatchVisionPregraspMoveJ() 仍以组合后的 Base 绝对位姿作为实际运动目标。
    cmd.referenceJoints = snapshot.actualJoints;

    // 必须先让统一门控接受命令，再更新待确认修正量。否则旧视觉命令仍在执行时，
    // 新请求会先覆盖旧修正，随后才被门控拒绝，旧命令到位后就可能累计错误的锚点。
    const bool queued = beginCommandWhenReady(cmd);
    if (!queued) {
        // 具体失败原因和停调度动作由 beginCommandWhenReady() 的统一错误出口负责。
        return false;
    }

    // 修正量在运动确认前仅作为“待完成”状态保存，绝不能提前写入
    // m_anchorAccumulatedToolX/Y；实际累计统一由 onPollTick() 的到位分支完成。
    m_pendingAlignmentCorrection = correction;
    m_pendingLargeRzExecution =
        qAbs(correction.rzDeg)
        >= m_runtimeSettings.vision.largeRzJumpThresholdDeg;
    return true;
}

bool HuayanScheduler::queueVisionFineCorrection(
    const VisionAlignment::ToolCorrection &correction)
{
    // X/Y 必须分别校验，不能使用合成距离掩盖某一轴单独越过运行时安全上限。
    if (qAbs(correction.xMm)
        > m_runtimeSettings.safety.maxSingleXyAdjustMm) {
        emitOperationError(
            QStringLiteral("[阶段一] 目标不可信：计划联合精修 X=%1mm 超过单次上限 %2mm，拒绝下发 MoveL")
                .arg(correction.xMm, 0, 'f', 1)
                .arg(m_runtimeSettings.safety.maxSingleXyAdjustMm, 0, 'f', 1));
        return false;
    }
    if (qAbs(correction.yMm)
        > m_runtimeSettings.safety.maxSingleXyAdjustMm) {
        emitOperationError(
            QStringLiteral("[阶段一] 目标不可信：计划联合精修 Y=%1mm 超过单次上限 %2mm，拒绝下发 MoveL")
                .arg(correction.yMm, 0, 'f', 1)
                .arg(m_runtimeSettings.safety.maxSingleXyAdjustMm, 0, 'f', 1));
        return false;
    }

    PendingCommand cmd;
    cmd.kind = PendingCommandKind::VisionFineCorrectionMoveL;
    cmd.label = QStringLiteral("视觉联合精修");
    cmd.targetPose.x = correction.xMm;
    cmd.targetPose.y = correction.yMm;
    cmd.targetPose.z = 0;
    cmd.targetPose.rx = 0;
    cmd.targetPose.ry = 0;
    cmd.targetPose.rz = correction.rzDeg;

    // 统一门控可能因为已有命令、控制器状态或 SDK 下发失败而返回 false。
    // 只有门控明确接受后，才能覆盖 pending 并进入精修状态，防止失败命令污染锚点。
    const bool queued = beginCommandWhenReady(cmd);
    if (!queued) {
        return false;
    }

    m_pendingAlignmentCorrection = correction;
    m_pendingLargeRzExecution =
        qAbs(cmd.targetPose.rz)
        >= m_runtimeSettings.vision.largeRzJumpThresholdDeg;
    m_stageStep = StageStep::FineCorrectAlignment;
    return true;
}

bool HuayanScheduler::readActualTcpPose(PalletPose *pose, QString *error) const
{
    if (!pose) {
        if (error)
            *error = QStringLiteral("输出参数为空");
        return false;
    }
    const int ret = HRIF_ReadActTcpPos(m_boxID, m_rbtID,
                                       pose->x, pose->y, pose->z,
                                       pose->rx, pose->ry, pose->rz);
    if (ret != 0) {
        const QString detail = describeError(m_boxID, ret);
        if (error) {
            *error = detail.isEmpty()
                ? QStringLiteral("ret=%1").arg(ret)
                : QStringLiteral("ret=%1（%2）").arg(ret).arg(detail);
        }
        return false;
    }
    return true;
}

QString HuayanScheduler::formatRobotStateSnapshot(const RobotStateSnapshot &snapshot) const
{
    return QStringLiteral("运动=%1 暂停=%2 错误=%3 主错误码=%4 状态机=%5/%6 标志读取返回码=%7 状态机读取返回码=%8 有效=%9")
        .arg(snapshot.movingState)
        .arg(snapshot.pauseState)
        .arg(snapshot.errorState)
        .arg(snapshot.errorCode)
        .arg(snapshot.nCurFSM)
        .arg(snapshot.strCurFSM)
        .arg(snapshot.flagsRet)
        .arg(snapshot.fsmRet)
        .arg(snapshot.valid ? QStringLiteral("是") : QStringLiteral("否"));
}

void HuayanScheduler::emitMoveRelFailureDiagnostics(
    const PendingCommand &cmd,
    const MotionDiagnosticSnapshot &before,
    const MotionDiagnosticSnapshot &after,
    int sdkReturnCode)
{
    static const QString kAxisNames[] = {
        QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z"),
        QStringLiteral("Rx"), QStringLiteral("Ry"), QStringLiteral("Rz")
    };
    const QString axis = cmd.poseId >= 0 && cmd.poseId < 6
        ? kAxisNames[cmd.poseId] : QString::number(cmd.poseId);
    const QString unit = cmd.poseId >= 0 && cmd.poseId < 3
        ? QStringLiteral("mm") : QStringLiteral("deg");
    const QString frame = cmd.kind == PendingCommandKind::MoveRelTool
        ? QStringLiteral("TCP/工具系") : QStringLiteral("UCS/基坐标系");
    const QString detail = describeError(m_boxID, sdkReturnCode);

    emit logMessage(QStringLiteral("[华沿][MoveRelL诊断][命令=%1] SDK拒绝：标签=%2 轴=%3 方向=%4 距离=%5%6 坐标系=%7 返回码=%8（%9）")
                        .arg(cmd.diagnosticCommandId)
                        .arg(cmd.label)
                        .arg(axis)
                        .arg(cmd.direction == 1 ? QStringLiteral("正向")
                                                : QStringLiteral("负向"))
                        .arg(cmd.distance, 0, 'f', 3)
                        .arg(unit)
                        .arg(frame)
                        .arg(sdkReturnCode)
                        .arg(detail.isEmpty() ? QStringLiteral("未知") : detail));
    emit logMessage(QStringLiteral("[华沿][MoveRelL诊断][命令=%1] 命令前TCP 实际=(%2,%3,%4,%5,%6,%7) 返回码=%8 指令=(%9,%10,%11,%12,%13,%14) 返回码=%15")
                        .arg(cmd.diagnosticCommandId)
                        .arg(before.actualTcp.x, 0, 'f', 3).arg(before.actualTcp.y, 0, 'f', 3)
                        .arg(before.actualTcp.z, 0, 'f', 3).arg(before.actualTcp.rx, 0, 'f', 3)
                        .arg(before.actualTcp.ry, 0, 'f', 3).arg(before.actualTcp.rz, 0, 'f', 3)
                        .arg(before.actualTcpRet)
                        .arg(before.commandedTcp.x, 0, 'f', 3).arg(before.commandedTcp.y, 0, 'f', 3)
                        .arg(before.commandedTcp.z, 0, 'f', 3).arg(before.commandedTcp.rx, 0, 'f', 3)
                        .arg(before.commandedTcp.ry, 0, 'f', 3).arg(before.commandedTcp.rz, 0, 'f', 3)
                        .arg(before.commandedTcpRet));
    emit logMessage(QStringLiteral("[华沿][MoveRelL诊断][命令=%1] 命令前关节=(%2,%3,%4,%5,%6,%7) 返回码=%8 状态={%9}")
                        .arg(cmd.diagnosticCommandId)
                        .arg(before.joints[0], 0, 'f', 3).arg(before.joints[1], 0, 'f', 3)
                        .arg(before.joints[2], 0, 'f', 3).arg(before.joints[3], 0, 'f', 3)
                        .arg(before.joints[4], 0, 'f', 3).arg(before.joints[5], 0, 'f', 3)
                        .arg(before.jointsRet)
                        .arg(formatRobotStateSnapshot(before.robotState)));
    emit logMessage(QStringLiteral("[华沿][MoveRelL诊断][命令=%1] 命令后状态={%2} 轴读取返回码=%3 轴总错误=%4 各轴错误=(%5,%6,%7,%8,%9,%10)")
                        .arg(cmd.diagnosticCommandId)
                        .arg(formatRobotStateSnapshot(after.robotState))
                        .arg(after.axisErrorsRet)
                        .arg(after.axisErrorCode)
                        .arg(after.axisErrors[0]).arg(after.axisErrors[1]).arg(after.axisErrors[2])
                        .arg(after.axisErrors[3]).arg(after.axisErrors[4]).arg(after.axisErrors[5]));
}

void HuayanScheduler::stopVisionWaitTimeout()
{
    if (m_stage == Stage::StageOne
        && (m_stageStep == StageStep::WaitForVision
            || m_stageStep == StageStep::ValidateVisionAlignment)
        && m_timeoutTimer->isActive()) {
        emit logMessage(QStringLiteral("[阶段一] 已收到视觉结果，停止视觉等待超时定时器"));
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
    m_pendingCommand.diagnosticCommandId = m_commandSeq;
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
    switch (cmd.kind) {
    case PendingCommandKind::RunFunc: {
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
        m_activeCommandKind = cmd.kind;
        m_activeCommandLabel = cmd.label;
        m_loggedRunFuncScriptRunning = false;
        startWaitForIdle(cmd.timeoutMs);
        return true;
    }

    case PendingCommandKind::MoveRelTool:
    case PendingCommandKind::MoveRelBase: {
        const int toolMotion = cmd.kind == PendingCommandKind::MoveRelTool ? 1 : 0;
        const MotionDiagnosticSnapshot before =
            readMotionDiagnosticSnapshot(true, false);
        const int nRet = HRIF_MoveRelL(m_boxID, m_rbtID, cmd.poseId, cmd.direction, cmd.distance, toolMotion);
        if (nRet != 0) {
            const MotionDiagnosticSnapshot after =
                readMotionDiagnosticSnapshot(false, true);
            emitMoveRelFailureDiagnostics(cmd, before, after, nRet);
            const QString detail = describeError(m_boxID, nRet);
            emitOperationError(detail.isEmpty()
                ? QStringLiteral("%1失败：%2").arg(cmd.label).arg(nRet)
                : QStringLiteral("%1失败：%2（%3）").arg(cmd.label).arg(nRet).arg(detail));
            return false;
        }
        m_activeCommandKind = cmd.kind;
        m_activeCommandLabel = cmd.label;
        m_loggedRunFuncScriptRunning = false;
        startWaitForIdle(cmd.timeoutMs);
        return true;
    }

    case PendingCommandKind::MoveJ: {
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
        m_activeCommandKind = cmd.kind;
        m_activeCommandLabel = cmd.label;
        m_loggedRunFuncScriptRunning = false;
        startWaitForIdle(cmd.timeoutMs);
        return true;
    }

    case PendingCommandKind::VisionPregraspMoveJ:
        return dispatchVisionPregraspMoveJ(cmd);

    case PendingCommandKind::VisionFineCorrectionMoveL:
        return dispatchVisionFineCorrectionMoveL(cmd);

    case PendingCommandKind::None:
        break;
    }

    return false;
}

bool HuayanScheduler::dispatchVisionPregraspMoveJ(const PendingCommand &cmd)
{
    emit logMessage(
        QStringLiteral("[阶段一][初始联合MoveJ] command=%1 "
                       "targetBase=(X=%2mm,Y=%3mm,Z=%4mm,Rx=%5°,Ry=%6°,Rz=%7°) "
                       "referenceJoints=(J1=%8°,J2=%9°,J3=%10°,J4=%11°,J5=%12°,J6=%13°)")
            .arg(cmd.diagnosticCommandId)
            .arg(cmd.targetPose.x, 0, 'f', 3)
            .arg(cmd.targetPose.y, 0, 'f', 3)
            .arg(cmd.targetPose.z, 0, 'f', 3)
            .arg(cmd.targetPose.rx, 0, 'f', 3)
            .arg(cmd.targetPose.ry, 0, 'f', 3)
            .arg(cmd.targetPose.rz, 0, 'f', 3)
            .arg(cmd.referenceJoints[0], 0, 'f', 3)
            .arg(cmd.referenceJoints[1], 0, 'f', 3)
            .arg(cmd.referenceJoints[2], 0, 'f', 3)
            .arg(cmd.referenceJoints[3], 0, 'f', 3)
            .arg(cmd.referenceJoints[4], 0, 'f', 3)
            .arg(cmd.referenceJoints[5], 0, 'f', 3));

    const int nRet = HRIF_WayPoint(
        // 控制盒编号和机器人编号沿用当前调度器连接配置。
        m_boxID, m_rbtID,
        // nMoveType=0：执行 MoveJ，而不是直线 MoveL。
        0,
        // dX～dRz：Base/UCS 下的绝对笛卡尔目标位姿。
        cmd.targetPose.x, cmd.targetPose.y, cmd.targetPose.z,
        cmd.targetPose.rx, cmd.targetPose.ry, cmd.targetPose.rz,
        // dJ1～dJ6：nIsUseJoint=0 时不是关节目标，而是笛卡尔逆解的当前实际关节参考。
        cmd.referenceJoints[0], cmd.referenceJoints[1],
        cmd.referenceJoints[2], cmd.referenceJoints[3],
        cmd.referenceJoints[4], cmd.referenceJoints[5],
        // TCP/UCS 名称沿用当前调度器实际配置，避免坐标系与视觉标定不一致。
        kTcpName.toStdString(), cmd.ucsName.toStdString(),
        // 速度、加速度和圆滑半径使用本轮运行时设置快照，不使用编译期常量。
        m_runtimeSettings.motion.velocity,
        m_runtimeSettings.motion.acceleration,
        m_runtimeSettings.motion.radius,
        // nIsUseJoint=0 保持笛卡尔目标有效；不启用寻位，IO 位和状态均为 0。
        0, 0, 0, 0,
        // 命令编号由上游载荷提供，用于控制器和本地诊断关联。
        cmd.cmdId.toStdString());
    if (nRet != 0) {
        const QString detail = describeError(m_boxID, nRet);
        emitOperationError(
            detail.isEmpty()
                ? QStringLiteral("%1失败：%2，命令=%3")
                      .arg(cmd.label)
                      .arg(nRet)
                      .arg(cmd.diagnosticCommandId)
                : QStringLiteral("%1失败：%2（%3），命令=%4")
                      .arg(cmd.label)
                      .arg(nRet)
                      .arg(detail)
                      .arg(cmd.diagnosticCommandId));
        return false;
    }

    m_activeCommandKind = cmd.kind;
    m_activeCommandLabel = cmd.label;
    m_activeVisionTargetPose = cmd.targetPose;
    m_activeVisionTargetPoseValid = true;
    m_loggedRunFuncScriptRunning = false;
    startWaitForIdle(cmd.timeoutMs);
    return true;
}

bool HuayanScheduler::dispatchVisionFineCorrectionMoveL(const PendingCommand &cmd)
{
    // 相对 Tool 位姿没有可直接比较的绝对终点；下发前用当前实际 TCP 和同一刚体
    // 变换计算 Base 绝对目标，供极短/零距离动作以真实 TCP 证据完成，不能靠时间盲判。
    Pose actualPose;
    const int actualRet = HRIF_ReadActTcpPos(
        m_boxID, m_rbtID,
        actualPose.x, actualPose.y, actualPose.z,
        actualPose.rx, actualPose.ry, actualPose.rz);
    if (actualRet != 0) {
        emitOperationError(QStringLiteral(
            "[阶段一][联合精修MoveL] 读取命令前实际TCP失败：ret=%1")
                               .arg(actualRet));
        return false;
    }
    const VisionAlignment::ToolCorrection correction{
        cmd.targetPose.x, cmd.targetPose.y, cmd.targetPose.rz};
    Pose expectedTargetPose;
    QString composeError;
    if (!composeVisionPregraspPose(
            actualPose, correction, &expectedTargetPose, &composeError)) {
        emitOperationError(QStringLiteral(
            "[阶段一][联合精修MoveL] 计算实际TCP目标失败：%1")
                               .arg(composeError));
        return false;
    }

    emit logMessage(
        QStringLiteral("[阶段一][联合精修MoveL] Tool增量=(%1,%2,0,0,0,%3)，%4，命令=%5")
            .arg(cmd.targetPose.x, 0, 'f', 3)
            .arg(cmd.targetPose.y, 0, 'f', 3)
            .arg(cmd.targetPose.rz, 0, 'f', 3)
            .arg(cmd.label)
            .arg(cmd.diagnosticCommandId));

    const int nRet = HRIF_WayPointRel(
        // 控制盒编号和机器人编号沿用当前调度器连接配置。
        m_boxID, m_rbtID,
        // nType=1：六个目标分量按同一条线性轨迹执行 MoveL。
        1,
        // nPointList=0：不读取控制器点位表，后续 dPos 和 dPos_J 参数必须全部传 0。
        0,
        // dPos_X～dPos_Rz：nPointList=0 时按 SDK 3.10.3 要求全部为 0。
        0, 0, 0, 0, 0, 0,
        // dPos_J1～dPos_J6：nPointList=0 时同样全部为 0。
        0, 0, 0, 0, 0, 0,
        // nrelMoveType=2：完整 SDK 接口文档 3.10.3 定义的 Tool 相对运动模式；
        // 不得改成普通叠加模式 1，否则视觉输出会在错误坐标系中执行。
        2,
        // nAxisMask_1～6 对应 X/Y/Z/Rx/Ry/Rz：只启用 X、Y、Rz。
        1, 1, 0, 0, 0, 1,
        // dTarget_1～6 对应 X/Y/Z/Rx/Ry/Rz：三轴增量在同一条线性轨迹命令中完成；
        // 不得拆成三个 HRIF_MoveRelL，否则轨迹和到位语义都会改变。
        cmd.targetPose.x,
        cmd.targetPose.y,
        0,
        0,
        0,
        cmd.targetPose.rz,
        // TCP/UCS 名称沿用当前调度器实际配置，确保 Tool 模式与视觉标定一致。
        kTcpName.toStdString(), cmd.ucsName.toStdString(),
        // 速度、加速度和圆滑半径使用本轮运行时设置快照。
        m_runtimeSettings.motion.velocity,
        m_runtimeSettings.motion.acceleration,
        m_runtimeSettings.motion.radius,
        // nIsUseJoint=0；不启用寻位，IO 位和状态均为 0。
        0, 0, 0, 0,
        // 命令编号由上游载荷提供，用于控制器和本地诊断关联。
        cmd.cmdId.toStdString());
    if (nRet != 0) {
        const QString detail = describeError(m_boxID, nRet);
        emitOperationError(
            detail.isEmpty()
                ? QStringLiteral("%1失败：%2，命令=%3")
                      .arg(cmd.label)
                      .arg(nRet)
                      .arg(cmd.diagnosticCommandId)
                : QStringLiteral("%1失败：%2（%3），命令=%4")
                      .arg(cmd.label)
                      .arg(nRet)
                      .arg(detail)
                      .arg(cmd.diagnosticCommandId));
        return false;
    }

    m_activeCommandKind = cmd.kind;
    m_activeCommandLabel = cmd.label;
    m_activeVisionTargetPose = expectedTargetPose;
    m_activeVisionTargetPoseValid = true;
    m_loggedRunFuncScriptRunning = false;
    startWaitForIdle(cmd.timeoutMs);
    return true;
}

void HuayanScheduler::resetArm()
{
    if (!ensureConnected())
        return;

    emit logMessage(QStringLiteral("[复位] 调用复位函数 Func_fuwei"));
    executeRunFunc(QStringLiteral("Func_fuwei"), 30000);
}
