#include "devicemanager.h"
#include "agvcontroller.h"
#include "autochargecoordinator.h"
#include "chargebusinessrules.h"
#include "chargepilecontroller.h"
#include "chargesettings.h"
#include "chargesettingstransaction.h"
#include "customSysScheduler.h"
#include "huayanScheduler.h"
#include "linemanager.h"
#include "lineorchestrator.h"
#include "nscanscheduler.h"
#include "palletscheduler.h"
#include "settingsmanager.h"
#include "visionclient.h"

#include <QDebug>
#include <QDir>
#include <QMutexLocker>
#include <QSettings>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QStandardPaths>

#include <utility>
#ifdef Q_OS_LINUX
#  include <QProcess>  // fill_light GPIO 控制（仅 Linux 编译）
#endif

// QSettings 组织/应用名与映射键（值格式 "工位:站点,工位:站点"，如 "2:3,5:7"）
static const char *kSettingsOrg  = "wh-robot";
static const char *kSettingsApp  = "robot-visual";
static const char *kStationMapKey = "agv/stationMap";

namespace {
// AGV 正常每 1 秒完成一轮监控读取；5 秒至少覆盖三个完整周期，既避免单轮
// Modbus 抖动误停，也能在自动充电期间及时废止陈旧电量和 LM1 位置。
constexpr int kAgvMonitorFreshnessTimeoutMs = 5000;

/// 同步扫码 SDK 的 worker 包装；moveToThread 后 scan() 在工作线程执行。
class NScanWorker : public QObject
{
    Q_OBJECT

public:
    explicit NScanWorker(std::shared_ptr<NScanScheduler> scheduler, QMutex *scanMutex)
        : m_scheduler(std::move(scheduler))
        , m_scanMutex(scanMutex)
    {
    }

public slots:
    void scan(const NScanScheduler::ScanOptions &options)
    {
        QMutexLocker locker(m_scanMutex);
        emit finished(m_scheduler->scan(options));
    }

signals:
    void finished(const NScanScheduler::ScanResult &result);

private:
    std::shared_ptr<NScanScheduler> m_scheduler;
    QMutex *m_scanMutex = nullptr;
};

unsigned long boundedScanWaitMs(const NScanScheduler::ScanOptions &options)
{
    const qint64 attempts = qMax(1, options.maxAttempts);
    const qint64 scanWaitMs = static_cast<qint64>(qMax(0, options.timeoutMs))
            * attempts
        + static_cast<qint64>(qMax(0, options.retryIntervalMs))
            * (attempts - 1)
        + 2000;
    return static_cast<unsigned long>(
        qBound<qint64>(qint64{2000}, scanWaitMs, qint64{60000}));
}

QString rawDataSummary(const QByteArray &rawData)
{
    constexpr qsizetype kSummaryBytes = 64;
    const QByteArray prefix = rawData.left(kSummaryBytes).toHex(' ');
    const QString suffix = rawData.size() > kSummaryBytes
        ? QStringLiteral(" ...")
        : QString();
    return QString::fromLatin1(prefix) + suffix;
}
}

DeviceManager::DeviceManager(QObject *parent)
    : QObject(parent),
      m_nscanScheduler(std::make_shared<NScanScheduler>())
{
    qRegisterMetaType<NScanScheduler::ScanResult>("NScanScheduler::ScanResult");
    qRegisterMetaType<NScanScheduler::ScanOptions>("NScanScheduler::ScanOptions");
    qRegisterMetaType<CustomSysScheduler::DayRecord>("CustomSysScheduler::DayRecord");
    qRegisterMetaType<Task>("Task");
    qRegisterMetaType<QList<Task>>("QList<Task>");
    qRegisterMetaType<LineSystemState>("LineSystemState");
    qRegisterMetaType<ChargePileSnapshot>("ChargePileSnapshot");
    qRegisterMetaType<ChargePileController::State>("ChargePileController::State");
    qRegisterMetaType<ChargePileController::SessionOrigin>(
        "ChargePileController::SessionOrigin");
    qRegisterMetaType<ChargePileController::StopReason>(
        "ChargePileController::StopReason");

    QString settingsDir =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (settingsDir.isEmpty())
        settingsDir = QDir::currentPath() + QStringLiteral("/config");
    QDir().mkpath(settingsDir);
    m_settingsManager = new SettingsManager(
        settingsDir + QStringLiteral("/runtime-settings.ini"), this);

    // UI 测试扫码使用独立线程；空闲时仅等待事件，不轮询、不消耗 CPU。
    auto *nscanThread = new QThread;
    auto *nscanWorker = new NScanWorker(m_nscanScheduler, &m_nscanScanMutex);
    nscanWorker->moveToThread(nscanThread);
    m_nscanTestThread = nscanThread;
    m_nscanTestWorker = nscanWorker;

    connect(this, &DeviceManager::nscanScanRequested,
            nscanWorker, &NScanWorker::scan, Qt::QueuedConnection);
    connect(nscanWorker, &NScanWorker::finished, this,
            [this](const NScanScheduler::ScanResult &result) {
        const QString summary = rawDataSummary(result.rawData);
        const QString finalLog = QStringLiteral(
            "[N-ScanHub] 扫码结束：状态=%1，尝试=%2，条码字节=%3，原始数据=%4%5")
            .arg(NScanScheduler::statusText(result.status))
            .arg(result.attempts)
            .arg(result.rawData.size())
            .arg(summary.isEmpty() ? QStringLiteral("<empty>") : summary)
            .arg(result.errorMessage.isEmpty()
                     ? QString()
                     : QStringLiteral("，错误=%1").arg(result.errorMessage));
        m_nscanTestRunning = false;
        emit nscanTestLog(finalLog);
        emit logMessage(finalLog);
        emit nscanTestFinished(result);
        emit nscanTestIdle();
    });
    connect(nscanThread, &QThread::finished,
            nscanWorker, &QObject::deleteLater);
    connect(nscanThread, &QThread::finished,
            nscanThread, &QObject::deleteLater);
    nscanThread->start();

    m_agvCtrl = new AgvController(this);
    connect(m_agvCtrl, &AgvController::connected,
            this, &DeviceManager::agvModbusConnected);
    connect(m_agvCtrl, &AgvController::disconnected,
            this, &DeviceManager::agvModbusDisconnected);
    connect(m_agvCtrl, &AgvController::errorOccurred,
            this, &DeviceManager::agvModbusError);
    // 连接成功后控制器自动读 [3x]00043，确认控制权未被外部调度系统抢占
    connect(m_agvCtrl, &AgvController::controlOwnershipRead, this, [this](bool seized) {
        emit logMessage(seized
            ? QStringLiteral("[AGV] 控制权已被外部抢占（[3x]00043=1），Modbus 指令可能无效")
            : QStringLiteral("[AGV] 控制权未被外部抢占（[3x]00043=0）✓"));
    });

    m_visionClient = new VisionHttpClient(this);
    connect(m_visionClient, &VisionHttpClient::selectionLogMessage,
            this, &DeviceManager::logMessage);
    connect(m_visionClient, &VisionHttpClient::statusChanged,
            this, [this](bool ok, const QString &msg) {
        emit logMessage(QString("[视觉] %1").arg(msg));
        emit cameraStatusChanged(ok, msg);
    });

    // 客户系统通信测试只验证 REST API 可达性和 actualQty 读取，暂不进入整线流程。
    m_customSysScheduler = new CustomSysScheduler(this);
    connect(m_customSysScheduler, &CustomSysScheduler::logMessage,
            this, &DeviceManager::logMessage);
    connect(m_customSysScheduler, &CustomSysScheduler::connectivityChecked,
            this, [this](bool ok, const QString &statusText, int) {
        emit customSystemStatusChanged(ok, statusText);
    });
    connect(m_customSysScheduler, &CustomSysScheduler::dayDataReady,
            this, &DeviceManager::customSystemDayDataReady);
    connect(m_customSysScheduler, &CustomSysScheduler::requestStarted,
            this, &DeviceManager::customSystemRequestStarted);
    connect(m_customSysScheduler, &CustomSysScheduler::requestFailed,
            this, &DeviceManager::customSystemRequestFailed);

    m_huayanScheduler = new HuayanScheduler(this);
    // HuayanScheduler 不拥有视觉客户端，只在每次阶段一拍照前注入目标选择上下文。
    m_huayanScheduler->setVisionClient(m_visionClient);

    connect(m_huayanScheduler, &HuayanScheduler::surveyReady,
            m_visionClient,    &VisionHttpClient::fetchInference);
    connect(m_visionClient,
            qOverload<double, double, double, double, double, double>(&VisionHttpClient::rawCoordinatesReady),
            m_huayanScheduler,
            qOverload<double, double, double, double, double, double>(&HuayanScheduler::setGrabOffset));
    connect(m_visionClient, &VisionHttpClient::noObjectDetected,
            m_huayanScheduler, &HuayanScheduler::onVisionNoObject);
    connect(m_visionClient, &VisionHttpClient::targetRejectedByTrustRule,
            m_huayanScheduler, &HuayanScheduler::onVisionTargetRejectedForPickup);
    connect(m_visionClient, &VisionHttpClient::errorOccurred,
            m_huayanScheduler, &HuayanScheduler::onVisionErrorForPickup);

    connect(m_huayanScheduler, &HuayanScheduler::logMessage,
            this, &DeviceManager::logMessage);
    connect(m_huayanScheduler, &HuayanScheduler::stageError,
            this, [this](const QString &msg) {
        emit logMessage(QStringLiteral("[华沿] 错误：%1").arg(msg));
    });
    connect(m_huayanScheduler, &HuayanScheduler::visionAlignmentFailed,
            this, [this](const QString &msg) {
        emit logMessage(QStringLiteral("[华沿][联合视觉对准] 已安全停止：%1").arg(msg));
    });

    m_palletScheduler = new PalletScheduler(this);

    // 主调度另建 worker，避免测试扫码完成信号误推进任务；两个 worker 通过同一
    // mutex 串行进入非线程安全的厂商 SDK。
    auto *lineScanThread = new QThread;
    auto *lineScanWorker = new NScanWorker(m_nscanScheduler, &m_nscanScanMutex);
    lineScanWorker->moveToThread(lineScanThread);
    m_lineScanThread = lineScanThread;
    m_lineScanWorker = lineScanWorker;

    connect(this, &DeviceManager::lineScanRequested,
            lineScanWorker, &NScanWorker::scan, Qt::QueuedConnection);
    connect(lineScanThread, &QThread::finished,
            lineScanWorker, &QObject::deleteLater);
    connect(lineScanThread, &QThread::finished,
            lineScanThread, &QObject::deleteLater);
    lineScanThread->start();

    // 主调度引用上面创建的唯一设备对象，不另建第二套 AGV/机械臂/码垛缓存。
    m_lineManager = new LineManager(m_agvCtrl,
                                    m_huayanScheduler,
                                    m_nscanScheduler.get(),
                                    m_palletScheduler,
                                    this);
    connect(m_lineManager, &LineManager::logMessage,
            this, &DeviceManager::logMessage);
    // 新调度层配置表已保存数字 LM，不再走旧 AGV 调试面板的工位映射。
    connect(m_lineManager, &LineManager::agvDispatchRequested,
            m_agvCtrl, &AgvController::sendToStation);
    // 调度扫码和 UI 测试扫码必须隔离，避免结果串线。
    connect(m_lineManager, &LineManager::scanRequested, this,
            [this](NScanScheduler::ScanOptions options) {
        options.ip = m_cfg.scannerIP.trimmed();
        m_lineScanOptions = options;

        if (!m_lineScanThread || !m_lineScanThread->isRunning()
            || !m_lineScanWorker) {
            NScanScheduler::ScanResult result;
            result.status = NScanScheduler::ScanResult::Status::SdkUnavailable;
            result.errorMessage = QStringLiteral("调度扫码工作线程不可用");
            emit logMessage(QStringLiteral("[LineManager] %1").arg(result.errorMessage));
            if (m_lineManager) {
                m_lineManager->onScanFinished(result);
            }
            return;
        }

        emit lineScanRequested(options);
    });
    // 扫码结果排队回到 UI 主线程，再由 LineManager 推进任务状态机。
    connect(lineScanWorker, &NScanWorker::finished,
            m_lineManager, &LineManager::onScanFinished, Qt::QueuedConnection);

    // 充电参数与运行参数使用同一 AppConfigLocation，自动授权开关刻意不在
    // ChargeSettings 中，因此每次进程启动都保持关闭。
    m_chargeSettingsPath =
        QDir(settingsDir).filePath(QStringLiteral("charge-settings.ini"));
    const ChargeSettingsLoadResult chargeLoaded =
        loadChargeSettings(m_chargeSettingsPath);
    m_chargeSettings = chargeLoaded.settings;
    for (const QString &warning : chargeLoaded.warnings)
        qWarning().noquote() << QStringLiteral("[充电参数] %1").arg(warning);

    // 所有充电入口共享这一组对象：控制器唯一持有套接字，协调器只产生意图。
    // 创建顺序固定为 AgvController -> LineManager -> ChargePileController ->
    // AutoChargeCoordinator，避免协调器在依赖尚未建立时收到初始状态。
    m_chargePileController = new ChargePileController(this);
    m_chargePileController->applySettings(m_chargeSettings);
    m_autoChargeCoordinator = new AutoChargeCoordinator(this);
    m_autoChargeCoordinator->applySettings(m_chargeSettings);

    // 充电期间仅每 30 秒只读复核一次 DO0，避免对仙工控制器造成高频轮询。
    // 定时器只在充电桩已经接受会话后启动；置高确认阶段绝不会并发查询。
    m_chargeDo0MonitorTimer = new QTimer(this);
    m_chargeDo0MonitorTimer->setInterval(30000);
    connect(m_chargeDo0MonitorTimer, &QTimer::timeout, this, [this] {
        if (m_chargeDo0Phase != ChargeDo0Phase::Active
            || m_do0SafeStopRequested) {
            return;
        }
        QString error;
        if (!m_agvCtrl->queryDo0(&error)) {
            // “已有 DO0 操作”表示上一轮尚未结束，不计作一次现场读失败；
            // 断线等明确无法读的错误才进入连续失败计数。
            if (m_agvCtrl->isConnected()) {
                emit logMessage(
                    QStringLiteral("[充电DO0] 本轮监控查询未启动：%1").arg(error));
                return;
            }
            ++m_do0MonitorReadFailures;
            emit logMessage(
                QStringLiteral("[充电DO0] 监控读取失败（%1/3）：%2")
                    .arg(m_do0MonitorReadFailures)
                    .arg(error));
            if (m_do0MonitorReadFailures >= 3) {
                requestDo0SafetyStop(
                    QStringLiteral("DO0 连续三次无法读取，不能确认车辆充电允许信号"));
            }
        }
    });

    m_agvMonitorFreshnessTimer = new QTimer(this);
    m_agvMonitorFreshnessTimer->setSingleShot(true);
    m_agvMonitorFreshnessTimer->setInterval(
        kAgvMonitorFreshnessTimeoutMs);
    connect(m_agvMonitorFreshnessTimer, &QTimer::timeout, this, [this] {
        const QString reason =
            QStringLiteral("AGV 完整监控快照超过 %1 ms 未更新")
                .arg(kAgvMonitorFreshnessTimeoutMs);
        if (notifyAgvMonitorLostIfFresh(
                m_agvMonitorFreshness, *m_autoChargeCoordinator, reason)) {
            emit logMessage(QStringLiteral("[自动充电] %1").arg(reason));
        }
    });
    connect(m_agvCtrl, &AgvController::disconnected, this, [this] {
        m_agvMonitorFreshnessTimer->stop();
        const QString reason = QStringLiteral("AGV Modbus 已断开");
        if (notifyAgvMonitorLostIfFresh(
                m_agvMonitorFreshness, *m_autoChargeCoordinator, reason)) {
            emit logMessage(QStringLiteral("[自动充电] %1").arg(reason));
        }
        if (m_chargeDo0Phase == ChargeDo0Phase::Opening)
            cancelPendingChargeStart(reason);
        if (m_chargeDo0Phase == ChargeDo0Phase::Active)
            requestDo0SafetyStop(QStringLiteral("AGV Modbus 断开，无法继续确认 DO0"));
    });
    connect(m_agvCtrl, &AgvController::connected,
            this, &DeviceManager::beginStartupDo0Check);
    connect(m_agvCtrl, &AgvController::do0EnsureFinished,
            this, &DeviceManager::handleDo0EnsureFinished);
    connect(m_agvCtrl, &AgvController::do0StateRead,
            this, &DeviceManager::handleDo0StateRead);

    connect(m_agvCtrl, &AgvController::monitorUpdated, this,
            [this](const AgvMonitorData &data) {
        // monitorUpdated 保证六个字段属于同一轮读取；业务层只保存这种完整快照。
        m_lastAgvMonitor = data;
        m_agvMonitorFreshness.markUpdated();
        m_agvMonitorFreshnessTimer->start();
        m_autoChargeCoordinator->onAgvMonitorUpdated(data);
    });
    connect(m_lineManager, &LineManager::systemStateChanged, this,
            [this](LineSystemState state, const QString &text) {
        m_autoChargeCoordinator->onLineStateChanged(state, text);
        if (state == LineSystemState::Error) {
            cancelChargePreflight(
                QStringLiteral("主调度已停止或进入错误状态"));
        } else if (m_chargePreflightIntent
                       == ChargePreflightIntent::ManualStart
                   && state != LineSystemState::Idle) {
            cancelChargePreflight(
                QStringLiteral("主调度状态已变化，取消手动充电预检"));
        }
        if (state == LineSystemState::Error
            && m_chargeDo0Phase == ChargeDo0Phase::Opening) {
            cancelPendingChargeStart(
                QStringLiteral("主调度进入错误状态，取消待启动充电"));
        }
    });
    connect(m_lineManager, &LineManager::queueChanged,
            m_autoChargeCoordinator, &AutoChargeCoordinator::onQueueChanged);
    connect(m_chargePileController, &ChargePileController::stateChanged, this,
            [this](ChargePileController::State state, const QString &text) {
        m_chargePileState = state;
        m_autoChargeCoordinator->onChargeControllerStateChanged(state, text);
    });
    connect(m_chargePileController,
            &ChargePileController::chargeSessionFinished,
            this,
            [this](const bool safe,
                   ChargePileController::SessionOrigin origin,
                   const QString &message) {
        m_chargeDo0MonitorTimer->stop();
        m_do0SafeStopRequested = false;

        // 只有充电桩已经明确完成停止、缩回、复位和终检后才关闭 DO0。
        // 不安全结果必须保留高电平，等待既有保守恢复流程给出后续安全结果。
        if (safe) {
            beginDo0CloseBestEffort(
                QStringLiteral("充电桩已安全收尾"));
        } else {
            m_chargeDo0Phase = ChargeDo0Phase::Active;
            emit logMessage(
                QStringLiteral("[充电DO0] 充电桩尚未确认安全，暂不关闭 DO0"));
        }
        m_autoChargeCoordinator->onChargeSessionFinished(
            safe, origin, message);
    });
    connect(m_chargePileController, &ChargePileController::queryFinished,
            this, [this](const bool ok, const QString &message) {
        if (!m_startupDo0PileQueryPending) {
            if (m_chargePreflightIntent != ChargePreflightIntent::None)
                handleChargePreflightFinished(ok, message);
            return;
        }
        m_startupDo0PileQueryPending = false;
        if (!ok) {
            m_chargeDo0Phase = ChargeDo0Phase::Idle;
            emit logMessage(
                QStringLiteral("[充电DO0] 启动恢复无法确认充电桩状态，保留 DO0 高电平：%1")
                    .arg(message));
            return;
        }

        const ChargePileSnapshot snapshot =
            m_chargePileController->snapshot();
        const bool clearlySafe =
            !snapshot.working
            && !snapshot.relayOn
            && snapshot.outputCurrentA <= m_chargeSettings.safeCurrentA;
        if (!clearlySafe) {
            m_chargeDo0Phase = ChargeDo0Phase::Idle;
            emit logMessage(
                QStringLiteral("[充电DO0] 启动恢复发现充电桩可能仍在工作，保留 DO0 高电平"));
            return;
        }
        beginDo0CloseBestEffort(
            QStringLiteral("启动恢复确认充电桩无输出"));
    }, Qt::QueuedConnection);
    // MainWindow 只订阅 DeviceManager 的业务边界信号；控制器仍由本对象唯一
    // 拥有，关闭流程不会因此出现第二个套接字或第二套安全状态机。
    connect(m_chargePileController,
            &ChargePileController::applicationShutdownFinished,
            this,
            [this](const bool safe, const QString &message) {
        if (!safe) {
            // 充电桩未安全时绝不为了退出程序关闭 DO0；沿用既有失败门禁，
            // 让操作员查询或重试保守恢复。
            m_chargeApplicationShutdownGate.finishRequest(false);
            emit applicationShutdownFinished(false, message);
            return;
        }

        // DO0 关闭失败只记录，不改变充电桩已经给出的安全结论；若关闭操作
        // 已由 chargeSessionFinished 启动，则只等待这一次轻量尝试结束。
        m_applicationShutdownWaitingForDo0 = true;
        m_pendingApplicationShutdownMessage = message;
        if (m_chargeDo0Phase != ChargeDo0Phase::Closing)
            beginDo0CloseBestEffort(QStringLiteral("应用关闭前"));
    });

    connect(m_autoChargeCoordinator, &AutoChargeCoordinator::dispatchHoldRequested,
            m_lineManager, &LineManager::setChargeDispatchHold);
    connect(m_lineManager, &LineManager::chargeDispatchHoldChanged,
            m_autoChargeCoordinator,
            &AutoChargeCoordinator::onDispatchHoldChanged,
            Qt::QueuedConnection);
    connect(m_autoChargeCoordinator, &AutoChargeCoordinator::returnHomeRequested,
            m_lineManager, &LineManager::requestChargeReturnHome);
    connect(m_autoChargeCoordinator,
            &AutoChargeCoordinator::automaticChargeStartRequested,
            this, [this] {
        if (m_chargeApplicationShutdownGate.blocksNewActions()) {
            m_autoChargeCoordinator->onAutomaticChargeStartResult(
                false, QStringLiteral("程序正在执行充电安全关闭"));
            return;
        }
        const QString continuationRejection =
            automaticStartContinuationRejection();
        if (!continuationRejection.isEmpty()) {
            m_autoChargeCoordinator->onAutomaticChargeStartResult(
                false, continuationRejection);
            return;
        }
        QString error;
        if (!beginChargePreflight(
                ChargePreflightIntent::AutomaticStart, &error)) {
            m_autoChargeCoordinator->onAutomaticChargeStartResult(false, error);
            m_lineManager->raiseExternalSystemError(
                QStringLiteral("自动充电安全预检未启动：%1").arg(error));
        }
    });
    connect(m_autoChargeCoordinator,
            &AutoChargeCoordinator::automaticChargeSafeStopRequested,
            this, [this](ChargePileController::StopReason reason) {
        // 只允许协调器自己拥有的 Automatic 会话触发此通道；手动会话绝不能
        // 因自动策略的一次迟到输入被错误停止。
        if (m_autoChargeCoordinator->automaticSessionActive())
            m_chargePileController->requestSafeStop(reason);
    });
    connect(m_autoChargeCoordinator,
            &AutoChargeCoordinator::automaticChargeConservativeRecoveryRequested,
            this,
            [this](ChargePileController::SessionOrigin origin,
                   ChargePileController::StopReason reason) {
        if (!m_autoChargeCoordinator->automaticSessionActive())
            return;
        QString error;
        const bool accepted =
            m_chargePileController->requestConservativeRecovery(origin, reason, &error);
        emit logMessage(
            accepted
                ? QStringLiteral("[自动充电] 控制器已接受保守安全恢复")
                : QStringLiteral("[自动充电] 保守安全恢复被拒绝：%1").arg(error));
    });
    connect(m_autoChargeCoordinator, &AutoChargeCoordinator::lineErrorRequested,
            m_lineManager, &LineManager::raiseExternalSystemError);

    // 充电子系统日志统一从 DeviceManager 出口进入现有主窗口日志；协调器的
    // criticalBatteryAlarm 同时会发布详细 logMessage，不在这里重复打印。
    connect(m_chargePileController, &ChargePileController::logMessage,
            this, &DeviceManager::logMessage);
    connect(m_autoChargeCoordinator, &AutoChargeCoordinator::logMessage,
            this, &DeviceManager::logMessage);

    // LineManager 构造期的初始信号早于上述连接，显式补齐同一份初始快照。
    m_autoChargeCoordinator->onLineStateChanged(
        m_lineManager->state(), QStringLiteral("初始化"));
    m_autoChargeCoordinator->onQueueChanged(m_lineManager->queueSnapshot());
    m_autoChargeCoordinator->onChargeControllerStateChanged(
        m_chargePileState, QStringLiteral("初始化"));

    m_lineOrch = new LineOrchestrator(m_agvCtrl, m_huayanScheduler, this);
    // 编排器请求派单 → 经映射表解析后下发（复用 dispatchAgv）
    connect(m_lineOrch, &LineOrchestrator::agvDispatchRequested,
            this, &DeviceManager::dispatchAgv);
    // 注入工位→站点解析器：到达判定须与 AGV 监控回报的物理站点号同空间比较
    m_lineOrch->setStationResolver([this](int ws) { return resolveStation(ws); });
    // 新旧两个顶层流程共用 AGV/机械臂；这里仅注入只读互斥判定，不转移对象所有权。
    m_lineManager->setExternalWorkflowRunning([this]() {
        return m_lineOrch && m_lineOrch->isRunning();
    });
    // 新 LineManager 在 Running/ReturningHome/Error 或保留当前任务时，都视为占用整线资源。
    m_lineOrch->setExternalWorkflowRunning([this]() {
        return m_lineManager
            && (m_lineManager->state() != LineSystemState::Idle
                || m_lineManager->currentTask().taskId != 0);
    });
    connect(m_lineOrch, &LineOrchestrator::lineLog,
            this, &DeviceManager::logMessage);
    connect(m_lineOrch, &LineOrchestrator::lineError, this, [this](const QString &msg) {
        emit logMessage(QStringLiteral("[整线错误] %1").arg(msg));
    });

    const SettingsLoadResult loaded = m_settingsManager->load();
    m_huayanScheduler->applyRuntimeSettings(loaded.settings);
    m_lineManager->applyRuntimeSettings(loaded.settings);
    for (const QString &warning : loaded.warnings)
        qWarning().noquote() << QStringLiteral("[运行参数] %1").arg(warning);

    loadStationMap();
}

const RuntimeSettings &DeviceManager::runtimeSettings() const
{
    return m_settingsManager->current();
}

bool DeviceManager::runtimeSettingsLocked() const
{
    return (m_huayanScheduler && m_huayanScheduler->isBusy())
        || (m_lineManager
            && (m_lineManager->state() != LineSystemState::Idle
                || m_lineManager->currentTask().taskId != 0))
        || (m_lineOrch && m_lineOrch->isRunning());
}

bool DeviceManager::applyRuntimeSettingsCandidate(
    const RuntimeSettings &candidate, QString *error)
{
    const SettingsValidation validation = validateRuntimeSettings(candidate);
    if (!validation.ok) {
        if (error)
            *error = validation.errors.join(QStringLiteral("；"));
        return false;
    }
    if (runtimeSettingsLocked()) {
        if (error)
            *error = QStringLiteral("任务运行中，不能修改运行参数");
        return false;
    }
    if (!m_settingsManager->stageCandidate(candidate, error))
        return false;

    const RuntimeSettings previous = m_settingsManager->current();
    m_huayanScheduler->applyRuntimeSettings(candidate);
    m_lineManager->applyRuntimeSettings(candidate);
    if (!m_settingsManager->commitStaged(candidate, error)) {
        m_huayanScheduler->applyRuntimeSettings(previous);
        m_lineManager->applyRuntimeSettings(previous);
        m_settingsManager->discardStaged();
        return false;
    }

    emit logMessage(QStringLiteral("[运行参数] 已保存并应用"));
    return true;
}

bool DeviceManager::applyChargeSettingsCandidate(
    const ChargeSettings &candidate, QString *error)
{
    if (error)
        error->clear();
    if (m_chargeApplicationShutdownGate.blocksNewActions()) {
        if (error)
            *error = QStringLiteral("程序正在执行充电安全关闭，不能修改充电参数");
        return false;
    }

    const ChargeSettingsValidation validation = validateChargeSettings(candidate);
    if (!validation.ok) {
        if (error)
            *error = validation.errors.join(QStringLiteral("；"));
        return false;
    }
    if (!m_chargePileController || !m_autoChargeCoordinator) {
        if (error)
            *error = QStringLiteral("充电业务对象尚未初始化");
        return false;
    }
    if (m_chargePileController->isBusy()
        || m_autoChargeCoordinator->automaticSessionActive()) {
        if (error)
            *error = QStringLiteral("充电查询、会话或安全收尾正在执行，不能修改参数");
        return false;
    }
    if (m_chargePileState == ChargePileController::State::Unknown
        || m_chargePileState == ChargePileController::State::Fault) {
        if (error)
            *error = QStringLiteral("充电桩状态未知或存在故障，必须先完成安全恢复");
        return false;
    }

    // 事务函数先让控制器无副作用确认可接受，再写盘和提交三份运行快照；
    // 任一步失败都保持协调器和 DeviceManager 当前快照不变。
    const ChargeSettingsTransactionTargets targets{
        m_chargeSettingsPath,
        m_chargePileController,
        m_autoChargeCoordinator,
        &m_chargeSettings};
    if (!applyChargeSettingsTransaction(targets, candidate, error))
        return false;

    emit logMessage(QStringLiteral("[充电参数] 已原子保存并应用"));
    return true;
}

bool DeviceManager::requestChargeStartWithDo0(
    const ChargePileController::SessionOrigin origin, QString *error)
{
    if (error)
        error->clear();
    if (m_chargeDo0Phase != ChargeDo0Phase::Idle) {
        if (error)
            *error = QStringLiteral("DO0 启动、监控或关闭操作正在执行");
        return false;
    }

    m_chargeDo0Phase = ChargeDo0Phase::Opening;
    m_pendingChargeOrigin = origin;
    m_pendingChargeStartCanceled = false;
    QString do0Error;
    if (!m_agvCtrl->ensureDo0(true, &do0Error)) {
        m_chargeDo0Phase = ChargeDo0Phase::Idle;
        m_pendingChargeOrigin.reset();
        if (error)
            *error = QStringLiteral("DO0 置高请求未启动：%1").arg(do0Error);
        return false;
    }

    emit logMessage(
        origin == ChargePileController::SessionOrigin::Automatic
            ? QStringLiteral("[自动充电] 正在确认车辆 DO0 高电平")
            : QStringLiteral("[手动充电] 正在确认车辆 DO0 高电平"));
    return true;
}

QString DeviceManager::manualChargePreflightRejection() const
{
    if (!m_lineManager || !m_chargePileController || !m_autoChargeCoordinator)
        return QStringLiteral("充电业务对象尚未初始化");

    ManualChargeStartContext context;
    context.lineState = m_lineManager->state();
    context.hasAgvMonitor = m_agvMonitorFreshness.hasFreshSnapshot();
    context.agv = m_lastAgvMonitor;
    context.controllerBusy = m_chargePileController->isBusy();
    context.controllerState = m_chargePileState;
    context.controllerShutdownRequired =
        m_chargePileController->shutdownRequired();
    context.automaticEnabled = m_autoChargeCoordinator->isEnabled();
    context.automaticSessionActive =
        m_autoChargeCoordinator->automaticSessionActive();
    return manualChargePreflightRejectionReason(context);
}

QString DeviceManager::automaticEnablePreflightRejection() const
{
    if (!m_chargePileController || !m_autoChargeCoordinator)
        return QStringLiteral("自动充电业务对象尚未初始化");

    AutomaticChargeEnableContext context;
    context.controllerBusy = m_chargePileController->isBusy();
    context.controllerState = m_chargePileState;
    context.automaticSessionActive =
        m_autoChargeCoordinator->automaticSessionActive();
    context.controllerShutdownRequired =
        m_chargePileController->shutdownRequired();
    return automaticChargeEnablePreflightRejectionReason(context);
}

QString DeviceManager::automaticStartContinuationRejection() const
{
    if (!m_lineManager || !m_chargePileController || !m_autoChargeCoordinator)
        return QStringLiteral("自动充电业务对象尚未初始化");
    if (!m_autoChargeCoordinator->isEnabled())
        return QStringLiteral("自动充电授权已关闭");
    if (m_chargeApplicationShutdownGate.blocksNewActions())
        return QStringLiteral("程序正在执行充电安全关闭");
    if (m_lineManager->state() == LineSystemState::Idle
        || m_lineManager->state() == LineSystemState::Error) {
        return QStringLiteral("主调度当前不允许自动充电");
    }
    if (!m_agvMonitorFreshness.hasFreshSnapshot()
        || m_lastAgvMonitor.curStation != 1) {
        return QStringLiteral("AGV 未以新鲜状态确认位于 LM1");
    }
    const quint16 navStatus = m_lastAgvMonitor.navStatus;
    if (navStatus != static_cast<quint16>(AgvController::NavStatus::None)
        && navStatus
               != static_cast<quint16>(AgvController::NavStatus::Arrived)) {
        return QStringLiteral("AGV 导航状态已不允许自动开始充电");
    }
    if (m_autoChargeCoordinator->automaticSessionActive())
        return QStringLiteral("自动充电会话已经启动");
    return {};
}

bool DeviceManager::beginChargePreflight(
    const ChargePreflightIntent intent, QString *error)
{
    if (error)
        error->clear();
    if (!m_chargePileController) {
        if (error)
            *error = QStringLiteral("充电控制器尚未初始化");
        return false;
    }
    if (intent == ChargePreflightIntent::None) {
        if (error)
            *error = QStringLiteral("充电预检意图无效");
        return false;
    }
    if (m_chargePreflightIntent != ChargePreflightIntent::None
        || m_chargePileController->isBusy()) {
        if (error)
            *error = QStringLiteral("充电控制器正在执行其他操作");
        return false;
    }
    if (m_chargeDo0Phase != ChargeDo0Phase::Idle
        || m_startupDo0PileQueryPending) {
        if (error)
            *error = QStringLiteral("DO0 启动恢复或充电时序正在执行");
        return false;
    }
    if (m_chargeApplicationShutdownGate.blocksNewActions()) {
        if (error)
            *error = QStringLiteral("程序正在执行充电安全关闭");
        return false;
    }

    // 必须先锁存来源再发起查询，因为控制器可在参数或连接错误时同步发布
    // queryFinished；结果处理器由此仍能准确消费本次预检且不会误入普通查询。
    m_chargePreflightIntent = intent;
    m_chargePileController->queryStatus();
    emit logMessage(QStringLiteral("[充电预检] 已发起充电桩实时只读安全查询"));
    return true;
}

void DeviceManager::cancelChargePreflight(const QString &reason)
{
    if (m_chargePreflightIntent == ChargePreflightIntent::None)
        return;

    const ChargePreflightIntent canceledIntent = m_chargePreflightIntent;
    m_chargePreflightIntent = ChargePreflightIntent::None;
    emit logMessage(QStringLiteral("[充电预检] 已取消：%1").arg(reason));

    // 协调器已经锁存 startIntentPending，自动开始被取消时必须且只需回执一次，
    // 否则后续人工复位后协调器仍会误认为旧启动请求在途。
    if (canceledIntent == ChargePreflightIntent::AutomaticStart) {
        m_autoChargeCoordinator->onAutomaticChargeStartResult(false, reason);
    }
}

void DeviceManager::handleChargePreflightFinished(
    const bool queryOk, const QString &message)
{
    // 先清除在途意图再继续，避免后续 setEnabled、DO0 或 Error 信号同步重入时
    // 把已完成查询再次当作有效预检。
    const ChargePreflightIntent intent = m_chargePreflightIntent;
    m_chargePreflightIntent = ChargePreflightIntent::None;
    if (intent == ChargePreflightIntent::None)
        return;

    QString continuationRejection;
    switch (intent) {
    case ChargePreflightIntent::ManualStart: {
        ManualChargeStartContext context;
        context.lineState = m_lineManager->state();
        context.hasAgvMonitor = m_agvMonitorFreshness.hasFreshSnapshot();
        context.agv = m_lastAgvMonitor;
        context.controllerBusy = m_chargePileController->isBusy();
        context.controllerState = m_chargePileState;
        context.controllerShutdownRequired =
            m_chargePileController->shutdownRequired();
        context.automaticEnabled = m_autoChargeCoordinator->isEnabled();
        context.automaticSessionActive =
            m_autoChargeCoordinator->automaticSessionActive();
        continuationRejection = manualChargeStartRejectionReason(context);
        break;
    }
    case ChargePreflightIntent::EnableAutomatic: {
        AutomaticChargeEnableContext context;
        context.controllerBusy = m_chargePileController->isBusy();
        context.controllerState = m_chargePileState;
        context.automaticSessionActive =
            m_autoChargeCoordinator->automaticSessionActive();
        context.controllerShutdownRequired =
            m_chargePileController->shutdownRequired();
        continuationRejection =
            automaticChargeEnableRejectionReason(context);
        break;
    }
    case ChargePreflightIntent::AutomaticStart:
        continuationRejection = automaticStartContinuationRejection();
        break;
    case ChargePreflightIntent::None:
        return;
    }

    const ChargePreflightOutcome outcome = classifyChargePreflightOutcome(
        queryOk,
        m_chargePileState,
        m_chargePileController->shutdownRequired(),
        continuationRejection.isEmpty());
    if (outcome == ChargePreflightOutcome::Canceled) {
        emit logMessage(
            QStringLiteral("[充电预检] 查询期间条件变化，已取消：%1")
                .arg(continuationRejection));
        if (intent == ChargePreflightIntent::ManualStart) {
            emit manualChargePreflightFinished(
                false, continuationRejection);
        } else if (intent == ChargePreflightIntent::EnableAutomatic) {
            emit automaticChargeEnablePreflightFinished(
                false, continuationRejection);
        } else {
            m_autoChargeCoordinator->onAutomaticChargeStartResult(
                false, continuationRejection);
        }
        return;
    }

    if (outcome == ChargePreflightOutcome::DeviceSafetyFailure) {
        const QString failure =
            QStringLiteral("充电桩实时安全预检失败：%1").arg(message);
        emit logMessage(QStringLiteral("[充电预检] %1").arg(failure));
        if (intent == ChargePreflightIntent::ManualStart) {
            emit manualChargePreflightFinished(false, failure);
        } else if (intent == ChargePreflightIntent::EnableAutomatic) {
            emit automaticChargeEnablePreflightFinished(false, failure);
        } else {
            m_autoChargeCoordinator->onAutomaticChargeStartResult(
                false, failure);
            m_lineManager->raiseExternalSystemError(failure);
        }
        return;
    }

    if (intent == ChargePreflightIntent::EnableAutomatic) {
        m_autoChargeCoordinator->setEnabled(true);
        emit automaticChargeEnablePreflightFinished(
            true, QStringLiteral("充电桩安全预检通过，自动充电授权已开启"));
        return;
    }

    QString startError;
    const ChargePileController::SessionOrigin origin =
        intent == ChargePreflightIntent::ManualStart
            ? ChargePileController::SessionOrigin::Manual
            : ChargePileController::SessionOrigin::Automatic;
    if (!requestChargeStartWithDo0(origin, &startError)) {
        if (intent == ChargePreflightIntent::ManualStart) {
            emit manualChargePreflightFinished(false, startError);
        } else {
            m_autoChargeCoordinator->onAutomaticChargeStartResult(
                false, startError);
            m_lineManager->raiseExternalSystemError(
                QStringLiteral("自动充电启动失败：%1").arg(startError));
        }
        return;
    }

    if (intent == ChargePreflightIntent::ManualStart) {
        emit manualChargePreflightFinished(
            true, QStringLiteral("安全预检通过，正在确认车辆 DO0 高电平"));
    }
}

void DeviceManager::cancelPendingChargeStart(const QString &reason)
{
    if (m_chargeDo0Phase != ChargeDo0Phase::Opening
        || !m_pendingChargeOrigin.has_value()) {
        return;
    }
    m_pendingChargeStartCanceled = true;
    emit logMessage(
        QStringLiteral("[充电DO0] 已取消待启动会话：%1").arg(reason));
}

void DeviceManager::handleDo0EnsureFinished(
    const bool targetHigh,
    const bool confirmed,
    const bool actualHigh,
    const QString &message)
{
    Q_UNUSED(actualHigh)

    if (m_chargeDo0Phase == ChargeDo0Phase::Closing) {
        if (!confirmed || targetHigh) {
            // DO0 是车辆侧普通 IO；关闭失败不推翻充电桩已经确认的安全结果，
            // 也不阻断主调度或应用退出，只保留一条现场可追溯日志。
            emit logMessage(
                QStringLiteral("[充电DO0] 关闭未确认，仅记录：%1").arg(message));
        } else {
            emit logMessage(QStringLiteral("[充电DO0] 已确认关闭：%1").arg(message));
        }
        m_chargeDo0Phase = ChargeDo0Phase::Idle;
        finishPendingApplicationShutdownAfterDo0();
        return;
    }

    if (m_chargeDo0Phase != ChargeDo0Phase::Opening
        || !m_pendingChargeOrigin.has_value()) {
        emit logMessage(
            QStringLiteral("[充电DO0] 收到非当前阶段的确认结果，已忽略：%1")
                .arg(message));
        return;
    }

    const ChargePileController::SessionOrigin origin =
        *m_pendingChargeOrigin;
    const bool canceled = m_pendingChargeStartCanceled;
    m_pendingChargeOrigin.reset();
    m_pendingChargeStartCanceled = false;

    if (!targetHigh || !confirmed || canceled) {
        const QString failure =
            canceled
                ? QStringLiteral("启动条件在 DO0 确认期间失效，已取消充电")
                : QStringLiteral("DO0 高电平未确认：%1").arg(message);
        if (origin == ChargePileController::SessionOrigin::Automatic) {
            m_autoChargeCoordinator->onAutomaticChargeStartResult(
                false, failure);
        } else {
            emit logMessage(
                QStringLiteral("[手动充电] 启动失败：%1").arg(failure));
        }
        beginDo0CloseBestEffort(QStringLiteral("启动失败或取消"));
        return;
    }

    QString controllerError;
    const bool accepted = m_chargePileController->startCharge(
        origin, &controllerError);
    if (origin == ChargePileController::SessionOrigin::Automatic) {
        m_autoChargeCoordinator->onAutomaticChargeStartResult(
            accepted,
            accepted ? QStringLiteral("DO0 已确认，控制器已接受自动充电会话")
                     : controllerError);
    }

    if (!accepted) {
        if (origin == ChargePileController::SessionOrigin::Manual) {
            emit logMessage(
                QStringLiteral("[手动充电] 充电桩拒绝启动：%1")
                    .arg(controllerError));
        }
        beginDo0CloseBestEffort(QStringLiteral("充电桩拒绝启动"));
        return;
    }

    m_chargeDo0Phase = ChargeDo0Phase::Active;
    m_do0MonitorReadFailures = 0;
    m_do0SafeStopRequested = false;
    m_chargeDo0MonitorTimer->start();
    emit logMessage(
        origin == ChargePileController::SessionOrigin::Automatic
            ? QStringLiteral("[自动充电] DO0 已确认，充电桩启动流程已开始")
            : QStringLiteral("[手动充电] DO0 已确认，充电桩启动流程已开始"));
}

void DeviceManager::handleDo0StateRead(
    const bool ok, const bool high, const QString &message)
{
    if (m_chargeDo0Phase == ChargeDo0Phase::StartupChecking) {
        if (!ok) {
            m_chargeDo0Phase = ChargeDo0Phase::Idle;
            emit logMessage(
                QStringLiteral("[充电DO0] 启动恢复读取失败，保持现状：%1")
                    .arg(message));
            return;
        }
        if (!high) {
            m_chargeDo0Phase = ChargeDo0Phase::Idle;
            emit logMessage(QStringLiteral("[充电DO0] 启动恢复确认 DO0 已关闭"));
            return;
        }

        // 遗留高电平不能直接关闭：先只读查询一次充电桩，只有工作位、
        // 继电器和输出电流都明确安全时才做一次关闭尝试。
        m_startupDo0PileQueryPending = true;
        m_chargePileController->queryStatus();
        return;
    }

    if (m_chargeDo0Phase != ChargeDo0Phase::Active
        || m_do0SafeStopRequested) {
        return;
    }
    if (ok && high) {
        m_do0MonitorReadFailures = 0;
        return;
    }
    if (ok && !high) {
        requestDo0SafetyStop(
            QStringLiteral("充电期间读到 DO0 已变为低电平"));
        return;
    }

    ++m_do0MonitorReadFailures;
    emit logMessage(
        QStringLiteral("[充电DO0] 监控读取失败（%1/3）：%2")
            .arg(m_do0MonitorReadFailures)
            .arg(message));
    if (m_do0MonitorReadFailures >= 3) {
        requestDo0SafetyStop(
            QStringLiteral("DO0 连续三次读取失败，不能确认车辆充电允许信号"));
    }
}

void DeviceManager::beginDo0CloseBestEffort(const QString &context)
{
    m_chargeDo0MonitorTimer->stop();
    m_chargeDo0Phase = ChargeDo0Phase::Closing;
    QString error;
    if (m_agvCtrl->ensureDo0(false, &error))
        return;

    emit logMessage(
        QStringLiteral("[充电DO0] %1后关闭请求未启动，仅记录：%2")
            .arg(context, error));
    m_chargeDo0Phase = ChargeDo0Phase::Idle;
    finishPendingApplicationShutdownAfterDo0();
}

void DeviceManager::requestDo0SafetyStop(const QString &reason)
{
    if (m_do0SafeStopRequested)
        return;
    m_do0SafeStopRequested = true;
    m_chargeDo0MonitorTimer->stop();
    emit logMessage(QStringLiteral("[充电DO0] %1，开始充电桩安全收尾").arg(reason));
    if (m_chargePileController->hasActiveChargeSession()) {
        m_chargePileController->requestSafeStop(
            ChargePileController::StopReason::Fault);
    }
}

void DeviceManager::beginStartupDo0Check()
{
    if (!m_chargePileController
        || m_chargeDo0Phase != ChargeDo0Phase::Idle
        || m_pendingChargeOrigin.has_value()
        || m_chargePileController->hasActiveChargeSession()
        || m_chargePileController->isBusy()) {
        return;
    }

    m_chargeDo0Phase = ChargeDo0Phase::StartupChecking;
    QString error;
    if (!m_agvCtrl->queryDo0(&error)) {
        m_chargeDo0Phase = ChargeDo0Phase::Idle;
        emit logMessage(
            QStringLiteral("[充电DO0] 启动恢复查询未启动，保持现状：%1")
                .arg(error));
    }
}

void DeviceManager::finishPendingApplicationShutdownAfterDo0()
{
    if (!m_applicationShutdownWaitingForDo0)
        return;
    m_applicationShutdownWaitingForDo0 = false;
    const QString message = m_pendingApplicationShutdownMessage;
    m_pendingApplicationShutdownMessage.clear();
    m_chargeApplicationShutdownGate.finishRequest(true);
    emit applicationShutdownFinished(true, message);
}

bool DeviceManager::startManualCharge(QString *error)
{
    if (error)
        error->clear();
    const auto reject = [this, error](const QString &reason) {
        if (error)
            *error = reason;
        emit logMessage(QStringLiteral("[手动充电] 拒绝启动：%1").arg(reason));
        return false;
    };

    if (!m_lineManager || !m_chargePileController || !m_autoChargeCoordinator)
        return reject(QStringLiteral("充电业务对象尚未初始化"));
    if (m_chargeApplicationShutdownGate.blocksNewActions())
        return reject(QStringLiteral("程序正在执行充电安全关闭，不能开始新会话"));

    const QString rejection = manualChargePreflightRejection();
    if (!rejection.isEmpty())
        return reject(rejection);

    QString preflightError;
    if (!beginChargePreflight(
            ChargePreflightIntent::ManualStart, &preflightError)) {
        return reject(preflightError);
    }

    emit logMessage(
        QStringLiteral("[手动充电] 正在查询充电桩实时状态，预检通过后自动继续"));
    return true;
}

bool DeviceManager::queryChargePileStatus(QString *error)
{
    if (error)
        error->clear();
    if (!m_chargePileController) {
        if (error)
            *error = QStringLiteral("充电控制器尚未初始化");
        return false;
    }
    if (m_chargeApplicationShutdownGate.blocksNewActions()) {
        if (error)
            *error = QStringLiteral("程序正在执行充电安全关闭，不能开始状态查询");
        return false;
    }
    if (m_chargePileController->isBusy()) {
        if (error)
            *error = QStringLiteral("充电控制器正在执行其他操作");
        return false;
    }

    m_chargePileController->queryStatus();
    return true;
}

bool DeviceManager::stopChargePile(QString *error)
{
    if (error)
        error->clear();
    if (!m_chargePileController) {
        if (error)
            *error = QStringLiteral("充电控制器尚未初始化");
        return false;
    }
    if (m_chargePreflightIntent != ChargePreflightIntent::None) {
        cancelChargePreflight(QStringLiteral("操作员请求停止充电"));
        return true;
    }

    if (m_chargeDo0Phase == ChargeDo0Phase::Opening) {
        cancelPendingChargeStart(QStringLiteral("操作员请求停止充电"));
        return true;
    }

    if (m_chargePileController->hasActiveChargeSession()) {
        m_chargePileController->requestSafeStop(
            ChargePileController::StopReason::Manual);
        return true;
    }

    if (m_chargePileController->isBusy()) {
        if (error)
            *error = QStringLiteral("只读查询或其他非充电操作正在执行，不能启动安全恢复");
        return false;
    }

    if (m_chargePileController->shutdownRequired()) {
        const bool automaticSessionActive =
            m_autoChargeCoordinator
            && m_autoChargeCoordinator->automaticSessionActive();
        const bool accepted =
            m_chargePileController->requestConservativeRecovery(
                selectStopRecoveryOrigin(automaticSessionActive),
                ChargePileController::StopReason::Manual,
                error);
        emit logMessage(
            accepted
                ? QStringLiteral("[手动充电] 已接受不安全终态后的保守恢复")
                : QStringLiteral("[手动充电] 保守恢复未启动：%1")
                      .arg(error ? *error : QStringLiteral("未知原因")));
        return accepted;
    }

    emit logMessage(QStringLiteral("[手动充电] 充电桩已经确认安全，无需重复停止"));
    return true;
}

bool DeviceManager::chargePileShutdownRequired() const
{
    // 控制器缺失也不能被解释为“安全”；MainWindow 会保持打开并向操作员
    // 报告无法启动安全收尾，而不会在对象状态未知时静默退出。
    return !m_chargePileController
           || m_chargePileController->shutdownRequired();
}

void DeviceManager::requestApplicationShutdown()
{
    // 冻结必须发生在任何协调器或控制器调用之前，避免同步信号重入接受新动作。
    m_chargeApplicationShutdownGate.beginRequest();
    cancelChargePreflight(QStringLiteral("应用正在关闭"));
    if (m_autoChargeCoordinator)
        m_autoChargeCoordinator->setEnabled(false);
    if (m_chargeDo0Phase == ChargeDo0Phase::Opening)
        cancelPendingChargeStart(QStringLiteral("应用正在关闭"));

    if (!m_chargePileController) {
        m_chargeApplicationShutdownGate.finishRequest(false);
        emit applicationShutdownFinished(
            false, QStringLiteral("充电控制器尚未初始化，无法确认充电桩安全状态。"));
        return;
    }
    m_chargePileController->requestApplicationShutdown();
}

bool DeviceManager::setAutoChargeEnabled(const bool enabled, QString *error)
{
    if (error)
        error->clear();
    if (!m_autoChargeCoordinator || !m_chargePileController) {
        if (error)
            *error = QStringLiteral("自动充电业务对象尚未初始化");
        return false;
    }
    if (m_chargeApplicationShutdownGate.blocksNewActions()) {
        // requestApplicationShutdown() 已经直接关闭协调器授权；这里拒绝所有
        // 新开关动作，防止旧 UI 事件在关闭流程中重新开启自动策略。
        if (error)
            *error = QStringLiteral("程序正在执行充电安全关闭，不能改变自动充电授权");
        return false;
    }

    // 关闭授权永远允许；若存在 Automatic 会话，协调器会保持派单并请求同一
    // 控制器安全收尾。关闭且无活动会话时，决策严格旁路原主调度。
    if (!enabled) {
        if (m_chargePreflightIntent
                == ChargePreflightIntent::EnableAutomatic
            || m_chargePreflightIntent
                   == ChargePreflightIntent::AutomaticStart) {
            cancelChargePreflight(QStringLiteral("自动充电授权已关闭"));
        }
        if (m_chargeDo0Phase == ChargeDo0Phase::Opening
            && m_pendingChargeOrigin
                   == ChargePileController::SessionOrigin::Automatic) {
            cancelPendingChargeStart(QStringLiteral("自动充电授权已关闭"));
        }
        m_autoChargeCoordinator->setEnabled(false);
        return true;
    }
    const QString rejection = automaticEnablePreflightRejection();
    if (!rejection.isEmpty()) {
        if (error)
            *error = rejection;
        return false;
    }

    if (!beginChargePreflight(
            ChargePreflightIntent::EnableAutomatic, error)) {
        return false;
    }
    emit logMessage(
        QStringLiteral("[自动充电] 正在确认充电桩安全，授权尚未生效"));
    return true;
}

DeviceManager::~DeviceManager()
{
    const unsigned long sharedScanWaitMs = qMin<unsigned long>(
        boundedScanWaitMs(m_nscanTestOptions) + boundedScanWaitMs(m_lineScanOptions),
        120000UL);
    const auto shutdownScanThread =
        [](QThread *thread, unsigned long waitMs, const char *warning) {
        if (!thread || !thread->isRunning())
            return;

        thread->quit();
        if (!thread->wait(waitMs))
            qWarning() << warning;
    };

    // scan() 不能安全中断，只等待当前参数对应的最大扫码时长。
    shutdownScanThread(m_lineScanThread.data(),
                       sharedScanWaitMs,
                       "LineManager scan thread did not finish before shutdown timeout");
    shutdownScanThread(m_nscanTestThread.data(),
                       boundedScanWaitMs(m_nscanTestOptions),
                       "N-ScanHub test thread did not finish before shutdown timeout");
}

bool DeviceManager::startStandaloneStageOne(int stationId)
{
    const std::optional<StationTaskConfig> configured =
        stationTaskConfig(stationId, runtimeSettings());
    if (!configured) {
        emit logMessage(QStringLiteral("[华沿测试] 工位号无效或配置缺失：%1").arg(stationId));
        return false;
    }
    if (!m_huayanScheduler || !m_huayanScheduler->isConnected()) {
        emit logMessage(QStringLiteral("[华沿测试] 机械臂未连接，拒绝启动工位%1阶段一").arg(stationId));
        return false;
    }
    if (m_lineManager
        && (m_lineManager->state() != LineSystemState::Idle
            || m_lineManager->currentTask().taskId != 0)) {
        emit logMessage(QStringLiteral("[华沿测试] 总调度未完全空闲，拒绝启动工位%1阶段一").arg(stationId));
        return false;
    }
    if (m_lineOrch && m_lineOrch->isRunning()) {
        emit logMessage(QStringLiteral("[华沿测试] 兼容整线流程仍在运行，拒绝单独测试"));
        return false;
    }
    if (m_huayanScheduler->isBusy()) {
        emit logMessage(QStringLiteral("[华沿测试] 机械臂已有动作运行，拒绝并发启动"));
        return false;
    }

    const StationTaskConfig *config = &*configured;
    HuayanScheduler::StationArmFunctions stationFuncs;
    stationFuncs.captureFunc = config->captureFunc;
    stationFuncs.afterGripMode = config->afterGripMode;
    stationFuncs.afterGripFunc = config->afterGripFunc;
    stationFuncs.grabZClearance = config->grabZClearance;
    stationFuncs.unloadPointFunc = config->unloadPointFunc;
    stationFuncs.unloadFunc = config->unloadFunc;
    m_huayanScheduler->setStationFunctions(stationFuncs);
    m_huayanScheduler->setPreGripScanEnabled(false);

    emit logMessage(QStringLiteral("[华沿测试] 工位%1阶段一配置已隔离注入：capture=%2，扫码=关闭")
                        .arg(stationId)
                        .arg(config->captureFunc));
    m_huayanScheduler->startStageOne();
    return true;
}

void DeviceManager::applyConfig()
{
    if (m_cfg.robotIP.isEmpty() || m_cfg.agvIP.isEmpty()) {
        emit logMessage(QStringLiteral("[配置] 机器人/AGV IP 不能为空"));
        return;
    }

    emit logMessage(QString("配置已更新 → 机械臂 %1  AGV %2:%3  视觉 %4:%5")
                        .arg(m_cfg.robotIP)
                        .arg(m_cfg.agvIP).arg(m_cfg.agvPort)
                        .arg(m_cfg.cameraIP).arg(m_cfg.cameraPort));
    emit configApplied(m_cfg.robotIP, m_cfg.agvIP);

    // 重连 AGV Modbus
    m_agvCtrl->disconnectFromHost();
    m_agvCtrl->connectToHost(m_cfg.agvIP, m_cfg.agvPort);

    // 更新视觉服务 URL（不需要重连，HTTP 是无状态协议）
    if (!m_cfg.cameraIP.isEmpty())
        m_visionClient->setServerUrl(m_cfg.cameraIP, m_cfg.cameraPort);

    m_huayanScheduler->setConnectionParams(m_cfg.huayanIP, m_cfg.huayanPort);
    m_customSysScheduler->setEndpoint(QUrl(m_cfg.customSysEndpoint.trimmed()));
}

void DeviceManager::testRobot()
{
    if (m_cfg.robotIP.isEmpty()) {
        emit logMessage(QStringLiteral("[测试] 机械臂 IP 为空"));
        return;
    }
    emit logMessage(QString("[测试] 正在连接机械臂 SDK %1:%2 ...")
                        .arg(m_cfg.robotIP).arg(m_cfg.huayanPort));
    emit robotStatusChanged(false, QStringLiteral("测试中..."));

    if (tcpPing(m_cfg.robotIP, m_cfg.huayanPort)) {
        emit logMessage(QString("[测试] 机械臂 SDK %1:%2 连接成功 ✓")
                            .arg(m_cfg.robotIP).arg(m_cfg.huayanPort));
        emit robotStatusChanged(true, QString("可达 %1").arg(m_cfg.robotIP));
    } else {
        emit logMessage(QString("[测试] 机械臂 SDK %1:%2 连接失败 ✗")
                            .arg(m_cfg.robotIP).arg(m_cfg.huayanPort));
        emit robotStatusChanged(false, QStringLiteral("不可达"));
    }
}

void DeviceManager::testAgv()
{
    if (m_cfg.agvIP.isEmpty()) {
        emit logMessage(QStringLiteral("[测试] AGV IP 为空"));
        return;
    }
    emit logMessage(QString("[测试] 正在连接 AGV %1:%2 ...")
                        .arg(m_cfg.agvIP).arg(m_cfg.agvPort));
    emit agvStatusChanged(false, QStringLiteral("测试中..."));

    if (tcpPing(m_cfg.agvIP, m_cfg.agvPort)) {
        emit logMessage(QString("[测试] AGV %1:%2 连接成功 ✓")
                            .arg(m_cfg.agvIP).arg(m_cfg.agvPort));
        emit agvStatusChanged(true, QString("可达 %1").arg(m_cfg.agvIP));
    } else {
        emit logMessage(QString("[测试] AGV %1:%2 连接失败 ✗")
                            .arg(m_cfg.agvIP).arg(m_cfg.agvPort));
        emit agvStatusChanged(false, QStringLiteral("不可达"));
    }
}

void DeviceManager::testCamera()
{
    if (m_cfg.cameraIP.isEmpty()) {
        emit logMessage(QStringLiteral("[视觉] 相机 IP 为空"));
        return;
    }
    emit logMessage(QString("[视觉] 正在检测服务 %1:%2 ...")
                        .arg(m_cfg.cameraIP).arg(m_cfg.cameraPort));
    emit cameraStatusChanged(false, QStringLiteral("检测中..."));

    // 先更新 URL，再发起检测
    m_visionClient->setServerUrl(m_cfg.cameraIP, m_cfg.cameraPort);
    m_visionClient->checkStatus(); // 异步，结果通过 statusChanged → cameraStatusChanged
}

void DeviceManager::testScanner()
{
    if (m_cfg.scannerIP.isEmpty()) {
        emit logMessage(QStringLiteral("[扫码器] IP 为空"));
        return;
    }
    emit logMessage(QString("[扫码器] 正在连接 %1:8080 ...").arg(m_cfg.scannerIP));
    emit scannerStatusChanged(false, QStringLiteral("连接中..."));

    if (tcpPing(m_cfg.scannerIP, 8080)) {
        emit logMessage(QString("[扫码器] %1:8080 端口可达 ✓").arg(m_cfg.scannerIP));
        emit scannerStatusChanged(true, QString("可达 %1").arg(m_cfg.scannerIP));
    } else {
        emit logMessage(QString("[扫码器] %1:8080 连接失败 ✗").arg(m_cfg.scannerIP));
        emit scannerStatusChanged(false, QStringLiteral("不可达"));
    }
}

void DeviceManager::testCustomSystem()
{
    // 客户现场只要求验证 HTTP 接口可达，WiFi 连接由操作系统负责。
    m_customSysScheduler->setEndpoint(QUrl(m_cfg.customSysEndpoint.trimmed()));
    m_customSysScheduler->testConnectivity();
}

void DeviceManager::fetchCustomSystemDayData()
{
    // 读取日统计接口并提取 actualQty，其余字段仅用于现场调试展示。
    m_customSysScheduler->setEndpoint(QUrl(m_cfg.customSysEndpoint.trimmed()));
    m_customSysScheduler->fetchDayData();
}

void DeviceManager::startNScanTest(const NScanScheduler::ScanOptions &options)
{
    if (m_nscanTestRunning) {
        const QString message = QStringLiteral("[N-ScanHub] 扫码测试正在运行，忽略重复请求");
        emit nscanTestLog(message);
        emit logMessage(message);
        return;
    }

    if (!m_nscanTestThread || !m_nscanTestThread->isRunning()
        || !m_nscanTestWorker) {
        const QString message = QStringLiteral("[N-ScanHub] 扫码工作线程不可用");
        emit nscanTestLog(message);
        emit logMessage(message);
        return;
    }

    m_nscanTestRunning = true;
    m_nscanTestOptions = options;

    const QString startLog = QStringLiteral(
        "[N-ScanHub] 开始扫码：%1:%2，超时=%3ms，最大尝试=%4，重试间隔=%5ms，触发=%6")
        .arg(options.ip.trimmed())
        .arg(options.port)
        .arg(options.timeoutMs)
        .arg(options.maxAttempts)
        .arg(options.retryIntervalMs)
        .arg(QString::fromLatin1(options.triggerCommand.toHex(' ')));
    emit nscanTestLog(startLog);
    emit logMessage(startLog);
    emit nscanTestStarted();
    emit nscanScanRequested(options);
}

void DeviceManager::toggleLight()
{
    m_lightOn = !m_lightOn; // 先翻转目标状态，再执行

#ifdef Q_OS_LINUX
    // Linux 专属：调用外部 GPIO 控制程序
    const QString lightBin = QStringLiteral(PROJECT_SOURCE_DIR "/tools/fill_light");
    const QString arg      = m_lightOn ? "on" : "off";

    QProcess proc;
    proc.start(lightBin, QStringList() << arg);
    const bool success = proc.waitForFinished(3000) && (proc.exitCode() == 0);

    if (!success) {
        emit logMessage(QString("[补光灯] %1失败 (exit=%2)")
                            .arg(m_lightOn ? "开启" : "关闭")
                            .arg(proc.exitCode()));
    } else {
        emit logMessage(QString("补光灯 → %1").arg(m_lightOn ? "开启" : "关闭"));
    }
    emit lightChanged(m_lightOn, success);
#else
    // 非 Linux 平台（Windows 开发环境）：模拟切换
    emit logMessage(QString("[补光灯] 模拟%1（当前平台不支持 GPIO 控制）")
                        .arg(m_lightOn ? "开启" : "关闭"));
    emit lightChanged(m_lightOn, true); // success=true 表示"切换"成功（虽然只是模拟）
#endif
}

void DeviceManager::applyHandEyeMatrix(const float m[16])
{
    m_visionClient->setHandEyeMatrix(m);
    emit logMessage(QStringLiteral("[手眼] 手眼矩阵已更新，新矩阵将立即生效"));
    emit handEyeMatrixApplied();
}

// 同步阻塞，仅用于手动测试按钮，不用于自动流程
bool DeviceManager::tcpPing(const QString &ip, int port, int timeoutMs)
{
    QTcpSocket sock;
    sock.connectToHost(ip, static_cast<quint16>(port));
    const bool ok = sock.waitForConnected(timeoutMs); // 阻塞等待
    if (ok) sock.disconnectFromHost();                // 握手成功后立即断开（只测试可达性）
    return ok;
}

void DeviceManager::loadStationMap()
{
    m_stationMap.clear();
    QSettings settings(kSettingsOrg, kSettingsApp);
    const QString raw = settings.value(kStationMapKey).toString();
    for (const QString &entry : raw.split(',', Qt::SkipEmptyParts)) {
        const QStringList kv = entry.split(':');
        if (kv.size() != 2) continue;
        bool okW = false, okS = false;
        const int w = kv[0].toInt(&okW);
        const int s = kv[1].toInt(&okS);
        if (okW && okS && w > 0 && s > 0 && w <= 65535 && s <= 65535)
            m_stationMap.insert(w, s);
    }
}

void DeviceManager::saveStationMap() const
{
    QStringList entries;
    for (auto it = m_stationMap.constBegin(); it != m_stationMap.constEnd(); ++it)
        entries << QString("%1:%2").arg(it.key()).arg(it.value());
    QSettings settings(kSettingsOrg, kSettingsApp);
    settings.setValue(kStationMapKey, entries.join(','));
}

void DeviceManager::setStationMap(const QHash<int, int> &map)
{
    m_stationMap = map;
    saveStationMap();
}

int DeviceManager::resolveStation(int workstation) const
{
    return m_stationMap.value(workstation, workstation);
}

void DeviceManager::dispatchAgv(int workstation)
{
    const int station = resolveStation(workstation);
    emit logMessage(QStringLiteral("[AGV] 派单：工位 %1 → 站点 %2").arg(workstation).arg(station));
    m_agvCtrl->sendToStation(station);
}

void DeviceManager::cancelAgvNav()
{
    m_agvCtrl->cancelNavigation();
    emit logMessage(QStringLiteral("[AGV] 已发送取消导航"));
}

void DeviceManager::pauseAgvNav()
{
    m_agvCtrl->pauseNavigation();
    emit logMessage(QStringLiteral("[AGV] 已发送暂停导航"));
}

void DeviceManager::resumeAgvNav()
{
    m_agvCtrl->resumeNavigation();
    emit logMessage(QStringLiteral("[AGV] 已发送继续导航"));
}

#include "devicemanager.moc"
