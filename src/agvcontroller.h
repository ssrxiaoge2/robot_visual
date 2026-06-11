#ifndef AGVCONTROLLER_H
#define AGVCONTROLLER_H

#include <QObject>
#include <QModbusTcpClient>
#include <QModbusDataUnit>
#include <QTimer>

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

private slots:
    void onStateChanged(QModbusDevice::State state);

private:
    /// 文档地址位（1 基）→ Modbus PDU 地址（0 基）
    static constexpr int pdu(int docAddr) { return docAddr - 1; }

    QModbusTcpClient *m_client         = nullptr;
    QTimer           *m_reconnectTimer = nullptr;
    QString           m_ip;
    int               m_port          = 502;
    int               m_targetStation = 0;
};

#endif // AGVCONTROLLER_H
