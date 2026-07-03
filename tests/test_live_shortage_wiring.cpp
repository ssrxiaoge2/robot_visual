#include <QtTest>

#include "devicemanager.h"

class LiveShortageWiringTest : public QObject
{
    Q_OBJECT

private slots:
    void deviceManager_owns_live_monitor_and_keeps_test_session_separate();
    void startLiveShortage_stops_test_session_and_stop_preserves_monitor_instance();
    void dispatchRequested_flows_into_lineManager_and_lifecycle_returns_to_monitor();
};

void LiveShortageWiringTest::deviceManager_owns_live_monitor_and_keeps_test_session_separate()
{
    DeviceManager manager;

    QVERIFY(manager.shortageTestSession() != nullptr);
    QVERIFY(manager.shortageMonitor() != nullptr);
    QVERIFY(manager.lineManager() != nullptr);
    QCOMPARE(manager.shortageTestSession()->metaObject()->indexOfSignal("shortageRequested(int)"), -1);
}

void LiveShortageWiringTest::startLiveShortage_stops_test_session_and_stop_preserves_monitor_instance()
{
    DeviceManager manager;
    ShortageMonitor *monitor = manager.shortageMonitor();
    QVERIFY(monitor != nullptr);

    manager.startShortageTest();
    QVERIFY(manager.shortageTestSession()->isRunning());

    manager.startLiveShortage();
    QVERIFY(!manager.shortageTestSession()->isRunning());
    QVERIFY(monitor->isRunning());

    const QList<LiveShortageStationSnapshot> beforeStop = monitor->snapshot();
    manager.stopLiveShortage();
    QVERIFY(!monitor->isRunning());
    QCOMPARE(manager.shortageMonitor(), monitor);
    QCOMPARE(monitor->snapshot().size(), beforeStop.size());
}

void LiveShortageWiringTest::dispatchRequested_flows_into_lineManager_and_lifecycle_returns_to_monitor()
{
    qRegisterMetaType<Task>("Task");

    DeviceManager manager;
    ShortageMonitor *monitor = manager.shortageMonitor();
    LineManager *lineManager = manager.lineManager();
    QVERIFY(monitor != nullptr);
    QVERIFY(lineManager != nullptr);

    QSignalSpy enqueuedSpy(lineManager, &LineManager::taskEnqueued);
    QSignalSpy startedSpy(lineManager, &LineManager::taskStarted);

    QVERIFY(QMetaObject::invokeMethod(monitor,
                                      "dispatchRequested",
                                      Qt::DirectConnection,
                                      Q_ARG(int, 3)));
    QCOMPARE(enqueuedSpy.count(), 1);

    const Task enqueuedTask = qvariant_cast<Task>(enqueuedSpy.takeFirst().at(0));
    QCOMPARE(enqueuedTask.stationId, 3);
    QCOMPARE(enqueuedTask.source, TaskSource::CustomerSystem);

    lineManager->start();
    QVERIFY(QMetaObject::invokeMethod(monitor,
                                      "dispatchRequested",
                                      Qt::DirectConnection,
                                      Q_ARG(int, 4)));
    QCOMPARE(startedSpy.count(), 1);

    const Task startedTask = qvariant_cast<Task>(startedSpy.takeFirst().at(0));
    QCOMPARE(startedTask.stationId, 3);
    QCOMPARE(startedTask.source, TaskSource::CustomerSystem);

    // 生产接线要求任务生命周期事实能回流到 monitor；这里只验证连接存在且可调用，
    // 不在本测试里重复覆盖库存算法与状态机细节，那些由 Task 3/7 的 monitor 测试负责。
    monitor->onTaskStarted(startedTask);
    monitor->onMaterialUnloaded(startedTask);
    monitor->onTaskFinished(startedTask);
}

QTEST_MAIN(LiveShortageWiringTest)

#include "test_live_shortage_wiring.moc"
