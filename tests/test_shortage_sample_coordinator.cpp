#include "customSysScheduler.h"

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

    QVERIFY(mesSpy.wait(2000));
    QVERIFY(plcSpy.wait(2000));

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
