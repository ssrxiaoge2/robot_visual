#include <QtTest>

#include "devicemanager.h"

class ShortageTestWiringTest : public QObject
{
    Q_OBJECT

private slots:
    void deviceManager_owns_test_session_and_start_stop_it();
    void deviceManager_only_forwards_test_session_signals();
    void deviceManager_start_live_shortage_stops_test_session_and_runs_live_monitor();
    void deviceManager_live_shortage_dispatch_wiring_stays_single_after_restart();
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

void ShortageTestWiringTest::deviceManager_start_live_shortage_stops_test_session_and_runs_live_monitor()
{
    DeviceManager manager;

    QVERIFY(manager.shortageTestSession() != nullptr);
    QVERIFY(manager.shortageMonitor() != nullptr);

    manager.startShortageTest();
    QVERIFY(manager.shortageTestSession()->isRunning());
    QVERIFY(!manager.shortageMonitor()->isRunning());

    manager.startLiveShortage();

    // 修改前只有测试会话存在，不需要验证与生产会话的互斥关系。
    // 现在真实缺料会话接入主调度后，启动真实模式必须先停测试模式，
    // 否则两套轮询会共享同一个 CustomSysScheduler，现场会出现重复请求和状态混淆。
    // 这里只验证生命周期互斥，不触碰任何 FIFO 或库存逻辑，因此不会影响 e1ffb3f 主流程。
    QVERIFY(!manager.shortageTestSession()->isRunning());
    QVERIFY(manager.shortageMonitor()->isRunning());

    manager.stopLiveShortage();
    QVERIFY(!manager.shortageMonitor()->isRunning());
}

void ShortageTestWiringTest::deviceManager_live_shortage_dispatch_wiring_stays_single_after_restart()
{
    DeviceManager manager;
    QVERIFY(manager.shortageMonitor() != nullptr);
    QVERIFY(manager.lineManager() != nullptr);

    manager.startLiveShortage();
    manager.stopLiveShortage();
    manager.startLiveShortage();

    const int beforeQueueSize = manager.lineManager()->queueSnapshot().size();

    // 修改前没有真实缺料到 FIFO 的生产接线，这里不会有任何队列变化。
    // 现在通过直接触发 ShortageMonitor::dispatchRequested，验证 DeviceManager 构造期
    // 建立的一次性 connect 仍然有效，并且重复 start/stop 不会重复 connect 成两次派单。
    // 由于 LineManager 在 Idle 也允许入队，这里不要求启动主调度即可验证“只入一单”。
    QVERIFY(QMetaObject::invokeMethod(manager.shortageMonitor(),
                                      "dispatchRequested",
                                      Qt::DirectConnection,
                                      Q_ARG(int, 3)));

    const QList<Task> queuedTasks = manager.lineManager()->queueSnapshot();
    QCOMPARE(queuedTasks.size(), beforeQueueSize + 1);
    QCOMPARE(queuedTasks.constLast().stationId, 3);
    QCOMPARE(queuedTasks.constLast().source, TaskSource::CustomerSystem);

    QVERIFY(QMetaObject::invokeMethod(manager.shortageMonitor(),
                                      "dispatchRequested",
                                      Qt::DirectConnection,
                                      Q_ARG(int, 4)));
    const QList<Task> queuedAgain = manager.lineManager()->queueSnapshot();
    QCOMPARE(queuedAgain.size(), beforeQueueSize + 2);
    QCOMPARE(queuedAgain.constLast().stationId, 4);
    QCOMPARE(queuedAgain.constLast().source, TaskSource::CustomerSystem);
}

QTEST_MAIN(ShortageTestWiringTest)

#include "test_shortagetestwiring.moc"
