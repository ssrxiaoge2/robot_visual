#include <QtTest>

#define private public
#include "shortagemonitor.h"
#undef private

Q_DECLARE_METATYPE(Task)

class FakeCustomSysScheduler : public CustomSysScheduler
{
    Q_OBJECT
public:
    explicit FakeCustomSysScheduler(QObject *parent = nullptr)
        : CustomSysScheduler(parent)
    {
    }

    void fetchMesDayData(quint64 roundId) override
    {
        mesRounds.append(roundId);
    }

    void fetchPlcBits(quint64 roundId, int startAddress, int length) override
    {
        plcCalls.append({roundId, startAddress, length});
    }

    struct PlcCall {
        quint64 roundId = 0;
        int startAddress = 0;
        int length = 0;
    };

    QList<quint64> mesRounds;
    QList<PlcCall> plcCalls;
};

namespace {

Task makeTask(quint64 taskId, int stationId, TaskSource source)
{
    Task task;
    task.taskId = taskId;
    task.stationId = stationId;
    task.source = source;
    return task;
}

CustomSysScheduler::PlcBitReply plcReply(const QHash<QString, bool> &bits)
{
    CustomSysScheduler::PlcBitReply reply;
    reply.ok = true;
    reply.bits = bits;
    reply.timestamp = QDateTime::currentDateTime();
    return reply;
}

quint64 triggerNextRound(ShortageMonitor &monitor, FakeCustomSysScheduler &scheduler)
{
    const int before = scheduler.mesRounds.size();
    const bool invoked = QMetaObject::invokeMethod(&monitor, "onPollTimerTimeout");
    Q_ASSERT(invoked);
    Q_ASSERT(scheduler.mesRounds.size() == before + 1);
    return scheduler.mesRounds.constLast();
}

quint64 completeRound(ShortageMonitor &monitor,
                     FakeCustomSysScheduler &scheduler,
                     qint64 actualQty,
                     ProductModel product,
                     ProductionMode mode)
{
    Q_UNUSED(monitor);
    const quint64 roundId = scheduler.mesRounds.constLast();

    emit scheduler.mesReplyReady(roundId, true, actualQty, QString());

    QHash<QString, bool> productBits;
    productBits.insert(QStringLiteral("L71"), product == ProductModel::Model88);
    productBits.insert(QStringLiteral("L72"), product == ProductModel::Model88R);
    productBits.insert(QStringLiteral("L73"), product == ProductModel::Model92);
    emit scheduler.plcReplyReady(roundId, 71, plcReply(productBits));

    QHash<QString, bool> modeBits;
    modeBits.insert(QStringLiteral("L68"), mode == ProductionMode::L68);
    modeBits.insert(QStringLiteral("L69"), mode == ProductionMode::L69);
    modeBits.insert(QStringLiteral("L1998"), mode == ProductionMode::L1998);
    emit scheduler.plcReplyReady(roundId, 68, plcReply(modeBits));
    emit scheduler.plcReplyReady(roundId, 1998, plcReply({{QStringLiteral("L1998"),
                                                           mode == ProductionMode::L1998}}));

    return roundId;
}

void emitRoundData(FakeCustomSysScheduler &scheduler,
                   quint64 roundId,
                   qint64 actualQty,
                   const QHash<QString, bool> &productBits,
                   const QHash<QString, bool> &modeBits,
                   bool include1998 = true)
{
    emit scheduler.mesReplyReady(roundId, true, actualQty, QString());
    emit scheduler.plcReplyReady(roundId, 71, plcReply(productBits));
    emit scheduler.plcReplyReady(roundId, 68, plcReply(modeBits));
    if (include1998) {
        emit scheduler.plcReplyReady(roundId, 1998,
                                     plcReply({{QStringLiteral("L1998"),
                                                modeBits.value(QStringLiteral("L1998"), false)}}));
    }
}

QList<LiveShortageStationSnapshot> snapshotFor(const ShortageMonitor &monitor)
{
    return monitor.snapshot();
}

int countStationDispatches(const QSignalSpy &spy, int stationId)
{
    int count = 0;
    for (const QList<QVariant> &call : spy) {
        if (!call.isEmpty() && call.constFirst().toInt() == stationId) {
            ++count;
        }
    }
    return count;
}

} // namespace

class ShortageMonitorTest : public QObject
{
    Q_OBJECT

private slots:
    void shortage_emits_once_until_unload();
    void unload_adds_box_and_may_request_one_followup();
    void ui_mock_never_changes_inventory();
    void product_and_mode_require_two_rounds();
    void restart_after_stop_preserves_runtime_inventory();
    void communication_timeout_and_late_replies_are_ignored();
    void stop_ignores_late_replies_from_old_round();
    void invalid_product_or_mode_bits_fail_round();
    void communication_recovery_rebuilds_baseline_without_catchup();
    void task_finish_before_unload_retries_shortage();
    void product_switch_waits_for_old_product_tasks();
    void mode_switch_preserves_inventory_and_rebaselines();
};

void ShortageMonitorTest::shortage_emits_once_until_unload()
{
    FakeCustomSysScheduler scheduler;
    ShortageMonitor monitor(&scheduler);
    QSignalSpy dispatchSpy(&monitor, &ShortageMonitor::dispatchRequested);

    monitor.start();
    completeRound(monitor, scheduler, 0, ProductModel::Model88, ProductionMode::L1998);

    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 0, ProductModel::Model88, ProductionMode::L1998);

    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 250, ProductModel::Model88, ProductionMode::L1998);
    QCOMPARE(countStationDispatches(dispatchSpy, 1), 1);

    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 250, ProductModel::Model88, ProductionMode::L1998);
    QCOMPARE(countStationDispatches(dispatchSpy, 1), 1);

    monitor.confirmDispatch(1, 101, true);
    monitor.onTaskStarted(makeTask(101, 1, TaskSource::CustomerSystem));

    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 250, ProductModel::Model88, ProductionMode::L1998);
    QCOMPARE(countStationDispatches(dispatchSpy, 1), 1);
}

void ShortageMonitorTest::unload_adds_box_and_may_request_one_followup()
{
    FakeCustomSysScheduler scheduler;
    ShortageMonitor monitor(&scheduler);
    QSignalSpy dispatchSpy(&monitor, &ShortageMonitor::dispatchRequested);

    monitor.start();
    completeRound(monitor, scheduler, 0, ProductModel::Model88, ProductionMode::L1998);

    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 0, ProductModel::Model88, ProductionMode::L1998);

    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 500, ProductModel::Model88, ProductionMode::L1998);
    QCOMPARE(countStationDispatches(dispatchSpy, 1), 1);

    monitor.confirmDispatch(1, 101, true);
    monitor.onTaskStarted(makeTask(101, 1, TaskSource::CustomerSystem));
    monitor.onMaterialUnloaded(makeTask(101, 1, TaskSource::CustomerSystem));

    const QList<LiveShortageStationSnapshot> stations = snapshotFor(monitor);
    QCOMPARE(stations.at(0).estimatedAvailable, 1000);
    QCOMPARE(countStationDispatches(dispatchSpy, 1), 2);
}

void ShortageMonitorTest::ui_mock_never_changes_inventory()
{
    FakeCustomSysScheduler scheduler;
    ShortageMonitor monitor(&scheduler);

    monitor.start();
    completeRound(monitor, scheduler, 0, ProductModel::Model88, ProductionMode::L1998);

    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 0, ProductModel::Model88, ProductionMode::L1998);

    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 500, ProductModel::Model88, ProductionMode::L1998);
    monitor.confirmDispatch(1, 101, true);

    const qint64 beforeUnload = snapshotFor(monitor).at(0).estimatedAvailable;
    monitor.onMaterialUnloaded(makeTask(101, 1, TaskSource::UiMock));
    QCOMPARE(snapshotFor(monitor).at(0).estimatedAvailable, beforeUnload);
}

void ShortageMonitorTest::product_and_mode_require_two_rounds()
{
    FakeCustomSysScheduler scheduler;
    ShortageMonitor monitor(&scheduler);
    QSignalSpy sampleSpy(&monitor, &ShortageMonitor::sampleUpdated);

    monitor.start();
    completeRound(monitor, scheduler, 0, ProductModel::Model88, ProductionMode::L68);
    QCOMPARE(sampleSpy.count(), 0);

    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 0, ProductModel::Model88, ProductionMode::L68);
    QCOMPARE(sampleSpy.count(), 1);
    QCOMPARE(sampleSpy.at(0).at(1).value<ProductModel>(), ProductModel::Model88);
    QCOMPARE(sampleSpy.at(0).at(2).value<ProductionMode>(), ProductionMode::L68);

    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 0, ProductModel::Model92, ProductionMode::L1998);
    QCOMPARE(sampleSpy.count(), 2);
    QCOMPARE(sampleSpy.at(1).at(1).value<ProductModel>(), ProductModel::Model88);
    QCOMPARE(sampleSpy.at(1).at(2).value<ProductionMode>(), ProductionMode::L68);

    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 0, ProductModel::Model92, ProductionMode::L1998);
    QCOMPARE(sampleSpy.count(), 3);
    QCOMPARE(sampleSpy.at(2).at(1).value<ProductModel>(), ProductModel::Model92);
    QCOMPARE(sampleSpy.at(2).at(2).value<ProductionMode>(), ProductionMode::L1998);
}

void ShortageMonitorTest::restart_after_stop_preserves_runtime_inventory()
{
    FakeCustomSysScheduler scheduler;
    ShortageMonitor monitor(&scheduler);

    monitor.start();
    completeRound(monitor, scheduler, 0, ProductModel::Model88, ProductionMode::L1998);

    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 0, ProductModel::Model88, ProductionMode::L1998);

    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 250, ProductModel::Model88, ProductionMode::L1998);

    const qint64 beforeStop = snapshotFor(monitor).at(0).estimatedAvailable;
    monitor.stop();
    QCOMPARE(snapshotFor(monitor).at(0).estimatedAvailable, beforeStop);

    monitor.start();
    completeRound(monitor, scheduler, 250, ProductModel::Model88, ProductionMode::L1998);
    QCOMPARE(snapshotFor(monitor).at(0).estimatedAvailable, beforeStop);
}

void ShortageMonitorTest::communication_timeout_and_late_replies_are_ignored()
{
    FakeCustomSysScheduler scheduler;
    ShortageMonitor monitor(&scheduler);
    QSignalSpy sampleSpy(&monitor, &ShortageMonitor::sampleUpdated);
    QSignalSpy statusSpy(&monitor, &ShortageMonitor::statusChanged);

    monitor.start();
    const quint64 roundId = scheduler.mesRounds.constLast();
    emit scheduler.mesReplyReady(roundId, true, 100, QString());
    emit scheduler.plcReplyReady(roundId, 71, plcReply({
        {QStringLiteral("L71"), true},
        {QStringLiteral("L72"), false},
        {QStringLiteral("L73"), false},
    }));

    QVERIFY(QMetaObject::invokeMethod(&monitor, "onRoundTimeout"));
    QCOMPARE(sampleSpy.count(), 0);
    QVERIFY(!statusSpy.isEmpty());
    QVERIFY(!statusSpy.constLast().at(1).toBool());

    emit scheduler.plcReplyReady(roundId, 68, plcReply({
        {QStringLiteral("L68"), false},
        {QStringLiteral("L69"), false},
        {QStringLiteral("L1998"), true},
    }));
    emit scheduler.plcReplyReady(roundId, 1998, plcReply({{QStringLiteral("L1998"), true}}));
    QCOMPARE(sampleSpy.count(), 0);
}

void ShortageMonitorTest::stop_ignores_late_replies_from_old_round()
{
    FakeCustomSysScheduler scheduler;
    ShortageMonitor monitor(&scheduler);
    QSignalSpy sampleSpy(&monitor, &ShortageMonitor::sampleUpdated);

    monitor.start();
    const quint64 roundId = scheduler.mesRounds.constLast();
    monitor.stop();

    emitRoundData(scheduler, roundId, 100, {
        {QStringLiteral("L71"), true},
        {QStringLiteral("L72"), false},
        {QStringLiteral("L73"), false},
    }, {
        {QStringLiteral("L68"), false},
        {QStringLiteral("L69"), false},
        {QStringLiteral("L1998"), true},
    });

    QCOMPARE(sampleSpy.count(), 0);
    QVERIFY(!monitor.isRunning());
}

void ShortageMonitorTest::invalid_product_or_mode_bits_fail_round()
{
    FakeCustomSysScheduler scheduler;
    ShortageMonitor monitor(&scheduler);
    QSignalSpy sampleSpy(&monitor, &ShortageMonitor::sampleUpdated);
    QSignalSpy statusSpy(&monitor, &ShortageMonitor::statusChanged);

    monitor.start();
    emitRoundData(scheduler, scheduler.mesRounds.constLast(), 0, {
        {QStringLiteral("L71"), true},
        {QStringLiteral("L72"), true},
        {QStringLiteral("L73"), false},
    }, {
        {QStringLiteral("L68"), false},
        {QStringLiteral("L69"), false},
        {QStringLiteral("L1998"), true},
    });
    QCOMPARE(sampleSpy.count(), 0);
    QVERIFY(!statusSpy.constLast().at(1).toBool());

    triggerNextRound(monitor, scheduler);
    emitRoundData(scheduler, scheduler.mesRounds.constLast(), 0, {
        {QStringLiteral("L71"), true},
        {QStringLiteral("L72"), false},
        {QStringLiteral("L73"), false},
    }, {
        {QStringLiteral("L68"), true},
        {QStringLiteral("L69"), true},
        {QStringLiteral("L1998"), false},
    });
    QCOMPARE(sampleSpy.count(), 0);
    QVERIFY(!statusSpy.constLast().at(1).toBool());
}

void ShortageMonitorTest::communication_recovery_rebuilds_baseline_without_catchup()
{
    FakeCustomSysScheduler scheduler;
    ShortageMonitor monitor(&scheduler);

    monitor.start();
    completeRound(monitor, scheduler, 0, ProductModel::Model88, ProductionMode::L1998);
    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 0, ProductModel::Model88, ProductionMode::L1998);
    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 100, ProductModel::Model88, ProductionMode::L1998);
    const qint64 beforeFailure = snapshotFor(monitor).at(0).estimatedAvailable;

    triggerNextRound(monitor, scheduler);
    const quint64 failedRound = scheduler.mesRounds.constLast();
    emit scheduler.mesReplyReady(failedRound, false, 0, QStringLiteral("MES fail"));

    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 300, ProductModel::Model88, ProductionMode::L1998);
    QCOMPARE(snapshotFor(monitor).at(0).estimatedAvailable, beforeFailure);

    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 320, ProductModel::Model88, ProductionMode::L1998);
    QCOMPARE(snapshotFor(monitor).at(0).estimatedAvailable, beforeFailure - 20);
}

void ShortageMonitorTest::task_finish_before_unload_retries_shortage()
{
    FakeCustomSysScheduler scheduler;
    ShortageMonitor monitor(&scheduler);
    QSignalSpy dispatchSpy(&monitor, &ShortageMonitor::dispatchRequested);

    monitor.start();
    completeRound(monitor, scheduler, 0, ProductModel::Model88, ProductionMode::L1998);
    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 0, ProductModel::Model88, ProductionMode::L1998);
    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 250, ProductModel::Model88, ProductionMode::L1998);
    QCOMPARE(countStationDispatches(dispatchSpy, 1), 1);

    monitor.confirmDispatch(1, 201, true);
    monitor.onTaskStarted(makeTask(201, 1, TaskSource::CustomerSystem));
    monitor.onTaskFinished(makeTask(201, 1, TaskSource::CustomerSystem));
    QCOMPARE(countStationDispatches(dispatchSpy, 1), 2);
}

void ShortageMonitorTest::product_switch_waits_for_old_product_tasks()
{
    FakeCustomSysScheduler scheduler;
    ShortageMonitor monitor(&scheduler);
    QSignalSpy dispatchSpy(&monitor, &ShortageMonitor::dispatchRequested);

    monitor.start();
    completeRound(monitor, scheduler, 0, ProductModel::Model88, ProductionMode::L1998);
    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 0, ProductModel::Model88, ProductionMode::L1998);
    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 250, ProductModel::Model88, ProductionMode::L1998);
    QCOMPARE(countStationDispatches(dispatchSpy, 1), 1);

    monitor.confirmDispatch(1, 301, true);
    monitor.onTaskStarted(makeTask(301, 1, TaskSource::CustomerSystem));

    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 250, ProductModel::Model92, ProductionMode::L1998);
    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 250, ProductModel::Model92, ProductionMode::L1998);
    QVERIFY(monitor.m_waitingForOldProductTasks);

    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 900, ProductModel::Model92, ProductionMode::L1998);
    QCOMPARE(snapshotFor(monitor).at(0).state, LiveShortageTaskState::WaitingOldProductTasks);
    QCOMPARE(countStationDispatches(dispatchSpy, 1), 1);

    monitor.onTaskFinished(makeTask(301, 1, TaskSource::CustomerSystem));
    QVERIFY(!monitor.m_waitingForOldProductTasks);
    QCOMPARE(countStationDispatches(dispatchSpy, 1), 2);
}

void ShortageMonitorTest::mode_switch_preserves_inventory_and_rebaselines()
{
    FakeCustomSysScheduler scheduler;
    ShortageMonitor monitor(&scheduler);

    monitor.start();
    completeRound(monitor, scheduler, 0, ProductModel::Model88, ProductionMode::L68);
    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 0, ProductModel::Model88, ProductionMode::L68);
    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 20, ProductModel::Model88, ProductionMode::L68);
    const qint64 beforeModeSwitch = snapshotFor(monitor).at(0).estimatedAvailable;

    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 20, ProductModel::Model88, ProductionMode::L1998);
    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 20, ProductModel::Model88, ProductionMode::L1998);
    QCOMPARE(snapshotFor(monitor).at(0).estimatedAvailable, beforeModeSwitch);

    triggerNextRound(monitor, scheduler);
    completeRound(monitor, scheduler, 30, ProductModel::Model88, ProductionMode::L1998);
    QCOMPARE(snapshotFor(monitor).at(0).estimatedAvailable, beforeModeSwitch - 10);
}

QTEST_MAIN(ShortageMonitorTest)
#include "test_shortagemonitor.moc"
