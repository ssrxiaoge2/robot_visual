#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QTableWidget>
#include <QtTest>

#include "shortagetestpanel.h"

class ShortageTestUiTest : public QObject
{
    Q_OBJECT

private slots:
    void panel_uses_fixed_chinese_labels_and_five_columns();
    void panel_buttons_emit_test_only_requests();
    void panel_renders_sample_and_inventory();
};

void ShortageTestUiTest::panel_uses_fixed_chinese_labels_and_five_columns()
{
    ShortageTestPanel panel;
    panel.show();
    QTest::qWait(10);

    QCOMPARE(panel.title(), QStringLiteral("缺料信号计算测试"));
    QCOMPARE(panel.objectName(), QStringLiteral("shortageTestGroup"));

    const auto buttons = panel.findChildren<QPushButton *>();
    QCOMPARE(buttons.size(), 2);
    QCOMPARE(panel.findChild<QPushButton *>(QStringLiteral("m_shortageTestStartBtn"))->text(),
             QStringLiteral("开始检测"));
    QCOMPARE(panel.findChild<QPushButton *>(QStringLiteral("m_shortageTestStopBtn"))->text(),
             QStringLiteral("停止检测"));
    QCOMPARE(panel.findChildren<QRadioButton *>().size(), 0);

    auto *table = panel.findChild<QTableWidget *>(QStringLiteral("m_shortageTestTable"));
    QVERIFY(table != nullptr);
    QCOMPARE(table->columnCount(), 5);
    QCOMPARE(table->horizontalHeaderItem(0)->text(), QStringLiteral("工位"));
    QCOMPARE(table->horizontalHeaderItem(1)->text(), QStringLiteral("预计可用"));
    QCOMPARE(table->horizontalHeaderItem(2)->text(), QStringLiteral("安全线"));
    QCOMPARE(table->horizontalHeaderItem(3)->text(), QStringLiteral("箱量"));
    QCOMPARE(table->horizontalHeaderItem(4)->text(), QStringLiteral("状态"));
}

void ShortageTestUiTest::panel_buttons_emit_test_only_requests()
{
    ShortageTestPanel panel;
    panel.show();
    QTest::qWait(10);

    auto *startButton = panel.findChild<QPushButton *>(QStringLiteral("m_shortageTestStartBtn"));
    auto *stopButton = panel.findChild<QPushButton *>(QStringLiteral("m_shortageTestStopBtn"));
    QVERIFY(startButton != nullptr);
    QVERIFY(stopButton != nullptr);

    QSignalSpy startSpy(&panel, &ShortageTestPanel::startTestRequested);
    QSignalSpy stopSpy(&panel, &ShortageTestPanel::stopTestRequested);

    QVERIFY(startButton->isEnabled());
    QVERIFY(!stopButton->isEnabled());

    QTest::mouseClick(startButton, Qt::LeftButton);
    QCOMPARE(startSpy.count(), 1);

    panel.setRunning(true);
    QVERIFY(!startButton->isEnabled());
    QVERIFY(stopButton->isEnabled());

    QTest::mouseClick(stopButton, Qt::LeftButton);
    QCOMPARE(stopSpy.count(), 1);
}

void ShortageTestUiTest::panel_renders_sample_and_inventory()
{
    ShortageTestPanel panel;
    panel.show();
    QTest::qWait(10);

    const QHash<QString, bool> bits = {
        {QStringLiteral("L68"), true},
        {QStringLiteral("L69"), false},
        {QStringLiteral("L71"), true},
        {QStringLiteral("L72"), false},
        {QStringLiteral("L73"), false},
        {QStringLiteral("L1998"), false},
    };
    panel.setStatus(QStringLiteral("等待生产信号稳定"), true);
    panel.setSample(1005, 5, ProductModel::Model88, ProductionMode::L68, bits);

    QList<StationConsumption> stations;
    StationConsumption station1;
    station1.stationId = 1;
    station1.estimatedAvailable = 990;
    station1.safetyStock = 1000;
    station1.boxQuantity = 250;
    station1.configured = true;
    station1.shortage = true;
    stations.append(station1);
    panel.setInventory(stations);

    auto *actualQtyEdit = panel.findChild<QLineEdit *>(QStringLiteral("m_shortageTestActualQtyEdit"));
    auto *deltaLabel = panel.findChild<QLabel *>(QStringLiteral("m_shortageTestDeltaLabel"));
    auto *productLabel = panel.findChild<QLabel *>(QStringLiteral("m_shortageTestProductLabel"));
    auto *modeLabel = panel.findChild<QLabel *>(QStringLiteral("m_shortageTestModeLabel"));
    auto *usageLabel = panel.findChild<QLabel *>(QStringLiteral("m_shortageTestUsageLabel"));
    auto *bitsLabel = panel.findChild<QLabel *>(QStringLiteral("m_shortageTestBitsLabel"));
    auto *statusLabel = panel.findChild<QLabel *>(QStringLiteral("m_shortageTestStatusLabel"));
    auto *table = panel.findChild<QTableWidget *>(QStringLiteral("m_shortageTestTable"));
    QVERIFY(actualQtyEdit != nullptr);
    QVERIFY(deltaLabel != nullptr);
    QVERIFY(productLabel != nullptr);
    QVERIFY(modeLabel != nullptr);
    QVERIFY(usageLabel != nullptr);
    QVERIFY(bitsLabel != nullptr);
    QVERIFY(statusLabel != nullptr);
    QVERIFY(table != nullptr);

    QCOMPARE(actualQtyEdit->text(), QStringLiteral("1005"));
    QCOMPARE(deltaLabel->text(), QStringLiteral("5"));
    QCOMPARE(productLabel->text(), QStringLiteral("88"));
    QCOMPARE(modeLabel->text(), QStringLiteral("L68"));
    QCOMPARE(usageLabel->text(), QStringLiteral("按表格工位用量"));
    QVERIFY(bitsLabel->text().contains(QStringLiteral("L68=1")));
    QCOMPARE(statusLabel->text(), QStringLiteral("等待生产信号稳定"));
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("990"));
    QCOMPARE(table->item(0, 2)->text(), QStringLiteral("1000"));
    QCOMPARE(table->item(0, 3)->text(), QStringLiteral("250"));
    QCOMPARE(table->item(0, 4)->text(), QStringLiteral("缺料（仅测试）"));
}

QTEST_MAIN(ShortageTestUiTest)

#include "test_shortagetestui.moc"
