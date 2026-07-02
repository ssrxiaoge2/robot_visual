#include "devicemanager.h"
#include "agvcontroller.h"
#include "customSysScheduler.h"
#include "huayanScheduler.h"
#include "linemanager.h"
#include "lineorchestrator.h"
#include "nscanscheduler.h"
#include "palletscheduler.h"
#include "visionclient.h"

#include <QDebug>
#include <QMutexLocker>
#include <QSettings>
#include <QTcpSocket>
#include <QThread>

#include <utility>
#ifdef Q_OS_LINUX
#  include <QProcess>  // fill_light GPIO 控制（仅 Linux 编译）
#endif

// QSettings 组织/应用名与映射键（值格式 "工位:站点,工位:站点"，如 "2:3,5:7"）
static const char *kSettingsOrg  = "wh-robot";
static const char *kSettingsApp  = "robot-visual";
static const char *kStationMapKey = "agv/stationMap";

namespace {
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
    qRegisterMetaType<CustomSysScheduler::PlcBitReply>("CustomSysScheduler::PlcBitReply");
    qRegisterMetaType<Task>("Task");
    qRegisterMetaType<QList<Task>>("QList<Task>");
    qRegisterMetaType<LineSystemState>("LineSystemState");
    qRegisterMetaType<ProductModel>("ProductModel");
    qRegisterMetaType<ProductionMode>("ProductionMode");
    qRegisterMetaType<QHash<QString, bool>>("QHash<QString,bool>");
    qRegisterMetaType<StationConsumption>("StationConsumption");
    qRegisterMetaType<QList<StationConsumption>>("QList<StationConsumption>");

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

    connect(m_huayanScheduler, &HuayanScheduler::surveyReady,
            m_visionClient,    &VisionHttpClient::fetchInference);
    connect(m_visionClient,    &VisionHttpClient::rawCoordinatesReady,
            m_huayanScheduler, &HuayanScheduler::setGrabOffset);
    connect(m_visionClient, &VisionHttpClient::noObjectDetected,
            m_huayanScheduler, &HuayanScheduler::onVisionNoObject);
    connect(m_visionClient, &VisionHttpClient::errorOccurred,
            m_huayanScheduler, &HuayanScheduler::onVisionErrorForPickup);

    connect(m_huayanScheduler, &HuayanScheduler::logMessage,
            this, &DeviceManager::logMessage);
    connect(m_huayanScheduler, &HuayanScheduler::stageError,
            this, [this](const QString &msg) {
        emit logMessage(QStringLiteral("[华沿] 错误：%1").arg(msg));
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

    m_shortageMonitor = new ShortageMonitor(m_customSysScheduler, this);
    connect(m_shortageMonitor, &ShortageMonitor::logMessage,
            this, &DeviceManager::logMessage);
    connect(m_shortageMonitor, &ShortageMonitor::statusChanged,
            this, &DeviceManager::shortageMonitorStatusChanged);
    connect(m_shortageMonitor, &ShortageMonitor::sampleUpdated,
            this, &DeviceManager::shortageMonitorSampleUpdated);
    connect(m_shortageMonitor, &ShortageMonitor::consumptionUpdated,
            this, &DeviceManager::shortageMonitorConsumptionUpdated);
    connect(m_shortageMonitor, &ShortageMonitor::shortageRequested,
            this, [this](int stationId) {
        const bool accepted = m_lineManager
            && m_lineManager->reportShortage(stationId, TaskSource::CustomerSystem);
        if (m_shortageMonitor) {
            m_shortageMonitor->confirmShortageAccepted(stationId, accepted);
        }
    });

    m_lineOrch = new LineOrchestrator(m_agvCtrl, m_huayanScheduler, this);
    // 编排器请求派单 → 经映射表解析后下发（复用 dispatchAgv）
    connect(m_lineOrch, &LineOrchestrator::agvDispatchRequested,
            this, &DeviceManager::dispatchAgv);
    // 注入工位→站点解析器：到达判定须与 AGV 监控回报的物理站点号同空间比较
    m_lineOrch->setStationResolver([this](int ws) { return resolveStation(ws); });
    connect(m_lineOrch, &LineOrchestrator::lineLog,
            this, &DeviceManager::logMessage);
    connect(m_lineOrch, &LineOrchestrator::lineError, this, [this](const QString &msg) {
        emit logMessage(QStringLiteral("[整线错误] %1").arg(msg));
    });

    loadStationMap();
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

void DeviceManager::startShortageMonitoring()
{
    if (!m_customSysScheduler || !m_shortageMonitor) {
        return;
    }
    m_customSysScheduler->setEndpoint(QUrl(m_cfg.customSysEndpoint.trimmed()));
    m_shortageMonitor->start();
}

void DeviceManager::stopShortageMonitoring()
{
    if (m_shortageMonitor) {
        m_shortageMonitor->stop();
    }
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
