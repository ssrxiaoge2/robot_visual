#include <QtTest>

#include "shortagemonitor.h"

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

class ShortageMonitorTest : public QObject
{
    Q_OBJECT

private slots:
    void start_immediately_requests_four_calls();
    void complete_round_emits_sample_only_after_all_fragments_arrive();
};

void ShortageMonitorTest::start_immediately_requests_four_calls()
{
    FakeCustomSysScheduler scheduler;
    ShortageMonitor monitor(&scheduler);

    monitor.start();

    QCOMPARE(scheduler.mesRounds.size(), 1);
    QCOMPARE(scheduler.plcCalls.size(), 3);
    QCOMPARE(scheduler.plcCalls.at(0).startAddress, 68);
    QCOMPARE(scheduler.plcCalls.at(1).startAddress, 71);
    QCOMPARE(scheduler.plcCalls.at(2).startAddress, 1998);
}

void ShortageMonitorTest::complete_round_emits_sample_only_after_all_fragments_arrive()
{
    FakeCustomSysScheduler scheduler;
    ShortageMonitor monitor(&scheduler);
    QSignalSpy sampleSpy(&monitor, &ShortageMonitor::sampleUpdated);

    monitor.start();
    QVERIFY(!scheduler.mesRounds.isEmpty());
    const quint64 roundId = scheduler.mesRounds.constFirst();

    emit scheduler.mesReplyReady(roundId, true, 123, QString());
    QCOMPARE(sampleSpy.count(), 0);

    CustomSysScheduler::PlcBitReply plc68;
    plc68.ok = true;
    plc68.bits.insert(QStringLiteral("L68"), true);
    plc68.bits.insert(QStringLiteral("L69"), false);
    plc68.timestamp = QDateTime::currentDateTime();
    emit scheduler.plcReplyReady(roundId, 68, plc68);
    QCOMPARE(sampleSpy.count(), 0);

    CustomSysScheduler::PlcBitReply plc71;
    plc71.ok = true;
    plc71.bits.insert(QStringLiteral("L71"), true);
    plc71.bits.insert(QStringLiteral("L72"), false);
    plc71.bits.insert(QStringLiteral("L73"), false);
    plc71.timestamp = QDateTime::currentDateTime();
    emit scheduler.plcReplyReady(roundId, 71, plc71);
    QCOMPARE(sampleSpy.count(), 0);

    CustomSysScheduler::PlcBitReply plc1998;
    plc1998.ok = true;
    plc1998.bits.insert(QStringLiteral("L1998"), false);
    plc1998.timestamp = QDateTime::currentDateTime();
    emit scheduler.plcReplyReady(roundId, 1998, plc1998);
    QCOMPARE(sampleSpy.count(), 1);
}

QTEST_MAIN(ShortageMonitorTest)
#include "test_shortagemonitor.moc"
