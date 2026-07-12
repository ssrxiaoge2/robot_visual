#include <QtTest>

#define private public
#include "huayanScheduler.h"
#include "linemanager.h"
#include "taskexecutor.h"
#undef private

class LiveShortageDispatchTest : public QObject
{
    Q_OBJECT

private slots:
    void reportShortageWithId_returnsTaskId_and_emitsTaskEnqueued();
    void runningLine_emitsTaskStarted_forDequeuedTask();
    void taskFinished_emits_forSuccessAndManualStopCleanup();
    void taskExecutor_emitsMaterialUnloaded_onlyAtArmUnloadCompletion();
    void pending_customer_task_starts_after_current_mock_task_finishes_in_fifo_order();
};

void LiveShortageDispatchTest::reportShortageWithId_returnsTaskId_and_emitsTaskEnqueued()
{
    qRegisterMetaType<Task>("Task");

    AgvController agv;
    HuayanScheduler arm;
    NScanScheduler scanner;
    PalletScheduler pallet;
    LineManager manager(&agv, &arm, &scanner, &pallet);

    QSignalSpy enqueuedSpy(&manager, &LineManager::taskEnqueued);

    const quint64 taskId = manager.reportShortageWithId(3, TaskSource::CustomerSystem);

    QVERIFY(taskId > 0);
    QCOMPARE(enqueuedSpy.count(), 1);

    const Task enqueuedTask = qvariant_cast<Task>(enqueuedSpy.takeFirst().at(0));
    QCOMPARE(enqueuedTask.taskId, taskId);
    QCOMPARE(enqueuedTask.stationId, 3);
    QCOMPARE(enqueuedTask.source, TaskSource::CustomerSystem);
    QCOMPARE(enqueuedTask.state, TaskState::Pending);
}

void LiveShortageDispatchTest::runningLine_emitsTaskStarted_forDequeuedTask()
{
    qRegisterMetaType<Task>("Task");

    AgvController agv;
    HuayanScheduler arm;
    NScanScheduler scanner;
    PalletScheduler pallet;
    LineManager manager(&agv, &arm, &scanner, &pallet);

    manager.start();

    QSignalSpy startedSpy(&manager, &LineManager::taskStarted);
    const quint64 taskId = manager.reportShortageWithId(4, TaskSource::UiMock);

    QCOMPARE(startedSpy.count(), 1);
    const Task startedTask = qvariant_cast<Task>(startedSpy.takeFirst().at(0));
    QCOMPARE(startedTask.taskId, taskId);
    QCOMPARE(startedTask.stationId, 4);
    QCOMPARE(startedTask.state, TaskState::Running);
    QCOMPARE(manager.currentTask().taskId, taskId);
}

void LiveShortageDispatchTest::taskFinished_emits_forSuccessAndManualStopCleanup()
{
    qRegisterMetaType<Task>("Task");

    AgvController agv;
    HuayanScheduler arm;
    NScanScheduler scanner;
    PalletScheduler pallet;
    LineManager manager(&agv, &arm, &scanner, &pallet);

    manager.start();
    const quint64 runningTaskId = manager.reportShortageWithId(5, TaskSource::CustomerSystem);
    Task succeededInput = manager.currentTask();
    QCOMPARE(succeededInput.taskId, runningTaskId);
    succeededInput.state = TaskState::Succeeded;
    succeededInput.step = TaskStep::Done;
    succeededInput.stepIndex = 15;

    QSignalSpy finishedSpy(&manager, &LineManager::taskFinished);
    QVERIFY(QMetaObject::invokeMethod(&manager,
                                      "onExecutorTaskSucceeded",
                                      Qt::DirectConnection,
                                      Q_ARG(Task, succeededInput)));
    QCOMPARE(finishedSpy.count(), 1);
    const Task succeededTask = qvariant_cast<Task>(finishedSpy.takeFirst().at(0));
    QCOMPARE(succeededTask.taskId, runningTaskId);
    QCOMPARE(succeededTask.state, TaskState::Succeeded);

    manager.start();
    const quint64 canceledRunningId = manager.reportShortageWithId(6, TaskSource::UiMock);
    const quint64 pendingTaskId = manager.reportShortageWithId(7, TaskSource::CustomerSystem);
    QVERIFY(canceledRunningId > 0);
    QVERIFY(pendingTaskId > canceledRunningId);

    manager.stop();

    QCOMPARE(finishedSpy.count(), 2);
    const Task canceledRunning = qvariant_cast<Task>(finishedSpy.at(0).at(0));
    const Task canceledPending = qvariant_cast<Task>(finishedSpy.at(1).at(0));
    QCOMPARE(canceledRunning.taskId, canceledRunningId);
    QCOMPARE(canceledRunning.state, TaskState::Canceled);
    QCOMPARE(canceledPending.taskId, pendingTaskId);
    QCOMPARE(canceledPending.state, TaskState::Canceled);
}

void LiveShortageDispatchTest::taskExecutor_emitsMaterialUnloaded_onlyAtArmUnloadCompletion()
{
    qRegisterMetaType<Task>("Task");

    AgvController agv;
    HuayanScheduler arm;
    NScanScheduler scanner;
    PalletScheduler pallet;
    TaskExecutor executor(&agv, &arm, &scanner, &pallet);
    QSignalBlocker blockArmSignals(&arm);
    QSignalSpy unloadedSpy(&executor, &TaskExecutor::materialUnloaded);

    executor.m_task.taskId = 99;
    executor.m_task.stationId = 8;
    executor.m_task.state = TaskState::Running;
    executor.m_stationCfg = stationConfig(8);
    executor.m_palletCfg = palletAreaConfig(PalletArea::LargeBox);
    executor.m_state = TaskExecutor::ExecState::ArmUnload;

    QVERIFY(QMetaObject::invokeMethod(&executor,
                                      "onArmStageCompleted",
                                      Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("倒料"))));
    QCOMPARE(unloadedSpy.count(), 1);

    const Task unloadedTask = qvariant_cast<Task>(unloadedSpy.takeFirst().at(0));
    QCOMPARE(unloadedTask.taskId, 99ULL);
    QCOMPARE(unloadedTask.stationId, 8);

    executor.m_state = TaskExecutor::ExecState::StowAfterUnload;
    QVERIFY(QMetaObject::invokeMethod(&executor,
                                      "onArmStageCompleted",
                                      Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("收姿态"))));
    QCOMPARE(unloadedSpy.count(), 0);
}

void LiveShortageDispatchTest::pending_customer_task_starts_after_current_mock_task_finishes_in_fifo_order()
{
    qRegisterMetaType<Task>("Task");

    AgvController agv;
    HuayanScheduler arm;
    NScanScheduler scanner;
    PalletScheduler pallet;
    LineManager manager(&agv, &arm, &scanner, &pallet);

    manager.start();
    const quint64 firstTaskId = manager.reportShortageWithId(8, TaskSource::UiMock);
    const quint64 secondTaskId = manager.reportShortageWithId(1, TaskSource::CustomerSystem);
    QCOMPARE(manager.currentTask().taskId, firstTaskId);

    QSignalSpy startedSpy(&manager, &LineManager::taskStarted);
    Task finishedFirst = manager.currentTask();
    finishedFirst.state = TaskState::Succeeded;
    finishedFirst.step = TaskStep::Done;
    finishedFirst.stepIndex = 15;
    manager.m_executor->m_state = TaskExecutor::ExecState::Idle;
    QVERIFY(QMetaObject::invokeMethod(&manager,
                                      "onExecutorTaskSucceeded",
                                      Qt::DirectConnection,
                                      Q_ARG(Task, finishedFirst)));

    QCOMPARE(startedSpy.count(), 1);
    const Task startedSecond = qvariant_cast<Task>(startedSpy.takeFirst().at(0));
    QCOMPARE(startedSecond.taskId, secondTaskId);
    QCOMPARE(startedSecond.stationId, 1);
    QCOMPARE(startedSecond.source, TaskSource::CustomerSystem);
}

QTEST_MAIN(LiveShortageDispatchTest)

#include "test_live_shortage_dispatch.moc"
