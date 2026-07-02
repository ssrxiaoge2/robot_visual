#include "customSysScheduler.h"

#include <QtTest>

class CustomSysSchedulerTest : public QObject
{
    Q_OBJECT

private slots:
    void parseDayReply_success();
    void parseDayReply_error_data();
    void parseDayReply_error();

    void parsePlcBitReply_success();
    void parsePlcBitReply_error_data();
    void parsePlcBitReply_error();
};

void CustomSysSchedulerTest::parseDayReply_success()
{
    const QByteArray payload = R"([
        {
            "id": 7,
            "statDate": "2026-07-02T08:00:00+08:00",
            "lineId": "L01",
            "lineName": "产线 1",
            "planQty": 18,
            "actualQty": 12,
            "okQty": 11,
            "ngQty": 1
        }
    ])";

    const CustomSysScheduler::ParseResult result =
        CustomSysScheduler::parseDayReply(payload);

    QVERIFY(result.ok);
    QCOMPARE(result.record.id, 7);
    QCOMPARE(result.record.actualQty, 12);
    QCOMPARE(result.record.planQty, 18);
    QCOMPARE(result.record.okQty, 11);
    QCOMPARE(result.record.ngQty, 1);
    QCOMPARE(result.record.lineId, QStringLiteral("L01"));
    QCOMPARE(result.record.lineName, QStringLiteral("产线 1"));
    QVERIFY(result.record.statDate.isValid());
    QVERIFY(result.errorMessage.isEmpty());
}

void CustomSysSchedulerTest::parseDayReply_error_data()
{
    QTest::addColumn<QByteArray>("payload");
    QTest::addColumn<QString>("expectedError");

    QTest::newRow("root-not-array")
        << QByteArray("{}")
        << QStringLiteral("返回不是 JSON 数组");
    QTest::newRow("empty-array")
        << QByteArray("[]")
        << QStringLiteral("返回数组为空");
    QTest::newRow("missing-actual-qty")
        << QByteArray(R"([{"planQty": 1}])")
        << QStringLiteral("缺少 actualQty 字段");
    QTest::newRow("actual-qty-not-number")
        << QByteArray(R"([{"actualQty": "12"}])")
        << QStringLiteral("actualQty 字段不是数字");
}

void CustomSysSchedulerTest::parseDayReply_error()
{
    QFETCH(QByteArray, payload);
    QFETCH(QString, expectedError);

    const CustomSysScheduler::ParseResult result =
        CustomSysScheduler::parseDayReply(payload);

    QVERIFY(!result.ok);
    QCOMPARE(result.errorMessage, expectedError);
}

void CustomSysSchedulerTest::parsePlcBitReply_success()
{
    const QByteArray payload = R"({
        "success": true,
        "data": [
            { "address": "L71", "value": true },
            { "address": "L72", "value": false },
            { "address": "L73", "value": true }
        ],
        "timestamp": "2026-07-02T15:12:32.6976771+08:00"
    })";
    const QStringList requiredAddresses = {
        QStringLiteral("L71"),
        QStringLiteral("L72"),
        QStringLiteral("L73")
    };

    const CustomSysScheduler::PlcBitReply result =
        CustomSysScheduler::parsePlcBitReply(payload, requiredAddresses);

    QVERIFY(result.ok);
    QVERIFY(result.timestamp.isValid());
    QCOMPARE(result.bits.size(), 3);
    QCOMPARE(result.bits.value(QStringLiteral("L71")), true);
    QCOMPARE(result.bits.value(QStringLiteral("L72")), false);
    QCOMPARE(result.bits.value(QStringLiteral("L73")), true);
    QVERIFY(result.errorMessage.isEmpty());
}

void CustomSysSchedulerTest::parsePlcBitReply_error_data()
{
    QTest::addColumn<QByteArray>("payload");
    QTest::addColumn<QStringList>("requiredAddresses");
    QTest::addColumn<QString>("expectedError");

    QTest::newRow("success-false")
        << QByteArray(R"({
               "success": false,
               "data": [],
               "timestamp": "2026-07-02T15:12:32+08:00"
           })")
        << QStringList()
        << QStringLiteral("PLC 响应 success=false");
    QTest::newRow("root-not-object")
        << QByteArray(R"([])")
        << QStringList()
        << QStringLiteral("PLC 响应根节点不是对象");
    QTest::newRow("data-not-array")
        << QByteArray(R"({
               "success": true,
               "data": {},
               "timestamp": "2026-07-02T15:12:32+08:00"
           })")
        << QStringList()
        << QStringLiteral("PLC data 字段不是数组");
    QTest::newRow("missing-address")
        << QByteArray(R"({
               "success": true,
               "data": [{ "value": true }],
               "timestamp": "2026-07-02T15:12:32+08:00"
           })")
        << QStringList()
        << QStringLiteral("PLC data[0] 缺少 address 字段");
    QTest::newRow("missing-value")
        << QByteArray(R"({
               "success": true,
               "data": [{ "address": "L71" }],
               "timestamp": "2026-07-02T15:12:32+08:00"
           })")
        << QStringList()
        << QStringLiteral("PLC data[0] 缺少 value 字段");
    QTest::newRow("value-not-bool")
        << QByteArray(R"({
               "success": true,
               "data": [{ "address": "L71", "value": 1 }],
               "timestamp": "2026-07-02T15:12:32+08:00"
           })")
        << QStringList()
        << QStringLiteral("PLC 地址 L71 的 value 不是 bool");
    QTest::newRow("missing-required-address")
        << QByteArray(R"({
               "success": true,
               "data": [{ "address": "L71", "value": true }],
               "timestamp": "2026-07-02T15:12:32+08:00"
           })")
        << (QStringList() << QStringLiteral("L71") << QStringLiteral("L72"))
        << QStringLiteral("PLC 响应缺少必需地址：L72");
    QTest::newRow("duplicate-address")
        << QByteArray(R"({
               "success": true,
               "data": [
                   { "address": "L71", "value": true },
                   { "address": "L71", "value": false }
               ],
               "timestamp": "2026-07-02T15:12:32+08:00"
           })")
        << QStringList()
        << QStringLiteral("PLC 地址重复：L71");
    QTest::newRow("invalid-timestamp")
        << QByteArray(R"({
               "success": true,
               "data": [{ "address": "L71", "value": true }],
               "timestamp": "bad-time"
           })")
        << QStringList()
        << QStringLiteral("PLC timestamp 无效");
}

void CustomSysSchedulerTest::parsePlcBitReply_error()
{
    QFETCH(QByteArray, payload);
    QFETCH(QStringList, requiredAddresses);
    QFETCH(QString, expectedError);

    const CustomSysScheduler::PlcBitReply result =
        CustomSysScheduler::parsePlcBitReply(payload, requiredAddresses);

    QVERIFY(!result.ok);
    QCOMPARE(result.errorMessage, expectedError);
    QVERIFY(result.bits.isEmpty());
    QVERIFY(!result.timestamp.isValid());
}

QTEST_APPLESS_MAIN(CustomSysSchedulerTest)

#include "test_customsysscheduler.moc"
