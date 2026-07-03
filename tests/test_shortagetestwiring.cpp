#include <QtTest>

#include "devicemanager.h"

class ShortageTestWiringTest : public QObject
{
    Q_OBJECT

private slots:
    void deviceManager_owns_test_session_and_start_stop_it();
    void deviceManager_only_forwards_test_session_signals();
};

void ShortageTestWiringTest::deviceManager_owns_test_session_and_start_stop_it()
{
    DeviceManager manager;

    QVERIFY(manager.shortageTestSession() != nullptr);
    QVERIFY(manager.lineManager() != nullptr);
    QCOMPARE(manager.shortageTestSession()->metaObject()->indexOfSignal("shortageRequested(int)"), -1);

    manager.startShortageTest();
    QVERIFY(manager.shortageTestSession()->isRunning());

    manager.stopShortageTest();
    QVERIFY(!manager.shortageTestSession()->isRunning());
}

void ShortageTestWiringTest::deviceManager_only_forwards_test_session_signals()
{
    DeviceManager manager;
    ShortageTestSession *session = manager.shortageTestSession();
    QVERIFY(session != nullptr);

    QSignalSpy statusSpy(&manager, &DeviceManager::shortageTestStatusChanged);
    QSignalSpy sampleSpy(&manager, &DeviceManager::shortageTestSampleUpdated);
    QSignalSpy inventorySpy(&manager, &DeviceManager::shortageTestInventoryUpdated);

    const int originalQueueSize = manager.lineManager()->queueSnapshot().size();
    const QHash<QString, bool> bits = {
        {QStringLiteral("L68"), true},
        {QStringLiteral("L69"), false},
        {QStringLiteral("L71"), true},
        {QStringLiteral("L72"), false},
        {QStringLiteral("L73"), false},
        {QStringLiteral("L1998"), false},
    };
    QList<StationConsumption> stations;
    StationConsumption firstStation;
    firstStation.stationId = 1;
    firstStation.estimatedAvailable = 990;
    firstStation.safetyStock = 1000;
    firstStation.boxQuantity = 250;
    firstStation.configured = true;
    firstStation.shortage = true;
    stations.append(firstStation);

    session->statusChanged(QStringLiteral("等待生产信号稳定"), true);
    session->sampleUpdated(1005, 5, ProductModel::Model88, ProductionMode::L68, bits);
    session->inventoryUpdated(stations);

    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(sampleSpy.count(), 1);
    QCOMPARE(inventorySpy.count(), 1);
    QCOMPARE(manager.lineManager()->queueSnapshot().size(), originalQueueSize);
}

QTEST_MAIN(ShortageTestWiringTest)

#include "test_shortagetestwiring.moc"
