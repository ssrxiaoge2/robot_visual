#include "devicemanager.h"
#include "agvcontroller.h"
#include "customSysScheduler.h"
#include "huayanScheduler.h"
#include "linemanager.h"
#include "lineorchestrator.h"
#include "liveshortagecoordinator.h"
#include "nscanscheduler.h"
#include "palletscheduler.h"
#include "shortageconfigstore.h"
#include "shortageengine.h"
#include "shortagesamplecoordinator.h"
#include "shortagestatestore.h"
#include "shortagetestcontroller.h"
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

class LineManagerShortageGateway final : public IShortageTaskGateway
{
public:
    /// lineManager 由 DeviceManager 持有；适配器不负责释放。
    explicit LineManagerShortageGateway(LineManager *lineManager)
        : m_lineManager(lineManager)
    {
    }

    TaskEnqueueResult append(int stationId,
                             TaskSource source,
                             quint64 replenishmentOrderNo) override
    {
        return m_lineManager->enqueueShortageTask(stationId, source, replenishmentOrderNo);
    }

    LineSystemState lineState() const override
    {
        return m_lineManager->state();
    }

private:
    LineManager *m_lineManager = nullptr; ///< 非拥有指针，只提供队尾追加和状态读取。
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
    qRegisterMetaType<CustomSysScheduler::LiveMesDayReply>(
        "CustomSysScheduler::LiveMesDayReply");
    qRegisterMetaType<CustomSysScheduler::PlcBitReply>(
        "CustomSysScheduler::PlcBitReply");
    qRegisterMetaType<Task>("Task");
    qRegisterMetaType<QList<Task>>("QList<Task>");
    qRegisterMetaType<LineSystemState>("LineSystemState");

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

    // 真实缺料 MES/PLC 协议层由 DeviceManager 唯一持有；当前任务只建立通信对象。
    m_liveShortageScheduler = new CustomSysScheduler(this);
    connect(m_liveShortageScheduler, &CustomSysScheduler::logMessage,
            this, &DeviceManager::logMessage);

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

    const ShortageConfiguration shortageConfiguration = ShortageConfigStore::sheet3Defaults();
    QString endpointError;
    if (!m_liveShortageScheduler->setLiveMesDayEndpoint(
            QUrl(shortageConfiguration.parameters.liveMesDayEndpoint), &endpointError)) {
        emit logMessage(QStringLiteral("[缺料] 正式 MES 地址配置失败：%1").arg(endpointError));
    }

    // 正式/测试缺料对象只在构造函数一次性创建和 connect；弹窗打开、来源切换不重复接线。
    m_productionShortageStore = std::make_unique<ShortageStateStore>(
        QStringLiteral(PROJECT_SOURCE_DIR), ShortageStateNamespace::Production);
    m_productionShortageEngine = std::make_unique<ShortageEngine>(
        shortageConfiguration, m_productionShortageStore.get());
    const ShortageStateLoadResult productionLoad = m_productionShortageStore->load();
    const ShortageEngineResult productionRestore =
        m_productionShortageEngine->installRestoredState(
            productionLoad, QDateTime::currentDateTimeUtc());
    if (!productionRestore.ok) {
        emit logMessage(QStringLiteral("[缺料] 恢复失败，正式自动派单锁定：%1")
                            .arg(productionRestore.messageZh));
    }

    m_shortageTaskGateway = std::make_unique<LineManagerShortageGateway>(m_lineManager);
    m_shortageSampleCoordinator = new ShortageSampleCoordinator(m_liveShortageScheduler, this);
    m_shortageTestController = new ShortageTestController(
        shortageConfiguration,
        QStringLiteral(PROJECT_SOURCE_DIR),
        m_shortageSampleCoordinator,
        [this]() -> ShortageOperationResult {
            if (m_liveShortageCoordinator && m_liveShortageCoordinator->liveInputActive()) {
                return {false, QStringLiteral("正式 Live 来源正在运行，处理动作=先切回 Mock")};
            }
            return {true, QStringLiteral("允许测试采样")};
        },
        this);
    m_liveShortageCoordinator = new LiveShortageCoordinator(
        m_productionShortageEngine.get(),
        m_shortageSampleCoordinator,
        m_shortageTaskGateway.get(),
        this,
        [this]() -> ShortageOperationResult {
            if (m_shortageTestController != nullptr
                && m_shortageTestController->fieldSamplingActive()) {
                return {
                    false,
                    QStringLiteral("独立测试现场采样正在运行，处理动作=先停止测试现场采样")
                };
            }
            return {true, QStringLiteral("允许正式 Live")};
        });

    connect(m_shortageSampleCoordinator, &ShortageSampleCoordinator::stableSampleReady,
            m_liveShortageCoordinator, &LiveShortageCoordinator::onStableSample);
    connect(m_shortageSampleCoordinator, &ShortageSampleCoordinator::communicationStateChanged,
            this, [this](ShortageCommunicationState, const QString &reason) {
        emit logMessage(QStringLiteral("[缺料采样] %1").arg(reason));
    });
    connect(m_lineManager, &LineManager::systemStateChanged,
            m_liveShortageCoordinator, &LiveShortageCoordinator::onLineStateChanged);
    connect(m_lineManager, &LineManager::shortageTaskAccepted,
            m_liveShortageCoordinator, &LiveShortageCoordinator::onTaskAccepted);
    connect(m_lineManager, &LineManager::shortageTaskStarted,
            m_liveShortageCoordinator, &LiveShortageCoordinator::onTaskStarted);
    connect(m_lineManager, &LineManager::shortageMaterialUnloaded,
            m_liveShortageCoordinator, &LiveShortageCoordinator::onMaterialUnloaded);
    connect(m_lineManager, &LineManager::shortageTaskTerminal,
            m_liveShortageCoordinator, &LiveShortageCoordinator::onTaskTerminal);
    connect(m_liveShortageCoordinator, &LiveShortageCoordinator::operationRejected,
            this, [this](const QString &reason) {
        emit logMessage(QStringLiteral("[缺料协调器] %1").arg(reason));
    });
    connect(m_liveShortageCoordinator, &LiveShortageCoordinator::criticalAlarmRaised,
            this, [this](const QString &reason) {
        emit logMessage(QStringLiteral("[缺料严重报警] %1").arg(reason));
    });
    connect(m_liveShortageCoordinator, &LiveShortageCoordinator::snapshotChanged,
            this, &DeviceManager::shortageSnapshotChanged);
    connect(m_shortageTestController, &ShortageTestController::eventLogged,
            this, [this](const QString &message) {
        emit logMessage(QStringLiteral("[缺料测试] %1").arg(message));
    });
    connect(m_shortageTestController, &ShortageTestController::operationRejected,
            this, [this](const QString &reason) {
        emit logMessage(QStringLiteral("[缺料测试] %1").arg(reason));
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
    // 缺料 QObject 显式按“测试源 -> 正式采样 -> 生产协调器 -> 测试控制器 -> 采样层 -> 通信层”
    // 顺序停止和删除，最后才释放非 QObject Engine/Store，避免依赖 QObject 子对象析构顺序。
    if (m_shortageTestController != nullptr)
        m_shortageTestController->stop();
    if (m_shortageSampleCoordinator != nullptr)
        m_shortageSampleCoordinator->stop();
    delete m_liveShortageCoordinator;
    m_liveShortageCoordinator = nullptr;
    delete m_shortageTestController;
    m_shortageTestController = nullptr;
    delete m_shortageSampleCoordinator;
    m_shortageSampleCoordinator = nullptr;
    delete m_liveShortageScheduler;
    m_liveShortageScheduler = nullptr;
    m_shortageTaskGateway.reset();

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

void DeviceManager::setShortageInputSource(ShortageInputSource source)
{
    if (source == ShortageInputSource::Live
        && m_shortageTestController != nullptr
        && m_shortageTestController->fieldSamplingActive()) {
        emit logMessage(QStringLiteral(
            "[缺料协调器] 正式 Live 启动失败：独立测试现场采样正在运行，处理动作=先停止测试现场采样"));
        return;
    }
    if (m_liveShortageCoordinator != nullptr)
        m_liveShortageCoordinator->setInputSource(source);
}

void DeviceManager::confirmShortageRecoveredState(bool accepted)
{
    if (m_liveShortageCoordinator != nullptr)
        m_liveShortageCoordinator->confirmRecoveredState(accepted);
}

void DeviceManager::requestManualShortageBox(int stationId, bool highStockRiskConfirmed)
{
    if (m_liveShortageCoordinator != nullptr)
        m_liveShortageCoordinator->requestManualBox(stationId, highStockRiskConfirmed);
}

void DeviceManager::applyShortageMaintenanceCorrection(
    ShortageMaintenanceCorrection correction)
{
    if (m_liveShortageCoordinator != nullptr)
        m_liveShortageCoordinator->applyMaintenanceCorrection(correction);
}

#include "devicemanager.moc"
