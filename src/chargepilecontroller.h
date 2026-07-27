#pragma once

#include "chargesettings.h"

#include <QDateTime>
#include <QByteArrayView>
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
    double outputVoltageV = 0.0; ///< 保持寄存器 0x0040，原始值按 0.1V 换算。
    double outputCurrentA = 0.0; ///< 保持寄存器 0x0041，原始值按 0.1A 换算。
    quint16 inputWord = 0;       ///< 输入信号状态字，保留全部厂家位供诊断。
    quint16 outputWord = 0;      ///< 输出信号状态字，包含工作和继电器反馈位。
    quint16 eventWord = 0;       ///< 事件状态字；事件本身不等同于阻断故障。
    quint16 faultWord = 0;       ///< E1 至 E16 故障位，具体豁免随流程阶段变化。
    bool extended = false;       ///< 推杆伸到位输入位。
    bool retracted = false;      ///< 推杆缩到位输入位，是安全终态必要条件。
    bool working = false;        ///< 充电桩工作反馈位。
    bool relayOn = false;        ///< 主继电器反馈位；缩枪前必须明确为 false。
    QDateTime sampledAt;         ///< 完整五组读取完成时间；无效表示尚无权威快照。
};

Q_DECLARE_METATYPE(ChargePileSnapshot)

/**
 * @brief 单条 Modbus 写请求中与安全恢复有关的稳定身份。
 *
 * 控制器在调用 QTcpSocket::write() 前就解析并保存该身份。这样即使套接字返回
 * 短写或 -1，也能明确记录“哪一条写命令的执行结果无法证明”，而不会因请求仍
 * 停留在 pending 阶段而丢失地址和值。
 */
struct ChargePileWriteIdentity
{
    quint8 function = 0; ///< 仅允许写单线圈 0x05 或写单寄存器 0x06。
    quint16 address = 0; ///< 命令目标地址，用于选择不重复写的恢复路径。
    quint16 value = 0;   ///< 已尝试写入的原始值或线圈编码。
};

/**
 * @brief 从 0x05/0x06 RTU 写请求中解析功能码、地址和值。
 * @return 非写请求或帧长度不足时返回 std::nullopt。
 */
std::optional<ChargePileWriteIdentity>
chargePileWriteIdentity(QByteArrayView frame);

/**
 * @brief 计算有限阶段下一轮查询的确定性等待时间。
 *
 * deadlineMs 为0表示无限监控，返回完整 pollIntervalMs；有限阶段返回轮询间隔与
 * 剩余截止时间的较小值。截止时间已经到达时返回1ms，让现有阶段回调立即进入
 * 超时分支，且不在0ms定时器中形成忙循环。
 */
int boundedChargePhasePollDelayMs(int pollIntervalMs, int deadlineMs,
                                  qint64 elapsedMs);

/** @brief 启动命令后对实时故障字应采取的动作，仅用于 WaitingForStart 阶段。 */
enum class ChargeStartupFaultAction {
    ContinueWaiting,
    RequestSafeStop
};

/// 现场故障字中 E11（充电连接/握手瞬态故障）的位掩码。
inline constexpr quint16 CHARGE_E11_MASK = 0x0400;
/// E11 首次出现后的只读观察窗口；窗口内绝不重发正常启动命令。
inline constexpr qint64 CHARGE_STARTUP_E11_GRACE_MS = 30000;

/**
 * @brief 确定启动等待阶段遇到故障字时是否允许继续只读观察。
 *
 * E2/E3/E4/E7/E8仍沿用现场厂家豁免。只有“尚未观察到任何充电输出且其余
 * 非豁免故障位均为0”的E11可以等待；达到30秒或已经出现输出后必须安全收尾。
 * e11ElapsedMs小于0表示本轮尚未观察到E11。
 */
ChargeStartupFaultAction decideChargeStartupFaultAction(
    quint16 faultWord, bool chargingOutputObserved, qint64 e11ElapsedMs);

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
    /** @brief 控制器对外发布的通信、业务阶段和安全终态。 */
    enum class State {
        Idle,                 ///< 无活动流程，尚不能单独证明充电桩安全。
        Connecting,           ///< 正在建立唯一 TCP 连接。
        Prechecking,          ///< 顺序读取五组状态，尚未发送写命令。
        WritingParameters,    ///< 正在写入本次启用的电气参数。
        ReadingBackParameters, ///< 正在回读并逐项确认写入值。
        SendingStart,         ///< 正在发送并确认正常启动线圈。
        WaitingForStart,      ///< 启动已确认，等待伸到位、工作或电流输出。
        Monitoring,           ///< 已观察到真实充电输出，持续轮询状态。
        SendingStop,          ///< 正在发送并确认停止线圈。
        WaitingForNoOutput,   ///< 等待工作/继电器关闭且电流降到安全值。
        Retracting,           ///< 正在发送并确认缩回线圈 ON。
        WaitingForRetracted,  ///< 等待缩到位，并在成功后释放缩回线圈。
        Resetting,            ///< 正在复位并执行最终完整安全查询。
        SafeComplete,         ///< 最新权威快照明确无输出且缩到位。
        Fault,                ///< 已知设备或协议故障，结果可能仍不安全。
        Unknown               ///< 写结果或现场状态无法证明，必须人工/保守恢复。
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
     * 保留同一 TCP 连接。输入未通过 validateChargeSettings()、存在活动查询/
     * 充电会话或安全恢复上下文尚未最终确认安全时返回 false，并保持旧配置。
     * @param error 拒绝时返回可直接用于现场日志的中文原因。
     * @return 控制器是否已经接受并应用整份设置。
     */
    bool applySettings(const ChargeSettings &settings, QString *error = nullptr);

    /**
     * @brief 无副作用检查当前控制器能否接受整份候选设置。
     *
     * DeviceManager 在写配置文件前调用此入口，避免已知会被控制器拒绝的候选
     * 先落盘。最终提交仍必须再次调用 applySettings()，不能把预检当成应用成功。
     */
    bool canApplySettings(const ChargeSettings &settings,
                          QString *error = nullptr) const;
    /// 返回控制器已经确认接受的整份设置副本，用于事务一致性检查和界面展示。
    ChargeSettings appliedSettings() const { return m_settings; }

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
     * @brief 在先前会话已以不安全终态结束后启动一轮独立保守恢复。
     *
     * 恢复先完整读取真实状态，再按停止、确认无输出、缩回、释放缩回线圈、
     * 复位和终检顺序推进；绝不发送 Start，也不重复结果不确定的同一写命令。
     * 成功或失败只通过 chargeSessionFinished 返回传入来源，不冒充应用关闭。
     */
    bool requestConservativeRecovery(SessionOrigin origin, StopReason reason,
                                     QString *error);

    /**
     * @brief 请求程序关闭前完成安全收尾。
     *
     * 若已有较低优先级停止在进行，仅升级原因并等待同一在途命令完成；绝不创建
     * 第二套并发收尾。最终由 applicationShutdownFinished 明确报告是否已安全。
     */
    void requestApplicationShutdown();

    /** @brief 返回连接、等待响应或排队发送状态读取请求时的忙碌状态。 */
    bool isBusy() const;
    /// 返回当前安全状态机阶段，供只读界面显示和集中可用性判断。
    State state() const { return m_state; }
    /// 只在充电或保守恢复流程持有控制器时返回真；单独只读查询不算充电会话。
    bool hasActiveChargeSession() const {
        return m_queryInProgress && m_flowMode == FlowMode::Charge;
    }

    /**
     * @brief 判断是否仍需要执行安全收尾。
     *
     * 仅当控制器处于 SafeComplete 且最新快照明确显示无输出并缩到位时返回 false；
     * 未知、故障、活动状态或任何可能有输出/伸出风险的状态均保守地返回 true。
     */
    bool shutdownRequired() const;

    /** @brief 返回最后一次完整或部分采样的副本，调用者不得修改控制器内部状态。 */
    ChargePileSnapshot snapshot() const;

    /**
     * @brief 返回尚未由相反命令或权威安全快照解决的写命令身份。
     *
     * 该只读诊断值可用于故障日志和测试确认；调用方不能据此自行发报文，
     * 所有恢复动作仍必须进入 requestConservativeRecovery()。
     */
    std::optional<ChargePileWriteIdentity> uncertainWriteIdentity() const;

signals:
    /// 阶段或安全终态发生变化；text 是可直接展示的中文原因。
    void stateChanged(ChargePileController::State state, const QString &text);
    /// 完成一轮五组读取后发布字段一致的快照。
    void snapshotChanged(const ChargePileSnapshot &snapshot);
    /// 只读查询最终结果；ok 不代表快照安全，调用方仍需检查状态和 shutdownRequired。
    void queryFinished(bool ok, const QString &message);
    /// 充电或保守恢复终态；safe=false 时禁止据此释放设备安全责任。
    void chargeSessionFinished(bool safe, ChargePileController::SessionOrigin origin,
                               const QString &message);
    /// 应用关闭请求的独立终态，避免普通会话结果误关闭窗口。
    void applicationShutdownFinished(bool safe, const QString &message);
    /// 结构化流程说明统一交给 DeviceManager 汇入主日志。
    void logMessage(const QString &message);

protected:
    /**
     * @brief 唯一套接字写出口，默认完整委托给当前 QTcpSocket。
     *
     * 设为虚函数仅用于测试稳定注入 QTcpSocket 难以制造的短写/-1 返回值；
     * 生产子系统不得另建发送器，队列、节流和在途身份仍由本类统一管理。
     */
    virtual qint64 writeFrame(const QByteArray &frame);

private:
    /**
     * @brief 在途或待发送的 RTU 请求描述。
     *
     * 每个请求显式携带预期站号、功能码、完整原始帧、读响应字节数、写标志、
     * 实际发送开始时间和成功回调，保证响应不能被错误地交给下一条请求处理。
     */
    struct Request {
        quint8 expectedSlaveId = 0;   ///< 响应必须回显的从站号。
        quint8 expectedFunction = 0;  ///< 响应必须匹配的功能码，异常码单独解析。
        QByteArray frame;             ///< 包含 CRC 的完整待发送 RTU 帧。
        int expectedDataBytes = 0;    ///< 读响应的数据区字节数；写请求固定为 0。
        bool isWriteCommand = false;  ///< 写失败时必须建立不确定写入恢复上下文。
        QDateTime startedAt;          ///< 真正调用 writeFrame() 的时刻，用于日志诊断。
        std::function<void(const QByteArray &response)> onSuccess; ///< 校验成功后的单次回调。
    };

    /** @brief 当前串行请求队列属于普通查询还是具有写入安全责任的会话。 */
    enum class FlowMode {
        None,   ///< 没有队列所有者。
        Query,  ///< 无副作用五组只读查询。
        Charge  ///< 正常充电或保守恢复流程。
    };

    /** @brief phaseTimer 到期时需要恢复执行的业务阶段。 */
    enum class DeferredPhase {
        None,                    ///< 当前没有业务阶段等待。
        WaitForStartPoll,        ///< 再次检查是否出现充电输出。
        MonitoringPoll,          ///< 充电期间下一轮状态监控。
        WaitForNoOutputPoll,     ///< 停止后再次确认无输出。
        WaitForRetractedPoll,    ///< 缩回后再次确认机械位置。
        RecoveryReconnect        ///< 通信恢复退避结束后重新建连。
    };

    /** 只读通信失败后从最近一个已唯一确认的写命令里程碑继续。 */
    enum class RecoveryAction {
        None,                     ///< 没有待恢复动作。
        BeginSafeStop,            ///< 最近唯一里程碑之前，保守地从停止阶段开始。
        ResumeWaitingForNoOutput, ///< Stop 已确认，只恢复无输出轮询。
        ReleaseRetractAndFail,    ///< 缩回 ON 已确认，必须优先释放线圈后再失败。
        RetryFinalSafetyQuery     ///< 复位或缩回 OFF 已确认，只重做最终只读查询。
    };

    /// 把读保持/输入寄存器请求加入唯一 FIFO，并绑定成功解析回调。
    void enqueueRead(quint8 function, quint16 address, quint16 count,
                     std::function<void(const QByteArray &response)> onSuccess);
    /// 输入寄存器读取的语义化包装，禁止调用方自行拼功能码。
    void enqueueInputRead(quint16 address, quint16 count,
                          std::function<void(const QByteArray &response)> onSuccess);
    /// 加入写单寄存器请求；写入结果未确认时会记录稳定命令身份。
    void enqueueWriteRegister(quint16 address, quint16 value,
                              std::function<void()> onSuccess = {});
    /// 加入写单线圈请求；onSuccess 只在回显地址和值均正确时执行。
    void enqueueWriteCoil(quint16 address, bool on,
                          std::function<void()> onSuccess = {});
    /// 固定顺序读取电压电流、输入、输出、事件和故障，完成后一次性发布快照。
    void enqueueSnapshotReads(State state, const QString &text,
                              std::function<void()> onComplete);
    /// 当前无 pending/in-flight 时从 FIFO 取出下一条请求。
    void beginNextRequest();
    /// 按相邻帧最小间隔决定立即发送或由 actionPollTimer 延后发送。
    void scheduleCurrentRequest();
    /// 建立 in-flight 身份后调用唯一写出口，并启动响应超时。
    void sendCurrentRequest();
    /// TCP 建连成功后停止连接超时并恢复当前 pending 请求。
    void handleConnected();
    /// 累积拆包/粘包数据，只把校验完整的期望响应交给当前请求。
    void handleReadyRead();
    /// 将套接字故障路由到只读恢复或写入不确定终态。
    void handleSocketError(QAbstractSocket::SocketError socketError);
    /// 区分建连超时和单请求响应超时，并保留写命令身份。
    void handleResponseTimeout();
    /// 相邻 RTU 帧节流结束后发送当前 pending 请求。
    void handleActionTimer();
    /// 根据 DeferredPhase 恢复相应业务轮询或通信重连。
    void handlePhaseTimer();
    /// 更新唯一阶段并同步发布界面文本与日志。
    void setState(State state, const QString &text);
    /// 清理当前传输并以 Fault/Unknown 结束；写结果不确定时必须进入 Unknown。
    void failOperation(const QString &reason, bool commandResultUnknown = false);
    /// 根据只读快照发布 SafeComplete、Idle 或 Unknown，并结束 Query 所有权。
    void finishQuery();
    /// 对每个 sessionId 只发布一次会话终态，并保留必要恢复上下文。
    void finishChargeSession(bool safe, const QString &message);
    /// 按启用项写入电压、电流、可选截止电流和可选桩端时长。
    void beginParameterWrites();
    /// 回读所有已写参数，任何不一致都进入安全失败。
    void beginParameterReadback();
    /// 写参数后重新读取完整状态，防止写入期间现场条件变化。
    void beginSecondPrecheck();
    /// 只发送现场 Python 已验证的正常启动线圈，不使用强制启动。
    void sendStartCommand();
    /// 等待启动输出并处理 E11 受控观察窗口。
    void pollWaitingForStart();
    /// 活动充电期间检查故障、电量停止意图和监控超时。
    void pollMonitoring();
    /// 将任意停止原因汇入 Stop→无输出→缩回→复位的唯一序列。
    void beginSafeShutdown();
    /// Stop 确认后反复读取，明确无输出前绝不允许缩枪。
    void pollWaitingForNoOutput();
    /// 发送缩回 ON，并在后续缩到位后负责发送相反 OFF 命令。
    void sendRetractCommand();
    /// 等待缩到位；超时或故障时仍优先尝试释放缩回线圈。
    void pollWaitingForRetracted();
    /// 在缩回线圈已释放后发送复位，再进入最终完整安全查询。
    void sendResetAndFinalCheck();
    /// 只读确认无输出、缩到位和恢复上下文均已解除。
    void beginFinalSafetyQuery();
    bool beginConservativeRecovery(SessionOrigin origin, StopReason reason,
                                   bool notifyApplication, QString *error);
    void beginConservativeRecoveryAfterSnapshot();
    /// 保守恢复无法证明安全时统一形成不安全会话终态。
    void finishConservativeRecoveryUnsafe(const QString &reason);
    /// 仅在权威安全快照充分覆盖全部写入里程碑后清除恢复责任。
    void clearSafetyRecoveryContext();
    /// 判断最新安全快照是否足以解除缩回线圈等不确定写入上下文。
    bool safeSnapshotResolvesRecoveryContext() const;
    /// 同一应用关闭代次只发布一次结果，防止窗口重复消费。
    void emitApplicationShutdownFinishedOnce(bool safe,
                                             const QString &message);
    /// 只读请求失败时根据已确认写入里程碑选择安全恢复动作。
    void handleReadFailure(const QString &reason);
    /// 清理旧传输并锁存恢复动作，随后按有限次数重新建连。
    void beginCommunicationRecovery(RecoveryAction action, const QString &reason);
    /// 执行一次恢复建连尝试，超过上限后按不确定终态失败。
    void startRecoveryConnection();
    /// 重连成功后从锁存的 RecoveryAction 继续，而不是重放未知写命令。
    void resumeAfterRecovery();
    /// 缩回 ON 已确认后的失败路径必须先尝试 OFF，再发布最终失败。
    void releaseRetractAfterFailure(const QString &reason);
    /// 用单次定时器保存下一业务阶段，禁止并行轮询。
    void schedulePhase(DeferredPhase phase, int delayMs);
    /**
     * @brief 按轮询间隔与当前有限阶段剩余时间的较小值安排下一轮。
     *
     * deadlineMs 为0时表示无限监控并沿用完整轮询间隔；有限阶段必须避免在接近
     * 截止点时再等待一个完整周期，否则停止和推杆超时会被配置的轮询值放大。
     */
    void schedulePhaseBeforeDeadline(DeferredPhase phase, int deadlineMs);
    bool snapshotHasBlockingFault(quint16 ignoredMask) const;
    /// 无输出要求工作位和继电器均关闭，且实时电流不超过安全阈值。
    bool snapshotHasNoOutput() const;
    /// 启动前要求无阻断故障、无输出且机械位置不存在矛盾。
    bool validatePrestartSnapshot(QString *reason) const;
    /// 设置完整采样时间并向上层发布不可修改的快照副本。
    void publishSnapshot();
    /// 停止定时器、断开连接并清除 FIFO/pending/in-flight 传输状态。
    void clearTransportWork();
    static int stopReasonPriority(StopReason reason);
    static QString stopReasonText(StopReason reason);
    static QString originText(SessionOrigin origin);
    void invalidateCommunicationContext(const QString &reason);
    static bool communicationTargetChanged(const ChargeSettings &previous,
                                           const ChargeSettings &next);
    static quint16 responseWord(const QByteArray &frame, int offset);

    ChargeSettings m_settings = ChargeSettings::defaults(); ///< 当前唯一生效参数快照。
    ChargePileSnapshot m_snapshot; ///< 最近一轮完整或部分更新的设备状态。
    State m_state = State::Idle;   ///< 对外发布的唯一控制器阶段。
    QTcpSocket m_socket;           ///< 唯一 RTU-over-TCP 连接，不跨线程共享。
    QTimer m_responseTimer;        ///< 连接或单条在途请求的响应上限。
    /**
     * 动作轮询定时器在任务三中承担连续 RTU 帧的最小间隔节流；后续有机械动作时
     * 可在同一串行出口上扩展为动作轮询，仍不会产生第二个并发发送器。
     */
    QTimer m_actionPollTimer;
    /** 机械动作与监控的轮询定时器；与 RTU 发送节流定时器完全分离。 */
    QTimer m_phaseTimer;
    QByteArray m_receiveBuffer; ///< TCP 拆包/粘包缓存，只由当前 in-flight 消费。
    QQueue<Request> m_requestQueue; ///< 同一流程尚未发送的串行请求 FIFO。
    /**
     * 已从队列取出但仍受 50ms 节流限制、尚未调用 QTcpSocket::write() 的请求。
     * 此阶段任何收到字节都不是本请求的合法响应，必须保守地中断查询。
     */
    std::optional<Request> m_pendingRequest;
    /**
     * 在调用 QTcpSocket::write() 前即设置的唯一在途请求。
     * 接收路径只能使用该对象校验并解析响应，避免迟到旧帧推进下一读取。
     * 若 write() 短写或失败，该对象还负责向故障路径提供准确的写命令身份。
     */
    std::optional<Request> m_inFlightRequest;
    QElapsedTimer m_lastSendTimer; ///< 从上一帧完整响应起计算 RTU 节流间隔。
    QElapsedTimer m_phaseElapsedTimer; ///< 当前有限业务阶段的总耗时基准。
    bool m_queryInProgress = false; ///< Query/Charge 共用的串行所有权门禁。
    FlowMode m_flowMode = FlowMode::None; ///< 当前请求队列的业务来源。
    std::function<void()> m_onQueueDrained; ///< 当前批次全部成功后的单次续接。
    DeferredPhase m_deferredPhase = DeferredPhase::None; ///< phaseTimer 的续接目标。

    /** 以下字段只属于当前充电会话，每次 startCharge() 接受请求时完整重置。 */
    quint64 m_sessionId = 0; ///< 单调递增会话号，用于区分现场日志代次。
    SessionOrigin m_sessionOrigin = SessionOrigin::Manual; ///< 当前会话所有权来源。
    StopReason m_stopReason = StopReason::Manual; ///< 可被更高优先级原因单向升级。
    bool m_seenChargingOutput = false; ///< 是否曾观察到工作/继电器/检测电流任一证据。
    bool m_safeStopRequested = false; ///< 已收到停止意图，当前阶段结束后必须收尾。
    bool m_safeShutdownStarted = false; ///< 防止重复建立 Stop/缩回/复位序列。
    bool m_applicationShutdownRequested = false; ///< 当前会话是否被应用关闭接管。
    bool m_applicationShutdownFinishedEmitted = false; ///< 应用关闭结果防重。
    bool m_sessionFinishedEmitted = false; ///< 普通会话结果防重。
    /** 启动命令后首次观察到E11时相对启动阶段计时器的毫秒值。 */
    std::optional<qint64> m_startupE11FirstSeenElapsedMs;
    bool m_startCommandConfirmed = false; ///< Start 回显已严格匹配。
    bool m_stopCommandConfirmed = false; ///< Stop 回显已严格匹配。
    bool m_retractOnConfirmed = false;   ///< 缩回 ON 回显已确认，后续必须负责释放。
    bool m_retractOffConfirmed = false;  ///< 缩回 OFF 回显已确认。
    bool m_resetCommandConfirmed = false; ///< Reset 回显已确认，仍需最终只读证明。
    /**
     * 安全恢复上下文从正常充电会话开始持续到最终完整快照确认安全。
     * 一次恢复通信失败不会清空它，下一次恢复必须从已确认的最远里程碑继续。
     */
    bool m_safetyRecoveryContextValid = false;
    /** 当前活动 Charge 流是否为“先完整只读、再按上下文恢复”的保守恢复。 */
    bool m_conservativeRecoveryActive = false;
    bool m_unknownGate = false; ///< 未经权威查询解除前禁止发布 SafeComplete。
    RecoveryAction m_recoveryAction = RecoveryAction::None; ///< 重连后唯一允许续接的动作。
    int m_recoveryAttempts = 0; ///< 当前通信恢复已使用的建连次数。
    QString m_recoveryReason;  ///< 首次触发恢复的现场上下文。
    QString m_terminalFailureAfterRetractOff; ///< 释放缩回线圈后需要发布的原失败原因。
    std::optional<ChargePileWriteIdentity> m_uncertainWrite; ///< 结果无法证明的写命令。
    quint16 m_expectedVoltageRaw = 0; ///< 本会话用于回读比较的 0.1V 原始值。
    quint16 m_expectedCurrentRaw = 0; ///< 本会话用于回读比较的 0.1A 原始值。
    std::optional<quint16> m_expectedCutoffRaw; ///< 启用时用于回读的截止电流原始值。
    std::optional<quint16> m_expectedMaxSecondsRaw; ///< 启用时用于回读的最大秒数。
};

Q_DECLARE_METATYPE(ChargePileController::State)
Q_DECLARE_METATYPE(ChargePileController::SessionOrigin)
Q_DECLARE_METATYPE(ChargePileController::StopReason)
