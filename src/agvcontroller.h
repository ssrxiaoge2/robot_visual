#ifndef AGVCONTROLLER_H
#define AGVCONTROLLER_H

#include <QObject>
#include <QModbusTcpClient>
#include <QModbusDataUnit>
#include <QTimer>

/// AGV 实时监控快照（监控轮询每周期发布一次）
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
 * 接口风格与 RobotController 对称，不依赖任何 UI 组件。
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

private slots:
    void onStateChanged(QModbusDevice::State state);

private:
    /// 文档地址位（1 基）→ Modbus PDU 地址（0 基）
    static constexpr int pdu(int docAddr) { return docAddr - 1; }

    void pollMonitor();

    QModbusTcpClient *m_client         = nullptr;
    QTimer           *m_reconnectTimer = nullptr;
    QTimer           *m_monitorTimer   = nullptr;
    AgvMonitorData    m_monitorData;
    QString           m_ip;
    int               m_port          = 502;
    int               m_targetStation = 0;
};

#endif // AGVCONTROLLER_H
