#include "shortageledger.h"

#include <limits>

#include <QTest>
#include <QTimeZone>

namespace {

ShortageConfiguration testConfiguration()
{
    ShortageConfiguration configuration;
    configuration.revision = 77;

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
            station.maximumStock = 2000;
            station.usageLeftRight = stationId;
            station.usageLeftOnly = stationId * 10;
            station.usageRightOnly = stationId * 100;
            configuration.stations.append(station);
        }
    }

    return configuration;
}

ShortageRuntimeState initializedState(qint64 stock = 1000)
{
    ShortageRuntimeState state;
    state.configurationRevision = 77;
    state.initialized = true;
    state.operatorConfirmedRestore = true;
    state.hasStableContext = true;
    state.product = ProductModel::Model88;
    state.mode = ProductionMode::LeftRight;
    state.stations.reserve(12);
    for (int stationId = 1; stationId <= 12; ++stationId) {
        ShortageStationRuntime station;
        station.stationId = stationId;
        station.stock = stock;
        state.stations.append(station);
    }
    return state;
}

ShortageSample sample(qint64 actualQty,
                      ProductModel product = ProductModel::Model88,
                      ProductionMode mode = ProductionMode::LeftRight)
{
    ShortageSample value;
    value.roundId = quint64(actualQty + 1000);
    value.product = product;
    value.mode = mode;
    value.actualQty = actualQty;
    value.capturedAtUtc = QDateTime::fromSecsSinceEpoch(1000 + actualQty, QTimeZone::UTC);
    return value;
}

qint64 stockOf(const ShortageRuntimeState &state, int stationId)
{
    for (const ShortageStationRuntime &station : state.stations) {
        if (station.stationId == stationId)
            return station.stock;
    }
    return std::numeric_limits<qint64>::min();
}

void expectAllStocks(const ShortageRuntimeState &state, qint64 stock)
{
    QCOMPARE(state.stations.size(), 12);
    for (const ShortageStationRuntime &station : state.stations)
        QCOMPARE(station.stock, stock);
}

} // namespace

class ShortageLedgerTest final : public QObject
{
    Q_OBJECT
private slots:
    void zeroInitializationRequiresExplicitConfirmation(); // LD-01。
    void firstSampleOnlyEstablishesBaseline();              // LD-02。
    void deltaDeductsUsageForCurrentProductAndMode();       // LD-03。
    void unchangedQtyDoesNotWriteAndStockMayBeNegative();   // LD-04。
    void automaticAndManualUnloadAddOneBoxOnly();           // LD-05。
    void failuresBeforeAndAfterUnloadKeepExactFacts();      // LD-06。
    void resetCandidateDistinguishesResetFromGlitch();      // LD-07。
    void arithmeticOverflowLocksWithoutPartialMutation();   // LD-08。
    void recoveredContextChangeRequiresMaintenance();       // ER-01/ER-04/ER-05。
};

void ShortageLedgerTest::zeroInitializationRequiresExplicitConfirmation()
{
    ShortageLedger ledger(testConfiguration());
    ShortageRuntimeState state = initializedState(123);
    state.initialized = false;
    state.configurationRevision = 0;

    const QDateTime now = QDateTime::fromSecsSinceEpoch(1, QTimeZone::UTC);
    const LedgerApplyResult rejected = ledger.initializeZero(&state, false, now);

    QVERIFY(!rejected.ok);
    QVERIFY(!rejected.changed);
    QVERIFY(!rejected.criticalLock);
    QCOMPARE(state.initialized, false);
    QCOMPARE(state.configurationRevision, quint64{0});
    expectAllStocks(state, 123);

    const LedgerApplyResult accepted = ledger.initializeZero(&state, true, now);

    QVERIFY2(accepted.ok, qPrintable(accepted.messageZh));
    QVERIFY(accepted.changed);
    QVERIFY(!accepted.criticalLock);
    QCOMPARE(state.initialized, true);
    QCOMPARE(state.configurationRevision, quint64{77});
    expectAllStocks(state, 0);
}

void ShortageLedgerTest::firstSampleOnlyEstablishesBaseline()
{
    ShortageLedger ledger(testConfiguration());
    ShortageRuntimeState state = initializedState(500);

    const LedgerApplyResult result = ledger.applyStableSample(&state, sample(1000));

    QVERIFY2(result.ok, qPrintable(result.messageZh));
    QVERIFY(result.changed);
    QVERIFY(!result.hasProductionDelta);
    QCOMPARE(result.productionDelta, qint64{0});
    QCOMPARE(state.actualQty.hasBaseline, true);
    QCOMPARE(state.actualQty.baseline, qint64{1000});
    expectAllStocks(state, 500);
}

void ShortageLedgerTest::deltaDeductsUsageForCurrentProductAndMode()
{
    ShortageLedger ledger(testConfiguration());
    ShortageRuntimeState state = initializedState(1000);
    state.actualQty.hasBaseline = true;
    state.actualQty.baseline = 1000;
    state.product = ProductModel::Model88R;
    state.mode = ProductionMode::LeftOnly;

    const LedgerApplyResult result =
        ledger.applyStableSample(&state, sample(1002, ProductModel::Model88R,
                                                ProductionMode::LeftOnly));

    QVERIFY2(result.ok, qPrintable(result.messageZh));
    QVERIFY(result.changed);
    QVERIFY(result.hasProductionDelta);
    QCOMPARE(result.productionDelta, qint64{2});
    QCOMPARE(state.actualQty.baseline, qint64{1002});
    QCOMPARE(stockOf(state, 1), qint64{980});
    QCOMPARE(stockOf(state, 12), qint64{760});
}

void ShortageLedgerTest::unchangedQtyDoesNotWriteAndStockMayBeNegative()
{
    ShortageLedger ledger(testConfiguration());
    ShortageRuntimeState state = initializedState(5);
    state.actualQty.hasBaseline = true;
    state.actualQty.baseline = 10;

    const LedgerApplyResult deducted = ledger.applyStableSample(&state, sample(11));
    QVERIFY2(deducted.ok, qPrintable(deducted.messageZh));
    QVERIFY(deducted.changed);
    QCOMPARE(stockOf(state, 12), qint64{-7});

    const ShortageRuntimeState afterDeduct = state;
    const LedgerApplyResult unchanged = ledger.applyStableSample(&state, sample(11));

    QVERIFY2(unchanged.ok, qPrintable(unchanged.messageZh));
    QVERIFY(!unchanged.changed);
    QVERIFY(!unchanged.hasProductionDelta);
    QCOMPARE(state.actualQty.baseline, afterDeduct.actualQty.baseline);
    QCOMPARE(stockOf(state, 12), stockOf(afterDeduct, 12));
}

void ShortageLedgerTest::automaticAndManualUnloadAddOneBoxOnly()
{
    ShortageLedger ledger(testConfiguration());
    ShortageRuntimeState state = initializedState(0);

    const LedgerApplyResult automatic =
        ledger.recordUnloadedBox(&state, 3, ReplenishmentOrigin::Automatic,
                                 QDateTime::fromSecsSinceEpoch(10, QTimeZone::UTC));

    QVERIFY2(automatic.ok, qPrintable(automatic.messageZh));
    QVERIFY(automatic.changed);
    QCOMPARE(stockOf(state, 3), qint64{103});

    const LedgerApplyResult manual =
        ledger.recordUnloadedBox(&state, 3, ReplenishmentOrigin::Manual,
                                 QDateTime::fromSecsSinceEpoch(20, QTimeZone::UTC));

    QVERIFY2(manual.ok, qPrintable(manual.messageZh));
    QVERIFY(manual.changed);
    QCOMPARE(stockOf(state, 3), qint64{206});
}

void ShortageLedgerTest::failuresBeforeAndAfterUnloadKeepExactFacts()
{
    ShortageLedger ledger(testConfiguration());
    ShortageRuntimeState beforeUnload = initializedState(10);

    ReplenishmentOrder failedBefore;
    failedBefore.orderNo = 1;
    failedBefore.stationId = 4;
    failedBefore.state = ReplenishmentOrderState::FailedBeforeUnload;
    failedBefore.unloadAccounted = false;
    beforeUnload.orders.append(failedBefore);

    QCOMPARE(stockOf(beforeUnload, 4), qint64{10});

    ShortageRuntimeState afterUnload = beforeUnload;
    const LedgerApplyResult unload =
        ledger.recordUnloadedBox(&afterUnload, 4, ReplenishmentOrigin::Automatic,
                                 QDateTime::fromSecsSinceEpoch(30, QTimeZone::UTC));
    QVERIFY2(unload.ok, qPrintable(unload.messageZh));

    ReplenishmentOrder failedAfter = failedBefore;
    failedAfter.orderNo = 2;
    failedAfter.state = ReplenishmentOrderState::FailedAfterUnload;
    failedAfter.unloadAccounted = true;
    afterUnload.orders.append(failedAfter);

    QCOMPARE(stockOf(afterUnload, 4), qint64{114});
}

void ShortageLedgerTest::resetCandidateDistinguishesResetFromGlitch()
{
    ShortageLedger ledger(testConfiguration());

    ShortageRuntimeState directReset = initializedState(1000);
    QVERIFY(ledger.applyStableSample(&directReset, sample(1000)).ok);
    LedgerApplyResult direct = ledger.applyStableSample(&directReset, sample(2));
    QVERIFY2(direct.ok, qPrintable(direct.messageZh));
    QVERIFY(direct.changed);
    QVERIFY(directReset.actualQty.hasResetCandidate);
    direct = ledger.applyStableSample(&directReset, sample(5));
    QVERIFY2(direct.ok, qPrintable(direct.messageZh));
    QCOMPARE(direct.productionDelta, qint64{5});
    QVERIFY2(direct.messageZh.contains(QStringLiteral("oldBaseline=1000")),
             qPrintable(direct.messageZh));
    QVERIFY2(direct.messageZh.contains(QStringLiteral("candidate=2")),
             qPrintable(direct.messageZh));
    QVERIFY2(direct.messageZh.contains(QStringLiteral("actualQty=5")),
             qPrintable(direct.messageZh));
    QVERIFY2(direct.messageZh.contains(QStringLiteral("处理动作")),
             qPrintable(direct.messageZh));
    QCOMPARE(directReset.actualQty.baseline, qint64{5});
    QCOMPARE(stockOf(directReset, 1), qint64{995});

    ShortageRuntimeState stagedReset = initializedState(1000);
    QVERIFY(ledger.applyStableSample(&stagedReset, sample(1000)).ok);
    QVERIFY(ledger.applyStableSample(&stagedReset, sample(900)).ok);
    QCOMPARE(stagedReset.actualQty.resetCandidate, qint64{900});
    QCOMPARE(stagedReset.actualQty.baseline, qint64{1000});
    QCOMPARE(stockOf(stagedReset, 1), qint64{1000});
    QVERIFY(ledger.applyStableSample(&stagedReset, sample(500)).ok);
    QCOMPARE(stagedReset.actualQty.resetCandidate, qint64{500});
    QCOMPARE(stagedReset.actualQty.baseline, qint64{1000});
    QCOMPARE(stockOf(stagedReset, 1), qint64{1000});
    QVERIFY(ledger.applyStableSample(&stagedReset, sample(0)).ok);
    QCOMPARE(stagedReset.actualQty.resetCandidate, qint64{0});
    QCOMPARE(stagedReset.actualQty.baseline, qint64{1000});
    QCOMPARE(stockOf(stagedReset, 1), qint64{1000});
    LedgerApplyResult staged = ledger.applyStableSample(&stagedReset, sample(2));
    QVERIFY2(staged.ok, qPrintable(staged.messageZh));
    QCOMPARE(staged.productionDelta, qint64{2});
    QCOMPARE(stagedReset.actualQty.baseline, qint64{2});
    QCOMPARE(stockOf(stagedReset, 1), qint64{998});
    staged = ledger.applyStableSample(&stagedReset, sample(5));
    QVERIFY2(staged.ok, qPrintable(staged.messageZh));
    QCOMPARE(staged.productionDelta, qint64{3});
    QCOMPARE(stagedReset.actualQty.baseline, qint64{5});
    QCOMPARE(stockOf(stagedReset, 1), qint64{995});

    ShortageRuntimeState glitch = initializedState(1000);
    QVERIFY(ledger.applyStableSample(&glitch, sample(1000)).ok);
    LedgerApplyResult glitchResult = ledger.applyStableSample(&glitch, sample(2));
    QVERIFY2(glitchResult.ok, qPrintable(glitchResult.messageZh));
    QVERIFY(glitchResult.changed);
    QCOMPARE(glitch.actualQty.baseline, qint64{1000});
    QCOMPARE(stockOf(glitch, 1), qint64{1000});
    glitchResult = ledger.applyStableSample(&glitch, sample(1005));
    QVERIFY2(glitchResult.ok, qPrintable(glitchResult.messageZh));
    QCOMPARE(glitchResult.productionDelta, qint64{5});
    QCOMPARE(glitch.actualQty.baseline, qint64{1005});
    QCOMPARE(stockOf(glitch, 1), qint64{995});
}

void ShortageLedgerTest::arithmeticOverflowLocksWithoutPartialMutation()
{
    ShortageConfiguration configuration = testConfiguration();
    for (ShortageStationConfig &station : configuration.stations) {
        if (station.product == ProductModel::Model92 && station.stationId == 12)
            station.usageRightOnly = std::numeric_limits<qint64>::max();
    }

    ShortageLedger ledger(configuration);
    ShortageRuntimeState state = initializedState(1000);
    state.actualQty.hasBaseline = true;
    state.actualQty.baseline = 1;
    state.product = ProductModel::Model92;
    state.mode = ProductionMode::RightOnly;
    const ShortageRuntimeState before = state;

    const LedgerApplyResult result =
        ledger.applyStableSample(&state, sample(3, ProductModel::Model92,
                                                ProductionMode::RightOnly));

    QVERIFY(!result.ok);
    QVERIFY(!result.changed);
    QVERIFY(result.criticalLock);
    QCOMPARE(state.criticalLock, before.criticalLock);
    QCOMPARE(state.actualQty.baseline, before.actualQty.baseline);
    QCOMPARE(state.actualQty.hasResetCandidate, before.actualQty.hasResetCandidate);
    QCOMPARE(stockOf(state, 1), stockOf(before, 1));
    QCOMPARE(stockOf(state, 12), stockOf(before, 12));
}

void ShortageLedgerTest::recoveredContextChangeRequiresMaintenance()
{
    ShortageLedger ledger(testConfiguration());
    ShortageRuntimeState state = initializedState(1000);
    state.product = ProductModel::Model88;
    state.mode = ProductionMode::LeftRight;
    state.actualQty.hasBaseline = true;
    state.actualQty.baseline = 1000;
    state.actualQty.interrupted = true;
    const ShortageRuntimeState before = state;

    ShortageSample recovered = sample(1005, ProductModel::Model88R,
                                      ProductionMode::LeftOnly);
    recovered.recoveredAfterInterruption = true;
    const LedgerApplyResult result = ledger.applyStableSample(&state, recovered);

    QVERIFY(!result.ok);
    QVERIFY(!result.changed);
    QVERIFY(result.criticalLock);
    QVERIFY2(result.messageZh.contains(QStringLiteral("断线恢复")),
             qPrintable(result.messageZh));
    QVERIFY2(result.messageZh.contains(QStringLiteral("oldProduct")),
             qPrintable(result.messageZh));
    QVERIFY2(result.messageZh.contains(QStringLiteral("newProduct")),
             qPrintable(result.messageZh));
    QVERIFY2(result.messageZh.contains(QStringLiteral("处理动作")),
             qPrintable(result.messageZh));
    QCOMPARE(state.product, before.product);
    QCOMPARE(state.mode, before.mode);
    QCOMPARE(state.actualQty.baseline, before.actualQty.baseline);
    QCOMPARE(stockOf(state, 1), stockOf(before, 1));
}

QTEST_MAIN(ShortageLedgerTest)
#include "test_shortage_ledger.moc"
