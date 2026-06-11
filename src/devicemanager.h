#ifndef DEVICEMANAGER_H
#define DEVICEMANAGER_H

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

class AgvController;
class VisionHttpClient;
class HuayanScheduler;

class DeviceManager : public QObject
{
    Q_OBJECT
public:
    struct Config {
        QString robotIP    = QStringLiteral("192.168.1.11");
        QString agvIP      = QStringLiteral("192.168.1.100");
        int     agvPort    = 502;
        QString cameraIP   = QStringLiteral("127.0.0.1");
        int     cameraPort = 8080;
        QString scannerIP  = QStringLiteral("192.168.1.12");
        QString huayanIP   = QStringLiteral("192.168.1.11");
        quint16 huayanPort = 10003;
    };

    explicit DeviceManager(QObject *parent = nullptr);

    AgvController    *agvController()    const { return m_agvCtrl;     }
    VisionHttpClient *visionClient()     const { return m_visionClient; }
    HuayanScheduler  *huayanScheduler() const { return m_huayanScheduler; }
    bool              lightIsOn()        const { return m_lightOn;      }
    const Config     &config()           const { return m_cfg;          }

    void setConfig(const Config &cfg) { m_cfg = cfg; }

    /// 工位号 → AGV 站点 id（未配置回退 id=工位号）
    int resolveStation(int workstation) const;
    QHash<int, int> stationMap() const { return m_stationMap; }
    void setStationMap(const QHash<int, int> &map);

public slots:
    void applyConfig();
    void testRobot();
    void testAgv();
    void testCamera();
    void testScanner();
    void toggleLight();
    void applyHandEyeMatrix(const float m[16]);
    void dispatchAgv(int workstation);
    void cancelAgvNav();
    void pauseAgvNav();
    void resumeAgvNav();

signals:
    void robotStatusChanged(bool ok, const QString &statusText);
    void agvStatusChanged(bool ok, const QString &statusText);
    void cameraStatusChanged(bool ok, const QString &statusText);
    void scannerStatusChanged(bool ok, const QString &statusText);
    void lightChanged(bool on, bool success);
    void configApplied(const QString &robotIP, const QString &agvIP);
    void agvModbusConnected();
    void agvModbusDisconnected();
    void agvModbusError(const QString &msg);
    void handEyeMatrixApplied();
    void logMessage(const QString &msg);

private:
    bool tcpPing(const QString &ip, int port, int timeoutMs = 2000);
    void loadStationMap();
    void saveStationMap() const;

    AgvController    *m_agvCtrl     = nullptr;
    VisionHttpClient *m_visionClient = nullptr;
    HuayanScheduler  *m_huayanScheduler = nullptr;
    Config            m_cfg;
    bool              m_lightOn      = false;
    QHash<int, int>   m_stationMap;
};

#endif // DEVICEMANAGER_H
