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
 * 当前任务只允许五组功能码 0x04 的状态读取。状态枚举提前覆盖完整充电流程，
 * 供后续任务扩展，但本类此阶段不会排队或发送任何功能码 0x05、0x06 的写命令。
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

    void enqueueRead(quint16 address, quint16 count,
                     std::function<void(const QByteArray &response)> onSuccess);
    void beginNextRequest();
    void scheduleCurrentRequest();
    void sendCurrentRequest();
    void handleConnected();
    void handleReadyRead();
    void handleSocketError(QAbstractSocket::SocketError socketError);
    void handleResponseTimeout();
    void handleActionTimer();
    void setState(State state, const QString &text);
    void failQuery(const QString &reason);
    void finishQuery();
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
    bool m_queryInProgress = false;
};

Q_DECLARE_METATYPE(ChargePileController::State)
