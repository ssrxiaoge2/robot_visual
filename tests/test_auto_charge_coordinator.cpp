#include <QtTest>

#include "autochargecoordinator.h"

namespace {

AutoChargeInputs runningInputs(const int battery, const int station = 1)
{
    AutoChargeInputs inputs;
    inputs.enabled = true;
    inputs.lineState = LineSystemState::Running;
    inputs.hasAgvMonitor = true;
    inputs.agv.battery = static_cast<quint16>(battery);
    inputs.agv.curStation = station;
    inputs.agv.navStatus = static_cast<quint16>(AgvController::NavStatus::None);
    return inputs;
}

} // namespace

class AutoChargeCoordinatorTest : public QObject
{
    Q_OBJECT

private slots:
    void disabledModeIsExactPassThrough();
    void startThresholdIsStrictAndRequestsReturnHome();
    void runningTaskIsAllowedToFinishBeforeReturningHome();
    void chargeStartsOnlyAtLm1WithNavigationIdle();
    void activeSessionUsesDispatchReadyThresholdWhenTasksWait();
    void activeSessionUsesNormalStopThresholdWithoutTasks();
    void criticalBatteryHoldsDispatchWithoutAbortingCurrentTask();
    void missingMonitorHoldsBeforeChargeAndFailsSafeDuringCharge();
    void disablingActiveAutomaticSessionKeepsHoldUntilSafeCompletion();
    void coordinatorSuppressesRepeatedActionEdges();
    void manualControllerActivityNeverBecomesAnAutomaticSession();
    void unsafeAutomaticCompletionKeepsOwnershipAndReportsErrorOnce();
    void staleAutomaticCompletionIsIgnoredWithoutOwnership();
    void identicalInputsDoNotRetryRejectedStartUntilRelevantChange();
    void errorEdgeStaysLatchedThroughoutOneRecoveryCycle();
    void runningTaskLowBatteryRequirementSurvivesMeasurementRecovery();
    void idleLowBatteryReturnRequirementSurvivesMeasurementRecovery();
    void disablingWithoutOwnedSessionStartsANewErrorCycle();
};

void AutoChargeCoordinatorTest::disabledModeIsExactPassThrough()
{
    AutoChargeInputs inputs;
    inputs.enabled = false;
    inputs.lineState = LineSystemState::Running;
    inputs.hasAgvMonitor = true;
    inputs.agv.battery = 5;
    inputs.agv.curStation = 9;
    inputs.pendingCount = 3;

    const AutoChargeDecision decision =
        decideAutoCharge(inputs, ChargeSettings::defaults());

    QVERIFY(!decision.holdDispatch);
    QVERIFY(!decision.requestReturnHome);
    QVERIFY(!decision.requestStartCharge);
    QVERIFY(!decision.requestSafeStop);
    QVERIFY(!decision.releaseDispatch);
    QVERIFY(!decision.raiseLineError);
}

void AutoChargeCoordinatorTest::startThresholdIsStrictAndRequestsReturnHome()
{
    AutoChargeInputs inputs = runningInputs(14, 9);
    AutoChargeDecision decision =
        decideAutoCharge(inputs, ChargeSettings::defaults());
    QVERIFY(decision.holdDispatch);
    QVERIFY(decision.requestReturnHome);
    QVERIFY(!decision.requestStartCharge);

    inputs.agv.battery = 15;
    decision = decideAutoCharge(inputs, ChargeSettings::defaults());
    QVERIFY(!decision.holdDispatch);
    QVERIFY(!decision.requestReturnHome);
    QVERIFY(!decision.requestStartCharge);
    QVERIFY(decision.releaseDispatch);
}

void AutoChargeCoordinatorTest::runningTaskIsAllowedToFinishBeforeReturningHome()
{
    AutoChargeInputs inputs = runningInputs(14, 9);
    inputs.currentTaskRunning = true;
    inputs.pendingCount = 2;

    const AutoChargeDecision decision =
        decideAutoCharge(inputs, ChargeSettings::defaults());

    QVERIFY(decision.holdDispatch);
    QVERIFY(!decision.requestReturnHome);
    QVERIFY(!decision.requestStartCharge);
    QVERIFY(!decision.requestSafeStop);
    QVERIFY(!decision.raiseLineError);
}

void AutoChargeCoordinatorTest::chargeStartsOnlyAtLm1WithNavigationIdle()
{
    AutoChargeInputs inputs = runningInputs(14, 1);
    inputs.agv.navStatus =
        static_cast<quint16>(AgvController::NavStatus::Running);

    AutoChargeDecision decision =
        decideAutoCharge(inputs, ChargeSettings::defaults());
    QVERIFY(decision.holdDispatch);
    QVERIFY(!decision.requestStartCharge);

    inputs.agv.navStatus =
        static_cast<quint16>(AgvController::NavStatus::Paused);
    decision = decideAutoCharge(inputs, ChargeSettings::defaults());
    QVERIFY(!decision.requestStartCharge);

    inputs.agv.navStatus =
        static_cast<quint16>(AgvController::NavStatus::Arrived);
    decision = decideAutoCharge(inputs, ChargeSettings::defaults());
    QVERIFY(decision.holdDispatch);
    QVERIFY(decision.requestStartCharge);

    inputs.chargeControllerBusy = true;
    decision = decideAutoCharge(inputs, ChargeSettings::defaults());
    QVERIFY(!decision.requestStartCharge);
}

void AutoChargeCoordinatorTest::activeSessionUsesDispatchReadyThresholdWhenTasksWait()
{
    AutoChargeInputs inputs = runningInputs(19);
    inputs.automaticSessionActive = true;
    inputs.chargeControllerBusy = true;
    inputs.pendingCount = 1;

    AutoChargeDecision decision =
        decideAutoCharge(inputs, ChargeSettings::defaults());
    QVERIFY(decision.holdDispatch);
    QVERIFY(!decision.requestSafeStop);

    inputs.agv.battery = 20;
    decision = decideAutoCharge(inputs, ChargeSettings::defaults());
    QVERIFY(decision.holdDispatch);
    QVERIFY(decision.requestSafeStop);
    QVERIFY(!decision.releaseDispatch);
}

void AutoChargeCoordinatorTest::activeSessionUsesNormalStopThresholdWithoutTasks()
{
    AutoChargeInputs inputs = runningInputs(79);
    inputs.automaticSessionActive = true;
    inputs.chargeControllerBusy = true;

    AutoChargeDecision decision =
        decideAutoCharge(inputs, ChargeSettings::defaults());
    QVERIFY(decision.holdDispatch);
    QVERIFY(!decision.requestSafeStop);

    inputs.agv.battery = 80;
    decision = decideAutoCharge(inputs, ChargeSettings::defaults());
    QVERIFY(decision.holdDispatch);
    QVERIFY(decision.requestSafeStop);
}

void AutoChargeCoordinatorTest::criticalBatteryHoldsDispatchWithoutAbortingCurrentTask()
{
    AutoChargeInputs inputs = runningInputs(10, 9);
    inputs.currentTaskRunning = true;

    const AutoChargeDecision decision =
        decideAutoCharge(inputs, ChargeSettings::defaults());

    QVERIFY(decision.holdDispatch);
    QVERIFY(!decision.requestReturnHome);
    QVERIFY(!decision.requestStartCharge);
    QVERIFY(!decision.requestSafeStop);
    // 低电量严重报警不能借由主调度 Error 强行中止正在执行的机械安全动作。
    QVERIFY(!decision.raiseLineError);
    QVERIFY(!decision.errorText.isEmpty());
}

void AutoChargeCoordinatorTest::missingMonitorHoldsBeforeChargeAndFailsSafeDuringCharge()
{
    AutoChargeInputs inputs;
    inputs.enabled = true;
    inputs.lineState = LineSystemState::Running;

    AutoChargeDecision decision =
        decideAutoCharge(inputs, ChargeSettings::defaults());
    QVERIFY(decision.holdDispatch);
    QVERIFY(!decision.requestStartCharge);
    QVERIFY(!decision.requestSafeStop);
    QVERIFY(!decision.raiseLineError);

    inputs.automaticSessionActive = true;
    inputs.chargeControllerBusy = true;
    decision = decideAutoCharge(inputs, ChargeSettings::defaults());
    QVERIFY(decision.holdDispatch);
    QVERIFY(decision.requestSafeStop);
    QVERIFY(decision.raiseLineError);
}

void AutoChargeCoordinatorTest::disablingActiveAutomaticSessionKeepsHoldUntilSafeCompletion()
{
    AutoChargeInputs inputs = runningInputs(30);
    inputs.enabled = false;
    inputs.automaticSessionActive = true;
    inputs.chargeControllerBusy = true;

    const AutoChargeDecision decision =
        decideAutoCharge(inputs, ChargeSettings::defaults());

    QVERIFY(decision.holdDispatch);
    QVERIFY(decision.requestSafeStop);
    QVERIFY(!decision.releaseDispatch);
    QVERIFY(!decision.requestReturnHome);
    QVERIFY(!decision.requestStartCharge);
    QVERIFY(!decision.raiseLineError);
}

void AutoChargeCoordinatorTest::coordinatorSuppressesRepeatedActionEdges()
{
    AutoChargeCoordinator coordinator;
    QSignalSpy holdSpy(&coordinator,
                       &AutoChargeCoordinator::dispatchHoldRequested);
    QSignalSpy returnSpy(&coordinator,
                         &AutoChargeCoordinator::returnHomeRequested);
    QSignalSpy startSpy(&coordinator,
                        &AutoChargeCoordinator::automaticChargeStartRequested);
    QSignalSpy stopSpy(&coordinator,
                       &AutoChargeCoordinator::automaticChargeSafeStopRequested);
    QSignalSpy errorSpy(&coordinator,
                        &AutoChargeCoordinator::lineErrorRequested);

    coordinator.setEnabled(true);
    coordinator.onLineStateChanged(LineSystemState::Running,
                                   QStringLiteral("主调度运行"));

    AgvMonitorData monitor;
    monitor.battery = 14;
    monitor.curStation = 9;
    monitor.navStatus = static_cast<quint16>(AgvController::NavStatus::None);
    coordinator.onAgvMonitorUpdated(monitor);
    coordinator.onAgvMonitorUpdated(monitor);

    QCOMPARE(holdSpy.count(), 1);
    QCOMPARE(holdSpy.at(0).at(0).toBool(), true);
    QCOMPARE(returnSpy.count(), 1);
    QCOMPARE(startSpy.count(), 0);

    monitor.curStation = 1;
    coordinator.onAgvMonitorUpdated(monitor);
    coordinator.onAgvMonitorUpdated(monitor);
    QCOMPARE(startSpy.count(), 1);

    coordinator.onAutomaticChargeStartResult(true, QString());
    QVERIFY(coordinator.automaticSessionActive());

    QList<Task> queue;
    Task pending;
    pending.state = TaskState::Pending;
    queue.append(pending);
    monitor.battery = 20;
    coordinator.onQueueChanged(queue);
    coordinator.onAgvMonitorUpdated(monitor);
    coordinator.onAgvMonitorUpdated(monitor);
    QCOMPARE(stopSpy.count(), 1);
    QCOMPARE(errorSpy.count(), 0);
}

void AutoChargeCoordinatorTest::manualControllerActivityNeverBecomesAnAutomaticSession()
{
    AutoChargeCoordinator coordinator;
    QSignalSpy startSpy(&coordinator,
                        &AutoChargeCoordinator::automaticChargeStartRequested);
    QSignalSpy releaseSpy(&coordinator,
                          &AutoChargeCoordinator::dispatchHoldRequested);

    coordinator.setEnabled(true);
    coordinator.onLineStateChanged(LineSystemState::Running,
                                   QStringLiteral("主调度运行"));

    // 手动充电和只读查询都可能让控制器进入忙碌状态，但它们没有经过自动启动回执，
    // 因而绝不能污染 automaticSessionActive。
    coordinator.onChargeControllerStateChanged(
        ChargePileController::State::Monitoring, QStringLiteral("手动充电"));

    AgvMonitorData monitor;
    monitor.battery = 14;
    monitor.curStation = 1;
    monitor.navStatus = static_cast<quint16>(AgvController::NavStatus::Arrived);
    coordinator.onAgvMonitorUpdated(monitor);
    QVERIFY(!coordinator.automaticSessionActive());
    QCOMPARE(startSpy.count(), 0);

    coordinator.onChargeSessionFinished(
        true, ChargePileController::SessionOrigin::Manual,
        QStringLiteral("手动会话安全完成"));
    QVERIFY(!coordinator.automaticSessionActive());

    coordinator.onChargeControllerStateChanged(
        ChargePileController::State::Idle, QStringLiteral("空闲"));
    QCOMPARE(startSpy.count(), 1);
    coordinator.onAutomaticChargeStartResult(true, QString());
    QVERIFY(coordinator.automaticSessionActive());

    coordinator.setEnabled(false);
    coordinator.setEnabled(false);
    QVERIFY(coordinator.automaticSessionActive());

    // 自动会话安全完成前只请求收尾并持续保持；完成后才解除保持。
    coordinator.onChargeSessionFinished(
        true, ChargePileController::SessionOrigin::Automatic,
        QStringLiteral("自动会话安全完成"));
    QVERIFY(!coordinator.automaticSessionActive());
    QVERIFY(releaseSpy.count() >= 2);
    QCOMPARE(releaseSpy.last().at(0).toBool(), false);
}

void AutoChargeCoordinatorTest::unsafeAutomaticCompletionKeepsOwnershipAndReportsErrorOnce()
{
    AutoChargeCoordinator coordinator;
    QSignalSpy recoverySpy(
        &coordinator,
        &AutoChargeCoordinator::automaticChargeConservativeRecoveryRequested);
    QSignalSpy errorSpy(&coordinator,
                        &AutoChargeCoordinator::lineErrorRequested);
    QSignalSpy holdSpy(&coordinator,
                       &AutoChargeCoordinator::dispatchHoldRequested);

    coordinator.setEnabled(true);
    coordinator.onLineStateChanged(LineSystemState::Running,
                                   QStringLiteral("主调度运行"));

    AgvMonitorData monitor;
    monitor.battery = 14;
    monitor.curStation = 1;
    monitor.navStatus = static_cast<quint16>(AgvController::NavStatus::Arrived);
    coordinator.onAgvMonitorUpdated(monitor);
    coordinator.onAutomaticChargeStartResult(true, QString());

    coordinator.onChargeSessionFinished(
        false, ChargePileController::SessionOrigin::Automatic,
        QStringLiteral("缩回结果未确认"));
    coordinator.onChargeSessionFinished(
        false, ChargePileController::SessionOrigin::Automatic,
        QStringLiteral("重复终态通知"));

    QVERIFY(coordinator.automaticSessionActive());
    QCOMPARE(recoverySpy.count(), 1);
    QCOMPARE(errorSpy.count(), 1);

    // 用正常阈值快照隔离“未知锁存是否清除”这一行为，避免安全完成后因为
    // 电量仍低于 15%而合法地发起下一次自动充电。
    monitor.battery = 15;
    coordinator.onAgvMonitorUpdated(monitor);
    coordinator.onChargeSessionFinished(
        true, ChargePileController::SessionOrigin::Automatic,
        QStringLiteral("后续已确认安全完成"));
    QVERIFY(!coordinator.automaticSessionActive());
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(holdSpy.last().at(0).toBool(), false);
}

void AutoChargeCoordinatorTest::staleAutomaticCompletionIsIgnoredWithoutOwnership()
{
    AutoChargeCoordinator coordinator;
    QSignalSpy holdSpy(&coordinator,
                       &AutoChargeCoordinator::dispatchHoldRequested);
    QSignalSpy stopSpy(&coordinator,
                       &AutoChargeCoordinator::automaticChargeSafeStopRequested);
    QSignalSpy recoverySpy(
        &coordinator,
        &AutoChargeCoordinator::automaticChargeConservativeRecoveryRequested);
    QSignalSpy errorSpy(&coordinator,
                        &AutoChargeCoordinator::lineErrorRequested);

    coordinator.onChargeControllerStateChanged(
        ChargePileController::State::Unknown, QStringLiteral("旧未知状态"));
    coordinator.onChargeSessionFinished(
        false, ChargePileController::SessionOrigin::Automatic,
        QStringLiteral("陈旧不安全终态"));
    coordinator.onChargeSessionFinished(
        true, ChargePileController::SessionOrigin::Automatic,
        QStringLiteral("陈旧安全终态"));

    QVERIFY(!coordinator.automaticSessionActive());
    QCOMPARE(holdSpy.count(), 0);
    QCOMPARE(stopSpy.count(), 0);
    QCOMPARE(recoverySpy.count(), 0);
    QCOMPARE(errorSpy.count(), 0);

    // 陈旧 safe 回执不得清除 controllerUnknown；真正开启自动模式后仍应保守报错。
    coordinator.setEnabled(true);
    coordinator.onLineStateChanged(LineSystemState::Running,
                                   QStringLiteral("主调度运行"));
    AgvMonitorData monitor;
    monitor.battery = 50;
    monitor.curStation = 1;
    coordinator.onAgvMonitorUpdated(monitor);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(holdSpy.last().at(0).toBool(), true);
}

void AutoChargeCoordinatorTest::identicalInputsDoNotRetryRejectedStartUntilRelevantChange()
{
    AutoChargeCoordinator coordinator;
    QSignalSpy startSpy(&coordinator,
                        &AutoChargeCoordinator::automaticChargeStartRequested);
    coordinator.setEnabled(true);
    coordinator.onLineStateChanged(LineSystemState::Running,
                                   QStringLiteral("主调度运行"));

    AgvMonitorData monitor;
    monitor.battery = 14;
    monitor.curStation = 1;
    monitor.navStatus = static_cast<quint16>(AgvController::NavStatus::Arrived);
    coordinator.onAgvMonitorUpdated(monitor);
    QCOMPARE(startSpy.count(), 1);
    coordinator.onAutomaticChargeStartResult(
        false, QStringLiteral("控制器暂时忙碌"));

    coordinator.onAgvMonitorUpdated(monitor);
    coordinator.onAgvMonitorUpdated(monitor);
    coordinator.onLineStateChanged(LineSystemState::Running,
                                   QStringLiteral("仅文案重复"));
    coordinator.onQueueChanged({});
    coordinator.onQueueChanged({});
    coordinator.onChargeControllerStateChanged(
        ChargePileController::State::Idle, QStringLiteral("重复空闲"));
    coordinator.onChargeControllerStateChanged(
        ChargePileController::State::Idle, QStringLiteral("重复空闲"));
    QCOMPARE(startSpy.count(), 1);

    monitor.battery = 13;
    coordinator.onAgvMonitorUpdated(monitor);
    QCOMPARE(startSpy.count(), 2);
}

void AutoChargeCoordinatorTest::errorEdgeStaysLatchedThroughoutOneRecoveryCycle()
{
    AutoChargeCoordinator coordinator;
    QSignalSpy errorSpy(&coordinator,
                        &AutoChargeCoordinator::lineErrorRequested);
    coordinator.setEnabled(true);
    coordinator.onLineStateChanged(LineSystemState::Running,
                                   QStringLiteral("主调度运行"));
    AgvMonitorData monitor;
    monitor.battery = 14;
    monitor.curStation = 1;
    monitor.navStatus = static_cast<quint16>(AgvController::NavStatus::Arrived);
    coordinator.onAgvMonitorUpdated(monitor);
    coordinator.onAutomaticChargeStartResult(true, QString());

    coordinator.onChargeControllerStateChanged(
        ChargePileController::State::Unknown, QStringLiteral("恢复前未知"));
    coordinator.onChargeControllerStateChanged(
        ChargePileController::State::Connecting, QStringLiteral("恢复连接"));
    coordinator.onChargeControllerStateChanged(
        ChargePileController::State::SendingStop, QStringLiteral("恢复停止"));
    coordinator.onChargeControllerStateChanged(
        ChargePileController::State::Unknown, QStringLiteral("同轮再次未知"));
    QCOMPARE(errorSpy.count(), 1);
}

void AutoChargeCoordinatorTest::runningTaskLowBatteryRequirementSurvivesMeasurementRecovery()
{
    AutoChargeCoordinator coordinator;
    QSignalSpy holdSpy(&coordinator,
                       &AutoChargeCoordinator::dispatchHoldRequested);
    QSignalSpy returnSpy(&coordinator,
                         &AutoChargeCoordinator::returnHomeRequested);
    QSignalSpy startSpy(&coordinator,
                        &AutoChargeCoordinator::automaticChargeStartRequested);

    coordinator.setEnabled(true);
    coordinator.onLineStateChanged(LineSystemState::Running,
                                   QStringLiteral("主调度运行"));
    Task running;
    running.state = TaskState::Running;
    coordinator.onQueueChanged({running});

    AgvMonitorData monitor;
    monitor.battery = 14;
    monitor.curStation = 9;
    monitor.navStatus = static_cast<quint16>(AgvController::NavStatus::None);
    coordinator.onAgvMonitorUpdated(monitor);
    QCOMPARE(holdSpy.last().at(0).toBool(), true);

    monitor.battery = 15;
    coordinator.onAgvMonitorUpdated(monitor);
    monitor.battery = 16;
    coordinator.onAgvMonitorUpdated(monitor);
    QCOMPARE(holdSpy.last().at(0).toBool(), true);

    Task pending;
    pending.state = TaskState::Pending;
    coordinator.onQueueChanged({pending});
    QCOMPARE(returnSpy.count(), 1);

    monitor.curStation = 1;
    monitor.navStatus = static_cast<quint16>(AgvController::NavStatus::Arrived);
    coordinator.onAgvMonitorUpdated(monitor);
    QCOMPARE(startSpy.count(), 1);
}

void AutoChargeCoordinatorTest::idleLowBatteryReturnRequirementSurvivesMeasurementRecovery()
{
    AutoChargeCoordinator coordinator;
    QSignalSpy holdSpy(&coordinator,
                       &AutoChargeCoordinator::dispatchHoldRequested);
    QSignalSpy returnSpy(&coordinator,
                         &AutoChargeCoordinator::returnHomeRequested);
    QSignalSpy startSpy(&coordinator,
                        &AutoChargeCoordinator::automaticChargeStartRequested);
    coordinator.setEnabled(true);
    coordinator.onLineStateChanged(LineSystemState::Running,
                                   QStringLiteral("主调度空闲运行"));

    AgvMonitorData monitor;
    monitor.battery = 14;
    monitor.curStation = 9;
    monitor.navStatus = static_cast<quint16>(AgvController::NavStatus::None);
    coordinator.onAgvMonitorUpdated(monitor);
    QCOMPARE(holdSpy.last().at(0).toBool(), true);
    QCOMPARE(returnSpy.count(), 1);

    monitor.battery = 16;
    coordinator.onAgvMonitorUpdated(monitor);
    QCOMPARE(holdSpy.last().at(0).toBool(), true);
    QCOMPARE(returnSpy.count(), 1);

    monitor.curStation = 1;
    monitor.navStatus = static_cast<quint16>(AgvController::NavStatus::Arrived);
    coordinator.onAgvMonitorUpdated(monitor);
    QCOMPARE(startSpy.count(), 1);
}

void AutoChargeCoordinatorTest::disablingWithoutOwnedSessionStartsANewErrorCycle()
{
    AutoChargeCoordinator coordinator;
    QSignalSpy errorSpy(&coordinator,
                        &AutoChargeCoordinator::lineErrorRequested);
    QSignalSpy holdSpy(&coordinator,
                       &AutoChargeCoordinator::dispatchHoldRequested);
    coordinator.setEnabled(true);
    coordinator.onLineStateChanged(LineSystemState::Running,
                                   QStringLiteral("主调度运行"));
    AgvMonitorData monitor;
    monitor.battery = 50;
    monitor.curStation = 1;
    coordinator.onAgvMonitorUpdated(monitor);
    coordinator.onChargeControllerStateChanged(
        ChargePileController::State::Unknown, QStringLiteral("第一轮未知"));
    QCOMPARE(errorSpy.count(), 1);

    coordinator.setEnabled(false);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(holdSpy.last().at(0).toBool(), false);
    coordinator.setEnabled(true);
    QCOMPARE(errorSpy.count(), 2);
}

QTEST_MAIN(AutoChargeCoordinatorTest)
#include "test_auto_charge_coordinator.moc"
