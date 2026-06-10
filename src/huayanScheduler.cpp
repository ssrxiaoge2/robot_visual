/**
 * @file huayanScheduler.cpp
 * @brief HuayanScheduler 华研机器人调度实现
 */

#include "huayanScheduler.h"
#include "HR_Pro.h"

#include <QTimer>
#include <vector>

namespace {
// 机器人运动和位姿常量，参考原有取料 / 卸料场景坐标
// 默认位姿仍保留示例值，真实场景下可通过 set*Pose 接口注入

// 运动参数，使用简单速度/加速度示例值
static constexpr double kMoveVelocity = 50.0;
static constexpr double kMoveAcceleration = 100.0;
static constexpr double kMoveRadius = 0.0;

// 闭环视觉矫正参数
static constexpr double kGrabTolerance     = 2.0;    // XY 偏移收敛阈值(mm)
static constexpr int    kMaxGrabIterations = 6;      // 最大矫正迭代次数（防死循环）
static constexpr int    kVisionSettleMs    = 1000;   // 移动后等视觉出新帧(ms)

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
    m_pollTimer->setInterval(300);
    connect(m_pollTimer, &QTimer::timeout, this, &HuayanScheduler::onPollTick);

    m_timeoutTimer = new QTimer(this);
    m_timeoutTimer->setSingleShot(true);
    connect(m_timeoutTimer, &QTimer::timeout, this, &HuayanScheduler::onStepTimeout);

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
        emit stageError(QStringLiteral("华研机器人连接参数未配置"));
        return false;
    }

    // 连接华研 CPS，默认控制盒 boxID / 机器人 ID
    int nRet = HRIF_Connect(m_boxID, m_ip.toStdString().c_str(), m_port);
    if (nRet != 0) {
        emit stageError(QStringLiteral("连接华研机器人失败：%1").arg(nRet));
        return false;
    }

    int nSimulateRobot = 0;
    nRet = HRIF_IsSimulateRobot(m_boxID, nSimulateRobot);
    if (nRet == 0) {
        const double overrideValue = (nSimulateRobot == 1) ? 1.0 : 0.15;
        HRIF_SetOverride(m_boxID, m_rbtID, overrideValue);
    }

    nRet = HRIF_XToStandby(m_boxID, m_rbtID);
    if (nRet != 0) {
        emit stageError(QStringLiteral("华研机器人进入 Standby 失败：%1").arg(nRet));
        return false;
    }

    m_connected = true;

    // 应用当前脚本，确保后续 RunFunc 能在该脚本里找到示教好的函数
    nRet = HRIF_SwitchScript(m_boxID, m_rbtID, m_scriptName.toStdString());
    if (nRet != 0)
        emit logMessage(QStringLiteral("[警告] 切换脚本 %1 失败：%2，RunFunc 可能无法调用函数")
                            .arg(m_scriptName).arg(nRet));

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

void HuayanScheduler::startStageOne()
{
    if (!ensureConnected())
        return;

    // 上一轮 stop() 的 GrpStop 会让机器人进入 ProgramStopped 态（20018），
    // 启动前先复位恢复可运动状态
    HRIF_GrpReset(m_boxID, m_rbtID);

    m_stage = Stage::StageOne;
    m_stageStep = StageStep::MoveToSurvey;
    m_grabIterations = 0;

    emit stageStarted(stageName(m_stage));
    proceedStage();
}

void HuayanScheduler::startStageTwo()
{
    if (!ensureConnected())
        return;

    m_stage = Stage::StageTwo;
    m_stageStep = StageStep::MoveToUnload;

    emit stageStarted(stageName(m_stage));
    proceedStage();
}

void HuayanScheduler::startStageThree()
{
    if (!ensureConnected())
        return;

    if (m_stackingFuncName.isEmpty()) {
        emit stageError(QStringLiteral("启动阶段三失败：未配置码垛脚本函数"));
        return;
    }

    m_stage = Stage::StageThree;
    m_stageStep = StageStep::ExecuteStackingFunction;

    emit stageStarted(stageName(m_stage));
    proceedStage();
}

void HuayanScheduler::stop()
{
    m_pollTimer->stop();
    m_timeoutTimer->stop();
    // 软件状态机之外，必须向控制器发停止指令让机器人真正减速停下
    if (m_connected) {
        int nRet = HRIF_GrpStop(m_boxID, m_rbtID);
        if (nRet != 0)
            emit logMessage(QStringLiteral("[警告] 停止指令未成功下发：%1").arg(nRet));
    }
    m_stage = Stage::None;
    m_stageStep = StageStep::None;
    emit logMessage(QStringLiteral("调度已停止"));
}

void HuayanScheduler::startWaitForIdle(int timeoutMs)
{
    m_pollCount = 0;
    m_timeoutTimer->start(timeoutMs);
    m_pollTimer->start();
}

void HuayanScheduler::onPollTick()
{
    int nMovingState, nEnableState, nErrorState, nErrorCode, nErrorAxis, nBreaking, nPause, nBlendingDone;
    int nRet = HRIF_ReadRobotFlags(m_boxID, m_rbtID,
                                    nMovingState, nEnableState, nErrorState,
                                    nErrorCode, nErrorAxis, nBreaking,
                                    nPause, nBlendingDone);
    if (nRet != 0) {
        emit stageError(QStringLiteral("读取机器人状态失败，错误码：%1").arg(nRet));
        stop();
        return;
    }
    if (nErrorState != 0) {
        const QString detail = describeError(m_boxID, nErrorCode);
        emit stageError(detail.isEmpty()
            ? QStringLiteral("机器人报错，错误码：%1").arg(nErrorCode)
            : QStringLiteral("机器人报错，错误码：%1（%2）").arg(nErrorCode).arg(detail));
        stop();
        return;
    }
    m_pollCount++;
    // 至少等待 2 次轮询（600ms）确保机器人已开始运动再判断是否停止
    if (m_pollCount >= 2 && nMovingState == 0) {
        m_pollTimer->stop();
        m_timeoutTimer->stop();
        // MoveToGrab 是多次 MoveRelL 串联，单次到位后继续下一个偏移分量
        if (m_stage == Stage::StageOne && m_stageStep == StageStep::MoveToGrab) {
            m_grabMoveIdx++;
            executeNextGrabMove();
        } else {
            advanceStep();
            proceedStage();
        }
    }
}

void HuayanScheduler::onStepTimeout()
{
    m_pollTimer->stop();
    emit stageError(QStringLiteral("步骤超时，机器人未在预期时间内完成动作"));
    stop();
}

void HuayanScheduler::proceedStage()
{
    // 根据当前阶段和当前步骤继续执行下一步动作
    switch (m_stage) {
    case Stage::StageOne:
    case Stage::StageTwo:
    case Stage::StageThree:
        executeCurrentStep();
        break;
    default:
        break;
    }
}

void HuayanScheduler::advanceStep()
{
    // 进入下一阶段步骤，当前阶段完成后继续工作流
    switch (m_stage) {
    case Stage::StageOne:
        switch (m_stageStep) {
        case StageStep::MoveToSurvey:  m_stageStep = StageStep::WaitForVision; break;
        case StageStep::MoveToGrab:    m_stageStep = StageStep::CloseGripper;  break;
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
    default:
        m_stageStep = StageStep::None;
        break;
    }
}

void HuayanScheduler::executeCurrentStep()
{
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
        case StageStep::MoveToGrab:
            executeNextGrabMove();
            break;
        case StageStep::CloseGripper:
            emit logMessage(QStringLiteral("[阶段一] 调用夹紧函数 %1").arg(m_gripFuncName));
            executeGripFunc();
            break;
        case StageStep::LiftLoad:
            emit logMessage(QStringLiteral("[阶段一] 抬升（调用拍照位函数 %1 回到安全高度）").arg(m_captureFuncName));
            executeRunFunc(m_captureFuncName);
            break;
        case StageStep::None:
            emit stageCompleted(stageName(m_stage));
            stop();
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
            emit stageCompleted(stageName(m_stage));
            stop();
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
            emit stageCompleted(stageName(m_stage));
            stop();
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }
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

    int nRet = HRIF_MoveJ(m_boxID, m_rbtID,
                          x, y, z, rx, ry, rz,
                          0, 0, 0, 0, 0, 0,
                          kTcpName.toStdString(), ucsName.toStdString(),
                          kMoveVelocity, kMoveAcceleration, kMoveRadius,
                          0, 0, 0, 0,
                          cmdId.toStdString());
    if (nRet != 0) {
        const QString detail = describeError(m_boxID, nRet);
        emit stageError(detail.isEmpty()
            ? QStringLiteral("移动指令失败：%1").arg(nRet)
            : QStringLiteral("移动指令失败：%1（%2）").arg(nRet).arg(detail));
        stop();
        return false;
    }

    startWaitForIdle();
    return true;
}

void HuayanScheduler::setGrabOffset(double x, double y, double z, double rz)
{
    if (m_stage != Stage::StageOne || m_stageStep != StageStep::WaitForVision)
        return;

    m_grabOffset = { x, y, z, 0.0, 0.0, rz };
    emit logMessage(QStringLiteral("[阶段一] 视觉偏移(工具系) X=%1 Y=%2 Z=%3 Rz=%4 (迭代 %5)")
                        .arg(x, 0, 'f', 1).arg(y, 0, 'f', 1)
                        .arg(z, 0, 'f', 1).arg(rz, 0, 'f', 1)
                        .arg(m_grabIterations));

    // 闭环收敛：XY 都小于阈值，或达到最大迭代次数 → 进入夹紧
    if ((qAbs(x) < kGrabTolerance && qAbs(y) < kGrabTolerance)
        || m_grabIterations >= kMaxGrabIterations) {
        emit logMessage(QStringLiteral("[阶段一] 视觉对准完成（X=%1 Y=%2），进入夹紧")
                            .arg(x, 0, 'f', 1).arg(y, 0, 'f', 1));
        m_stageStep = StageStep::CloseGripper;
        proceedStage();
        return;
    }

    // 未收敛：工具坐标系 XY 平面微调，移动完成后回 WaitForVision 重新拍照
    m_grabIterations++;
    m_grabMoves.clear();
    auto addMove = [this](int poseId, double v) {
        if (qAbs(v) < 0.5) return;   // 忽略 <0.5mm 的微小偏移
        m_grabMoves.append({ poseId, v >= 0 ? 1 : 0, qAbs(v) });
    };
    addMove(0, x);  // X
    addMove(1, y);  // Y

    m_grabMoveIdx = 0;
    m_stageStep = StageStep::MoveToGrab;
    proceedStage();
}

bool HuayanScheduler::executeNextGrabMove()
{
    if (m_grabMoveIdx >= m_grabMoves.size()) {
        // 本次 XY 微调完成，回到拍照状态，等视觉出新帧后重新检测（闭环）
        emit logMessage(QStringLiteral("[阶段一] 本次微调完成，等待视觉更新后重新检测"));
        m_stageStep = StageStep::WaitForVision;
        QTimer::singleShot(kVisionSettleMs, this, [this] {
            if (m_stage == Stage::StageOne && m_stageStep == StageStep::WaitForVision)
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

    // nToolMotion=1：在工具(TCP)坐标系下做相对运动
    int nRet = HRIF_MoveRelL(m_boxID, m_rbtID, mv.poseId, mv.direction, mv.distance, 1);
    if (nRet != 0) {
        const QString detail = describeError(m_boxID, nRet);
        emit stageError(detail.isEmpty()
            ? QStringLiteral("相对运动失败：%1").arg(nRet)
            : QStringLiteral("相对运动失败：%1（%2）").arg(nRet).arg(detail));
        stop();
        return false;
    }

    startWaitForIdle();
    return true;
}

void HuayanScheduler::setSurveyPose(const HuayanScheduler::Pose &p)
{
    m_surveyPose = p;
}

void HuayanScheduler::releaseGripper()
{
    if (!ensureConnected())
        return;

    // 阶段结束后机器人可能处于 ProgramStopped 态，先复位再调用
    HRIF_GrpReset(m_boxID, m_rbtID);

    std::vector<string> params;
    int nRet = HRIF_RunFunc(m_boxID, m_releaseFuncName.toStdString(), params);
    if (nRet != 0) {
        const QString detail = describeError(m_boxID, nRet);
        emit stageError(detail.isEmpty()
            ? QStringLiteral("松开夹爪失败：%1").arg(nRet)
            : QStringLiteral("松开夹爪失败：%1（%2）").arg(nRet).arg(detail));
        return;
    }
    emit logMessage(QStringLiteral("已调用松开夹爪 %1").arg(m_releaseFuncName));
}

bool HuayanScheduler::setGripper(bool open)
{
    // 使用华研 SDK 控制末端夹爪开闭：0 闭合, 1 打开
    if (!ensureConnected())
        return false;

    int nRet = HRIF_SetToolMotion(m_boxID, m_rbtID, open ? 1 : 0);
    if (nRet != 0) {
        emit stageError(QStringLiteral("夹爪控制失败：%1").arg(nRet));
        stop();
        return false;
    }

    // 夹爪是 IO 动作，nMovingState 不反映其状态，固定等待 1.5s 让气动/伺服完成动作
    QTimer::singleShot(1500, this, [this] {
        if (m_stage != Stage::None) {
            advanceStep();
            proceedStage();
        }
    });
    return true;
}

bool HuayanScheduler::executeRunFunc(const QString &funcName, int timeoutMs)
{
    if (!ensureConnected())
        return false;

    std::vector<string> params;
    int nRet = HRIF_RunFunc(m_boxID, funcName.toStdString(), params);
    if (nRet != 0) {
        const QString detail = describeError(m_boxID, nRet);
        emit stageError(detail.isEmpty()
            ? QStringLiteral("调用函数 %1 失败：%2").arg(funcName).arg(nRet)
            : QStringLiteral("调用函数 %1 失败：%2（%3）").arg(funcName).arg(nRet).arg(detail));
        stop();
        return false;
    }

    startWaitForIdle(timeoutMs);
    return true;
}

bool HuayanScheduler::executeGripFunc()
{
    if (!ensureConnected())
        return false;

    std::vector<string> params;
    int nRet = HRIF_RunFunc(m_boxID, m_gripFuncName.toStdString(), params);
    if (nRet != 0) {
        const QString detail = describeError(m_boxID, nRet);
        emit stageError(detail.isEmpty()
            ? QStringLiteral("调用夹紧函数失败：%1").arg(nRet)
            : QStringLiteral("调用夹紧函数失败：%1（%2）").arg(nRet).arg(detail));
        stop();
        return false;
    }

    // 夹爪是 IO/气动动作，nMovingState 不反映其状态，固定等待让动作完成
    QTimer::singleShot(2500, this, [this] {
        if (m_stage != Stage::None) {
            advanceStep();
            proceedStage();
        }
    });
    return true;
}

bool HuayanScheduler::executeFlipUnload()
{
    // 调用华研端脚本函数完成卸料翻转动作
    if (!ensureConnected())
        return false;

    std::vector<string> params;
    int nRet = HRIF_RunFunc(m_boxID, kFlipFuncName.toStdString(), params);
    if (nRet != 0) {
        emit stageError(QStringLiteral("卸料翻转脚本执行失败：%1").arg(nRet));
        stop();
        return false;
    }

    startWaitForIdle(60000);  // 翻转脚本最长等待 60s
    return true;
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

    std::vector<string> params;
    for (const QString &entry : m_stackingParams)
        params.push_back(entry.toStdString());

    int nRet = HRIF_RunFunc(m_boxID, m_stackingFuncName.toStdString(), params);
    if (nRet != 0) {
        emit stageError(QStringLiteral("码垛脚本执行失败：%1").arg(nRet));
        stop();
        return false;
    }

    startWaitForIdle(120000);  // 码垛脚本最长等待 120s
    return true;
}
