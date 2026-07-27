#ifndef DEVICEMANAGER_H
#define DEVICEMANAGER_H

#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>
#include <optional>

#include "agvmonitorfreshnessguard.h"
#include "chargepilecontroller.h"
#include "chargesettings.h"
#include "chargeshutdownpolicy.h"
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
class QTimer;
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
    bool hasAgvMonitor() const {
        return m_agvMonitorFreshness.hasFreshSnapshot();
    }
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
    /**
     * @brief 将人工停止或不安全终态后的重试汇入控制器唯一的安全状态机。
     *
     * 活动充电会话使用 requestSafeStop()；若先前会话已经以 Unknown/Fault
     * 结束、当前没有活动 flow 但 shutdownRequired() 仍为真，则显式启动保守
     * 恢复。自动会话仍保留 Automatic 来源，使协调器能接收最终结果并释放保持；
     * 非自动所有权才使用 Manual，避免面板“停止充电”退化为 no-op。
     */
    bool stopChargePile(QString *error = nullptr);
    /// 返回唯一控制器是否仍有输出、机构或未知写命令等关闭风险。
    bool chargePileShutdownRequired() const;
    /// 应用关闭收尾开始后冻结所有新充电动作；失败结果到达后解除供人工补救。
    bool chargeApplicationShutdownInProgress() const {
        return m_chargeApplicationShutdownGate.blocksNewActions();
    }
    /**
     * @brief 请求程序关闭前安全收尾，只转发给唯一 ChargePileController。
     *
     * MainWindow 不持有套接字，也不能通过析构或断开 TCP 代替停止、缩回和复位。
     */
    void requestApplicationShutdown();
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
    /// 唯一控制器对一次应用关闭请求发布的最终安全结果。
    void applicationShutdownFinished(bool safe, const QString &message);
    /// 手动开始的一键只读预检及后续 DO0 启动请求的异步接受结果。
    void manualChargePreflightFinished(bool accepted, const QString &message);
    /// 自动授权基线预检的异步结果；enabled=true 时协调器授权已实际生效。
    void automaticChargeEnablePreflightFinished(bool enabled,
                                                 const QString &message);

private:
    /**
     * @brief 三个入口共用的在途只读预检意图。
     *
     * 该枚举只负责把一次 queryFinished 路由回原业务入口，不复制充电控制器
     * 状态机。None 也确保普通“查询状态”和启动 DO0 恢复查询不会误触发启动。
     */
    enum class ChargePreflightIntent {
        None,            ///< 普通查询或当前没有一键预检。
        ManualStart,     ///< 面板“开始充电”触发的新鲜安全查询。
        EnableAutomatic, ///< 自动授权生效前的只读基线查询。
        AutomaticStart   ///< 主调度实际置高 DO0 前的即时安全查询。
    };

    /**
     * @brief DeviceManager 内部的轻量 DO0 时序阶段。
     *
     * 不新增独立协调器类：手动与自动启动共用 Opening，充电期间为 Active，
     * 安全收尾后共用 Closing；StartupChecking 仅用于连接恢复时清理遗留高电平。
     */
    enum class ChargeDo0Phase {
        Idle,            ///< 没有 DO0 操作或充电会话所有权。
        Opening,         ///< 等待 DO0 高电平确认，尚未启动充电桩。
        Active,          ///< 充电桩会话活动，30 秒周期复核 DO0。
        Closing,         ///< 充电桩已安全，正在轻量尝试关闭 DO0。
        StartupChecking  ///< 软件重连后检查并处理遗留高电平。
    };

    bool tcpPing(const QString &ip, int port, int timeoutMs = 2000);
    void loadStationMap();
    void saveStationMap() const;
    /// 手动和自动开始共用入口：先确认 DO0 高，再把同一来源交给充电桩控制器。
    bool requestChargeStartWithDo0(
        ChargePileController::SessionOrigin origin, QString *error);
    /// 串行锁存三种业务意图并发起一轮无写操作的充电桩完整查询。
    bool beginChargePreflight(ChargePreflightIntent intent, QString *error);
    /// 消费当前预检结果，重新校验业务条件后续接授权、DO0 或主调度 Error。
    void handleChargePreflightFinished(bool queryOk, const QString &message);
    /// 主动关闭、Stop 或应用退出时撤销意图；自动开始必须释放协调器 pending。
    void cancelChargePreflight(const QString &reason);
    /// 采集手动开始上下文，并允许初始 Idle 进入只读预检但不允许直接启动。
    QString manualChargePreflightRejection() const;
    /// 采集自动授权上下文，并允许初始 Idle 建立安全基线。
    QString automaticEnablePreflightRejection() const;
    /// 自动查询结束前复核授权、调度生命周期、LM1 和导航空闲条件。
    QString automaticStartContinuationRejection() const;
    /// DO0 高电平确认期间只标记取消，待异步确认返回后统一安全结束。
    void cancelPendingChargeStart(const QString &reason);
    /// 路由 DO0 高/低确认：高确认后启动桩，低确认后只完成轻量关闭。
    void handleDo0EnsureFinished(bool targetHigh, bool confirmed,
                                 bool actualHigh, const QString &message);
    /// 处理启动恢复或活动监控的 DO0 只读结果，两种阶段采用不同安全语义。
    void handleDo0StateRead(bool ok, bool high, const QString &message);
    /// 充电桩安全完成后尝试关闭车辆许可；失败只记录，不推翻桩端安全结果。
    void beginDo0CloseBestEffort(const QString &context);
    /// 活动会话中 DO0 变低或连续读失败时只提交一次桩端安全停止。
    void requestDo0SafetyStop(const QString &reason);
    /// AGV 连接后检查遗留 DO0；高电平时必须先确认充电桩无输出。
    void beginStartupDo0Check();
    /// 应用关闭已等待 DO0 轻量关闭时，发布桩端既有安全结论。
    void finishPendingApplicationShutdownAfterDo0();

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
    AgvMonitorFreshnessGuard m_agvMonitorFreshness; ///< 统一去重断线和快照超时边沿。
    QTimer *m_agvMonitorFreshnessTimer = nullptr; ///< 每轮完整快照重启的单次新鲜度定时器。
    AgvMonitorData m_lastAgvMonitor; ///< 最近完整 AGV 电量、位置和导航状态。
    ChargePileController::State m_chargePileState =
        ChargePileController::State::Idle; ///< 缓存同步 stateChanged，供事务门禁使用。
    ChargeApplicationShutdownGate m_chargeApplicationShutdownGate; ///< 关闭期间的新动作冻结门禁。
    ChargeDo0Phase m_chargeDo0Phase = ChargeDo0Phase::Idle; ///< 手动/自动共用的轻量 DO0 时序。
    std::optional<ChargePileController::SessionOrigin>
        m_pendingChargeOrigin; ///< Opening 阶段等待 DO0 置高确认的会话来源。
    bool m_pendingChargeStartCanceled = false; ///< Stop/关闭/调度错误是否取消了待启动会话。
    QTimer *m_chargeDo0MonitorTimer = nullptr; ///< 充电期间每 30 秒只读复核 DO0。
    int m_do0MonitorReadFailures = 0; ///< 充电期间连续只读失败次数，成功即清零。
    bool m_do0SafeStopRequested = false; ///< 防止同一 DO0 异常重复请求安全停止。
    bool m_startupDo0PileQueryPending = false; ///< 启动恢复高电平后等待充电桩只读查询。
    bool m_applicationShutdownWaitingForDo0 = false; ///< 充电桩安全后等待一次 DO0 关闭尝试。
    QString m_pendingApplicationShutdownMessage; ///< 暂存充电桩关闭成功说明。
    ChargePreflightIntent m_chargePreflightIntent =
        ChargePreflightIntent::None; ///< 当前唯一在途的一键只读预检来源。
};

#endif // DEVICEMANAGER_H
