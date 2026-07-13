#include "customSysScheduler.h"
#include "shortagesamplecoordinator.h"

#include <QCoreApplication>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>
#include <limits>

namespace {

QString sourcePath(const QString &relativePath)
{
    return QDir(QString::fromUtf8(ROBOT_VISUAL_SOURCE_DIR)).filePath(relativePath);
}

QString readSourceText(const QString &relativePath)
{
    QFile file(sourcePath(relativePath));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(file.readAll());
}

QByteArray mesPayload(const QString &actualQtyLiteral)
{
    return QStringLiteral(R"([{"id":1,"statDate":"2026-06-25T08:00:00","lineId":"L1","lineName":"A线","planQty":10,"actualQty":%1,"okQty":9,"ngQty":1}])")
        .arg(actualQtyLiteral)
        .toUtf8();
}

QByteArray plcPayload(const QString &dataItems,
                      bool success = true,
                      const QString &timestamp = QStringLiteral("2026-06-25T08:00:00.123Z"))
{
    return QStringLiteral(R"({"success":%1,"timestamp":"%2","data":[%3]})")
        .arg(success ? QStringLiteral("true") : QStringLiteral("false"),
             timestamp,
             dataItems)
        .toUtf8();
}

QByteArray plcItem(int address, const QString &valueLiteral)
{
    return QStringLiteral(R"({"address":"L%1","value":%2})")
        .arg(address)
        .arg(valueLiteral)
        .toUtf8();
}

class LocalProtocolServer final : public QTcpServer
{
    Q_OBJECT
public:
    explicit LocalProtocolServer(QObject *parent = nullptr)
        : QTcpServer(parent)
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    const QByteArray requestBytes = socket->readAll();
                    const QList<QByteArray> lines = requestBytes.split('\n');
                    if (lines.isEmpty())
                        return;

                    const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
                    if (requestLine.size() < 2)
                        return;

                    const QUrl url(QStringLiteral("http://127.0.0.1") + QString::fromLatin1(requestLine.at(1)));
                    paths.append(url.path());
                    queries.append(QUrlQuery(url));

                    QByteArray body;
                    if (url.path() == QStringLiteral("/api/MesData/day")) {
                        body = mesPayload(QStringLiteral("922337203685477580"));
                    } else if (url.path() == QStringLiteral("/api/PlcData/GetLBitRegister")) {
                        body = plcPayload(QString::fromUtf8(plcItem(68, QStringLiteral("true")))
                            + QStringLiteral(",")
                            + QString::fromUtf8(plcItem(69, QStringLiteral("false"))));
                    } else {
                        body = R"({"error":"unexpected path"})";
                    }

                    const QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                        + QByteArray::number(body.size())
                        + "\r\nConnection: close\r\n\r\n"
                        + body;
                    socket->write(response);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QStringList paths;
    QList<QUrlQuery> queries;
};

class FakeCustomSysScheduler final : public CustomSysScheduler
{
    Q_OBJECT
public:
    using BitList = QList<std::pair<int, bool>>;

    struct PlcRequest {
        quint64 roundId = 0;
        int startAddress = 0;
        int length = 0;
    };

    explicit FakeCustomSysScheduler(QObject *parent = nullptr)
        : CustomSysScheduler(parent)
    {
    }

    void fetchMesDayData(quint64 roundId) override { mesRequests.append(roundId); }

    void fetchPlcBits(quint64 roundId, int startAddress, int length) override
    {
        plcRequests.append(PlcRequest{roundId, startAddress, length});
    }

    void replyMes(quint64 roundId, qint64 actualQty)
    {
        LiveMesDayReply reply;
        reply.ok = true;
        reply.actualQty = actualQty;
        emit mesReplyReady(roundId, reply);
    }

    void failMes(quint64 roundId, const QString &reason)
    {
        LiveMesDayReply reply;
        reply.errorMessage = reason;
        emit mesReplyReady(roundId, reply);
    }

    void replyPlc(quint64 roundId, int startAddress, std::initializer_list<std::pair<int, bool>> bits)
    {
        replyPlc(roundId, startAddress, BitList(bits));
    }

    void replyPlc(quint64 roundId, int startAddress, const BitList &bits)
    {
        PlcBitReply reply;
        reply.ok = true;
        reply.timestamp = QStringLiteral("2026-06-25T08:00:00.123Z");
        for (const auto &bit : bits)
            reply.values.insert(bit.first, bit.second);
        emit plcReplyReady(roundId, startAddress, reply);
    }

    void failPlc(quint64 roundId, int startAddress, const QString &reason)
    {
        PlcBitReply reply;
        reply.errorMessage = reason;
        emit plcReplyReady(roundId, startAddress, reply);
    }

    QList<quint64> mesRequests;
    QList<PlcRequest> plcRequests;
};

ShortageParameters fastParameters()
{
    ShortageParameters parameters;
    parameters.sampleIntervalSeconds = 5;
    parameters.roundTimeoutSeconds = 1;
    parameters.communicationAlarmMinutes = 1;
    return parameters;
}

FakeCustomSysScheduler::BitList productBits(ProductModel product)
{
    switch (product) {
    case ProductModel::Model88:
        return FakeCustomSysScheduler::BitList{{71, true}, {72, false}, {73, false}};
    case ProductModel::Model88R:
        return FakeCustomSysScheduler::BitList{{71, false}, {72, true}, {73, false}};
    case ProductModel::Model92:
        return FakeCustomSysScheduler::BitList{{71, false}, {72, false}, {73, true}};
    }
    return FakeCustomSysScheduler::BitList{{71, false}, {72, false}, {73, false}};
}

FakeCustomSysScheduler::BitList modeBits68(ProductionMode mode)
{
    switch (mode) {
    case ProductionMode::LeftRight:
        return FakeCustomSysScheduler::BitList{{68, true}, {69, false}};
    case ProductionMode::LeftOnly:
        return FakeCustomSysScheduler::BitList{{68, false}, {69, true}};
    case ProductionMode::RightOnly:
        return FakeCustomSysScheduler::BitList{{68, false}, {69, false}};
    }
    return FakeCustomSysScheduler::BitList{{68, false}, {69, false}};
}

bool modeBit1998(ProductionMode mode)
{
    return mode == ProductionMode::RightOnly;
}

void completeRound(FakeCustomSysScheduler &scheduler,
                   quint64 roundId,
                   qint64 actualQty,
                   ProductModel product = ProductModel::Model88,
                   ProductionMode mode = ProductionMode::LeftRight)
{
    scheduler.replyMes(roundId, actualQty);
    scheduler.replyPlc(roundId, 68, modeBits68(mode));
    scheduler.replyPlc(roundId, 71, productBits(product));
    scheduler.replyPlc(roundId, 1998, {{1998, modeBit1998(mode)}});
}

void establishInitialContext(ShortageSampleCoordinator &coordinator,
                             FakeCustomSysScheduler &scheduler,
                             qint64 secondActualQty = 100,
                             ProductModel product = ProductModel::Model88,
                             ProductionMode mode = ProductionMode::LeftRight)
{
    coordinator.start();
    completeRound(scheduler, scheduler.mesRequests.last(), secondActualQty - 1, product, mode);
    coordinator.triggerNextRoundForTest();
    completeRound(scheduler, scheduler.mesRequests.last(), secondActualQty, product, mode);
}

} // namespace

class ShortageSampleCoordinatorTest final : public QObject
{
    Q_OBJECT
private slots:
    void mesParserKeepsOldBranchPayloadContract();
    void plcParserKeepsOldBranchPayloadContract();
    void plcParserRejectsMissingDuplicateOrWrongTypeBits();
    void protocolReplyKeepsRoundIdAndAddressRange();
    void legacyDiagnosticSurfaceIsAbsent();
    void emitsOnlyAfterAllFourRepliesOfSameRound();
    void timeoutRejectsWholeRound();
    void staleReplyCannotCompleteNewRound();
    void productBitsRequireExactlyOneTrue();
    void modeBitsRequireExactlyOneTrue();
    void contextRequiresTwoConsecutiveValidRounds();
    void newTimingParametersApplyToNextRound();
    void reconnectAtOrAboveBaselineProducesCatchupSample();
    void alarmTurnsRedAtConfiguredDuration();
    void reconnectBelowBaselineRequiresMaintenance();
    void httpAndJsonErrorsRejectWithoutPartialState();
    void actualQtyUsesSignedSixtyFourBitValidation();
    void oneRoundContextGlitchDoesNotSwitch();
    void twoStableRoundsCreatePendingContext();
    void directContextConfirmationSlotCanActivateImmediately();
    void allNineContextCombinationsAreRecognized();
};

void ShortageSampleCoordinatorTest::mesParserKeepsOldBranchPayloadContract()
{
    const auto ok = CustomSysScheduler::parseMesDayReply(
        mesPayload(QStringLiteral("922337203685477580")));
    QVERIFY2(ok.ok, qPrintable(ok.errorMessage));
    QCOMPARE(ok.actualQty, qint64{922337203685477580});
    QVERIFY(ok.errorMessage.isEmpty());

    const auto missing = CustomSysScheduler::parseMesDayReply(
        QByteArrayLiteral(R"([{"id":1,"statDate":"2026-06-25T08:00:00"}])"));
    QVERIFY(!missing.ok);
    QVERIFY2(missing.errorMessage.contains(QStringLiteral("actualQty")),
             qPrintable(missing.errorMessage));

    const auto negative = CustomSysScheduler::parseMesDayReply(
        mesPayload(QStringLiteral("-1")));
    QVERIFY(!negative.ok);
    QVERIFY2(negative.errorMessage.contains(QStringLiteral("actualQty")),
             qPrintable(negative.errorMessage));

    const auto overflow = CustomSysScheduler::parseMesDayReply(
        mesPayload(QStringLiteral("9223372036854775808")));
    QVERIFY(!overflow.ok);
    QVERIFY2(overflow.errorMessage.contains(QStringLiteral("actualQty")),
             qPrintable(overflow.errorMessage));
}

void ShortageSampleCoordinatorTest::plcParserKeepsOldBranchPayloadContract()
{
    const auto reply = CustomSysScheduler::parsePlcBitReply(
        plcPayload(QString::fromUtf8(plcItem(68, QStringLiteral("true")))
            + QStringLiteral(",")
            + QString::fromUtf8(plcItem(69, QStringLiteral("false")))),
        68,
        2);

    QVERIFY2(reply.ok, qPrintable(reply.errorMessage));
    QCOMPARE(reply.timestamp, QStringLiteral("2026-06-25T08:00:00.123Z"));
    QCOMPARE(reply.values.size(), 2);
    QVERIFY(reply.values.contains(68));
    QVERIFY(reply.values.contains(69));
    QCOMPARE(reply.values.value(68), true);
    QCOMPARE(reply.values.value(69), false);
    QVERIFY(reply.errorMessage.isEmpty());
}

void ShortageSampleCoordinatorTest::plcParserRejectsMissingDuplicateOrWrongTypeBits()
{
    const auto missing = CustomSysScheduler::parsePlcBitReply(
        plcPayload(QString::fromUtf8(plcItem(68, QStringLiteral("true")))),
        68,
        2);
    QVERIFY(!missing.ok);
    QVERIFY2(missing.errorMessage.contains(QStringLiteral("缺少")),
             qPrintable(missing.errorMessage));

    const auto duplicate = CustomSysScheduler::parsePlcBitReply(
        plcPayload(QString::fromUtf8(plcItem(68, QStringLiteral("true")))
            + QStringLiteral(",")
            + QString::fromUtf8(plcItem(68, QStringLiteral("false")))),
        68,
        1);
    QVERIFY(!duplicate.ok);
    QVERIFY2(duplicate.errorMessage.contains(QStringLiteral("重复")),
             qPrintable(duplicate.errorMessage));

    const auto outOfRange = CustomSysScheduler::parsePlcBitReply(
        plcPayload(QString::fromUtf8(plcItem(68, QStringLiteral("true")))
            + QStringLiteral(",")
            + QString::fromUtf8(plcItem(70, QStringLiteral("false")))),
        68,
        2);
    QVERIFY(!outOfRange.ok);
    QVERIFY2(outOfRange.errorMessage.contains(QStringLiteral("越界")),
             qPrintable(outOfRange.errorMessage));

    const auto wrongType = CustomSysScheduler::parsePlcBitReply(
        plcPayload(QString::fromUtf8(plcItem(68, QStringLiteral("\"true\"")))),
        68,
        1);
    QVERIFY(!wrongType.ok);
    QVERIFY2(wrongType.errorMessage.contains(QStringLiteral("value")),
             qPrintable(wrongType.errorMessage));
}

void ShortageSampleCoordinatorTest::protocolReplyKeepsRoundIdAndAddressRange()
{
    LocalProtocolServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    CustomSysScheduler scheduler;
    QString endpointError;
    QVERIFY2(scheduler.setLiveMesDayEndpoint(
                 QUrl(QStringLiteral("http://127.0.0.1:%1/api/MesData/day")
                          .arg(server.serverPort())),
                 &endpointError),
             qPrintable(endpointError));
    QVERIFY(scheduler.setRequestTimeoutMs(1000));

    QSignalSpy mesSpy(&scheduler, &CustomSysScheduler::mesReplyReady);
    QSignalSpy plcSpy(&scheduler, &CustomSysScheduler::plcReplyReady);
    QVERIFY(mesSpy.isValid());
    QVERIFY(plcSpy.isValid());

    scheduler.fetchMesDayData(1001);
    scheduler.fetchPlcBits(1002, 68, 2);

    QTRY_VERIFY_WITH_TIMEOUT(mesSpy.count() > 0, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(plcSpy.count() > 0, 2000);

    QCOMPARE(mesSpy.first().at(0).toULongLong(), quint64{1001});
    const auto mesReply = qvariant_cast<CustomSysScheduler::LiveMesDayReply>(
        mesSpy.first().at(1));
    QVERIFY2(mesReply.ok, qPrintable(mesReply.errorMessage));
    QCOMPARE(mesReply.actualQty, qint64{922337203685477580});

    QCOMPARE(plcSpy.first().at(0).toULongLong(), quint64{1002});
    QCOMPARE(plcSpy.first().at(1).toInt(), 68);
    const auto plcReply = qvariant_cast<CustomSysScheduler::PlcBitReply>(
        plcSpy.first().at(2));
    QVERIFY2(plcReply.ok, qPrintable(plcReply.errorMessage));
    QCOMPARE(plcReply.values.value(68), true);
    QCOMPARE(plcReply.values.value(69), false);

    QVERIFY(server.paths.contains(QStringLiteral("/api/MesData/day")));
    QVERIFY(server.paths.contains(QStringLiteral("/api/PlcData/GetLBitRegister")));
    bool sawPlcQuery = false;
    for (const QUrlQuery &query : server.queries) {
        if (query.queryItemValue(QStringLiteral("StartAddress")) == QStringLiteral("68")
            && query.queryItemValue(QStringLiteral("length")) == QStringLiteral("2")) {
            sawPlcQuery = true;
            break;
        }
    }
    QVERIFY(sawPlcQuery);
}

void ShortageSampleCoordinatorTest::emitsOnlyAfterAllFourRepliesOfSameRound()
{
    FakeCustomSysScheduler scheduler;
    ShortageSampleCoordinator coordinator(&scheduler);
    coordinator.setParameters(fastParameters());
    QSignalSpy stableSpy(&coordinator, &ShortageSampleCoordinator::stableSampleReady);
    QSignalSpy rejectedSpy(&coordinator, &ShortageSampleCoordinator::sampleRejected);

    establishInitialContext(coordinator, scheduler);
    stableSpy.clear();
    rejectedSpy.clear();

    coordinator.triggerNextRoundForTest();
    const quint64 roundId = scheduler.mesRequests.last();
    scheduler.replyMes(roundId, 120);
    scheduler.replyPlc(roundId, 68, modeBits68(ProductionMode::LeftRight));
    scheduler.replyPlc(roundId, 71, productBits(ProductModel::Model88));
    QCOMPARE(stableSpy.count(), 0);

    scheduler.replyPlc(roundId, 1998, {{1998, false}});
    QCOMPARE(rejectedSpy.count(), 0);
    QCOMPARE(stableSpy.count(), 1);
    const auto sample = qvariant_cast<ShortageSample>(stableSpy.takeFirst().at(0));
    QCOMPARE(sample.roundId, roundId);
    QCOMPARE(sample.actualQty, qint64{120});
}

void ShortageSampleCoordinatorTest::timeoutRejectsWholeRound()
{
    FakeCustomSysScheduler scheduler;
    ShortageSampleCoordinator coordinator(&scheduler);
    coordinator.setParameters(fastParameters());
    QSignalSpy stableSpy(&coordinator, &ShortageSampleCoordinator::stableSampleReady);
    QSignalSpy rejectedSpy(&coordinator, &ShortageSampleCoordinator::sampleRejected);
    QSignalSpy stateSpy(&coordinator, &ShortageSampleCoordinator::communicationStateChanged);

    coordinator.start();
    const quint64 roundId = scheduler.mesRequests.last();
    coordinator.triggerRoundTimeoutForTest();
    QCOMPARE(rejectedSpy.count(), 1);
    QCOMPARE(rejectedSpy.first().at(0).toULongLong(), roundId);
    QCOMPARE(qvariant_cast<ShortageCommunicationState>(stateSpy.last().at(0)),
             ShortageCommunicationState::Interrupted);

    completeRound(scheduler, roundId, 10);
    QCOMPARE(stableSpy.count(), 0);
    QCOMPARE(rejectedSpy.count(), 1);
}

void ShortageSampleCoordinatorTest::staleReplyCannotCompleteNewRound()
{
    FakeCustomSysScheduler scheduler;
    ShortageSampleCoordinator coordinator(&scheduler);
    coordinator.setParameters(fastParameters());
    QSignalSpy stableSpy(&coordinator, &ShortageSampleCoordinator::stableSampleReady);
    QSignalSpy rejectedSpy(&coordinator, &ShortageSampleCoordinator::sampleRejected);

    coordinator.start();
    const quint64 staleRound = scheduler.mesRequests.last();
    scheduler.replyMes(staleRound, 1);
    coordinator.triggerRoundTimeoutForTest();

    coordinator.triggerNextRoundForTest();
    const quint64 newRound = scheduler.mesRequests.last();
    scheduler.replyPlc(staleRound, 68, modeBits68(ProductionMode::LeftRight));
    scheduler.replyPlc(staleRound, 71, productBits(ProductModel::Model88));
    scheduler.replyPlc(staleRound, 1998, {{1998, false}});
    completeRound(scheduler, newRound, 20);
    QCOMPARE(stableSpy.count(), 0);
    QCOMPARE(rejectedSpy.count(), 1);

    coordinator.triggerNextRoundForTest();
    completeRound(scheduler, scheduler.mesRequests.last(), 21);
    QCOMPARE(stableSpy.count(), 1);
}

void ShortageSampleCoordinatorTest::productBitsRequireExactlyOneTrue()
{
    FakeCustomSysScheduler scheduler;
    ShortageSampleCoordinator coordinator(&scheduler);
    coordinator.setParameters(fastParameters());
    QSignalSpy rejectedSpy(&coordinator, &ShortageSampleCoordinator::sampleRejected);
    QSignalSpy stableSpy(&coordinator, &ShortageSampleCoordinator::stableSampleReady);

    coordinator.start();
    const quint64 roundId = scheduler.mesRequests.last();
    scheduler.replyMes(roundId, 10);
    scheduler.replyPlc(roundId, 68, modeBits68(ProductionMode::LeftRight));
    scheduler.replyPlc(roundId, 71, {{71, true}, {72, true}, {73, false}});
    scheduler.replyPlc(roundId, 1998, {{1998, false}});

    QCOMPARE(stableSpy.count(), 0);
    QCOMPARE(rejectedSpy.count(), 1);
    const QString reason = rejectedSpy.first().at(1).toString();
    QVERIFY2(reason.contains(QStringLiteral("L71=true")), qPrintable(reason));
    QVERIFY2(reason.contains(QStringLiteral("L72=true")), qPrintable(reason));
    QVERIFY2(reason.contains(QStringLiteral("L73=false")), qPrintable(reason));
    QVERIFY2(reason.contains(QStringLiteral("L68=true")), qPrintable(reason));
    QVERIFY2(reason.contains(QStringLiteral("L69=false")), qPrintable(reason));
    QVERIFY2(reason.contains(QStringLiteral("L1998=false")), qPrintable(reason));
}

void ShortageSampleCoordinatorTest::modeBitsRequireExactlyOneTrue()
{
    FakeCustomSysScheduler scheduler;
    ShortageSampleCoordinator coordinator(&scheduler);
    coordinator.setParameters(fastParameters());
    QSignalSpy rejectedSpy(&coordinator, &ShortageSampleCoordinator::sampleRejected);
    QSignalSpy stableSpy(&coordinator, &ShortageSampleCoordinator::stableSampleReady);

    coordinator.start();
    const quint64 roundId = scheduler.mesRequests.last();
    scheduler.replyMes(roundId, 10);
    scheduler.replyPlc(roundId, 68, {{68, true}, {69, true}});
    scheduler.replyPlc(roundId, 71, productBits(ProductModel::Model88));
    scheduler.replyPlc(roundId, 1998, {{1998, false}});

    QCOMPARE(stableSpy.count(), 0);
    QCOMPARE(rejectedSpy.count(), 1);
    const QString reason = rejectedSpy.first().at(1).toString();
    QVERIFY2(reason.contains(QStringLiteral("模式")), qPrintable(reason));
    QVERIFY2(reason.contains(QStringLiteral("L68=true")), qPrintable(reason));
    QVERIFY2(reason.contains(QStringLiteral("L69=true")), qPrintable(reason));
    QVERIFY2(reason.contains(QStringLiteral("L1998=false")), qPrintable(reason));
}

void ShortageSampleCoordinatorTest::contextRequiresTwoConsecutiveValidRounds()
{
    FakeCustomSysScheduler scheduler;
    ShortageSampleCoordinator coordinator(&scheduler);
    coordinator.setParameters(fastParameters());
    QSignalSpy stableSpy(&coordinator, &ShortageSampleCoordinator::stableSampleReady);
    QSignalSpy contextSpy(&coordinator, &ShortageSampleCoordinator::contextChangeConfirmed);

    coordinator.start();
    completeRound(scheduler, scheduler.mesRequests.last(), 10, ProductModel::Model88, ProductionMode::LeftRight);
    QCOMPARE(stableSpy.count(), 0);
    QCOMPARE(contextSpy.count(), 0);

    coordinator.triggerNextRoundForTest();
    completeRound(scheduler, scheduler.mesRequests.last(), 11, ProductModel::Model88R, ProductionMode::LeftRight);
    QCOMPARE(stableSpy.count(), 0);
    QCOMPARE(contextSpy.count(), 0);

    coordinator.triggerNextRoundForTest();
    completeRound(scheduler, scheduler.mesRequests.last(), 12, ProductModel::Model88R, ProductionMode::LeftRight);
    QCOMPARE(contextSpy.count(), 1);
    QCOMPARE(stableSpy.count(), 1);
    const auto sample = qvariant_cast<ShortageSample>(stableSpy.first().at(0));
    QCOMPARE(sample.product, ProductModel::Model88R);
    QCOMPARE(sample.mode, ProductionMode::LeftRight);
}

void ShortageSampleCoordinatorTest::newTimingParametersApplyToNextRound()
{
    FakeCustomSysScheduler scheduler;
    ShortageSampleCoordinator coordinator(&scheduler);
    ShortageParameters parameters = fastParameters();
    parameters.roundTimeoutSeconds = 2;
    coordinator.setParameters(parameters);

    coordinator.start();
    QCOMPARE(coordinator.activeRoundTimeoutSecondsForTest(), 2);

    parameters.roundTimeoutSeconds = 7;
    parameters.sampleIntervalSeconds = 9;
    coordinator.setParameters(parameters);
    QCOMPARE(coordinator.activeRoundTimeoutSecondsForTest(), 2);

    coordinator.triggerNextRoundForTest();
    QCOMPARE(coordinator.activeRoundTimeoutSecondsForTest(), 7);
    QCOMPARE(coordinator.activeRoundIntervalSecondsForTest(), 9);
}

void ShortageSampleCoordinatorTest::reconnectAtOrAboveBaselineProducesCatchupSample()
{
    FakeCustomSysScheduler scheduler;
    ShortageSampleCoordinator coordinator(&scheduler);
    coordinator.setParameters(fastParameters());
    QSignalSpy stableSpy(&coordinator, &ShortageSampleCoordinator::stableSampleReady);

    establishInitialContext(coordinator, scheduler, 100);
    stableSpy.clear();
    coordinator.triggerNextRoundForTest();
    coordinator.triggerRoundTimeoutForTest();

    coordinator.triggerNextRoundForTest();
    completeRound(scheduler, scheduler.mesRequests.last(), 130);
    QCOMPARE(stableSpy.count(), 1);
    const auto sample = qvariant_cast<ShortageSample>(stableSpy.first().at(0));
    QCOMPARE(sample.actualQty, qint64{130});
    QVERIFY(sample.recoveredAfterInterruption);
}

void ShortageSampleCoordinatorTest::alarmTurnsRedAtConfiguredDuration()
{
    FakeCustomSysScheduler scheduler;
    ShortageSampleCoordinator coordinator(&scheduler);
    ShortageParameters parameters = fastParameters();
    parameters.communicationAlarmMinutes = 1;
    coordinator.setParameters(parameters);
    QSignalSpy stateSpy(&coordinator, &ShortageSampleCoordinator::communicationStateChanged);

    coordinator.start();
    coordinator.triggerRoundTimeoutForTest();
    QCOMPARE(qvariant_cast<ShortageCommunicationState>(stateSpy.last().at(0)),
             ShortageCommunicationState::Interrupted);

    coordinator.advanceFailureDurationForTest(60);
    coordinator.triggerNextRoundForTest();
    coordinator.triggerRoundTimeoutForTest();
    QCOMPARE(qvariant_cast<ShortageCommunicationState>(stateSpy.last().at(0)),
             ShortageCommunicationState::Alarm);

    FakeCustomSysScheduler upperBoundScheduler;
    ShortageSampleCoordinator upperBoundCoordinator(&upperBoundScheduler);
    parameters.communicationAlarmMinutes = 120;
    upperBoundCoordinator.setParameters(parameters);
    QSignalSpy upperBoundStateSpy(&upperBoundCoordinator,
                                  &ShortageSampleCoordinator::communicationStateChanged);

    upperBoundCoordinator.start();
    upperBoundCoordinator.triggerRoundTimeoutForTest();
    upperBoundCoordinator.advanceFailureDurationForTest(60 * 60 - 1);
    upperBoundCoordinator.triggerNextRoundForTest();
    upperBoundCoordinator.triggerRoundTimeoutForTest();
    QCOMPARE(qvariant_cast<ShortageCommunicationState>(upperBoundStateSpy.last().at(0)),
             ShortageCommunicationState::Interrupted);

    upperBoundCoordinator.advanceFailureDurationForTest(1);
    upperBoundCoordinator.triggerNextRoundForTest();
    upperBoundCoordinator.triggerRoundTimeoutForTest();
    QCOMPARE(qvariant_cast<ShortageCommunicationState>(upperBoundStateSpy.last().at(0)),
             ShortageCommunicationState::Alarm);
}

void ShortageSampleCoordinatorTest::reconnectBelowBaselineRequiresMaintenance()
{
    FakeCustomSysScheduler scheduler;
    ShortageSampleCoordinator coordinator(&scheduler);
    coordinator.setParameters(fastParameters());
    QSignalSpy stableSpy(&coordinator, &ShortageSampleCoordinator::stableSampleReady);
    QSignalSpy rejectedSpy(&coordinator, &ShortageSampleCoordinator::sampleRejected);
    QSignalSpy stateSpy(&coordinator, &ShortageSampleCoordinator::communicationStateChanged);

    establishInitialContext(coordinator, scheduler, 100);
    stableSpy.clear();
    coordinator.triggerNextRoundForTest();
    coordinator.triggerRoundTimeoutForTest();
    rejectedSpy.clear();

    coordinator.triggerNextRoundForTest();
    completeRound(scheduler, scheduler.mesRequests.last(), 90);
    QCOMPARE(stableSpy.count(), 0);
    QCOMPARE(rejectedSpy.count(), 1);
    QCOMPARE(qvariant_cast<ShortageCommunicationState>(stateSpy.last().at(0)),
             ShortageCommunicationState::RecoveryNeedsReview);

    const int requestCountAfterMaintenanceStop = scheduler.mesRequests.size();
    coordinator.triggerNextRoundForTest();
    QCOMPARE(scheduler.mesRequests.size(), requestCountAfterMaintenanceStop);
}

void ShortageSampleCoordinatorTest::httpAndJsonErrorsRejectWithoutPartialState()
{
    FakeCustomSysScheduler scheduler;
    ShortageSampleCoordinator coordinator(&scheduler);
    coordinator.setParameters(fastParameters());
    QSignalSpy stableSpy(&coordinator, &ShortageSampleCoordinator::stableSampleReady);
    QSignalSpy rejectedSpy(&coordinator, &ShortageSampleCoordinator::sampleRejected);

    coordinator.start();
    quint64 roundId = scheduler.mesRequests.last();
    scheduler.failMes(roundId, QStringLiteral("HTTP 500"));
    scheduler.replyPlc(roundId, 68, modeBits68(ProductionMode::LeftRight));
    scheduler.replyPlc(roundId, 71, productBits(ProductModel::Model88));
    scheduler.replyPlc(roundId, 1998, {{1998, false}});
    QCOMPARE(rejectedSpy.count(), 1);
    QCOMPARE(stableSpy.count(), 0);

    coordinator.triggerNextRoundForTest();
    completeRound(scheduler, scheduler.mesRequests.last(), 10);
    QCOMPARE(stableSpy.count(), 0);

    coordinator.triggerNextRoundForTest();
    completeRound(scheduler, scheduler.mesRequests.last(), 11);
    QCOMPARE(stableSpy.count(), 1);
}

void ShortageSampleCoordinatorTest::actualQtyUsesSignedSixtyFourBitValidation()
{
    FakeCustomSysScheduler scheduler;
    ShortageSampleCoordinator coordinator(&scheduler);
    coordinator.setParameters(fastParameters());
    QSignalSpy stableSpy(&coordinator, &ShortageSampleCoordinator::stableSampleReady);
    QSignalSpy rejectedSpy(&coordinator, &ShortageSampleCoordinator::sampleRejected);

    coordinator.start();
    completeRound(scheduler, scheduler.mesRequests.last(), std::numeric_limits<qint64>::max());
    coordinator.triggerNextRoundForTest();
    completeRound(scheduler, scheduler.mesRequests.last(), std::numeric_limits<qint64>::max());
    QCOMPARE(stableSpy.count(), 1);
    auto sample = qvariant_cast<ShortageSample>(stableSpy.takeFirst().at(0));
    QCOMPARE(sample.actualQty, std::numeric_limits<qint64>::max());

    coordinator.triggerNextRoundForTest();
    completeRound(scheduler, scheduler.mesRequests.last(), -1);
    QCOMPARE(rejectedSpy.count(), 1);
    QCOMPARE(stableSpy.count(), 0);
}

void ShortageSampleCoordinatorTest::oneRoundContextGlitchDoesNotSwitch()
{
    FakeCustomSysScheduler scheduler;
    ShortageSampleCoordinator coordinator(&scheduler);
    coordinator.setParameters(fastParameters());
    QSignalSpy stableSpy(&coordinator, &ShortageSampleCoordinator::stableSampleReady);
    QSignalSpy contextSpy(&coordinator, &ShortageSampleCoordinator::contextChangeConfirmed);

    establishInitialContext(coordinator, scheduler, 100);
    stableSpy.clear();
    contextSpy.clear();

    coordinator.triggerNextRoundForTest();
    completeRound(scheduler, scheduler.mesRequests.last(), 101, ProductModel::Model92, ProductionMode::RightOnly);
    QCOMPARE(stableSpy.count(), 0);
    QCOMPARE(contextSpy.count(), 0);

    coordinator.triggerNextRoundForTest();
    completeRound(scheduler, scheduler.mesRequests.last(), 102, ProductModel::Model88, ProductionMode::LeftRight);
    QCOMPARE(contextSpy.count(), 0);
    QCOMPARE(stableSpy.count(), 1);
    const auto sample = qvariant_cast<ShortageSample>(stableSpy.first().at(0));
    QCOMPARE(sample.product, ProductModel::Model88);
    QCOMPARE(sample.mode, ProductionMode::LeftRight);
}

void ShortageSampleCoordinatorTest::twoStableRoundsCreatePendingContext()
{
    FakeCustomSysScheduler scheduler;
    ShortageSampleCoordinator coordinator(&scheduler);
    coordinator.setParameters(fastParameters());
    QSignalSpy stableSpy(&coordinator, &ShortageSampleCoordinator::stableSampleReady);
    QSignalSpy contextSpy(&coordinator, &ShortageSampleCoordinator::contextChangeConfirmed);

    establishInitialContext(coordinator, scheduler, 100);
    stableSpy.clear();
    contextSpy.clear();

    coordinator.triggerNextRoundForTest();
    completeRound(scheduler, scheduler.mesRequests.last(), 101, ProductModel::Model92, ProductionMode::RightOnly);
    coordinator.triggerNextRoundForTest();
    completeRound(scheduler, scheduler.mesRequests.last(), 102, ProductModel::Model92, ProductionMode::RightOnly);
    QCOMPARE(stableSpy.count(), 0);
    QCOMPARE(contextSpy.count(), 1);
    QCOMPARE(qvariant_cast<ProductModel>(contextSpy.first().at(0)), ProductModel::Model92);
    QCOMPARE(qvariant_cast<ProductionMode>(contextSpy.first().at(1)), ProductionMode::RightOnly);

    coordinator.triggerNextRoundForTest();
    completeRound(scheduler, scheduler.mesRequests.last(), 103, ProductModel::Model92, ProductionMode::RightOnly);
    QCOMPARE(stableSpy.count(), 0);

    coordinator.activateConfirmedContextForTestOrCaller(ProductModel::Model92, ProductionMode::RightOnly);
    coordinator.triggerNextRoundForTest();
    completeRound(scheduler, scheduler.mesRequests.last(), 104, ProductModel::Model92, ProductionMode::RightOnly);
    QCOMPARE(stableSpy.count(), 1);
    const auto activatedSample = qvariant_cast<ShortageSample>(stableSpy.first().at(0));
    QCOMPARE(activatedSample.product, ProductModel::Model92);
    QCOMPARE(activatedSample.mode, ProductionMode::RightOnly);
}

void ShortageSampleCoordinatorTest::directContextConfirmationSlotCanActivateImmediately()
{
    FakeCustomSysScheduler scheduler;
    ShortageSampleCoordinator coordinator(&scheduler);
    coordinator.setParameters(fastParameters());
    QSignalSpy stableSpy(&coordinator, &ShortageSampleCoordinator::stableSampleReady);

    establishInitialContext(coordinator, scheduler, 100);
    stableSpy.clear();

    connect(&coordinator,
            &ShortageSampleCoordinator::contextChangeConfirmed,
            &coordinator,
            [&coordinator](ProductModel product, ProductionMode mode) {
                coordinator.activateConfirmedContextForTestOrCaller(product, mode);
            },
            Qt::DirectConnection);

    coordinator.triggerNextRoundForTest();
    completeRound(scheduler, scheduler.mesRequests.last(), 101, ProductModel::Model92, ProductionMode::RightOnly);
    coordinator.triggerNextRoundForTest();
    completeRound(scheduler, scheduler.mesRequests.last(), 102, ProductModel::Model92, ProductionMode::RightOnly);
    QCOMPARE(stableSpy.count(), 0);

    coordinator.triggerNextRoundForTest();
    completeRound(scheduler, scheduler.mesRequests.last(), 103, ProductModel::Model92, ProductionMode::RightOnly);
    QCOMPARE(stableSpy.count(), 1);
    const auto activatedSample = qvariant_cast<ShortageSample>(stableSpy.first().at(0));
    QCOMPARE(activatedSample.product, ProductModel::Model92);
    QCOMPARE(activatedSample.mode, ProductionMode::RightOnly);
}

void ShortageSampleCoordinatorTest::allNineContextCombinationsAreRecognized()
{
    const QList<ProductModel> products = {
        ProductModel::Model88,
        ProductModel::Model88R,
        ProductModel::Model92
    };
    const QList<ProductionMode> modes = {
        ProductionMode::LeftRight,
        ProductionMode::LeftOnly,
        ProductionMode::RightOnly
    };

    for (ProductModel product : products) {
        for (ProductionMode mode : modes) {
            FakeCustomSysScheduler scheduler;
            ShortageSampleCoordinator coordinator(&scheduler);
            coordinator.setParameters(fastParameters());
            QSignalSpy stableSpy(&coordinator, &ShortageSampleCoordinator::stableSampleReady);

            coordinator.start();
            completeRound(scheduler, scheduler.mesRequests.last(), 1, product, mode);
            coordinator.triggerNextRoundForTest();
            completeRound(scheduler, scheduler.mesRequests.last(), 2, product, mode);
            QCOMPARE(stableSpy.count(), 1);
            const auto sample = qvariant_cast<ShortageSample>(stableSpy.first().at(0));
            QCOMPARE(sample.product, product);
            QCOMPARE(sample.mode, mode);
        }
    }
}

void ShortageSampleCoordinatorTest::legacyDiagnosticSurfaceIsAbsent()
{
    const QHash<QString, QStringList> forbiddenByFile = {
        {QStringLiteral("src/mainwindow.h"),
         {QStringLiteral("m_customSys"), QStringLiteral("onCustomSystem"),
          QStringLiteral("initCustomSystemPanel"), QStringLiteral("setCustomSystemInputsEnabled")}},
        {QStringLiteral("src/mainwindow.cpp"),
         {QStringLiteral("客户系统通信测试"), QStringLiteral("m_customSys"),
          QStringLiteral("onCustomSystem"), QStringLiteral("initCustomSystemPanel"),
          QStringLiteral("setCustomSystemInputsEnabled")}},
        {QStringLiteral("src/devicemanager.h"),
         {QStringLiteral("customSysEndpoint"), QStringLiteral("testCustomSystem"),
          QStringLiteral("fetchCustomSystemDayData"), QStringLiteral("customSystemStatusChanged"),
          QStringLiteral("customSystemDayDataReady"), QStringLiteral("m_customSysScheduler")}},
        {QStringLiteral("src/devicemanager.cpp"),
         {QStringLiteral("CustomSysScheduler::DayRecord"), QStringLiteral("testCustomSystem"),
          QStringLiteral("fetchCustomSystemDayData"), QStringLiteral("m_customSysScheduler")}},
        {QStringLiteral("src/customSysScheduler.h"),
         {QStringLiteral("DayRecord"), QStringLiteral("defaultEndpoint("),
          QStringLiteral("testConnectivity("), QStringLiteral("fetchDayData("),
          QStringLiteral("connectivityChecked"), QStringLiteral("dayDataReady")}},
        {QStringLiteral("src/customSysScheduler.cpp"),
         {QStringLiteral("192.168.115.229"), QStringLiteral("DayRecord"),
          QStringLiteral("testConnectivity("), QStringLiteral("fetchDayData(")}}
    };

    QStringList violations;
    for (auto it = forbiddenByFile.cbegin(); it != forbiddenByFile.cend(); ++it) {
        const QString text = readSourceText(it.key());
        QVERIFY2(!text.isEmpty(), qPrintable(QStringLiteral("无法读取 %1").arg(it.key())));
        for (const QString &needle : it.value()) {
            if (text.contains(needle))
                violations.append(QStringLiteral("%1 contains %2").arg(it.key(), needle));
        }
    }

    const QString dmHeader = readSourceText(QStringLiteral("src/devicemanager.h"));
    const QString dmSource = readSourceText(QStringLiteral("src/devicemanager.cpp"));
    QVERIFY(dmHeader.contains(QStringLiteral("m_liveShortageScheduler")));
    QVERIFY(dmSource.contains(QStringLiteral("m_liveShortageScheduler")));

    int productionConstructors = 0;
    QDirIterator cppIt(sourcePath(QStringLiteral("src")),
                       QStringList{QStringLiteral("*.cpp")},
                       QDir::Files,
                       QDirIterator::Subdirectories);
    while (cppIt.hasNext()) {
        QFile file(cppIt.next());
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString text = QString::fromUtf8(file.readAll());
        productionConstructors += text.count(QStringLiteral("new CustomSysScheduler(this)"));
    }
    QCOMPARE(productionConstructors, 1);

    QVERIFY2(violations.isEmpty(), qPrintable(violations.join(QLatin1Char('\n'))));
}

QTEST_MAIN(ShortageSampleCoordinatorTest)

#include "test_shortage_sample_coordinator.moc"
