#include <QPushButton>
#include <QRadioButton>
#include <QTableWidget>
#include <QtTest>

#include "mainwindow.h"

class LiveShortageUiTest : public QObject
{
    Q_OBJECT

private slots:
    void dispatch_panel_defaults_to_mock_mode_and_uses_three_live_columns();
    void switching_to_live_disables_mock_buttons_without_stopping_lineManager();
};

void LiveShortageUiTest::dispatch_panel_defaults_to_mock_mode_and_uses_three_live_columns()
{
    MainWindow window;
    window.show();
    QTest::qWait(10);

    QRadioButton *mockRadio = window.findChild<QRadioButton *>(QStringLiteral("mockShortageRadio"));
    QRadioButton *liveRadio = window.findChild<QRadioButton *>(QStringLiteral("liveShortageRadio"));
    QVERIFY(mockRadio != nullptr);
    QVERIFY(liveRadio != nullptr);
    QVERIFY(mockRadio->isChecked());
    QVERIFY(!liveRadio->isChecked());

    QTableWidget *liveTable = window.findChild<QTableWidget *>(QStringLiteral("liveShortageTable"));
    QVERIFY(liveTable != nullptr);
    QCOMPARE(liveTable->columnCount(), 3);
    QCOMPARE(liveTable->horizontalHeaderItem(0)->text(), QStringLiteral("工位"));
    QCOMPARE(liveTable->horizontalHeaderItem(1)->text(), QStringLiteral("库存/安全线"));
    QCOMPARE(liveTable->horizontalHeaderItem(2)->text(), QStringLiteral("状态"));
}

void LiveShortageUiTest::switching_to_live_disables_mock_buttons_without_stopping_lineManager()
{
    MainWindow window;
    window.show();
    QTest::qWait(10);

    QRadioButton *liveRadio = window.findChild<QRadioButton *>(QStringLiteral("liveShortageRadio"));
    QVERIFY(liveRadio != nullptr);

    const auto buttons = window.findChildren<QPushButton *>();
    int stationButtonCount = 0;
    for (QPushButton *button : buttons) {
        if (button->property("stationId").isValid()) {
            ++stationButtonCount;
            QVERIFY(button->isEnabled());
        }
    }
    QCOMPARE(stationButtonCount, 12);

    DeviceManager *manager = window.findChild<DeviceManager *>();
    QVERIFY(manager != nullptr);
    const LineManager *lineManager = manager->lineManager();
    QVERIFY(lineManager != nullptr);
    QCOMPARE(lineManager->state(), LineSystemState::Idle);

    QTest::mouseClick(liveRadio, Qt::LeftButton);
    QTest::qWait(10);
    QCOMPARE(lineManager->state(), LineSystemState::Idle);

    for (QPushButton *button : buttons) {
        if (button->property("stationId").isValid()) {
            QVERIFY(!button->isEnabled());
        }
    }
}

QTEST_MAIN(LiveShortageUiTest)

#include "test_live_shortage_ui.moc"
