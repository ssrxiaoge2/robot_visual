#ifndef AGVCONTROLLER_H
#define AGVCONTROLLER_H

#include <QObject>
#include <QModbusTcpClient>
#include <QModbusDataUnit>
#include <QTimer>

/// AGV 实时监控快照；只有 monitorUpdated 发出时，各字段才属于同一轮完整读取。
struct AgvMonitorData {
    int     navStation = 0;     ///< [3x]00007 当前导航站点
    quint16 locStatus  = 0;     ///< [3x]00008 定位状态
    quint16 navStatus  = 0;     ///< [3x]00009 导航状态
    quint16 battery    = 0;     ///< [3x]00013 电池电量 0-100
    int     curStation = 0;     ///< [3x]00034 当前所在站点（严格判定）
    bool    ctrlSeized = false; ///< [3x]00043 控制权被外部抢占
};
Q_DECLARE_METATYPE(AgvMonitorData)

/*!
 * \brief 仙工 AGV Modbus TCP 客户端
 *
 * 负责向 AGV 下发目标站点指令，并轮询导航状态直到到达/失败。
 * 不依赖任何 UI 组件。
 *
 * 寄存器映射来自仙工官方 Modbus 寄存器表（可写寄存器/只读寄存器/
 * 可写状态量/只读状态量）。文档地址位以 00001 为起始地址，
 * 发送 Modbus 请求时统一减 1 转为 0 基 PDU 地址。
 *
 *   [4x] 00001  目标站点 id（写入 >0 触发路径导航，AGV 收到后自动清 0）
 *   [3x] 00007  当前导航站点 id
 *   [3x] 00008  定位状态（0=失败 1=正确 2=重定位中 3=完成）
 *   [3x] 00009  导航状态（见 NavStatus 枚举）
 *   [3x] 00043  控制权是否被外部抢占（0=否 1=是）
 *   [0x] 00006  取消导航（写 1 触发，AGV 收到后自动清 0）
 */
class AgvController : public QObject
{
    Q_OBJECT

public:
    /// AGV 导航状态（[3x]00009 寄存器值）
    enum class NavStatus : quint16 {
        None     = 0,  ///< 无导航任务
        Waiting  = 1,  ///< 等待执行导航
        Running  = 2,  ///< 正在执行导航
        Paused   = 3,  ///< 导航暂停
        Arrived  = 4,  ///< 到达
        Failed   = 5,  ///< 失败
        Canceled = 6,  ///< 取消
        Timeout  = 7,  ///< 超时
    };
    Q_ENUM(NavStatus)

    // ── 寄存器地址（文档地址位，1 基）─────────────────────────
    static constexpr int REG_TARGET_STATION = 1;  ///< [4x] 目标站点 id
    static constexpr int REG_NAV_STATION    = 7;  ///< [3x] 当前导航站点 id
    static constexpr int REG_LOC_STATUS     = 8;  ///< [3x] 定位状态
    static constexpr int REG_NAV_STATUS     = 9;  ///< [3x] 导航状态
    static constexpr int REG_CTRL_SEIZED    = 43; ///< [3x] 控制权是否被外部抢占
    static constexpr int COIL_CANCEL_NAV    = 6;  ///< [0x] 取消导航
    static constexpr int REG_BATTERY        = 13; ///< [3x] 电池电量
    static constexpr int REG_CUR_STATION    = 34; ///< [3x] 当前所在站点
    static constexpr int COIL_PAUSE_NAV     = 4;  ///< [0x] 暂停导航
    static constexpr int COIL_RESUME_NAV    = 5;  ///< [0x] 继续导航
    static constexpr int COIL_DO0_LOW       = 20; ///< [0x] 写1将DO0置为低电平
    static constexpr int COIL_DO0_HIGH      = 60; ///< [0x] 写1将DO0置为高电平
    static constexpr int DISCRETE_DO0_STATE = 60; ///< [1x] DO0实际电平
    static constexpr int DO0_CONFIRM_ATTEMPTS = 3; ///< 写入后最多只读确认次数
    static constexpr int DO0_CONFIRM_INTERVAL_MS = 500; ///< 相邻确认读取间隔

    explicit AgvController(QObject *parent = nullptr);
    ~AgvController() override;

    void connectToHost(const QString &ip, int port = 502);
    void disconnectFromHost();
    bool isConnected() const;

    /// 写目标站点 id 到 [4x]00001，触发 AGV 路径导航
    void sendToStation(int stationNo);
    /// 读 [3x]00007-00009（导航站点/定位状态/导航状态），结果通过 statusRead 信号异步返回
    void readStatusRegister();
    /// 写 [0x]00006=1 取消当前导航
    void cancelNavigation();
    /// 读 [3x]00043 控制权状态，结果通过 controlOwnershipRead 信号异步返回
    void readControlOwnership();
    /// 写 [0x]00004=1 暂停当前导航
    void pauseNavigation();
    /// 写 [0x]00005=1 继续导航
    void resumeNavigation();
    /**
     * @brief 确保DO0达到目标电平，并用只读状态确认最终结果。
     *
     * 返回值只表示异步请求已被接受；最终结果通过do0EnsureFinished发布。
     * 置高与置低分别使用仙工定义的两个自清零命令线圈，写响应无论成功或失败
     * 都不会触发同一命令重发，只会最多读取三次[1x]00060确认真实状态。
     */
    bool ensureDo0(bool high, QString *error = nullptr);
    /// 只读查询[1x]00060；不会写任何DO命令，结果通过do0StateRead发布。
    bool queryDo0(QString *error = nullptr);
    /// 启动监控轮询（连接成功后自动调用）
    void startMonitor(int intervalMs = 1000);
    void stopMonitor();

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString &msg);

    /// AGV 状态轮询完成：导航状态 + 当前导航站点 id
    void statusRead(AgvController::NavStatus status, int navStation);
    /// 已到达本次 sendToStation() 下发的目标站点（status==Arrived 且站点匹配）
    void arrived();
    /// 控制权读取完成（true = 被外部抢占，此时 Modbus 指令可能无效）
    void controlOwnershipRead(bool externallySeized);
    /// 监控轮询完成，发布最新快照
    void monitorUpdated(const AgvMonitorData &data);
    /**
     * @brief DO0目标电平确认结束。
     * @param targetHigh 本次请求的目标电平。
     * @param confirmed 是否明确读到目标电平；为false时不得使用actualHigh做安全判断。
     * @param actualHigh 最后一次成功读到的电平，或读取全部失败时的占位值。
     * @param message 中文结果说明。
     */
    void do0EnsureFinished(bool targetHigh, bool confirmed, bool actualHigh,
                           const QString &message);
    /// DO0独立只读查询结果；ok为false时high仅为占位值。
    void do0StateRead(bool ok, bool high, const QString &message);

private slots:
    void onStateChanged(QModbusDevice::State state);

private:
    /// 文档地址位（1 基）→ Modbus PDU 地址（0 基）
    static constexpr int pdu(int docAddr) { return docAddr - 1; }

    /// 周期读取两段输入寄存器，只有两段均成功才发布字段一致的完整快照。
    void pollMonitor();
    /// ensureDO0 的统一只读阶段：写前避免重复写，写后验证真实电平。
    void readDo0ForEnsure();
    /// 每轮 ensureDO0 最多发送一次置高或置低命令，随后只允许读取确认。
    void sendDo0Write();
    /// 在有限确认次数之间等待固定间隔，避免对 AGV 连续叠发读取。
    void scheduleDo0Confirmation();
    /// 清理 ensureDO0 所有权并发布一次最终确认结果。
    void finishDo0Ensure(bool confirmed, bool actualHigh,
                         const QString &message);
    /// 清理独立只读所有权并发布结果，不改变 DO0 电平。
    void finishDo0Query(bool ok, bool high, const QString &message);
    /// 断线或析构时结束当前 DO0 操作；不尝试补发任何写命令。
    void cancelDo0Operation(const QString &reason);

    QModbusTcpClient *m_client         = nullptr; ///< QObject 子对象，Modbus TCP 主站。
    QTimer           *m_reconnectTimer = nullptr; ///< 断线后每 5s 尝试重连。
    QTimer           *m_monitorTimer   = nullptr; ///< 周期读取导航、位置和控制权。
    AgvMonitorData    m_monitorData;              ///< 最近一轮完整监控快照。
    QString           m_ip;                       ///< 非空表示允许自动重连。
    int               m_port          = 502;      ///< Modbus TCP 标准端口。
    int               m_targetStation = 0;        ///< 最近一次 sendToStation 的目标 LM。
    bool              m_monitorBusy   = false; ///< 防止上一轮读取未完成时叠发请求
    QTimer            *m_do0ConfirmTimer = nullptr; ///< DO0有限只读确认间隔。
    bool               m_do0OperationBusy = false; ///< 防止DO0写确认与独立查询并发。
    bool               m_do0StandaloneQuery = false; ///< 当前操作是否为独立只读查询。
    bool               m_do0TargetHigh = false; ///< 当前ensureDo0的目标电平。
    bool               m_do0WriteSent = false; ///< 本轮是否已经发送唯一一次写命令。
    int                m_do0ConfirmAttempts = 0; ///< 写后已完成的只读确认次数。
    int                m_do0InitialReadFailures = 0; ///< 写前连续读取失败次数。
};

#endif // AGVCONTROLLER_H
