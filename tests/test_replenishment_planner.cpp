#include "replenishmentplanner.h"
#include "shortageengine.h"

#include <limits>

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
    ShortageConfiguration configuration;
    configuration.revision = 505;
    configuration.parameters.preUnloadFailureLimit = 3;

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
            station.sitePosition = QStringLiteral("P%1").arg(stationId);
            station.partNumber = QStringLiteral("PN%1").arg(stationId);
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

ShortageRuntimeState initializedState(qint64 stock = 100)
{
    ShortageRuntimeState state;
    state.configurationRevision = 505;
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

ShortageStationRuntime *station(ShortageRuntimeState *state, int stationId)
{
    for (ShortageStationRuntime &item : state->stations) {
        if (item.stationId == stationId)
            return &item;
    }
    return nullptr;
}

const ShortageStationRuntime *station(const ShortageRuntimeState &state, int stationId)
{
    for (const ShortageStationRuntime &item : state.stations) {
        if (item.stationId == stationId)
            return &item;
    }
    return nullptr;
}

const ReplenishmentOrder *orderByNo(const ShortageRuntimeState &state, quint64 orderNo)
{
    for (const ReplenishmentOrder &order : state.orders) {
        if (order.orderNo == orderNo)
            return &order;
    }
    return nullptr;
}

TaskFact fact(TaskFactKind kind, quint64 orderNo, quint64 taskId, int stationId,
              qint64 seconds = 1000)
{
    TaskFact value;
    value.kind = kind;
    value.replenishmentOrderNo = orderNo;
    value.taskId = taskId;
    value.stationId = stationId;
    value.origin = ReplenishmentOrigin::Automatic;
    value.occurredAtUtc = utc(seconds);
    value.reasonZh = QStringLiteral("测试事实");
    return value;
}

void makeQueuedOrder(ShortageRuntimeState *state, int stationId, quint64 orderNo,
                     quint64 taskId)
{
    ReplenishmentOrder order;
    order.orderNo = orderNo;
    order.stationId = stationId;
    order.origin = ReplenishmentOrigin::Automatic;
    order.state = ReplenishmentOrderState::Queued;
    order.taskId = taskId;
    order.createdAtUtc = utc(10 + int(orderNo));
    state->orders.append(order);
    state->nextReplenishmentOrderNo = std::max(state->nextReplenishmentOrderNo, orderNo + 1);
}

void addAwaitingOrder(ShortageRuntimeState *state, int stationId, quint64 orderNo)
{
    ReplenishmentOrder order;
    order.orderNo = orderNo;
    order.stationId = stationId;
    order.origin = ReplenishmentOrigin::Automatic;
    order.state = ReplenishmentOrderState::AwaitingDispatch;
    order.createdAtUtc = utc(10 + int(orderNo));
    state->orders.append(order);
    state->nextReplenishmentOrderNo = std::max(state->nextReplenishmentOrderNo, orderNo + 1);
}

} // namespace

class ReplenishmentPlannerTest final : public QObject
{
    Q_OBJECT
private slots:
    void belowMinimumTriggersButEqualityDoesNot();       // PL-01。
    void reachingMaximumStopsWithoutExtraBox();          // PL-02。
    void triggerTimeThenStationIdDefinesStableOrder();   // PL-03。
    void activeStationIsNotPreemptedBeforeMaximum();     // PL-04。
    void oneStationHasAtMostOneNotUnloadedTask();        // PL-05。
    void rejectedDispatchRetriesSameOrderNumber();       // PL-06。
    void duplicateUnloadLocksAndDoesNotAddSecondBox();   // TK-04。
    void unknownOrMismatchedTaskLocksWithoutMutation();  // TK-05。
    void systemErrorReleasesNotUnloadedOccupation();     // TK-06/PS-11。
    void thirdPreUnloadFailurePausesOnlyThatStation();   // TK-07/ER-03。
    void manualHighStockOrderRequiresRiskConfirmation(); // TK-08。
    void acceptedDispatchPersistsUnsafeRestoreLock();    // PS-15/PL-06：重启后不允许普通恢复已入队任务。
    void pendingContextWaitsForOldTasksThenDeducts();    // PS-11：旧任务排空前只保存待切换产量。
    void restoredStateInstallsOnlyAfterFullValidation(); // PS-15：恢复值只能经 Engine 安装。
    void unsafeRestoreLocksWithoutReplacingEngineState();// PS-15：不安全恢复不发布部分状态。
};

void ReplenishmentPlannerTest::belowMinimumTriggersButEqualityDoesNot()
{
    ShortageRuntimeState state = initializedState(100);
    station(&state, 1)->stock = 9;
    station(&state, 2)->stock = 10;

    ReplenishmentPlanner planner(testConfiguration());
    const PlannerApplyResult result = planner.reevaluate(&state, utc(100));

    QVERIFY2(result.ok, qPrintable(result.messageZh));
    QCOMPARE(state.waitingStationIds, QList<int>({1}));
    QCOMPARE(state.activeStationId, 1);
    QCOMPARE(state.orders.size(), 1);
    QCOMPARE(state.orders[0].stationId, 1);
    QCOMPARE(state.orders[0].orderNo, quint64{1});
}

void ReplenishmentPlannerTest::reachingMaximumStopsWithoutExtraBox()
{
    ShortageRuntimeState state = initializedState(100);
    station(&state, 3)->stock = 9;

    ReplenishmentPlanner planner(testConfiguration());
    QVERIFY(planner.reevaluate(&state, utc(100)).ok);
    QCOMPARE(state.activeStationId, 3);
    QCOMPARE(state.orders.size(), 1);

    station(&state, 3)->stock = 200;
    const PlannerApplyResult result = planner.reevaluate(&state, utc(200));

    QVERIFY2(result.ok, qPrintable(result.messageZh));
    QCOMPARE(state.activeStationId, 0);
    QVERIFY(state.waitingStationIds.isEmpty());
    QCOMPARE(state.orders.size(), 1);
    QVERIFY(!planner.nextDispatchRequest(state).has_value());
}

void ReplenishmentPlannerTest::triggerTimeThenStationIdDefinesStableOrder()
{
    ShortageRuntimeState state = initializedState(100);
    ReplenishmentPlanner planner(testConfiguration());

    station(&state, 5)->stock = 8;
    QVERIFY(planner.reevaluate(&state, utc(100)).ok);
    station(&state, 2)->stock = 8;
    station(&state, 4)->stock = 8;
    QVERIFY(planner.reevaluate(&state, utc(200)).ok);

    QCOMPARE(state.waitingStationIds, QList<int>({5, 2, 4}));
    QCOMPARE(station(state, 5)->firstLowAtUtc, utc(100));
    QCOMPARE(station(state, 2)->firstLowAtUtc, utc(200));
    QCOMPARE(station(state, 4)->firstLowAtUtc, utc(200));
}

void ReplenishmentPlannerTest::activeStationIsNotPreemptedBeforeMaximum()
{
    ShortageRuntimeState state = initializedState(100);
    ReplenishmentPlanner planner(testConfiguration());

    station(&state, 6)->stock = 8;
    QVERIFY(planner.reevaluate(&state, utc(100)).ok);
    station(&state, 1)->stock = -50;
    QVERIFY(planner.reevaluate(&state, utc(200)).ok);

    QCOMPARE(state.activeStationId, 6);
    QVERIFY(planner.nextDispatchRequest(state).has_value());
    QCOMPARE(planner.nextDispatchRequest(state)->stationId, 6);
}

void ReplenishmentPlannerTest::oneStationHasAtMostOneNotUnloadedTask()
{
    ShortageRuntimeState state = initializedState(100);
    ReplenishmentPlanner planner(testConfiguration());
    station(&state, 7)->stock = 0;
    QVERIFY(planner.reevaluate(&state, utc(100)).ok);
    const quint64 orderNo = state.orders.first().orderNo;
    QVERIFY(planner.markDispatchResult(&state, orderNo, true, 7001, QString()).ok);
    QVERIFY(planner.reevaluate(&state, utc(200)).ok);

    QCOMPARE(state.orders.size(), 1);
    QVERIFY(!planner.nextDispatchRequest(state).has_value());
}

void ReplenishmentPlannerTest::rejectedDispatchRetriesSameOrderNumber()
{
    ShortageRuntimeState state = initializedState(100);
    ReplenishmentPlanner planner(testConfiguration());
    station(&state, 8)->stock = 0;
    QVERIFY(planner.reevaluate(&state, utc(100)).ok);
    const quint64 orderNo = planner.nextDispatchRequest(state)->replenishmentOrderNo;

    const PlannerApplyResult rejected =
        planner.markDispatchResult(&state, orderNo, false, 0, QStringLiteral("FIFO满"));

    QVERIFY2(rejected.ok, qPrintable(rejected.messageZh));
    QVERIFY2(rejected.changed, qPrintable(rejected.messageZh));
    QCOMPARE(state.orders.size(), 1);
    QCOMPARE(state.orders.first().state, ReplenishmentOrderState::AwaitingDispatch);
    QCOMPARE(planner.nextDispatchRequest(state)->replenishmentOrderNo, orderNo);
    QCOMPARE(state.nextReplenishmentOrderNo, orderNo + 1);
}

void ReplenishmentPlannerTest::duplicateUnloadLocksAndDoesNotAddSecondBox()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ShortageStateStore store(dir.path(), ShortageStateNamespace::Production);
    ShortageEngine engine(testConfiguration(), &store);
    QVERIFY(engine.initializeZero(true, utc(1)).ok);
    QVERIFY(engine.requestManualBox(4, true, utc(2)).ok);
    const ShortageDispatchRequest request = engine.nextDispatchRequest().value();
    QVERIFY(engine.recordDispatchResult(request.replenishmentOrderNo, true, 4001, QString()).ok);
    QVERIFY(engine.recordTaskStarted(fact(TaskFactKind::Started, request.replenishmentOrderNo, 4001, 4)).ok);

    const ShortageEngineResult first =
        engine.recordMaterialUnloaded(fact(TaskFactKind::MaterialUnloaded,
                                           request.replenishmentOrderNo, 4001, 4, 10));
    QVERIFY2(first.ok, qPrintable(first.messageZh));
    QCOMPARE(station(engine.state(), 4)->stock, qint64{104});

    const ShortageEngineResult duplicate =
        engine.recordMaterialUnloaded(fact(TaskFactKind::MaterialUnloaded,
                                           request.replenishmentOrderNo, 4001, 4, 11));
    QVERIFY(!duplicate.ok);
    QVERIFY(duplicate.criticalLock);
    QCOMPARE(station(engine.state(), 4)->stock, qint64{104});
    QVERIFY(!engine.nextDispatchRequest().has_value());
}

void ReplenishmentPlannerTest::unknownOrMismatchedTaskLocksWithoutMutation()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ShortageStateStore store(dir.path(), ShortageStateNamespace::Production);
    ShortageEngine engine(testConfiguration(), &store);
    QVERIFY(engine.initializeZero(true, utc(1)).ok);
    QVERIFY(engine.requestManualBox(5, true, utc(2)).ok);
    const ShortageDispatchRequest request = engine.nextDispatchRequest().value();
    QVERIFY(engine.recordDispatchResult(request.replenishmentOrderNo, true, 5001, QString()).ok);
    const ShortageRuntimeState before = engine.state();

    const ShortageEngineResult result =
        engine.recordTaskStarted(fact(TaskFactKind::Started, request.replenishmentOrderNo, 9999, 5));

    QVERIFY(!result.ok);
    QVERIFY(result.criticalLock);
    QCOMPARE(station(engine.state(), 5)->stock, station(before, 5)->stock);
    QCOMPARE(orderByNo(engine.state(), request.replenishmentOrderNo)->state,
             ReplenishmentOrderState::Queued);
}

void ReplenishmentPlannerTest::systemErrorReleasesNotUnloadedOccupation()
{
    ShortageRuntimeState state = initializedState(100);
    ReplenishmentPlanner planner(testConfiguration());
    makeQueuedOrder(&state, 9, 90, 9001);
    state.activeStationId = 9;
    QVERIFY(planner.markTaskStarted(&state, fact(TaskFactKind::Started, 90, 9001, 9)).ok);

    const PlannerApplyResult result =
        planner.markTerminal(&state, fact(TaskFactKind::SystemError, 90, 9001, 9),
                             testConfiguration().parameters.preUnloadFailureLimit);

    QVERIFY2(result.ok, qPrintable(result.messageZh));
    QCOMPARE(orderByNo(state, 90)->state, ReplenishmentOrderState::Canceled);
    QCOMPARE(state.activeStationId, 0);
}

void ReplenishmentPlannerTest::thirdPreUnloadFailurePausesOnlyThatStation()
{
    ShortageRuntimeState state = initializedState(100);
    ReplenishmentPlanner planner(testConfiguration());
    state.activeStationId = 10;
    for (int i = 1; i <= 3; ++i) {
        makeQueuedOrder(&state, 10, quint64(100 + i), quint64(1000 + i));
        QVERIFY(planner.markTaskStarted(&state, fact(TaskFactKind::Started, quint64(100 + i),
                                                    quint64(1000 + i), 10, i)).ok);
        QVERIFY(planner.markTerminal(&state, fact(TaskFactKind::Failed, quint64(100 + i),
                                                  quint64(1000 + i), 10, 10 + i),
                                     3).ok);
    }

    QCOMPARE(station(state, 10)->consecutivePreUnloadFailures, 3);
    QVERIFY(station(state, 10)->automaticPaused);
    QVERIFY(!station(state, 11)->automaticPaused);
    QCOMPARE(state.activeStationId, 0);
}

void ReplenishmentPlannerTest::manualHighStockOrderRequiresRiskConfirmation()
{
    ShortageRuntimeState state = initializedState(100);
    ReplenishmentPlanner planner(testConfiguration());

    PlannerApplyResult result = planner.requestManualBox(&state, 11, false, utc(100));
    QVERIFY(!result.ok);
    QCOMPARE(state.orders.size(), 0);

    result = planner.requestManualBox(&state, 11, true, utc(101));
    QVERIFY2(result.ok, qPrintable(result.messageZh));
    QCOMPARE(state.orders.size(), 1);
    QCOMPARE(state.orders.first().origin, ReplenishmentOrigin::Manual);
    QCOMPARE(state.orders.first().stationId, 11);
}

void ReplenishmentPlannerTest::acceptedDispatchPersistsUnsafeRestoreLock()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ShortageStateStore store(dir.path(), ShortageStateNamespace::Production);
    ShortageEngine engine(testConfiguration(), &store);
    QVERIFY(engine.initializeZero(true, utc(1)).ok);
    QVERIFY(engine.requestManualBox(6, true, utc(2)).ok);
    const ShortageDispatchRequest request = engine.nextDispatchRequest().value();
    QVERIFY(engine.recordDispatchResult(request.replenishmentOrderNo, true, 6001,
                                        QStringLiteral("已入FIFO")).ok);

    const ShortageStateLoadResult loaded = store.load();
    QVERIFY2(loaded.ok, qPrintable(loaded.messageZh));
    QCOMPARE(orderByNo(loaded.state, request.replenishmentOrderNo)->state,
             ReplenishmentOrderState::Queued);

    ShortageEngine restarted(testConfiguration(), &store);
    const ShortageEngineResult restore = restarted.installRestoredState(loaded, utc(10));
    QVERIFY(!restore.ok);
    QVERIFY(restore.criticalLock);
    QVERIFY(!restarted.nextDispatchRequest().has_value());
}

void ReplenishmentPlannerTest::pendingContextWaitsForOldTasksThenDeducts()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ShortageStateStore store(dir.path(), ShortageStateNamespace::Production);
    ShortageEngine engine(testConfiguration(), &store);
    QVERIFY(engine.initializeZero(true, utc(1)).ok);

    ShortageSample first;
    first.roundId = 1;
    first.product = ProductModel::Model88;
    first.mode = ProductionMode::LeftRight;
    first.actualQty = 1000;
    first.capturedAtUtc = utc(2);
    QVERIFY(engine.applyStableSample(first).ok);

    const ShortageDispatchRequest request = engine.nextDispatchRequest().value();
    QVERIFY(engine.recordDispatchResult(request.replenishmentOrderNo, true, 1001, QString()).ok);

    ShortageSample changed = first;
    changed.roundId = 2;
    changed.product = ProductModel::Model92;
    changed.mode = ProductionMode::LeftRight;
    changed.actualQty = 1002;
    changed.capturedAtUtc = utc(4);
    QVERIFY(engine.applyStableSample(changed).ok);

    QCOMPARE(engine.state().product, ProductModel::Model88);
    QVERIFY(engine.state().hasPendingContext);
    QCOMPARE(engine.state().pendingProduct, ProductModel::Model92);
    QVERIFY(engine.state().hasPendingActualQty);
    QCOMPARE(engine.state().pendingActualQty, qint64{1002});
    QCOMPARE(station(engine.state(), 1)->stock, qint64{0});

    QVERIFY(engine.activatePendingContextIfDrained(true, utc(5)).ok);
    QCOMPARE(engine.state().product, ProductModel::Model92);
    QVERIFY(!engine.state().hasPendingContext);
    QVERIFY(!engine.state().hasPendingActualQty);
    QCOMPARE(engine.state().actualQty.baseline, qint64{1002});
    QCOMPARE(station(engine.state(), 1)->stock, qint64{-2});
}

void ReplenishmentPlannerTest::restoredStateInstallsOnlyAfterFullValidation()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ShortageStateStore store(dir.path(), ShortageStateNamespace::Production);
    ShortageEngine engine(testConfiguration(), &store);

    ShortageRuntimeState restored = initializedState(50);
    restored.operatorConfirmedRestore = false;
    addAwaitingOrder(&restored, 2, 22);
    ShortageStateLoadResult load;
    load.ok = true;
    load.stateFound = true;
    load.source = ShortageRestoreSource::Main;
    load.state = restored;

    const ShortageEngineResult installed = engine.installRestoredState(load, utc(1));
    QVERIFY2(installed.ok, qPrintable(installed.messageZh));
    QCOMPARE(engine.state().orders.size(), 1);
    QVERIFY(!engine.nextDispatchRequest().has_value());

    QVERIFY(engine.confirmRestoredState(true, utc(2)).ok);
    QVERIFY(engine.nextDispatchRequest().has_value());
    QCOMPARE(engine.nextDispatchRequest()->replenishmentOrderNo, quint64{22});
}

void ReplenishmentPlannerTest::unsafeRestoreLocksWithoutReplacingEngineState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ShortageStateStore store(dir.path(), ShortageStateNamespace::Production);
    ShortageEngine engine(testConfiguration(), &store);
    QVERIFY(engine.initializeZero(true, utc(1)).ok);
    const ShortageRuntimeState before = engine.state();

    ShortageRuntimeState unsafe = initializedState(999);
    unsafe.configurationRevision = 12345;
    ShortageStateLoadResult load;
    load.ok = true;
    load.stateFound = true;
    load.source = ShortageRestoreSource::Main;
    load.state = unsafe;

    const ShortageEngineResult result = engine.installRestoredState(load, utc(2));
    QVERIFY(!result.ok);
    QVERIFY(result.criticalLock);
    QCOMPARE(engine.state().configurationRevision, before.configurationRevision);
    QCOMPARE(station(engine.state(), 1)->stock, station(before, 1)->stock);
    QVERIFY(!engine.nextDispatchRequest().has_value());
}

QTEST_MAIN(ReplenishmentPlannerTest)
#include "test_replenishment_planner.moc"
