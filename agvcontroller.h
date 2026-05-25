#ifndef AGVCONTROLLER_H
#define AGVCONTROLLER_H

#include <QObject>
#include <QModbusTcpClient>
#include <QModbusDataUnit>
#include <QTimer>

/*!
 * \brief 仙工 AGV Modbus TCP 客户端
 *
 * 负责向 AGV 发送目标工位指令，并轮询 AGV 运行状态（空闲/行走中/到位/故障）。
 * 接口风格与 RobotController 对称，不依赖任何 UI 组件。
 *
 * 仙工 AGV Modbus 寄存器（Holding，单位：无量纲整数）：
 *   Holding 1000 — 目标工位编号（上位机写入）
 *   Input   1001 — AGV 当前状态字（上位机读取）
 *     0 = 空闲/停止  1 = 行走中  2 = 已到位  3 = 故障
 */
class AgvController : public QObject
{
    Q_OBJECT

public:
    /// AGV 运行状态（与 Input 1001 寄存器值对应）
    enum class AgvStatus : quint16 {
        Idle    = 0,  ///< 空闲 / 停止
        Moving  = 1,  ///< 行走中
        Arrived = 2,  ///< 已到位
        Fault   = 3,  ///< 故障
    };
    Q_ENUM(AgvStatus)

    // ── Modbus 寄存器地址 ─────────────────────────────────────
    static constexpr int REG_TARGET_STATION = 1000; ///< Holding: 目标工位编号
    static constexpr int REG_STATUS         = 1001; ///< Input:   AGV 状态字

    explicit AgvController(QObject *parent = nullptr);
    ~AgvController() override;

    void connectToHost(const QString &ip, int port = 502);
    void disconnectFromHost();
    bool isConnected() const;

    /// 向 AGV 写入目标工位编号，触发 AGV 自动行走
    void sendToStation(int stationNo);
    /// 请求读取 AGV 当前状态，结果通过 statusRead 信号异步返回
    void readStatusRegister();

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString &msg);

    /// AGV 状态寄存器读取完成
    void statusRead(AgvController::AgvStatus status);
    /// AGV 已到位（status == Arrived）快捷信号，由 statusRead 内部派发
    void arrived();

private slots:
    void onStateChanged(QModbusDevice::State state);

private:
    QModbusTcpClient *m_client         = nullptr;
    QTimer           *m_reconnectTimer = nullptr;
    QString           m_ip;
    int               m_port = 502;
};

#endif // AGVCONTROLLER_H
