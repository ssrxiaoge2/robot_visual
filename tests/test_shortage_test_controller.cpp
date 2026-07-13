#include "shortagetestcontroller.h"

#include "shortagesamplecoordinator.h"

#include <QCryptographicHash>
#include <QFile>
#include <QDirIterator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>
#include <type_traits>

namespace {

QDateTime utc(qint64 seconds)
{
    return QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::UTC);
}

ShortageConfiguration testConfiguration()
{
    ShortageConfiguration configuration;
    configuration.revision = 7007;
    configuration.parameters.preUnloadFailureLimit = 2;

    const QList<ProductModel> products {
        ProductModel::Model88,
        ProductModel::Model88R,
        ProductModel::Model92,
    };
    for (ProductModel product : products) {
        for (int stationId = 1; stationId <= 12; ++stationId) {
            ShortageStationConfig station;
            station.product = product;
            station.stationId = stationId;
            station.temporaryNo = QString::number(stationId);
            station.sitePosition = QStringLiteral("测试位置%1").arg(stationId);
            station.partNumber = QStringLiteral("TEST-PN-%1").arg(stationId);
            station.enabled = true;
            station.boxQuantity = 100 + stationId;
            station.minimumStock = 10;
            station.maximumStock = 200;
            station.usageLeftRight = stationId;
            station.usageLeftOnly = stationId * 10;
            station.usageRightOnly = stationId * 100;
            configuration.stations.append(station);
        }
    }
    return configuration;
}

ShortageRuntimeState productionState()
{
    ShortageRuntimeState state;
    state.configurationRevision = 7007;
    state.initialized = true;
    state.operatorConfirmedRestore = true;
    state.hasStableContext = true;
    state.product = ProductModel::Model88;
    state.mode = ProductionMode::LeftRight;
    state.actualQty.hasBaseline = true;
    state.actualQty.baseline = 1000;
    state.nextReplenishmentOrderNo = 1;
    state.nextAuditSequence = 1;
    for (int stationId = 1; stationId <= 12; ++stationId) {
        ShortageStationRuntime station;
        station.stationId = stationId;
        station.stock = 100;
        state.stations.append(station);
    }
    return state;
}

QByteArray productionHash(const QString &directoryPath)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QStringList names {
        QStringLiteral("production-state.json"),
        QStringLiteral("production-state.backup.json"),
        QStringLiteral("production-events.jsonl"),
    };
    const QDir directory(directoryPath);
    for (const QString &name : names) {
        const QString path = directory.filePath(name);
        hash.addData(name.toUtf8());
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            hash.addData(QByteArrayLiteral("<missing>"));
            continue;
        }
        hash.addData(file.readAll());
    }
    return hash.result();
}

QString readProjectFile(const QString &relativePath)
{
    QFile file(QStringLiteral(ROBOT_VISUAL_SOURCE_DIR) + QLatin1Char('/') + relativePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

const ShortageStationRuntime *station(const ShortageRuntimeState &state, int stationId)
{
    for (const ShortageStationRuntime &item : state.stations) {
        if (item.stationId == stationId)
            return &item;
    }
    return nullptr;
}

} // namespace

class ShortageTestControllerTest final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void controllerOwnsTheSameEngineTypeAsProduction();       // IN-01。
    void simulatedDispatchNeverCallsMainFifo();               // IN-02。
    void everyTestActionEmitsNoHardwareCommand();             // IN-03。
    void failedFieldSamplingGuardKeepsControllerInactive();
    void inactiveContextConfirmationDoesNotReachEngine();
    void testSaveClearReloadKeepsProductionHash();            // IN-04。
    void fullScenarioCoversSamplePlanUnloadFailureRestart();  // 完整闭环。
};

void ShortageTestControllerTest::initTestCase()
{
    qRegisterMetaType<ShortageUiSnapshot>("ShortageUiSnapshot");
}

void ShortageTestControllerTest::controllerOwnsTheSameEngineTypeAsProduction()
{
    static_assert(std::is_same_v<decltype(std::declval<ShortageTestController>().engineForTest()),
                                 const ShortageEngine &>);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ShortageTestController controller(testConfiguration(), directory.path(), nullptr,
                                      [] { return ShortageOperationResult{true, QStringLiteral("允许")}; });

    QCOMPARE(controller.stateNamespaceForTest(), ShortageStateNamespace::StandaloneTest);
    QCOMPARE(controller.engineForTest().configuration().revision, quint64{7007});
}

void ShortageTestControllerTest::simulatedDispatchNeverCallsMainFifo()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ShortageTestController controller(testConfiguration(), directory.path(), nullptr,
                                      [] { return ShortageOperationResult{true, QStringLiteral("允许")}; });
    QSignalSpy snapshotSpy(&controller, &ShortageTestController::snapshotChanged);
    const QString controllerHeader = readProjectFile(QStringLiteral("src/shortagetestcontroller.h"));
    const QString controllerSource = readProjectFile(QStringLiteral("src/shortagetestcontroller.cpp"));
    QVERIFY2(!controllerHeader.isEmpty(), "controller header must be readable");
    QVERIFY2(!controllerSource.isEmpty(), "controller source must be readable");
    const QString controllerText = controllerHeader + controllerSource;
    QVERIFY2(!controllerText.contains(QStringLiteral("TaskQueue")),
             "standalone controller must not depend on the main FIFO TaskQueue");
    QVERIFY2(!controllerText.contains(QStringLiteral("LineManager")),
             "standalone controller must not depend on LineManager/FIFO orchestration");
    QVERIFY2(!controllerText.contains(QStringLiteral("HuayanScheduler")),
             "standalone controller must not dispatch through the robot scheduler");
    QVERIFY2(!controllerText.contains(QStringLiteral("AgvController")),
             "standalone controller must not command AGV hardware");

    controller.initializeZeroAfterConfirmation();
    controller.applyManualSample(ProductModel::Model88, ProductionMode::LeftRight, 0);
    controller.applyManualSample(ProductModel::Model88, ProductionMode::LeftRight, 100);
    controller.simulateDispatchAccepted();

    QVERIFY(snapshotSpy.count() >= 3);
    const ShortageRuntimeState state =
        qvariant_cast<ShortageUiSnapshot>(snapshotSpy.last().at(0)).runtime;
    QCOMPARE(state.orders.size(), 1);
    QCOMPARE(state.orders.first().state, ReplenishmentOrderState::Running);
    QVERIFY(state.orders.first().taskId >= 900000000000ULL);
}

void ShortageTestControllerTest::everyTestActionEmitsNoHardwareCommand()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ShortageTestController controller(testConfiguration(), directory.path(), nullptr,
                                      [] { return ShortageOperationResult{true, QStringLiteral("允许")}; });
    QSignalSpy rejectedSpy(&controller, &ShortageTestController::operationRejected);
    QSignalSpy snapshotSpy(&controller, &ShortageTestController::snapshotChanged);
    const QString controllerSource = readProjectFile(QStringLiteral("src/shortagetestcontroller.cpp"));
    const QString controllerTest = readProjectFile(QStringLiteral("tests/test_shortage_test_controller.cpp"));
    QVERIFY2(!controllerSource.isEmpty(), "controller source must be readable");
    QVERIFY2(!controllerSource.contains(QStringLiteral("fetchMesDayData")),
             "controller must never call MES requests directly");
    QVERIFY2(!controllerSource.contains(QStringLiteral("fetchPlcBits")),
             "controller must never call PLC requests directly");
    QVERIFY2(!controllerSource.contains(QStringLiteral("CustomSysScheduler")),
             "controller must not know the live MES/PLC scheduler type");
    const QString liveSchedulerConstruction =
        QStringLiteral("CustomSysScheduler ") + QStringLiteral("scheduler");
    QVERIFY2(!controllerTest.contains(liveSchedulerConstruction),
             "standalone controller tests must not construct live scheduler objects");

    controller.initializeZeroAfterConfirmation();
    controller.applyManualSample(ProductModel::Model88, ProductionMode::LeftRight, 0);
    controller.applyManualSample(ProductModel::Model88, ProductionMode::LeftRight, 100);
    controller.startFieldSampling();
    controller.simulateDispatchRejected();
    controller.simulateDispatchAccepted();
    controller.simulateFailureBeforeUnload();
    controller.simulateDispatchAccepted();
    controller.simulateMaterialUnloaded();
    controller.simulateFailureAfterUnload();
    controller.resendLastUnloadFact();
    controller.stop();

    QVERIFY(rejectedSpy.count() >= 1);
    QVERIFY(!snapshotSpy.isEmpty());
    const ShortageUiSnapshot snapshot =
        qvariant_cast<ShortageUiSnapshot>(snapshotSpy.last().at(0));
    QCOMPARE(snapshot.communication, ShortageCommunicationState::Stopped);
    QCOMPARE(snapshot.inputSource, ShortageInputSource::Mock);
}

void ShortageTestControllerTest::failedFieldSamplingGuardKeepsControllerInactive()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ShortageTestController controller(testConfiguration(), directory.path(), nullptr,
                                      [] {
                                          return ShortageOperationResult{
                                              false, QStringLiteral("正式 Live 未停止")};
                                      });
    QSignalSpy rejectedSpy(&controller, &ShortageTestController::operationRejected);
    QSignalSpy snapshotSpy(&controller, &ShortageTestController::snapshotChanged);

    controller.startFieldSampling();

    QCOMPARE(rejectedSpy.count(), 1);
    QCOMPARE(snapshotSpy.count(), 1);
    const ShortageUiSnapshot snapshot =
        qvariant_cast<ShortageUiSnapshot>(snapshotSpy.last().at(0));
    QCOMPARE(snapshot.communication, ShortageCommunicationState::Stopped);
    QCOMPARE(snapshot.inputSource, ShortageInputSource::Mock);
}

void ShortageTestControllerTest::inactiveContextConfirmationDoesNotReachEngine()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ShortageSampleCoordinator coordinator(nullptr);
    ShortageTestController controller(testConfiguration(), directory.path(), &coordinator,
                                      [] { return ShortageOperationResult{true, QStringLiteral("允许")}; });
    QSignalSpy snapshotSpy(&controller, &ShortageTestController::snapshotChanged);

    QMetaObject::invokeMethod(&coordinator,
                              "contextChangeConfirmed",
                              Qt::DirectConnection,
                              Q_ARG(ProductModel, ProductModel::Model88R),
                              Q_ARG(ProductionMode, ProductionMode::LeftOnly));

    QCOMPARE(snapshotSpy.count(), 0);
}

void ShortageTestControllerTest::testSaveClearReloadKeepsProductionHash()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    ShortageStateStore productionStore(directory.path(), ShortageStateNamespace::Production);
    ShortageAuditEvent event;
    event.sequence = 1;
    event.occurredAtUtc = utc(1);
    event.eventType = QStringLiteral("production_seed");
    event.messageZh = QStringLiteral("正式状态种子");
    QVERIFY2(productionStore.saveCritical(productionState(), event).ok, "production seed failed");
    const QByteArray before = productionHash(directory.path());

    ShortageTestController controller(testConfiguration(), directory.path(), nullptr,
                                      [] { return ShortageOperationResult{true, QStringLiteral("允许")}; });
    controller.initializeZeroAfterConfirmation();
    controller.applyManualSample(ProductModel::Model88, ProductionMode::LeftRight, 0);
    controller.saveTestState();
    controller.clearTestStateAfterConfirmation();
    controller.reloadTestState();

    QCOMPARE(productionHash(directory.path()), before);
}

void ShortageTestControllerTest::fullScenarioCoversSamplePlanUnloadFailureRestart()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray productionBefore = productionHash(directory.path());

    ShortageTestController controller(testConfiguration(), directory.path(), nullptr,
                                      [] { return ShortageOperationResult{true, QStringLiteral("允许")}; });
    QSignalSpy rejectedSpy(&controller, &ShortageTestController::operationRejected);
    QSignalSpy snapshotSpy(&controller, &ShortageTestController::snapshotChanged);

    controller.initializeZeroAfterConfirmation();
    controller.applyManualSample(ProductModel::Model88, ProductionMode::LeftRight, 0);
    controller.applyManualSample(ProductModel::Model88, ProductionMode::LeftRight, 100);
    controller.simulateDispatchRejected();
    controller.simulateDispatchAccepted();
    controller.simulateFailureBeforeUnload();
    controller.simulateDispatchAccepted();
    controller.simulateMaterialUnloaded();
    controller.simulateFailureAfterUnload();
    controller.applyManualSample(ProductModel::Model88, ProductionMode::LeftRight, 130);
    controller.simulateDispatchAccepted();
    controller.simulateMaterialUnloaded();
    controller.resendLastUnloadFact();
    controller.saveTestState();

    QVERIFY(!snapshotSpy.isEmpty());
    ShortageUiSnapshot snapshot = qvariant_cast<ShortageUiSnapshot>(snapshotSpy.last().at(0));
    QVERIFY(snapshot.runtime.criticalLock);
    QVERIFY(snapshot.runtime.criticalReasonZh.contains(QStringLiteral("倒料")));
    const ShortageStationRuntime *stationOne = station(snapshot.runtime, 1);
    QVERIFY(stationOne != nullptr);
    QCOMPARE(stationOne->stock, qint64{72});

    ShortageTestController restarted(testConfiguration(), directory.path(), nullptr,
                                     [] { return ShortageOperationResult{true, QStringLiteral("允许")}; });
    QSignalSpy restartSpy(&restarted, &ShortageTestController::snapshotChanged);
    restarted.reloadTestState();

    QVERIFY(!restartSpy.isEmpty());
    snapshot = qvariant_cast<ShortageUiSnapshot>(restartSpy.last().at(0));
    QVERIFY(snapshot.runtime.criticalLock);
    QCOMPARE(station(snapshot.runtime, 1)->stock, qint64{72});
    QCOMPARE(productionHash(directory.path()), productionBefore);
    QVERIFY(rejectedSpy.count() >= 1);
}

QTEST_MAIN(ShortageTestControllerTest)
#include "test_shortage_test_controller.moc"
