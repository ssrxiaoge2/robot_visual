#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QTest>

namespace {

QString sourceText(const QString &relativePath)
{
    QFile file(QStringLiteral(ROBOT_VISUAL_SOURCE_DIR) + QLatin1Char('/') + relativePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        qFatal("无法读取源码契约文件: %s", qPrintable(relativePath));
    return QString::fromUtf8(file.readAll());
}

void requireContains(const QString &haystack, const QString &needle)
{
    QVERIFY2(haystack.contains(needle), qPrintable(QStringLiteral("缺少源码契约片段: %1").arg(needle)));
}

} // namespace

class LiveShortageUiTest final : public QObject
{
    Q_OBJECT

private slots:
    void mockAndLiveRadiosAreExclusiveAndMockIsDefault();
    void twelveButtonsKeepCompactStationLabels();
    void liveButtonBuildsMatchingStationConfirmation();
    void stationAndFifoTablesShareOneTabWidget();
    void fifoShowsTaskSourceAndCountInTabTitle();
    void activeAutomaticPlanDisablesManualButtons();
    void configAndRecoveryOpenSeparateDialogs();
};

void LiveShortageUiTest::mockAndLiveRadiosAreExclusiveAndMockIsDefault()
{
    const QString cpp = sourceText(QStringLiteral("src/mainwindow.cpp"));
    requireContains(cpp, QStringLiteral("shortageSourceButtonGroup"));
    requireContains(cpp, QStringLiteral("shortageMockRadio"));
    requireContains(cpp, QStringLiteral("shortageLiveRadio"));
    requireContains(cpp, QStringLiteral("setExclusive(true)"));
    requireContains(cpp, QStringLiteral("m_shortageMockRadio->setChecked(true)"));
    requireContains(cpp, QStringLiteral("DeviceManager::setShortageInputSource"));
}

void LiveShortageUiTest::twelveButtonsKeepCompactStationLabels()
{
    const QString cpp = sourceText(QStringLiteral("src/mainwindow.cpp"));
    requireContains(cpp, QStringLiteral("for (int stationId = 1; stationId <= kLineStationCount; ++stationId)"));
    requireContains(cpp, QStringLiteral("QStringLiteral(\"工位%1\").arg(stationId)"));
    requireContains(cpp, QStringLiteral("stationGrid->addWidget(button, (stationId - 1) / 3, (stationId - 1) % 3)"));
    requireContains(cpp, QStringLiteral("m_shortageStationTitle->setText(m_shortageLiveRadio->isChecked()"));
    QVERIFY2(!cpp.contains(QStringLiteral("580→600/补料中")),
             "12 个工位按钮不得承载真实库存摘要");
}

void LiveShortageUiTest::liveButtonBuildsMatchingStationConfirmation()
{
    const QString cpp = sourceText(QStringLiteral("src/mainwindow.cpp"));
    requireContains(cpp, QStringLiteral("manualBoxConfirmation(stationId)"));
    requireContains(cpp, QStringLiteral("confirmation.stationId != stationId"));
    requireContains(cpp, QStringLiteral("产品："));
    requireContains(cpp, QStringLiteral("模式："));
    requireContains(cpp, QStringLiteral("现场位置："));
    requireContains(cpp, QStringLiteral("品号："));
    requireContains(cpp, QStringLiteral("每箱数量："));
    requireContains(cpp, QStringLiteral("当前库存："));
    requireContains(cpp, QStringLiteral("最高安全位："));
    requireContains(cpp, QStringLiteral("requestManualShortageBox(stationId, confirmation.highStockRisk)"));
}

void LiveShortageUiTest::stationAndFifoTablesShareOneTabWidget()
{
    const QString cpp = sourceText(QStringLiteral("src/mainwindow.cpp"));
    requireContains(cpp, QStringLiteral("lineStatusTabs"));
    requireContains(cpp, QStringLiteral("lineStationStatusTable"));
    requireContains(cpp, QStringLiteral("lineFifoQueueTable"));
    requireContains(cpp, QStringLiteral("m_lineStatusTabs->addTab(m_lineStationTable"));
    requireContains(cpp, QStringLiteral("m_lineStatusTabs->addTab(m_lineQueueTable"));
    requireContains(cpp, QStringLiteral("QStringLiteral(\"工位、库存、最低/最高、状态\")"));
}

void LiveShortageUiTest::fifoShowsTaskSourceAndCountInTabTitle()
{
    const QString cpp = sourceText(QStringLiteral("src/mainwindow.cpp"));
    requireContains(cpp, QStringLiteral("QStringLiteral(\"来源\")"));
    requireContains(cpp, QStringLiteral("taskSourceText(task.source)"));
    requireContains(cpp, QStringLiteral("FIFO 队列(%1)"));
    requireContains(cpp, QStringLiteral("m_lineStatusTabs->setTabText"));
}

void LiveShortageUiTest::activeAutomaticPlanDisablesManualButtons()
{
    const QString cpp = sourceText(QStringLiteral("src/mainwindow.cpp"));
    requireContains(cpp, QStringLiteral("snapshot.runtime.activeStationId != 0"));
    requireContains(cpp, QStringLiteral("m_shortageLiveRadio->isChecked()"));
    requireContains(cpp, QStringLiteral("button->setEnabled(shortageEnabled && !manualBlockedByLivePlan)"));
}

void LiveShortageUiTest::configAndRecoveryOpenSeparateDialogs()
{
    const QString cpp = sourceText(QStringLiteral("src/mainwindow.cpp"));
    requireContains(cpp, QStringLiteral("ShortageConfigDialog"));
    requireContains(cpp, QStringLiteral("shortageConfigDialogButton"));
    requireContains(cpp, QStringLiteral("ShortageRecoveryDialog"));
    requireContains(cpp, QStringLiteral("shortageRecoveryDialogButton"));
    requireContains(cpp, QStringLiteral("applyShortageMaintenanceCorrection"));
    QVERIFY2(!cpp.contains(QStringLiteral("客户系统通信测试")),
             "不得重新加入旧客户系统通信测试入口");
}

QTEST_MAIN(LiveShortageUiTest)
#include "test_live_shortage_ui.moc"
