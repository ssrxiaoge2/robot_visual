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

// 华研机器人默认 TCP/UCS 名称
static const QString kTcpName = QStringLiteral("TCP");
static const QString kFlipFuncName = QStringLiteral("FlipUnload");

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

    // 默认位姿（与原硬编码值对应），可被外部覆写
    m_pickupPose.x = 1000.0; m_pickupPose.y = 2000.0; m_pickupPose.z = 1500.0;
    m_pickupPose.rx = 0.0; m_pickupPose.ry = 0.0; m_pickupPose.rz = 0.0;

    m_pickupLiftPose = m_pickupPose;
    m_pickupLiftPose.z = 2500.0;

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

    m_stage = Stage::StageOne;
    m_stageStep = StageStep::MoveToSurvey;

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
        emit stageError(QStringLiteral("机器人报错，错误码：%1").arg(nErrorCode));
        stop();
        return;
    }
    m_pollCount++;
    // 至少等待 2 次轮询（600ms）确保机器人已开始运动再判断是否停止
    if (m_pollCount >= 2 && nMovingState == 0) {
        m_pollTimer->stop();
        m_timeoutTimer->stop();
        advanceStep();
        proceedStage();
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
            emit logMessage(QStringLiteral("[阶段一] 移动到拍照位"));
            executeMoveJ(m_surveyPose.x, m_surveyPose.y, m_surveyPose.z,
                         m_surveyPose.rx, m_surveyPose.ry, m_surveyPose.rz);
            break;
        case StageStep::WaitForVision:
            emit logMessage(QStringLiteral("[阶段一] 已到拍照位，等待视觉推理结果"));
            emit surveyReady();
            m_timeoutTimer->start(10000);
            break;
        case StageStep::MoveToGrab:
            emit logMessage(QStringLiteral("[阶段一] 视觉就绪，移动到抓取位（工具坐标系）"));
            executeMoveJ(m_grabOffset.x, m_grabOffset.y, m_grabOffset.z,
                         m_grabOffset.rx, m_grabOffset.ry, m_grabOffset.rz,
                         QStringLiteral("0"), QStringLiteral("TCP"));
            break;
        case StageStep::CloseGripper:
            emit logMessage(QStringLiteral("[阶段一] 夹爪闭合，夹取物料"));
            setGripper(false);
            break;
        case StageStep::LiftLoad:
            emit logMessage(QStringLiteral("[阶段一] 抬升物料"));
            executeMoveJ(m_pickupLiftPose.x, m_pickupLiftPose.y, m_pickupLiftPose.z,
                         m_pickupLiftPose.rx, m_pickupLiftPose.ry, m_pickupLiftPose.rz);
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
        emit stageError(QStringLiteral("移动指令失败：%1").arg(nRet));
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
    m_stageStep = StageStep::MoveToGrab;
    proceedStage();
}

void HuayanScheduler::setSurveyPose(const HuayanScheduler::Pose &p)
{
    m_surveyPose = p;
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
