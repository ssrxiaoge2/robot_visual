#include "liveshortagecoordinator.h"
#include "shortageconfigstore.h"
#include "shortageengine.h"
#include "shortagestatestore.h"
#include "shortagetestcontroller.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

namespace {

QDateTime utc(qint64 seconds)
{
    return QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::UTC);
}

ShortageConfiguration testConfiguration()
{
    ShortageConfiguration configuration = ShortageConfigStore::sheet3Defaults();
    configuration.revision = 910;
    for (ShortageStationConfig &station : configuration.stations) {
        station.enabled = true;
        station.boxQuantity = 100 + station.stationId;
        station.minimumStock = 10;
        station.maximumStock = 200;
        station.usageLeftRight = station.stationId;
        station.usageLeftOnly = station.stationId * 10;
        station.usageRightOnly = station.stationId * 100;
    }
    return configuration;
}

ShortageRuntimeState initializedState(qint64 stock = 100)
{
    ShortageRuntimeState state;
    state.configurationRevision = 910;
    state.initialized = true;
    state.operatorConfirmedRestore = true;
    state.hasStableContext = true;
    state.product = ProductModel::Model88;
    state.mode = ProductionMode::LeftRight;
    state.actualQty.hasBaseline = true;
    state.actualQty.baseline = 1000;
    state.nextAuditSequence = 1;
    state.nextReplenishmentOrderNo = 1;
    for (int stationId = 1; stationId <= 12; ++stationId) {
        ShortageStationRuntime station;
        station.stationId = stationId;
        station.stock = stock;
        state.stations.append(station);
    }
    return state;
}

ShortageStateLoadResult loadResult(const ShortageRuntimeState &state)
{
    ShortageStateLoadResult result;
    result.ok = true;
    result.stateFound = true;
    result.source = ShortageRestoreSource::Main;
    result.state = state;
    result.messageZh = QStringLiteral("测试恢复");
    return result;
}

const ShortageStationRuntime *station(const ShortageRuntimeState &state, int stationId)
{
    for (const ShortageStationRuntime &candidate : state.stations) {
        if (candidate.stationId == stationId)
            return &candidate;
    }
    return nullptr;
}

ShortageStationRuntime *station(ShortageRuntimeState *state, int stationId)
{
    for (ShortageStationRuntime &candidate : state->stations) {
        if (candidate.stationId == stationId)
            return &candidate;
    }
    return nullptr;
}

Task task(quint64 taskId,
          int stationId,
          TaskSource source,
          quint64 orderNo,
          TaskState state = TaskState::Running)
{
    Task value;
    value.taskId = taskId;
    value.stationId = stationId;
    value.source = source;
    value.replenishmentOrderNo = orderNo;
    value.state = state;
    return value;
}

QString readSourceFile(const QString &relativePath)
{
    QFile file(QStringLiteral(ROBOT_VISUAL_SOURCE_DIR "/") + relativePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        qFatal("无法读取源码契约文件");
    return QString::fromUtf8(file.readAll());
}

int countOccurrences(const QString &haystack, const QString &needle)
{
    int count = 0;
    int from = 0;
    while ((from = haystack.indexOf(needle, from)) >= 0) {
        ++count;
        from += needle.size();
    }
    return count;
}

class FakeGateway final : public IShortageTaskGateway
{
public:
    TaskEnqueueResult append(int stationId,
                             TaskSource source,
                             quint64 replenishmentOrderNo) override
    {
        ++appendCalls;
        stations.append(stationId);
        sources.append(source);
        orderNos.append(replenishmentOrderNo);
        if (!acceptNext) {
            acceptNext = true;
            return {false, 0, QStringLiteral("测试拒收：FIFO 满")};
        }
        return {true, nextTaskId++, QString()};
    }

    LineSystemState lineState() const override { return state; }
    bool pendingFifoEmpty() const override { return pendingFifoEmptyValue; }
    bool currentTaskEmpty() const override { return currentTaskEmptyValue; }

    LineSystemState state = LineSystemState::Running;
    bool pendingFifoEmptyValue = true;
    bool currentTaskEmptyValue = true;
    bool acceptNext = true;
    quint64 nextTaskId = 7001;
    int appendCalls = 0;
    QList<int> stations;
    QList<TaskSource> sources;
    QList<quint64> orderNos;
};

struct Harness {
    QTemporaryDir dir;
    ShortageConfiguration configuration = testConfiguration();
    ShortageStateStore store {dir.path(), ShortageStateNamespace::Production};
    ShortageEngine engine {configuration, &store};
    FakeGateway gateway;
    bool standaloneFieldSamplingActive = false;
    LiveShortageCoordinator coordinator {
        &engine,
        nullptr,
        &gateway,
        nullptr,
        [this]() -> ShortageOperationResult {
            if (standaloneFieldSamplingActive) {
                return {false, QStringLiteral("独立测试现场采样正在运行，处理动作=先停止测试现场采样")};
            }
            return {true, QStringLiteral("允许正式 Live")};
        }};
};

void installConfirmedState(Harness *harness, ShortageRuntimeState state = initializedState())
{
    QVERIFY2(harness->engine.installRestoredState(loadResult(state), utc(1)).ok,
             "恢复状态必须先安装成功");
    QVERIFY2(harness->engine.confirmRestoredState(true, utc(2)).ok,
             "测试默认确认恢复摘要");
}

void applySample(Harness *harness, int stationId, qint64 actualQty)
{
    ShortageSample sample;
    sample.roundId = quint64(actualQty);
    sample.product = ProductModel::Model88;
    sample.mode = ProductionMode::LeftRight;
    sample.actualQty = actualQty;
    sample.capturedAtUtc = utc(actualQty);
    harness->coordinator.onStableSample(sample);
    QVERIFY(station(harness->engine.state(), stationId) != nullptr);
}

} // namespace

class LiveShortageCoordinatorTest final : public QObject
{
    Q_OBJECT
private slots:
    void mockTaskUnloadNeverChangesFormalLedger();
    void switchingToMockStopsNewIntentButFinishesReal();
    void selectingLiveDoesNotStartLineManager();
    void directLiveStartIsRejectedWhileStandaloneFieldSamplingActive();
    void sharedStableSampleIsProcessedByTestOnlyWhenProductionLiveRejected();
    void repeatedModeSwitchDoesNotDuplicateConnections();
    void oldTasksMustDrainBeforeContextActivation();
    void contextSwitchDoesNotChangeStationStocks();
    void dispatchAppendsOneBoxAndRetriesRejection();
    void manualBoxUsesSameLedgerAndAudit();
    void inputErrorRejectsOnlyCurrentRound();
    void invalidConfigBlocksLiveMode();
    void pausedStationDoesNotBlockOtherStations();
    void criticalLockStopsNewAutomaticOnly();
    void maintenanceCorrectionRechecksUnsafeRuntimeBeforeTouchingLedger();
    void everyErrorContainsStructuredChineseDetails();
    void startupInstallsRestoredStateBeforeUserConfirm();
    void startupRestoreFailureBlocksLiveAndDispatch();
    void deviceManagerUsesOnlyTheSingleLiveScheduler();
};

void LiveShortageCoordinatorTest::mockTaskUnloadNeverChangesFormalLedger()
{
    Harness harness;
    installConfirmedState(&harness);

    harness.coordinator.onMaterialUnloaded(task(10, 4, TaskSource::UiMock, 0));

    QCOMPARE(station(harness.engine.state(), 4)->stock, qint64{100});
    QCOMPARE(harness.gateway.appendCalls, 0);
}

void LiveShortageCoordinatorTest::switchingToMockStopsNewIntentButFinishesReal()
{
    Harness harness;
    installConfirmedState(&harness);
    harness.coordinator.setInputSource(ShortageInputSource::Live);
    applySample(&harness, 5, 1100);
    QCOMPARE(harness.gateway.appendCalls, 1);
    const quint64 orderNo = harness.gateway.orderNos.first();
    const quint64 taskId = harness.gateway.nextTaskId - 1;

    harness.coordinator.setInputSource(ShortageInputSource::Mock);
    applySample(&harness, 6, 1200);
    const int acceptedStationId = harness.gateway.stations.first();
    const qint64 beforeUnload = station(harness.engine.state(), acceptedStationId)->stock;
    const qint64 boxQuantity = 100 + acceptedStationId;
    harness.coordinator.onTaskStarted(
        task(taskId, acceptedStationId, TaskSource::LiveAutomatic, orderNo));
    harness.coordinator.onMaterialUnloaded(
        task(taskId, acceptedStationId, TaskSource::LiveAutomatic, orderNo));

    QCOMPARE(harness.gateway.appendCalls, 1);
    QCOMPARE(station(harness.engine.state(), acceptedStationId)->stock,
             beforeUnload + boxQuantity);
}

void LiveShortageCoordinatorTest::selectingLiveDoesNotStartLineManager()
{
    const QString header = readSourceFile(QStringLiteral("src/liveshortagecoordinator.h"));
    QVERIFY(header.contains(QStringLiteral("class IShortageTaskGateway")));
    QVERIFY(!header.contains(QStringLiteral("start()")));

    Harness harness;
    installConfirmedState(&harness);
    harness.gateway.state = LineSystemState::Idle;
    harness.coordinator.setInputSource(ShortageInputSource::Live);
    applySample(&harness, 1, 1100);

    QCOMPARE(harness.gateway.appendCalls, 0);
}

void LiveShortageCoordinatorTest::directLiveStartIsRejectedWhileStandaloneFieldSamplingActive()
{
    Harness harness;
    installConfirmedState(&harness);
    harness.standaloneFieldSamplingActive = true;
    QSignalSpy rejected(&harness.coordinator, &LiveShortageCoordinator::operationRejected);

    harness.coordinator.setInputSource(ShortageInputSource::Live);
    applySample(&harness, 1, 1100);

    QVERIFY(!harness.coordinator.liveInputActive());
    QCOMPARE(harness.gateway.appendCalls, 0);
    QVERIFY(!rejected.isEmpty());
    const QString reason = rejected.first().at(0).toString();
    QVERIFY(reason.contains(QStringLiteral("独立测试现场采样正在运行")));
    QVERIFY(reason.contains(QStringLiteral("处理动作")));

    harness.standaloneFieldSamplingActive = false;
    harness.coordinator.setInputSource(ShortageInputSource::Live);
    applySample(&harness, 1, 1200);

    QVERIFY(harness.coordinator.liveInputActive());
    QCOMPARE(harness.gateway.appendCalls, 1);
}

void LiveShortageCoordinatorTest::sharedStableSampleIsProcessedByTestOnlyWhenProductionLiveRejected()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ShortageSampleCoordinator sampleCoordinator(nullptr);

    ShortageTestController testController(
        testConfiguration(),
        directory.path(),
        &sampleCoordinator,
        [] { return ShortageOperationResult{true, QStringLiteral("允许测试采样")}; });
    testController.initializeZeroAfterConfirmation();
    // 修改前测试控制器默认可直接启动现场采样；现场问题修复后默认手工源，
    // 共享采样互斥场景必须显式切到现场源，才代表真实测试窗口的操作顺序。
    testController.selectInputSource(ShortageTestInputSource::Field);
    testController.startFieldSampling();
    QVERIFY(testController.fieldSamplingActive());

    ShortageStateStore productionStore(directory.path(), ShortageStateNamespace::Production);
    ShortageEngine productionEngine(testConfiguration(), &productionStore);
    QVERIFY(productionEngine.installRestoredState(loadResult(initializedState()), utc(1)).ok);
    QVERIFY(productionEngine.confirmRestoredState(true, utc(2)).ok);
    FakeGateway gateway;
    LiveShortageCoordinator productionCoordinator(
        &productionEngine,
        &sampleCoordinator,
        &gateway,
        nullptr,
        [&testController]() -> ShortageOperationResult {
            if (testController.fieldSamplingActive()) {
                return {
                    false,
                    QStringLiteral("独立测试现场采样正在运行，处理动作=先停止测试现场采样")
                };
            }
            return {true, QStringLiteral("允许正式 Live")};
        });
    connect(&sampleCoordinator, &ShortageSampleCoordinator::stableSampleReady,
            &productionCoordinator, &LiveShortageCoordinator::onStableSample);
    QSignalSpy rejected(&productionCoordinator, &LiveShortageCoordinator::operationRejected);
    QSignalSpy testSnapshots(&testController, &ShortageTestController::snapshotChanged);

    productionCoordinator.setInputSource(ShortageInputSource::Live);
    ShortageSample sharedSample;
    sharedSample.roundId = 77;
    sharedSample.product = ProductModel::Model88;
    sharedSample.mode = ProductionMode::LeftRight;
    sharedSample.actualQty = 1100;
    sharedSample.capturedAtUtc = utc(1100);
    QVERIFY(QMetaObject::invokeMethod(&sampleCoordinator,
                                      "stableSampleReady",
                                      Qt::DirectConnection,
                                      Q_ARG(ShortageSample, sharedSample)));

    QVERIFY(!productionCoordinator.liveInputActive());
    QCOMPARE(gateway.appendCalls, 0);
    QCOMPARE(productionEngine.state().actualQty.baseline, qint64{1000});
    QVERIFY(!rejected.isEmpty());
    QVERIFY(testSnapshots.count() >= 1);
    const ShortageRuntimeState testState =
        qvariant_cast<ShortageUiSnapshot>(testSnapshots.last().at(0)).runtime;
    QCOMPARE(testState.actualQty.baseline, qint64{1100});
}

void LiveShortageCoordinatorTest::repeatedModeSwitchDoesNotDuplicateConnections()
{
    Harness harness;
    installConfirmedState(&harness);
    for (int i = 0; i < 20; ++i) {
        harness.coordinator.setInputSource(ShortageInputSource::Live);
        harness.coordinator.setInputSource(ShortageInputSource::Mock);
    }
    harness.coordinator.setInputSource(ShortageInputSource::Live);
    applySample(&harness, 4, 1100);

    QCOMPARE(harness.gateway.appendCalls, 1);
}

void LiveShortageCoordinatorTest::oldTasksMustDrainBeforeContextActivation()
{
    Harness harness;
    installConfirmedState(&harness);
    harness.coordinator.setInputSource(ShortageInputSource::Live);
    applySample(&harness, 2, 1100);

    ShortageSample changed;
    changed.roundId = 2;
    changed.product = ProductModel::Model92;
    changed.mode = ProductionMode::LeftRight;
    changed.actualQty = 1102;
    changed.capturedAtUtc = utc(1102);
    harness.coordinator.onStableSample(changed);

    QVERIFY(harness.engine.state().hasPendingContext);
    QCOMPARE(harness.engine.state().product, ProductModel::Model88);
}

void LiveShortageCoordinatorTest::contextSwitchDoesNotChangeStationStocks()
{
    Harness harness;
    installConfirmedState(&harness);
    const ShortageRuntimeState before = harness.engine.state();

    ShortageSample changed;
    changed.roundId = 3;
    changed.product = ProductModel::Model92;
    changed.mode = ProductionMode::LeftRight;
    changed.actualQty = 1000;
    changed.capturedAtUtc = utc(1000);
    harness.coordinator.onStableSample(changed);

    for (int stationId = 1; stationId <= 12; ++stationId)
        QCOMPARE(station(harness.engine.state(), stationId)->stock,
                 station(before, stationId)->stock);
}

void LiveShortageCoordinatorTest::dispatchAppendsOneBoxAndRetriesRejection()
{
    Harness harness;
    installConfirmedState(&harness);
    harness.gateway.acceptNext = false;
    harness.coordinator.setInputSource(ShortageInputSource::Live);

    applySample(&harness, 3, 1100);
    QCOMPARE(harness.gateway.appendCalls, 1);
    const quint64 firstOrderNo = harness.gateway.orderNos.first();

    harness.coordinator.onLineStateChanged(LineSystemState::Running, QStringLiteral("运行"));
    QCOMPARE(harness.gateway.appendCalls, 2);
    QCOMPARE(harness.gateway.orderNos.last(), firstOrderNo);
}

void LiveShortageCoordinatorTest::manualBoxUsesSameLedgerAndAudit()
{
    Harness harness;
    installConfirmedState(&harness);
    harness.coordinator.setInputSource(ShortageInputSource::Live);

    const ManualBoxConfirmation confirmation = harness.coordinator.manualBoxConfirmation(4);
    QCOMPARE(confirmation.stationId, 4);
    QCOMPARE(confirmation.currentStock, qint64{100});
    harness.coordinator.requestManualBox(4, true);

    QCOMPARE(harness.gateway.appendCalls, 1);
    QCOMPARE(harness.gateway.sources.first(), TaskSource::LiveManual);
    QVERIFY(harness.gateway.orderNos.first() != 0);
}

void LiveShortageCoordinatorTest::inputErrorRejectsOnlyCurrentRound()
{
    Harness harness;
    installConfirmedState(&harness);
    QSignalSpy rejected(&harness.coordinator, &LiveShortageCoordinator::operationRejected);
    harness.coordinator.setInputSource(ShortageInputSource::Live);

    ShortageSample bad;
    bad.roundId = 4;
    bad.product = ProductModel::Model88;
    bad.mode = ProductionMode::LeftRight;
    bad.actualQty = -1;
    bad.capturedAtUtc = utc(4);
    harness.coordinator.onStableSample(bad);
    QCOMPARE(rejected.size(), 1);

    applySample(&harness, 1, 1100);
    QCOMPARE(harness.gateway.appendCalls, 1);
}

void LiveShortageCoordinatorTest::invalidConfigBlocksLiveMode()
{
    Harness harness;
    harness.configuration.stations.first().boxQuantity = 0;
    ShortageStateStore store(harness.dir.path(), ShortageStateNamespace::Production);
    ShortageEngine engine(harness.configuration, &store);
    FakeGateway gateway;
    LiveShortageCoordinator coordinator(&engine, nullptr, &gateway);
    QVERIFY(engine.installRestoredState(loadResult(initializedState()), utc(1)).ok);
    QVERIFY(engine.confirmRestoredState(true, utc(2)).ok);
    QSignalSpy rejected(&coordinator, &LiveShortageCoordinator::operationRejected);

    coordinator.setInputSource(ShortageInputSource::Live);

    QCOMPARE(gateway.appendCalls, 0);
    QVERIFY(!rejected.isEmpty());
}

void LiveShortageCoordinatorTest::pausedStationDoesNotBlockOtherStations()
{
    Harness harness;
    ShortageRuntimeState state = initializedState();
    station(&state, 1)->automaticPaused = true;
    station(&state, 1)->stock = -100;
    station(&state, 2)->stock = -100;
    station(&state, 2)->firstLowAtUtc = utc(10);
    state.waitingStationIds = {2};
    state.activeStationId = 2;
    installConfirmedState(&harness, state);
    harness.coordinator.setInputSource(ShortageInputSource::Live);

    applySample(&harness, 2, 1001);

    QCOMPARE(harness.gateway.appendCalls, 1);
    QCOMPARE(harness.gateway.stations.first(), 2);
}

void LiveShortageCoordinatorTest::criticalLockStopsNewAutomaticOnly()
{
    Harness harness;
    ShortageRuntimeState state = initializedState();
    state.criticalLock = true;
    state.criticalReasonZh = QStringLiteral("测试严重锁定");
    installConfirmedState(&harness, state);
    QSignalSpy rejected(&harness.coordinator, &LiveShortageCoordinator::operationRejected);
    harness.coordinator.setInputSource(ShortageInputSource::Live);

    applySample(&harness, 3, 1100);
    QCOMPARE(harness.gateway.appendCalls, 0);

    harness.coordinator.requestManualBox(3, true);

    QCOMPARE(harness.gateway.appendCalls, 1);
    QCOMPARE(harness.gateway.sources.first(), TaskSource::LiveManual);
    QVERIFY(harness.gateway.orderNos.first() != 0);
    QCOMPARE(rejected.size(), 0);
}

void LiveShortageCoordinatorTest::maintenanceCorrectionRechecksUnsafeRuntimeBeforeTouchingLedger()
{
    struct UnsafeCase {
        QString name;
        LineSystemState lineState = LineSystemState::Idle;
        bool liveActive = false;
        bool fifoEmpty = true;
        bool currentTaskEmpty = true;
        QString expectedReason;
    };
    const QList<UnsafeCase> cases {
        {QStringLiteral("整线运行"), LineSystemState::Running, false, true, true,
         QStringLiteral("LineManager 正在 Running/ReturningHome")},
        {QStringLiteral("真实采样未停"), LineSystemState::Idle, true, true, true,
         QStringLiteral("正式采样未停止")},
        {QStringLiteral("FIFO 非空"), LineSystemState::Idle, false, false, true,
         QStringLiteral("Pending FIFO 不为空")},
        {QStringLiteral("当前任务非空"), LineSystemState::Idle, false, true, false,
         QStringLiteral("当前执行任务不为空")},
    };

    for (const UnsafeCase &testCase : cases) {
        Harness harness;
        installConfirmedState(&harness);
        harness.gateway.state = testCase.lineState;
        harness.gateway.pendingFifoEmptyValue = testCase.fifoEmpty;
        harness.gateway.currentTaskEmptyValue = testCase.currentTaskEmpty;
        if (testCase.liveActive)
            harness.coordinator.setInputSource(ShortageInputSource::Live);
        QSignalSpy rejected(&harness.coordinator, &LiveShortageCoordinator::operationRejected);

        ShortageMaintenanceCorrection correction;
        correction.stationId = 4;
        correction.oldStock = station(harness.engine.state(), 4)->stock;
        correction.newStock = 777;
        correction.reason = QStringLiteral("测试维护修正业务层复验");
        correction.typedStationId = QString::number(correction.stationId);

        harness.coordinator.applyMaintenanceCorrection(correction);

        QCOMPARE(station(harness.engine.state(), 4)->stock, qint64{100});
        QVERIFY2(!rejected.isEmpty(), qPrintable(testCase.name));
        QVERIFY2(rejected.last().at(0).toString().contains(testCase.expectedReason),
                 qPrintable(testCase.name));
    }
}

void LiveShortageCoordinatorTest::everyErrorContainsStructuredChineseDetails()
{
    Harness harness;
    installConfirmedState(&harness);
    QSignalSpy rejected(&harness.coordinator, &LiveShortageCoordinator::operationRejected);

    harness.coordinator.requestManualBox(99, false);

    QVERIFY(!rejected.isEmpty());
    const QString reason = rejected.takeFirst().at(0).toString();
    QVERIFY(reason.contains(QStringLiteral("失败")));
    QVERIFY(reason.contains(QStringLiteral("stationId")) || reason.contains(QStringLiteral("工位")));
    QVERIFY(reason.contains(QStringLiteral("处理动作")));
}

void LiveShortageCoordinatorTest::startupInstallsRestoredStateBeforeUserConfirm()
{
    QTemporaryDir dir;
    ShortageConfiguration configuration = testConfiguration();
    ShortageStateStore store(dir.path(), ShortageStateNamespace::Production);
    ShortageEngine engine(configuration, &store);
    FakeGateway gateway;
    LiveShortageCoordinator coordinator(&engine, nullptr, &gateway);
    QVERIFY(engine.installRestoredState(loadResult(initializedState()), utc(1)).ok);
    coordinator.setInputSource(ShortageInputSource::Live);

    ShortageSample sample;
    sample.roundId = 15;
    sample.product = ProductModel::Model88;
    sample.mode = ProductionMode::LeftRight;
    sample.actualQty = 1100;
    sample.capturedAtUtc = utc(1100);
    coordinator.onStableSample(sample);

    QVERIFY(engine.state().initialized);
    QVERIFY(!engine.state().operatorConfirmedRestore);
    QCOMPARE(gateway.appendCalls, 0);
}

void LiveShortageCoordinatorTest::startupRestoreFailureBlocksLiveAndDispatch()
{
    QTemporaryDir dir;
    ShortageConfiguration configuration = testConfiguration();
    ShortageStateStore store(dir.path(), ShortageStateNamespace::Production);
    ShortageEngine engine(configuration, &store);
    ShortageStateLoadResult failed;
    failed.ok = false;
    failed.requiresMaintenance = true;
    failed.messageZh = QStringLiteral("三份状态均损坏");
    QVERIFY(!engine.installRestoredState(failed, utc(1)).ok);
    FakeGateway gateway;
    LiveShortageCoordinator coordinator(&engine, nullptr, &gateway);
    QSignalSpy rejected(&coordinator, &LiveShortageCoordinator::operationRejected);

    coordinator.setInputSource(ShortageInputSource::Live);
    coordinator.onLineStateChanged(LineSystemState::Running, QStringLiteral("运行"));

    QVERIFY(!coordinator.liveInputActive());
    QCOMPARE(gateway.appendCalls, 0);
    QVERIFY(!rejected.isEmpty());
}

void LiveShortageCoordinatorTest::deviceManagerUsesOnlyTheSingleLiveScheduler()
{
    const QString header = readSourceFile(QStringLiteral("src/devicemanager.h"));
    const QString source = readSourceFile(QStringLiteral("src/devicemanager.cpp"));

    QVERIFY(header.contains(QStringLiteral("CustomSysScheduler *m_liveShortageScheduler = nullptr")));
    QVERIFY(!header.contains(QStringLiteral("m_customSysScheduler")));
    QVERIFY(!source.contains(QStringLiteral("192.168.115.229")));
    QVERIFY(source.contains(QStringLiteral("new CustomSysScheduler(this)")));
    QCOMPARE(countOccurrences(source, QStringLiteral("new CustomSysScheduler")), 1);
    QVERIFY(source.contains(QStringLiteral("new LiveShortageCoordinator")));
    QVERIFY(header.contains(QStringLiteral("LiveShortageCoordinator *liveShortageCoordinator() const")));
    QVERIFY(header.contains(QStringLiteral("void setShortageInputSource(ShortageInputSource source)")));
    QVERIFY(header.contains(QStringLiteral("void confirmShortageRecoveredState(bool accepted)")));
    QVERIFY(header.contains(QStringLiteral("void requestManualShortageBox(int stationId, bool highStockRiskConfirmed)")));
    QVERIFY(header.contains(QStringLiteral("void applyShortageMaintenanceCorrection(ShortageMaintenanceCorrection correction)")));
    QVERIFY(header.contains(QStringLiteral("void shortageSnapshotChanged(ShortageUiSnapshot snapshot)")));
    QVERIFY(source.contains(QStringLiteral("&LiveShortageCoordinator::snapshotChanged")));
    QVERIFY(source.contains(QStringLiteral("&DeviceManager::shortageSnapshotChanged")));
    QVERIFY2(source.contains(QStringLiteral("fieldSamplingActive()")),
             "DeviceManager must reject production Live while standalone field sampling is active");
    QVERIFY2(source.contains(QStringLiteral("正式 Live 启动失败"))
                 && source.contains(QStringLiteral("独立测试现场采样正在运行")),
             "reverse mutual exclusion rejection must provide a Chinese reason");
}

QTEST_MAIN(LiveShortageCoordinatorTest)
#include "test_live_shortage_coordinator.moc"
