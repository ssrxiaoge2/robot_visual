#include <QFile>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include "agvcontroller.h"
#include "autochargecoordinator.h"
#include "chargebusinessrules.h"
#include "huayanScheduler.h"
#include "linemanager.h"
#include "taskexecutor.h"

namespace {

bool g_autoFinishTask = false;
int g_agvCancelCount = 0;
int g_executorStopCount = 0;

QString readSource(const QString &relativePath)
{
    QFile file(QStringLiteral(PROJECT_SOURCE_DIR "/") + relativePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

} // namespace

// 这些替身只隔离真实设备 SDK 和单任务状态机；被验证的 LineManager 是生产实现。
// TaskExecutor 替身仍按生产信号顺序发布 Running -> Succeeded，确保任务完成后的
// 派单/返航决策由真实 LineManager 执行，而不是在测试中复刻调度判断。
TaskExecutor::TaskExecutor(AgvController *agv,
                           HuayanScheduler *arm,
                           NScanScheduler *scanner,
                           PalletScheduler *pallet,
                           QObject *parent)
    : QObject(parent)
    , m_agv(agv)
    , m_arm(arm)
    , m_scanner(scanner)
    , m_pallet(pallet)
{
}

bool TaskExecutor::isBusy() const
{
    return m_state != ExecState::Idle;
}

Task TaskExecutor::currentTask() const
{
    return m_task;
}

void TaskExecutor::applyRuntimeSettings(const RuntimeSettings &settings)
{
    m_runtimeSettings = settings;
}

void TaskExecutor::start(const Task &task)
{
    m_task = task;
    m_state = ExecState::AgvToPickup;
    emit taskUpdated(m_task);

    if (!g_autoFinishTask)
        return;

    QTimer::singleShot(0, this, [this] {
        Task finished = m_task;
        finished.state = TaskState::Succeeded;
        finished.step = TaskStep::Done;
        finished.statusText = QStringLiteral("测试任务完成");
        m_state = ExecState::Idle;
        m_task = finished;
        emit taskUpdated(finished);
        emit taskSucceeded(finished);
    });
}

void TaskExecutor::stopForSystemError(const QString &reason)
{
    if (m_state == ExecState::Idle)
        return;
    ++g_executorStopCount;
    m_state = ExecState::Idle;
    Task stopped = m_task;
    stopped.state = TaskState::Canceled;
    stopped.lastError = reason;
    emit taskUpdated(stopped);
    emit systemError(stopped, reason);
}

void TaskExecutor::onScanFinished(const NScanScheduler::ScanResult &result)
{
    Q_UNUSED(result);
}

void TaskExecutor::onAgvMonitor(const AgvMonitorData &data) { Q_UNUSED(data); }
void TaskExecutor::onArmStageCompleted(const QString &stageName) { Q_UNUSED(stageName); }
void TaskExecutor::onArmStageError(const QString &reason) { Q_UNUSED(reason); }
void TaskExecutor::onArmVisionAlignmentFailed(const QString &reason) { Q_UNUSED(reason); }
void TaskExecutor::onPreGripScanRequested() {}
void TaskExecutor::onPreGripScanSearchMoveCompleted(double currentYOffsetMm)
{
    Q_UNUSED(currentYOffsetMm);
}
void TaskExecutor::onPreGripScanSearchMoveError(const QString &reason) { Q_UNUSED(reason); }
void TaskExecutor::onPreGripScanCaptureReturnCompleted() {}
void TaskExecutor::onPreGripScanCaptureReturnError(const QString &reason) { Q_UNUSED(reason); }
void TaskExecutor::onToolRotationCompleted() {}
void TaskExecutor::onToolRotationError(const QString &reason) { Q_UNUSED(reason); }
void TaskExecutor::onPalletPlaceCompleted() {}
void TaskExecutor::onPalletPlaceError(const QString &reason) { Q_UNUSED(reason); }
void TaskExecutor::onAgvTimeout() {}

AgvController::AgvController(QObject *parent)
    : QObject(parent)
{
}

AgvController::~AgvController() = default;

void AgvController::cancelNavigation()
{
    ++g_agvCancelCount;
}

void AgvController::onStateChanged(QModbusDevice::State state)
{
    Q_UNUSED(state);
}

bool HuayanScheduler::isBusy() const
{
    return false;
}

void HuayanScheduler::stop(bool emitStoppedLog)
{
    Q_UNUSED(emitStoppedLog);
}

class LineManagerChargeHoldTest : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        g_autoFinishTask = false;
        g_agvCancelCount = 0;
        g_executorStopCount = 0;
    }

    void automaticChargingDisabledKeepsOriginalDispatchPath()
    {
        LineManager manager(nullptr, nullptr, nullptr, nullptr);
        QVERIFY(!manager.chargeDispatchHeld());

        manager.reportShortage(2);
        manager.start();

        QCOMPARE(manager.state(), LineSystemState::Running);
        QCOMPARE(manager.currentTask().stationId, 2);
        QCOMPARE(manager.currentTask().state, TaskState::Running);
        QCOMPARE(manager.queueSnapshot().size(), 1);
    }

    void holdStopsBeforeTakingFifoHeadAndReleaseResumesOnlyWhileRunning()
    {
        LineManager manager(nullptr, nullptr, nullptr, nullptr);
        manager.setChargeDispatchHold(true, QStringLiteral("低电量"));
        manager.reportShortage(3);
        manager.start();

        QVERIFY(manager.chargeDispatchHeld());
        QCOMPARE(manager.currentTask().taskId, quint64{0});
        QCOMPARE(manager.queueSnapshot().size(), 1);
        QCOMPARE(manager.queueSnapshot().first().state, TaskState::Pending);

        manager.setChargeDispatchHold(false, QStringLiteral("充电安全完成"));
        QVERIFY(!manager.chargeDispatchHeld());
        QCOMPARE(manager.currentTask().stationId, 3);
        QCOMPARE(manager.currentTask().state, TaskState::Running);

        LineManager idleManager(nullptr, nullptr, nullptr, nullptr);
        idleManager.setChargeDispatchHold(true, QStringLiteral("测试"));
        idleManager.reportShortage(4);
        idleManager.setChargeDispatchHold(false, QStringLiteral("仍未启动"));
        QCOMPARE(idleManager.state(), LineSystemState::Idle);
        QCOMPARE(idleManager.currentTask().taskId, quint64{0});
        QCOMPARE(idleManager.queueSnapshot().size(), 1);
    }

    void holdAllowsPendingQueueToReturnHomeAndNewTaskDoesNotCancelTrip()
    {
        AgvController agv;
        LineManager manager(&agv, nullptr, nullptr, nullptr);
        QSignalSpy dispatchSpy(&manager, &LineManager::agvDispatchRequested);

        manager.setChargeDispatchHold(true, QStringLiteral("低电量回充"));
        manager.reportShortage(5);
        manager.start();
        manager.requestChargeReturnHome();

        QCOMPARE(manager.state(), LineSystemState::ReturningHome);
        QCOMPARE(dispatchSpy.count(), 1);
        QCOMPARE(dispatchSpy.first().first().toInt(), 1);
        QCOMPARE(manager.queueSnapshot().size(), 1);

        manager.reportShortage(6);
        QCOMPARE(manager.state(), LineSystemState::ReturningHome);
        QCOMPARE(g_agvCancelCount, 0);
        QCOMPARE(manager.queueSnapshot().size(), 2);
    }

    void holdAfterCurrentTaskCompletesKeepsNextTaskAndReturnsHome()
    {
        g_autoFinishTask = true;
        AgvController agv;
        LineManager manager(&agv, nullptr, nullptr, nullptr);

        manager.reportShortage(7);
        manager.start();
        manager.setChargeDispatchHold(true, QStringLiteral("任务中检测到低电量"));
        manager.reportShortage(8);

        QTRY_COMPARE(manager.state(), LineSystemState::ReturningHome);
        QCOMPARE(manager.currentTask().taskId, quint64{0});
        QCOMPARE(manager.queueSnapshot().size(), 1);
        QCOMPARE(manager.queueSnapshot().first().stationId, 8);
        QCOMPARE(manager.queueSnapshot().first().state, TaskState::Pending);
    }

    void ordinaryReturnHomeStillCancelsForNewTaskWhenHoldIsOff()
    {
        AgvController agv;
        LineManager manager(&agv, nullptr, nullptr, nullptr);
        manager.start();
        QCOMPARE(manager.state(), LineSystemState::ReturningHome);

        manager.reportShortage(9);

        QCOMPARE(g_agvCancelCount, 1);
        QCOMPARE(manager.state(), LineSystemState::Running);
        QCOMPARE(manager.currentTask().stationId, 9);
    }

    void stopResetAndExternalErrorClearHold()
    {
        LineManager manager(nullptr, nullptr, nullptr, nullptr);
        manager.setChargeDispatchHold(true, QStringLiteral("测试 Stop"));
        manager.stop();
        QCOMPARE(manager.state(), LineSystemState::Error);
        QVERIFY(!manager.chargeDispatchHeld());

        manager.resetError();
        QCOMPARE(manager.state(), LineSystemState::Idle);
        QVERIFY(!manager.chargeDispatchHeld());

        manager.setChargeDispatchHold(true, QStringLiteral("测试外部故障"));
        manager.raiseExternalSystemError(QStringLiteral("充电故障"));
        QCOMPARE(manager.state(), LineSystemState::Error);
        QVERIFY(!manager.chargeDispatchHeld());
    }

    void externalErrorStopsRunningExecutorAndResetCanAcceptNewTask()
    {
        LineManager manager(nullptr, nullptr, nullptr, nullptr);
        manager.reportShortage(2);
        manager.reportShortage(3);
        manager.start();
        QCOMPARE(manager.currentTask().stationId, 2);
        QCOMPARE(manager.currentTask().state, TaskState::Running);

        const QString reason = QStringLiteral("充电桩状态未知");
        manager.raiseExternalSystemError(reason);

        QCOMPARE(g_executorStopCount, 1);
        QCOMPARE(manager.state(), LineSystemState::Error);
        QCOMPARE(manager.currentTask().stationId, 2);
        QCOMPARE(manager.currentTask().state, TaskState::Canceled);
        QCOMPARE(manager.currentTask().step, TaskStep::Done);
        QCOMPARE(manager.currentTask().lastError, reason);
        QVERIFY(manager.queueSnapshot().isEmpty());

        manager.resetError();
        manager.reportShortage(4);
        manager.start();

        QCOMPARE(manager.state(), LineSystemState::Running);
        QCOMPARE(manager.currentTask().stationId, 4);
        QCOMPARE(manager.currentTask().state, TaskState::Running);
    }

    void activeAutomaticSessionReassertsHoldAfterStopResetBeforeDispatch()
    {
        AgvController agv;
        LineManager manager(&agv, nullptr, nullptr, nullptr);
        AutoChargeCoordinator coordinator;
        coordinator.applySettings(ChargeSettings::defaults());

        connect(&manager, &LineManager::systemStateChanged,
                &coordinator, &AutoChargeCoordinator::onLineStateChanged);
        connect(&manager, &LineManager::queueChanged,
                &coordinator, &AutoChargeCoordinator::onQueueChanged);
        connect(&coordinator, &AutoChargeCoordinator::dispatchHoldRequested,
                &manager, &LineManager::setChargeDispatchHold);
        // 反向确认必须排队，避免 Stop/enterError 尚未完成 setState() 时同步重评估。
        connect(&manager, &LineManager::chargeDispatchHoldChanged,
                &coordinator, &AutoChargeCoordinator::onDispatchHoldChanged,
                Qt::QueuedConnection);

        AgvMonitorData monitor;
        monitor.battery = 14;
        monitor.curStation = 1;
        monitor.navStatus =
            static_cast<quint16>(AgvController::NavStatus::None);
        emit agv.monitorUpdated(monitor);
        coordinator.onAgvMonitorUpdated(monitor);
        coordinator.setEnabled(true);

        QSignalSpy startSpy(
            &coordinator,
            &AutoChargeCoordinator::automaticChargeStartRequested);
        manager.start();
        QCOMPARE(startSpy.count(), 1);
        QVERIFY(manager.chargeDispatchHeld());
        coordinator.onAutomaticChargeStartResult(
            true, QStringLiteral("测试自动会话已接受"));
        QVERIFY(coordinator.automaticSessionActive());

        manager.stop();
        QCOMPARE(manager.state(), LineSystemState::Error);
        QVERIFY(!manager.chargeDispatchHeld());

        manager.resetError();
        QCOMPARE(manager.state(), LineSystemState::Idle);
        QTRY_VERIFY(manager.chargeDispatchHeld());

        manager.reportShortage(10);
        manager.start();
        QCOMPARE(manager.state(), LineSystemState::Running);
        QCOMPARE(manager.currentTask().taskId, quint64{0});
        QCOMPARE(manager.queueSnapshot().size(), 1);
        QCOMPARE(manager.queueSnapshot().first().state, TaskState::Pending);

        // 有待执行任务时，控制器会在电量达到“允许接单电量”后安全完成；
        // 若仍保留 14% 输入，协调器按设计会立即开始下一轮低电充电。
        monitor.battery = 20;
        emit agv.monitorUpdated(monitor);
        coordinator.onAgvMonitorUpdated(monitor);
        coordinator.onChargeSessionFinished(
            true,
            ChargePileController::SessionOrigin::Automatic,
            QStringLiteral("测试安全完成"));
        QTRY_VERIFY(!manager.chargeDispatchHeld());
        QCOMPARE(manager.currentTask().stationId, 10);
        QCOMPARE(manager.currentTask().state, TaskState::Running);
    }

    void stopClearingHoldNeverStartsAPendingTask()
    {
        LineManager manager(nullptr, nullptr, nullptr, nullptr);
        manager.setChargeDispatchHold(
            true, QStringLiteral("测试 Stop 前保持"));
        manager.reportShortage(11);
        manager.start();
        QCOMPARE(manager.currentTask().taskId, quint64{0});
        QCOMPARE(manager.queueSnapshot().size(), 1);

        manager.stop();

        QCOMPARE(manager.state(), LineSystemState::Error);
        QCOMPARE(g_executorStopCount, 0);
        QCOMPARE(manager.currentTask().taskId, quint64{0});
        QVERIFY(manager.queueSnapshot().isEmpty());
        QVERIFY(!manager.chargeDispatchHeld());
    }

    void manualChargeBusinessGateAcceptsOnlyIdleLm1AndIdleNavigation()
    {
        ManualChargeStartContext context;
        context.lineState = LineSystemState::Idle;
        context.hasAgvMonitor = true;
        context.agv.curStation = 1;
        context.agv.navStatus =
            static_cast<quint16>(AgvController::NavStatus::None);

        QVERIFY(manualChargeStartRejectionReason(context).isEmpty());
        context.agv.navStatus =
            static_cast<quint16>(AgvController::NavStatus::Arrived);
        QVERIFY(manualChargeStartRejectionReason(context).isEmpty());

        context.lineState = LineSystemState::Running;
        QVERIFY(!manualChargeStartRejectionReason(context).isEmpty());
        context.lineState = LineSystemState::Idle;
        context.hasAgvMonitor = false;
        QVERIFY(!manualChargeStartRejectionReason(context).isEmpty());
        context.hasAgvMonitor = true;
        context.agv.curStation = 2;
        QVERIFY(!manualChargeStartRejectionReason(context).isEmpty());
    }

    void manualChargeBusinessGateRejectsEveryNonIdleNavigationAndModeConflict()
    {
        ManualChargeStartContext context;
        context.lineState = LineSystemState::Idle;
        context.hasAgvMonitor = true;
        context.agv.curStation = 1;

        const QList<AgvController::NavStatus> rejectedStatuses = {
            AgvController::NavStatus::Waiting,
            AgvController::NavStatus::Running,
            AgvController::NavStatus::Paused,
            AgvController::NavStatus::Failed,
            AgvController::NavStatus::Canceled,
            AgvController::NavStatus::Timeout,
        };
        for (AgvController::NavStatus status : rejectedStatuses) {
            context.agv.navStatus = static_cast<quint16>(status);
            QVERIFY2(!manualChargeStartRejectionReason(context).isEmpty(),
                     qPrintable(QStringLiteral("导航状态 %1 必须拒绝手动充电")
                                    .arg(static_cast<int>(status))));
        }

        context.agv.navStatus =
            static_cast<quint16>(AgvController::NavStatus::None);
        context.controllerBusy = true;
        QVERIFY(!manualChargeStartRejectionReason(context).isEmpty());
        context.controllerBusy = false;
        context.controllerState = ChargePileController::State::Unknown;
        QVERIFY(!manualChargeStartRejectionReason(context).isEmpty());
        context.controllerState = ChargePileController::State::Fault;
        QVERIFY(!manualChargeStartRejectionReason(context).isEmpty());
        context.controllerState = ChargePileController::State::Idle;
        context.automaticEnabled = true;
        QVERIFY(!manualChargeStartRejectionReason(context).isEmpty());
        context.automaticEnabled = false;
        context.automaticSessionActive = true;
        QVERIFY(!manualChargeStartRejectionReason(context).isEmpty());
    }

    void unresolvedControllerSafetyContextRejectsManualAndAutomaticAuthorization()
    {
        ManualChargeStartContext manual;
        manual.lineState = LineSystemState::Idle;
        manual.hasAgvMonitor = true;
        manual.agv.curStation = 1;
        manual.agv.navStatus =
            static_cast<quint16>(AgvController::NavStatus::None);
        manual.controllerState = ChargePileController::State::SafeComplete;
        manual.controllerShutdownRequired = true;
        QVERIFY(manualChargeStartRejectionReason(manual)
                    .contains(QStringLiteral("安全")));

        AutomaticChargeEnableContext automatic;
        automatic.controllerState = ChargePileController::State::SafeComplete;
        automatic.controllerShutdownRequired = true;
        QVERIFY(automaticChargeEnableRejectionReason(automatic)
                    .contains(QStringLiteral("安全")));
    }

    void phasePollingDelayUsesRemainingDeadlineDeterministically()
    {
        // 与控制器实际调度共用同一纯函数：有限阶段只等待剩余120ms，不能再
        // 追加完整250ms；无限监控则保留原轮询间隔。
        QCOMPARE(boundedChargePhasePollDelayMs(250, 650, 530), 120);
        QCOMPARE(boundedChargePhasePollDelayMs(250, 650, 649), 1);
        QCOMPARE(boundedChargePhasePollDelayMs(250, 650, 650), 1);
        QCOMPARE(boundedChargePhasePollDelayMs(250, 0, 5000), 250);
    }

    void deviceManagerKeepsChargingOutOfTaskExecutorAndAddsBusinessGates()
    {
        const QString taskHeader = readSource(QStringLiteral("src/taskexecutor.h"));
        const QString taskSource = readSource(QStringLiteral("src/taskexecutor.cpp"));
        const QString deviceHeader = readSource(QStringLiteral("src/devicemanager.h"));
        const QString deviceSource = readSource(QStringLiteral("src/devicemanager.cpp"));

        QVERIFY(!taskHeader.contains(QStringLiteral("ChargePileController")));
        QVERIFY(!taskHeader.contains(QStringLiteral("AutoChargeCoordinator")));
        QVERIFY(!taskSource.contains(QStringLiteral("ChargePileController")));
        QVERIFY(!taskSource.contains(QStringLiteral("AutoChargeCoordinator")));

        QVERIFY(deviceHeader.contains(QStringLiteral("startManualCharge")));
        QVERIFY(deviceSource.contains(QStringLiteral("manualChargeStartRejectionReason")));
        QVERIFY(deviceSource.contains(QStringLiteral("requestConservativeRecovery")));
        QVERIFY(deviceSource.contains(QStringLiteral("charge-settings.ini")));
    }
};

QTEST_GUILESS_MAIN(LineManagerChargeHoldTest)
#include "test_line_manager_charge_hold_contract.moc"
