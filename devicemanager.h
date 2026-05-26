#ifndef DEVICEMANAGER_H
#define DEVICEMANAGER_H

#include <QObject>
#include <QString>

class RobotController;
class AgvController;
class VisionHttpClient;

/*!
 * \brief 设备管理器
 *
 * 负责所有硬件设备的配置管理、连通性测试、补光灯控制，
 * 以及 Modbus TCP 机械臂/AGV 连接的生命周期管理。
 * 不依赖任何 UI 组件，仅通过信号向外通知状态变化和日志。
 *
 * 管理的设备：
 *   ① RobotController  — 华沿机械臂 Modbus TCP
 *   ② AgvController    — 仙工 AGV Modbus TCP
 *   ③ VisionHttpClient — 视觉服务 HTTP API（Flask，端口 8080）
 *   ④ 补光灯           — Linux GPIO（fill_light 程序）
 */
class DeviceManager : public QObject
{
    Q_OBJECT
public:
    /// 所有设备的 IP/端口配置
    struct Config {
        QString robotIP    = QStringLiteral("192.168.10.10");
        int     robotPort  = 502;
        QString agvIP      = QStringLiteral("192.168.192.5");
        int     agvPort    = 502;
        QString cameraIP   = QStringLiteral("192.168.1.10");
        int     cameraPort = 8080; ///< 视觉服务 Flask HTTP 端口（默认 8080）
        QString scannerIP  = QStringLiteral("192.168.2.100");
    };

    explicit DeviceManager(QObject *parent = nullptr);

    RobotController  *robotController()  const { return m_robotCtrl;   }
    AgvController    *agvController()    const { return m_agvCtrl;     }
    VisionHttpClient *visionClient()     const { return m_visionClient; }
    bool              lightIsOn()        const { return m_lightOn;      }
    const Config     &config()           const { return m_cfg;          }

    /// 更新配置（不立即连接，需显式调用 applyConfig）
    void setConfig(const Config &cfg) { m_cfg = cfg; }

public slots:
    /// 应用当前配置并重新连接机械臂 Modbus
    void applyConfig();
    /// TCP 连通性测试（各设备独立）
    void testRobot();
    void testAgv();
    void testCamera();
    void testScanner();
    /// 切换补光灯开/关状态
    void toggleLight();

signals:
    // ── 设备连通状态（ok=true 可达，statusText 供指示灯显示）──
    void robotStatusChanged(bool ok, const QString &statusText);
    void agvStatusChanged(bool ok, const QString &statusText);
    void cameraStatusChanged(bool ok, const QString &statusText);
    void scannerStatusChanged(bool ok, const QString &statusText);

    // ── 补光灯（on=目标状态，success=操作是否成功）──
    void lightChanged(bool on, bool success);

    // ── 配置已应用（供 UI 更新指示灯为"待连接"）──
    void configApplied(const QString &robotIP, int port, const QString &agvIP);

    // ── Modbus 连接状态（透传 RobotController 信号）──
    void robotModbusConnected();
    void robotModbusDisconnected();
    void robotModbusError(const QString &msg);

    // ── Modbus 连接状态（透传 AgvController 信号）────
    void agvModbusConnected();
    void agvModbusDisconnected();
    void agvModbusError(const QString &msg);

    // ── 日志消息（供 MainWindow 转发到日志控件）──
    void logMessage(const QString &msg);

private:
    /// 同步 TCP 连通性探测，成功返回 true
    bool tcpPing(const QString &ip, int port, int timeoutMs = 2000);

    RobotController  *m_robotCtrl    = nullptr;
    AgvController    *m_agvCtrl     = nullptr;
    VisionHttpClient *m_visionClient = nullptr;
    Config            m_cfg;
    bool              m_lightOn      = true;
};

#endif // DEVICEMANAGER_H
