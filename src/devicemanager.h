#ifndef DEVICEMANAGER_H
#define DEVICEMANAGER_H

#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>

#include "chargepilecontroller.h"
#include "chargesettings.h"
#include "customSysScheduler.h"
#include "lineconfig.h"
#include "linemanager.h"
#include "nscanscheduler.h"
#include "runtimesettings.h"

class AgvController;
class AutoChargeCoordinator;
class VisionHttpClient;
class HuayanScheduler;
class LineOrchestrator;
class PalletScheduler;
class QThread;
class SettingsManager;

Q_DECLARE_METATYPE(NScanScheduler::ScanResult)
Q_DECLARE_METATYPE(NScanScheduler::ScanOptions)

/**
 * @brief 所有设备对象和跨线程 worker 的唯一所有者/接线中心。
 *
 * MainWindow 只通过 getter/槽访问设备；LineManager 与测试面板复用同一批控制器。
 * 调度扫码和 UI 测试扫码使用不同 worker，结果必须保持隔离。
 */
class DeviceManager : public QObject
{
    Q_OBJECT
public:
    /// 现场网络配置；端口单位为 TCP 端口号，修改后需调用 applyConfig()。
    struct Config {
        QString robotIP    = QStringLiteral("192.168.1.11");
        QString agvIP      = QStringLiteral("192.168.1.100");
        int     agvPort    = 502;
        QString cameraIP   = QStringLiteral("127.0.0.1");
        int     cameraPort = 8080;
        QString scannerIP  = QStringLiteral("192.168.1.12");
        QString huayanIP   = QStringLiteral("192.168.1.11");
        quint16 huayanPort = 10003;
        QString customSysEndpoint = QStringLiteral("http://192.168.115.229:5084/api/MesData/day");
    };

    explicit DeviceManager(QObject *parent = nullptr);
    ~DeviceManager() override;

    AgvController    *agvController()    const { return m_agvCtrl;     }
    VisionHttpClient *visionClient()     const { return m_visionClient; }
    HuayanScheduler  *huayanScheduler() const { return m_huayanScheduler; }
    LineOrchestrator *lineOrchestrator() const { return m_lineOrch; }
    LineManager      *lineManager()     const { return m_lineManager; }
    NScanScheduler   *nscanScheduler() const { return m_nscanScheduler.get(); }
    PalletScheduler  *palletScheduler() const { return m_palletScheduler; }
    CustomSysScheduler *customSysScheduler() const { return m_customSysScheduler; }
    /// 返回 DeviceManager 唯一拥有的充电桩控制器；UI/协调器不得另建通信实例。
    ChargePileController *chargePileController() const { return m_chargePileController; }
    /// 返回只产生业务意图、不直接发送 AGV 或 Modbus 命令的自动充电协调器。
    AutoChargeCoordinator *autoChargeCoordinator() const { return m_autoChargeCoordinator; }
    /// 返回已通过校验并成功持久化的当前充电参数快照。
    const ChargeSettings &chargeSettings() const { return m_chargeSettings; }
    /// 是否已取得一轮字段一致的 AGV 监控快照。
    bool hasAgvMonitor() const { return m_hasAgvMonitor; }
    /// 最近一轮完整 AGV 快照；hasAgvMonitor()==false 时调用方不得据此做安全决策。
    AgvMonitorData lastAgvMonitor() const { return m_lastAgvMonitor; }
    bool              lightIsOn()        const { return m_lightOn;      }
    bool              nscanTestRunning() const { return m_nscanTestRunning; }
    const Config     &config()           const { return m_cfg;          }
    const RuntimeSettings &runtimeSettings() const;
    bool runtimeSettingsLocked() const;
    bool applyRuntimeSettingsCandidate(const RuntimeSettings &candidate,
                                       QString *error);
    /**
     * @brief 校验、原子保存并事务式应用一份充电候选设置。
     *
     * 查询/会话/恢复在途或控制器处于 Fault/Unknown 时拒绝。只有文件原子提交
     * 成功后才同时更新控制器、协调器和本对象快照，保存失败不会改变运行参数。
     */
    bool applyChargeSettingsCandidate(const ChargeSettings &candidate,
                                      QString *error);
    /**
     * @brief 执行业务层手动开始门禁并向唯一控制器提交 Manual 会话。
     *
     * 界面控件禁用不能替代这里对 LineManager Idle、LM1、导航空闲、自动模式
     * 关闭以及控制器非忙碌/非未知状态的复核。
     */
    bool startManualCharge(QString *error);
    /// 提交只读五组状态查询；不会发送任何写寄存器或写线圈命令。
    bool queryChargePileStatus(QString *error);
    /// 将当前活动充电会话汇入控制器唯一的安全停止状态机。
    void stopChargePile();
    /// 设置本次进程的自动授权；该值默认关闭且绝不写入 charge-settings.ini。
    bool setAutoChargeEnabled(bool enabled, QString *error);

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
    void testCustomSystem();
    void fetchCustomSystemDayData();
    void startNScanTest(const NScanScheduler::ScanOptions &options);
    void toggleLight();
    void applyHandEyeMatrix(const float m[16]);
    void dispatchAgv(int workstation);
    void cancelAgvNav();
    void pauseAgvNav();
    void resumeAgvNav();
    /**
     * @brief 启动指定工位的手工阶段一测试。
     * @param stationId 现场工位号，允许 1～12。
     * @return 已完成互斥检查和配置注入并发起阶段一时返回 true；拒绝时返回 false 并记录原因。
     *
     * 该入口固定关闭扫码，只复用 HuayanScheduler 的既有阶段一状态机；总调度不得调用。
     */
    bool startStandaloneStageOne(int stationId);

signals:
    void robotStatusChanged(bool ok, const QString &statusText);
    void agvStatusChanged(bool ok, const QString &statusText);
    void cameraStatusChanged(bool ok, const QString &statusText);
    void scannerStatusChanged(bool ok, const QString &statusText);
    void nscanTestStarted();
    void nscanTestFinished(const NScanScheduler::ScanResult &result);
    void nscanTestIdle();
    void nscanTestLog(const QString &message);
    void nscanScanRequested(const NScanScheduler::ScanOptions &options);
    void lineScanRequested(const NScanScheduler::ScanOptions &options);
    void customSystemStatusChanged(bool ok, const QString &statusText);
    void customSystemDayDataReady(const CustomSysScheduler::DayRecord &record,
                                  const QString &rawJson);
    void customSystemRequestStarted(const QString &operation);
    void customSystemRequestFailed(const QString &operation,
                                   const QString &errorMessage,
                                   const QString &rawJson);
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

    AgvController    *m_agvCtrl     = nullptr;       ///< QObject 子对象，唯一 AGV 通信实例。
    VisionHttpClient *m_visionClient = nullptr;      ///< QObject 子对象，视觉 HTTP 与坐标转换。
    HuayanScheduler  *m_huayanScheduler = nullptr;  ///< QObject 子对象，唯一机械臂状态机。
    LineOrchestrator *m_lineOrch = nullptr;          ///< 旧单工位参考流程，不是新调度主线。
    LineManager      *m_lineManager = nullptr;       ///< 12 工位连续补料主调度。
    ChargePileController *m_chargePileController = nullptr; ///< 唯一 RTU-over-TCP 充电桩控制器。
    AutoChargeCoordinator *m_autoChargeCoordinator = nullptr; ///< 自动阈值策略协调器，不拥有设备。
    PalletScheduler  *m_palletScheduler = nullptr;   ///< 主流程和配置 UI 共用的码垛缓存。
    CustomSysScheduler *m_customSysScheduler = nullptr;
    std::shared_ptr<NScanScheduler> m_nscanScheduler;
    QMutex            m_nscanScanMutex;      ///< 厂商扫码 SDK 串行保护，两个 worker 共用。
    QPointer<QThread> m_nscanTestThread;
    QPointer<QObject> m_nscanTestWorker;
    QPointer<QThread> m_lineScanThread;       ///< 主调度专用扫码线程。
    QPointer<QObject> m_lineScanWorker;       ///< 只把结果送回 LineManager。
    NScanScheduler::ScanOptions m_nscanTestOptions;
    NScanScheduler::ScanOptions m_lineScanOptions;
    Config            m_cfg;
    bool              m_lightOn      = false;
    bool              m_nscanTestRunning = false;
    QHash<int, int>   m_stationMap;
    SettingsManager  *m_settingsManager = nullptr;
    QString m_chargeSettingsPath; ///< AppConfigLocation 下的原子充电参数文件路径。
    ChargeSettings m_chargeSettings = ChargeSettings::defaults(); ///< 已持久化且已生效的快照。
    bool m_hasAgvMonitor = false; ///< 业务层手动门禁不得使用未完整更新的 AGV 默认值。
    AgvMonitorData m_lastAgvMonitor; ///< 最近完整 AGV 电量、位置和导航状态。
    ChargePileController::State m_chargePileState =
        ChargePileController::State::Idle; ///< 缓存同步 stateChanged，供事务门禁使用。
};

#endif // DEVICEMANAGER_H
