#pragma once

#include "chargesettings.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QMetaType>
#include <QObject>
#include <QQueue>
#include <QTcpSocket>
#include <QTimer>

#include <functional>
#include <optional>

/**
 * @brief 充电桩一次只读采样得到的完整状态快照。
 *
 * 原始状态字保留在快照中，便于上层根据后续现场定义读取任意位；常用的
 * 伸到位、缩到位、工作和继电器状态同时展开为具名字段，避免业务层重复位运算。
 */
struct ChargePileSnapshot
{
    double outputVoltageV = 0.0;
    double outputCurrentA = 0.0;
    quint16 inputWord = 0;
    quint16 outputWord = 0;
    quint16 eventWord = 0;
    quint16 faultWord = 0;
    bool extended = false;
    bool retracted = false;
    bool working = false;
    bool relayOn = false;
    QDateTime sampledAt;
};

Q_DECLARE_METATYPE(ChargePileSnapshot)

/**
 * @brief 充电桩通信与后续充电流程共用的唯一控制器。
 *
 * 所有只读查询、参数写入和线圈命令都经过同一个串行请求出口。完整充电会话严格
 * 复现现场 Python 脚本的预检、设参回读、启动监控、停止缩回复位五阶段顺序。
 */
class ChargePileController : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Idle,
        Connecting,
        Prechecking,
        WritingParameters,
        ReadingBackParameters,
        SendingStart,
        WaitingForStart,
        Monitoring,
        SendingStop,
        WaitingForNoOutput,
        Retracting,
        WaitingForRetracted,
        Resetting,
        SafeComplete,
        Fault,
        Unknown
    };
    Q_ENUM(State)

    /** @brief 标识会话由调试面板手动发起，还是由自动充电协调器发起。 */
    enum class SessionOrigin {
        Manual,
        Automatic
    };
    Q_ENUM(SessionOrigin)

    /**
     * @brief 统一安全收尾的原因。
     *
     * 后到的高优先级原因可以升级当前停止原因，但不会重新发送已经在途或已经确认
     * 的停止、缩回、复位命令。ApplicationShutdown 始终具有最高优先级。
     */
    enum class StopReason {
        Manual,
        AutomaticTaskReady,
        AutomaticTargetReached,
        AutomaticDisabled,
        LineStop,
        ApplicationShutdown,
        Fault,
        MonitorTimeout
    };
    Q_ENUM(StopReason)

    explicit ChargePileController(QObject *parent = nullptr);

    /**
     * @brief 应用已经校验过的通信与安全阈值设置。
     *
     * host、port 或 slaveId 变化时会取消旧查询、断开旧连接并使旧快照失效，
     * 防止后续帧落到错误设备；电压、电流和超时等非通信目标参数可热更新，
     * 保留同一 TCP 连接。输入未通过 validateChargeSettings() 时保持旧配置。
     */
    void applySettings(const ChargeSettings &settings);

    /**
     * @brief 顺序读取输出量、输入位、输出位、事件和故障五组状态。
     *
     * 当已有查询在途时立即发出失败的 queryFinished 信号，不创建并发连接、
     * 不插队报文，也绝不以写命令作为重试或恢复手段。
     */
    void queryStatus();

    /**
     * @brief 启动一个完整五阶段充电会话。
     * @param origin 会话来源，完成信号会原样携带该值供上层解除互斥。
     * @param error 同步拒绝时返回中文原因；异步协议故障通过完成信号报告。
     * @return 请求是否被控制器接受。已有查询/会话在途或配置无效时返回 false。
     */
    bool startCharge(SessionOrigin origin, QString *error);

    /** @brief 请求所有活动阶段汇入唯一安全收尾状态机。 */
    void requestSafeStop(StopReason reason);

    /**
     * @brief 请求程序关闭前完成安全收尾。
     *
     * 若已有较低优先级停止在进行，仅升级原因并等待同一在途命令完成；绝不创建
     * 第二套并发收尾。最终由 applicationShutdownFinished 明确报告是否已安全。
     */
    void requestApplicationShutdown();

    /** @brief 返回连接、等待响应或排队发送状态读取请求时的忙碌状态。 */
    bool isBusy() const;

    /**
     * @brief 判断是否仍需要执行安全收尾。
     *
     * 仅当控制器处于 SafeComplete 且最新快照明确显示无输出并缩到位时返回 false；
     * 未知、故障、活动状态或任何可能有输出/伸出风险的状态均保守地返回 true。
     */
    bool shutdownRequired() const;

    /** @brief 返回最后一次完整或部分采样的副本，调用者不得修改控制器内部状态。 */
    ChargePileSnapshot snapshot() const;

signals:
    void stateChanged(ChargePileController::State state, const QString &text);
    void snapshotChanged(const ChargePileSnapshot &snapshot);
    void queryFinished(bool ok, const QString &message);
    void chargeSessionFinished(bool safe, ChargePileController::SessionOrigin origin,
                               const QString &message);
    void applicationShutdownFinished(bool safe, const QString &message);
    void logMessage(const QString &message);

private:
    /**
     * @brief 在途或待发送的 RTU 请求描述。
     *
     * 每个请求显式携带预期站号、功能码、完整原始帧、读响应字节数、写标志、
     * 实际发送开始时间和成功回调，保证响应不能被错误地交给下一条请求处理。
     */
    struct Request {
        quint8 expectedSlaveId = 0;
        quint8 expectedFunction = 0;
        QByteArray frame;
        int expectedDataBytes = 0;
        bool isWriteCommand = false;
        QDateTime startedAt;
        std::function<void(const QByteArray &response)> onSuccess;
    };

    enum class FlowMode {
        None,
        Query,
        Charge
    };

    enum class DeferredPhase {
        None,
        WaitForStartPoll,
        MonitoringPoll,
        WaitForNoOutputPoll,
        WaitForRetractedPoll,
        RecoveryReconnect
    };

    /** 只读通信失败后从最近一个已唯一确认的写命令里程碑继续。 */
    enum class RecoveryAction {
        None,
        BeginSafeStop,
        ResumeWaitingForNoOutput,
        ReleaseRetractAndFail,
        RetryFinalSafetyQuery
    };

    void enqueueRead(quint8 function, quint16 address, quint16 count,
                     std::function<void(const QByteArray &response)> onSuccess);
    void enqueueInputRead(quint16 address, quint16 count,
                          std::function<void(const QByteArray &response)> onSuccess);
    void enqueueWriteRegister(quint16 address, quint16 value,
                              std::function<void()> onSuccess = {});
    void enqueueWriteCoil(quint16 address, bool on,
                          std::function<void()> onSuccess = {});
    void enqueueSnapshotReads(State state, const QString &text,
                              std::function<void()> onComplete);
    void beginNextRequest();
    void scheduleCurrentRequest();
    void sendCurrentRequest();
    void handleConnected();
    void handleReadyRead();
    void handleSocketError(QAbstractSocket::SocketError socketError);
    void handleResponseTimeout();
    void handleActionTimer();
    void handlePhaseTimer();
    void setState(State state, const QString &text);
    void failOperation(const QString &reason, bool commandResultUnknown = false);
    void finishQuery();
    void finishChargeSession(bool safe, const QString &message);
    void beginParameterWrites();
    void beginParameterReadback();
    void beginSecondPrecheck();
    void sendStartCommand();
    void pollWaitingForStart();
    void pollMonitoring();
    void beginSafeShutdown();
    void pollWaitingForNoOutput();
    void sendRetractCommand();
    void pollWaitingForRetracted();
    void sendResetAndFinalCheck();
    void beginFinalSafetyQuery();
    void handleReadFailure(const QString &reason);
    void beginCommunicationRecovery(RecoveryAction action, const QString &reason);
    void startRecoveryConnection();
    void resumeAfterRecovery();
    void releaseRetractAfterFailure(const QString &reason);
    void schedulePhase(DeferredPhase phase, int delayMs);
    bool snapshotHasBlockingFault(quint16 ignoredMask) const;
    bool snapshotHasNoOutput() const;
    bool validatePrestartSnapshot(QString *reason) const;
    void publishSnapshot();
    void clearTransportWork();
    static int stopReasonPriority(StopReason reason);
    static QString stopReasonText(StopReason reason);
    static QString originText(SessionOrigin origin);
    void invalidateCommunicationContext(const QString &reason);
    static bool communicationTargetChanged(const ChargeSettings &previous,
                                           const ChargeSettings &next);
    static quint16 responseWord(const QByteArray &frame, int offset);

    ChargeSettings m_settings = ChargeSettings::defaults();
    ChargePileSnapshot m_snapshot;
    State m_state = State::Idle;
    QTcpSocket m_socket;
    QTimer m_responseTimer;
    /**
     * 动作轮询定时器在任务三中承担连续 RTU 帧的最小间隔节流；后续有机械动作时
     * 可在同一串行出口上扩展为动作轮询，仍不会产生第二个并发发送器。
     */
    QTimer m_actionPollTimer;
    /** 机械动作与监控的轮询定时器；与 RTU 发送节流定时器完全分离。 */
    QTimer m_phaseTimer;
    QByteArray m_receiveBuffer;
    QQueue<Request> m_requestQueue;
    /**
     * 已从队列取出但仍受 50ms 节流限制、尚未调用 QTcpSocket::write() 的请求。
     * 此阶段任何收到字节都不是本请求的合法响应，必须保守地中断查询。
     */
    std::optional<Request> m_pendingRequest;
    /**
     * 仅在完整 RTU 帧成功交给 QTcpSocket 后设置的唯一在途请求。
     * 接收路径只能使用该对象校验并解析响应，避免迟到旧帧推进下一读取。
     */
    std::optional<Request> m_inFlightRequest;
    QElapsedTimer m_lastSendTimer;
    QElapsedTimer m_phaseElapsedTimer;
    bool m_queryInProgress = false;
    FlowMode m_flowMode = FlowMode::None;
    std::function<void()> m_onQueueDrained;
    DeferredPhase m_deferredPhase = DeferredPhase::None;

    /** 以下字段只属于当前充电会话，每次 startCharge() 接受请求时完整重置。 */
    quint64 m_sessionId = 0;
    SessionOrigin m_sessionOrigin = SessionOrigin::Manual;
    StopReason m_stopReason = StopReason::Manual;
    bool m_seenChargingOutput = false;
    bool m_safeStopRequested = false;
    bool m_safeShutdownStarted = false;
    bool m_applicationShutdownRequested = false;
    bool m_sessionFinishedEmitted = false;
    bool m_startCommandConfirmed = false;
    bool m_stopCommandConfirmed = false;
    bool m_retractOnConfirmed = false;
    bool m_retractOffConfirmed = false;
    bool m_resetCommandConfirmed = false;
    bool m_unknownGate = false;
    RecoveryAction m_recoveryAction = RecoveryAction::None;
    int m_recoveryAttempts = 0;
    QString m_recoveryReason;
    QString m_terminalFailureAfterRetractOff;
    quint16 m_expectedVoltageRaw = 0;
    quint16 m_expectedCurrentRaw = 0;
    std::optional<quint16> m_expectedCutoffRaw;
    std::optional<quint16> m_expectedMaxSecondsRaw;
};

Q_DECLARE_METATYPE(ChargePileController::State)
Q_DECLARE_METATYPE(ChargePileController::SessionOrigin)
Q_DECLARE_METATYPE(ChargePileController::StopReason)
