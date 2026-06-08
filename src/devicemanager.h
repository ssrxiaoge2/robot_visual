#ifndef DEVICEMANAGER_H
#define DEVICEMANAGER_H

#include <QList>
#include <QObject>
#include <QString>

class RobotController;
class AgvController;
class VisionHttpClient;
class HuayanScheduler;

class DeviceManager : public QObject
{
    Q_OBJECT
public:
    struct Config {
        QString robotIP    = QStringLiteral("192.168.10.10");
        int     robotPort  = 502;
        QString agvIP      = QStringLiteral("192.168.192.5");
        int     agvPort    = 502;
        QString cameraIP   = QStringLiteral("192.168.1.10");
        int     cameraPort = 8080;
        QString scannerIP  = QStringLiteral("192.168.2.100");
        QString huayanIP   = QStringLiteral("192.168.10.10");
        quint16 huayanPort = 10003;
    };

    explicit DeviceManager(QObject *parent = nullptr);

    RobotController  *robotController()  const { return m_robotCtrl;   }
    AgvController    *agvController()    const { return m_agvCtrl;     }
    VisionHttpClient *visionClient()     const { return m_visionClient; }
    HuayanScheduler  *huayanScheduler() const { return m_huayanScheduler; }
    bool              lightIsOn()        const { return m_lightOn;      }
    const Config     &config()           const { return m_cfg;          }

    void setConfig(const Config &cfg) { m_cfg = cfg; }

public slots:
    void applyConfig();
    void testRobot();
    void testAgv();
    void testCamera();
    void testScanner();
    void toggleLight();
    void debugReadRobotRegisters(int addr, int count);
    void debugWriteRobotRegister(int addr, quint16 value);
    void applyHandEyeMatrix(const float m[16]);

signals:
    void robotStatusChanged(bool ok, const QString &statusText);
    void agvStatusChanged(bool ok, const QString &statusText);
    void cameraStatusChanged(bool ok, const QString &statusText);
    void scannerStatusChanged(bool ok, const QString &statusText);
    void lightChanged(bool on, bool success);
    void configApplied(const QString &robotIP, int port, const QString &agvIP);
    void robotModbusConnected();
    void robotModbusDisconnected();
    void robotModbusError(const QString &msg);
    void agvModbusConnected();
    void agvModbusDisconnected();
    void agvModbusError(const QString &msg);
    void debugRegistersRead(int startAddr, const QList<quint16> &values);
    void handEyeMatrixApplied();
    void logMessage(const QString &msg);

private:
    bool tcpPing(const QString &ip, int port, int timeoutMs = 2000);

    RobotController  *m_robotCtrl    = nullptr;
    AgvController    *m_agvCtrl     = nullptr;
    VisionHttpClient *m_visionClient = nullptr;
    HuayanScheduler  *m_huayanScheduler = nullptr;
    Config            m_cfg;
    bool              m_lightOn      = false;
};

#endif // DEVICEMANAGER_H
