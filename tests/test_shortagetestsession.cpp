#include <QtTest>

#include "shortagetestsession.h"

class FakeCustomSysScheduler : public CustomSysScheduler
{
    Q_OBJECT
public:
    explicit FakeCustomSysScheduler(QObject *parent = nullptr)
        : CustomSysScheduler(parent)
    {
    }

    struct PlcCall {
        quint64 roundId = 0;
        int startAddress = 0;
        int length = 0;
    };

    void fetchMesDayData(quint64 roundId) override
    {
        mesRounds.append(roundId);
    }

    void fetchPlcBits(quint64 roundId, int startAddress, int length) override
    {
        plcCalls.append({roundId, startAddress, length});
    }

    QList<quint64> mesRounds;
    QList<PlcCall> plcCalls;
};

static QHash<QString, bool> validBits88L68()
{
    return {
        {QStringLiteral("L68"), true},
        {QStringLiteral("L69"), false},
        {QStringLiteral("L71"), true},
        {QStringLiteral("L72"), false},
        {QStringLiteral("L73"), false},
        {QStringLiteral("L1998"), false},
    };
}

static void emitCompleteRound(FakeCustomSysScheduler &client,
                              quint64 roundId,
                              qint64 actualQty,
                              const QHash<QString, bool> &bits)
{
    emit client.mesReplyReady(roundId, true, actualQty, QString());

    CustomSysScheduler::PlcBitReply plc68;
    plc68.ok = true;
    plc68.bits.insert(QStringLiteral("L68"), bits.value(QStringLiteral("L68")));
    plc68.bits.insert(QStringLiteral("L69"), bits.value(QStringLiteral("L69")));
    plc68.timestamp = QDateTime::currentDateTime();
    emit client.plcReplyReady(roundId, 68, plc68);

    CustomSysScheduler::PlcBitReply plc71;
    plc71.ok = true;
    plc71.bits.insert(QStringLiteral("L71"), bits.value(QStringLiteral("L71")));
    plc71.bits.insert(QStringLiteral("L72"), bits.value(QStringLiteral("L72")));
    plc71.bits.insert(QStringLiteral("L73"), bits.value(QStringLiteral("L73")));
    plc71.timestamp = QDateTime::currentDateTime();
    emit client.plcReplyReady(roundId, 71, plc71);

    CustomSysScheduler::PlcBitReply plc1998;
    plc1998.ok = true;
    plc1998.bits.insert(QStringLiteral("L1998"), bits.value(QStringLiteral("L1998")));
    plc1998.timestamp = QDateTime::currentDateTime();
    emit client.plcReplyReady(roundId, 1998, plc1998);
}

class ShortageTestSessionTest : public QObject
{
    Q_OBJECT

private slots:
    void start_immediately_requests_four_calls();
    void complete_round_requires_all_fragments();
    void stop_ignores_late_reply();
    void stable_two_rounds_enable_deduction();
    void invalid_bits_report_fault();
};

void ShortageTestSessionTest::start_immediately_requests_four_calls()
{
    FakeCustomSysScheduler client;
    ShortageTestSession session(&client);

    session.start();

    QCOMPARE(client.mesRounds.size(), 1);
    QCOMPARE(client.plcCalls.size(), 3);
    QCOMPARE(client.plcCalls.at(0).startAddress, 68);
    QCOMPARE(client.plcCalls.at(1).startAddress, 71);
    QCOMPARE(client.plcCalls.at(2).startAddress, 1998);
}

void ShortageTestSessionTest::complete_round_requires_all_fragments()
{
    FakeCustomSysScheduler client;
    ShortageTestSession session(&client);
    QSignalSpy inventorySpy(&session, &ShortageTestSession::inventoryUpdated);

    session.start();
    const quint64 roundId = client.mesRounds.constFirst();
    emit client.mesReplyReady(roundId, true, 1000, QString());
    QCOMPARE(inventorySpy.count(), 0);

    CustomSysScheduler::PlcBitReply plc68;
    plc68.ok = true;
    plc68.bits.insert(QStringLiteral("L68"), true);
    plc68.bits.insert(QStringLiteral("L69"), false);
    emit client.plcReplyReady(roundId, 68, plc68);
    QCOMPARE(inventorySpy.count(), 0);
}

void ShortageTestSessionTest::stop_ignores_late_reply()
{
    FakeCustomSysScheduler client;
    ShortageTestSession session(&client);
    QSignalSpy inventorySpy(&session, &ShortageTestSession::inventoryUpdated);

    session.start();
    const quint64 roundId = client.mesRounds.constFirst();
    session.stop();
    emitCompleteRound(client, roundId, 1000, validBits88L68());
    QCOMPARE(inventorySpy.count(), 0);
}

void ShortageTestSessionTest::stable_two_rounds_enable_deduction()
{
    FakeCustomSysScheduler client;
    ShortageTestSession session(&client);
    QSignalSpy inventorySpy(&session, &ShortageTestSession::inventoryUpdated);

    session.start();
    const quint64 roundId = client.mesRounds.constFirst();
    emitCompleteRound(client, roundId, 1000, validBits88L68());
    QCOMPARE(session.snapshot().first().estimatedAvailable, 1250);

    QVERIFY(QMetaObject::invokeMethod(&session, "onPollTimerTimeout", Qt::DirectConnection));
    const quint64 roundId2 = client.mesRounds.constLast();
    emitCompleteRound(client, roundId2, 1020, validBits88L68());
    QCOMPARE(session.snapshot().first().estimatedAvailable, 1230);
    QCOMPARE(inventorySpy.count(), 2);
}

void ShortageTestSessionTest::invalid_bits_report_fault()
{
    FakeCustomSysScheduler client;
    ShortageTestSession session(&client);
    QSignalSpy statusSpy(&session, &ShortageTestSession::statusChanged);

    session.start();
    const quint64 roundId = client.mesRounds.constFirst();
    QHash<QString, bool> invalidBits = validBits88L68();
    invalidBits.insert(QStringLiteral("L72"), true);
    emitCompleteRound(client, roundId, 1000, invalidBits);

    QVERIFY(!statusSpy.isEmpty());
    QCOMPARE(statusSpy.constLast().at(0).toString(), QStringLiteral("产品信号不唯一"));
}

QTEST_MAIN(ShortageTestSessionTest)

#include "test_shortagetestsession.moc"
