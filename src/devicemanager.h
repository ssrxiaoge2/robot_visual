#ifndef DEVICEMANAGER_H
#define DEVICEMANAGER_H

#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>

#include "customSysScheduler.h"
#include "lineconfig.h"
#include "linemanager.h"
#include "nscanscheduler.h"
#include "shortagetypes.h"

class AgvController;
class VisionHttpClient;
class HuayanScheduler;
class LineOrchestrator;
class PalletScheduler;
class QThread;
class IShortageTaskGateway;
class LiveShortageCoordinator;
class ShortageEngine;
class ShortageSampleCoordinator;
class ShortageStateStore;
class ShortageTestController;

Q_DECLARE_METATYPE(NScanScheduler::ScanResult)
Q_DECLARE_METATYPE(NScanScheduler::ScanOptions)
Q_DECLARE_METATYPE(Task)
Q_DECLARE_METATYPE(QList<Task>)
Q_DECLARE_METATYPE(LineSystemState)

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
    /// 真实缺料 MES/PLC 协议对象由 DeviceManager 唯一持有；Task 10 再接入采样协调器。
    CustomSysScheduler *liveShortageScheduler() const { return m_liveShortageScheduler; }
    /// 生产缺料协调器由 DeviceManager 唯一持有；UI 可读取只读确认摘要，不取得所有权。
    LiveShortageCoordinator *liveShortageCoordinator() const { return m_liveShortageCoordinator; }
    /// 独立完整逻辑测试控制器由配置弹窗短期连接；DeviceManager 保持所有权。
    ShortageTestController *shortageTestController() const { return m_shortageTestController; }
    bool              lightIsOn()        const { return m_lightOn;      }
    bool              nscanTestRunning() const { return m_nscanTestRunning; }
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
    void startNScanTest(const NScanScheduler::ScanOptions &options);
    void toggleLight();
    void applyHandEyeMatrix(const float m[16]);
    void dispatchAgv(int workstation);
    void cancelAgvNav();
    void pauseAgvNav();
    void resumeAgvNav();
    /// UI 切换正式缺料输入源；DeviceManager 只转发到唯一生产协调器，不重复接线。
    void setShortageInputSource(ShortageInputSource source);
    /// UI 确认启动恢复摘要；accepted=false 时生产协调器继续保持停止门禁。
    void confirmShortageRecoveredState(bool accepted);
    /// UI 人工补一箱入口；仍走生产协调器账本、审计和 FIFO 队尾追加路径。
    void requestManualShortageBox(int stationId, bool highStockRiskConfirmed);
    /// UI 维护修正入口；生产协调器负责再次校验并调用 Engine 维护事务。
    void applyShortageMaintenanceCorrection(ShortageMaintenanceCorrection correction);

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
    void lightChanged(bool on, bool success);
    void configApplied(const QString &robotIP, const QString &agvIP);
    void agvModbusConnected();
    void agvModbusDisconnected();
    void agvModbusError(const QString &msg);
    void handEyeMatrixApplied();
    void logMessage(const QString &msg);
    /// 生产缺料快照转发信号；UI 只观察 DeviceManager 暴露的唯一协调器状态。
    void shortageSnapshotChanged(ShortageUiSnapshot snapshot);

private:
    bool tcpPing(const QString &ip, int port, int timeoutMs = 2000);
    void loadStationMap();
    void saveStationMap() const;

    AgvController    *m_agvCtrl     = nullptr;       ///< QObject 子对象，唯一 AGV 通信实例。
    VisionHttpClient *m_visionClient = nullptr;      ///< QObject 子对象，视觉 HTTP 与坐标转换。
    HuayanScheduler  *m_huayanScheduler = nullptr;  ///< QObject 子对象，唯一机械臂状态机。
    LineOrchestrator *m_lineOrch = nullptr;          ///< 旧单工位参考流程，不是新调度主线。
    LineManager      *m_lineManager = nullptr;       ///< 12 工位连续补料主调度。
    PalletScheduler  *m_palletScheduler = nullptr;   ///< 主流程和配置 UI 共用的码垛缓存。
    std::unique_ptr<ShortageStateStore> m_productionShortageStore; ///< 正式状态仓库，后于生产 QObject 释放。
    std::unique_ptr<ShortageEngine> m_productionShortageEngine; ///< 正式缺料账本/计划核心。
    std::unique_ptr<ShortageStateStore> m_testShortageStore; ///< 独立测试状态仓库，禁止触碰正式文件。
    std::unique_ptr<ShortageEngine> m_testShortageEngine; ///< 预留测试核心所有权，当前由测试控制器内部重建。
    std::unique_ptr<IShortageTaskGateway> m_shortageTaskGateway; ///< 生产 FIFO 队尾适配器，无重排能力。
    CustomSysScheduler *m_liveShortageScheduler = nullptr; ///< 唯一真实缺料 MES/PLC 通信对象。
    ShortageSampleCoordinator *m_shortageSampleCoordinator = nullptr; ///< 唯一采样层，测试/正式互斥复用。
    ShortageTestController *m_shortageTestController = nullptr; ///< 独立测试控制器，由 DeviceManager 显式删除。
    LiveShortageCoordinator *m_liveShortageCoordinator = nullptr; ///< 生产协调器，只接线一次。
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
};

#endif // DEVICEMANAGER_H
