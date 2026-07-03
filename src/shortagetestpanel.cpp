#include "shortagetestpanel.h"

#include <QAbstractItemView>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QTableWidget>
#include <QVBoxLayout>

ShortageTestPanel::ShortageTestPanel(QWidget *parent)
    : QGroupBox(QStringLiteral("缺料信号计算测试"), parent)
{
    setObjectName(QStringLiteral("shortageTestGroup"));

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(5);

    auto *actionRow = new QHBoxLayout();
    m_shortageTestStartBtn = new QPushButton(QStringLiteral("开始检测"), this);
    m_shortageTestStopBtn = new QPushButton(QStringLiteral("停止检测"), this);
    m_shortageTestIndicator = new DeviceIndicator(QStringLiteral("现场系统"), this);
    m_shortageTestStartBtn->setObjectName(QStringLiteral("m_shortageTestStartBtn"));
    m_shortageTestStopBtn->setObjectName(QStringLiteral("m_shortageTestStopBtn"));
    m_shortageTestIndicator->setObjectName(QStringLiteral("m_shortageTestIndicator"));
    m_shortageTestStartBtn->setFixedHeight(28);
    m_shortageTestStopBtn->setFixedHeight(28);
    actionRow->addWidget(m_shortageTestStartBtn);
    actionRow->addWidget(m_shortageTestStopBtn);
    actionRow->addStretch();
    actionRow->addWidget(m_shortageTestIndicator);
    layout->addLayout(actionRow);

    auto *line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    layout->addWidget(line);

    auto *form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignTop);
    m_shortageTestActualQtyEdit = new QLineEdit(this);
    m_shortageTestActualQtyEdit->setObjectName(QStringLiteral("m_shortageTestActualQtyEdit"));
    m_shortageTestActualQtyEdit->setReadOnly(true);
    m_shortageTestActualQtyEdit->setPlaceholderText(QStringLiteral("尚未检测"));
    m_shortageTestDeltaLabel = new QLabel(QStringLiteral("0"), this);
    m_shortageTestProductLabel = new QLabel(QStringLiteral("-"), this);
    m_shortageTestModeLabel = new QLabel(QStringLiteral("-"), this);
    m_shortageTestUsageLabel = new QLabel(QStringLiteral("按表格工位用量"), this);
    m_shortageTestBitsLabel = new QLabel(QStringLiteral("-"), this);
    m_shortageTestStatusLabel = new QLabel(m_currentStatusText, this);
    m_shortageTestDeltaLabel->setObjectName(QStringLiteral("m_shortageTestDeltaLabel"));
    m_shortageTestProductLabel->setObjectName(QStringLiteral("m_shortageTestProductLabel"));
    m_shortageTestModeLabel->setObjectName(QStringLiteral("m_shortageTestModeLabel"));
    m_shortageTestUsageLabel->setObjectName(QStringLiteral("m_shortageTestUsageLabel"));
    m_shortageTestBitsLabel->setObjectName(QStringLiteral("m_shortageTestBitsLabel"));
    m_shortageTestStatusLabel->setObjectName(QStringLiteral("m_shortageTestStatusLabel"));
    m_shortageTestBitsLabel->setWordWrap(true);
    m_shortageTestStatusLabel->setWordWrap(true);
    m_shortageTestBitsLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_shortageTestStatusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    form->addRow(QStringLiteral("actualQty:"), m_shortageTestActualQtyEdit);
    form->addRow(QStringLiteral("产量增量:"), m_shortageTestDeltaLabel);
    form->addRow(QStringLiteral("产品:"), m_shortageTestProductLabel);
    form->addRow(QStringLiteral("生产方式:"), m_shortageTestModeLabel);
    form->addRow(QStringLiteral("单件用量:"), m_shortageTestUsageLabel);
    form->addRow(QStringLiteral("L 位:"), m_shortageTestBitsLabel);
    form->addRow(QStringLiteral("状态:"), m_shortageTestStatusLabel);
    layout->addLayout(form);

    m_shortageTestTable = new QTableWidget(12, 5, this);
    m_shortageTestTable->setObjectName(QStringLiteral("m_shortageTestTable"));
    m_shortageTestTable->setHorizontalHeaderLabels(
        {QStringLiteral("工位"),
         QStringLiteral("预计可用"),
         QStringLiteral("安全线"),
         QStringLiteral("箱量"),
         QStringLiteral("状态")});
    m_shortageTestTable->verticalHeader()->setVisible(false);
    m_shortageTestTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_shortageTestTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_shortageTestTable->setFocusPolicy(Qt::NoFocus);
    m_shortageTestTable->setAlternatingRowColors(true);
    m_shortageTestTable->setMinimumHeight(220);
    m_shortageTestTable->horizontalHeader()->setStretchLastSection(true);
    m_shortageTestTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(m_shortageTestTable);

    initializeTable();
    setRunning(false);
    setStatus(QStringLiteral("等待首轮基线"), false);

    connect(m_shortageTestStartBtn, &QPushButton::clicked, this, [this] {
        setRunning(true);
        emit startTestRequested();
    });
    connect(m_shortageTestStopBtn, &QPushButton::clicked, this, [this] {
        setRunning(false);
        emit stopTestRequested();
    });
}

void ShortageTestPanel::setRunning(bool running)
{
    if (m_shortageTestStartBtn) {
        m_shortageTestStartBtn->setEnabled(!running);
    }
    if (m_shortageTestStopBtn) {
        m_shortageTestStopBtn->setEnabled(running);
    }
}

void ShortageTestPanel::setStatus(const QString &text, bool healthy)
{
    m_currentStatusText = text;
    m_shortageTestIndicator->setStatus(healthy, text);
    m_shortageTestStatusLabel->setText(text);
    if (text == QStringLiteral("缺料信号测试已停止")) {
        setRunning(false);
    }
}

void ShortageTestPanel::setSample(qint64 actualQty,
                                  qint64 delta,
                                  ProductModel product,
                                  ProductionMode mode,
                                  QHash<QString, bool> bits)
{
    m_shortageTestActualQtyEdit->setText(QString::number(actualQty));
    m_shortageTestDeltaLabel->setText(QString::number(delta));
    m_shortageTestProductLabel->setText(productModelText(product));
    m_shortageTestModeLabel->setText(productionModeText(mode));
    m_shortageTestUsageLabel->setText(usageSummaryText(mode));

    QStringList parts;
    for (const QString &name : {QStringLiteral("L68"), QStringLiteral("L69"),
                                QStringLiteral("L71"), QStringLiteral("L72"),
                                QStringLiteral("L73"), QStringLiteral("L1998")}) {
        parts.append(QStringLiteral("%1=%2").arg(name, bits.value(name) ? QStringLiteral("1")
                                                                        : QStringLiteral("0")));
    }
    m_shortageTestBitsLabel->setText(parts.join(QStringLiteral("  ")));
}

void ShortageTestPanel::setInventory(QList<StationConsumption> stations)
{
    for (const StationConsumption &station : stations) {
        const int row = station.stationId - 1;
        if (row < 0 || row >= m_shortageTestTable->rowCount()) {
            continue;
        }

        m_shortageTestTable->item(row, 1)->setText(QString::number(station.estimatedAvailable));
        m_shortageTestTable->item(row, 2)->setText(
            station.safetyStock > 0 ? QString::number(station.safetyStock) : QStringLiteral("0"));
        m_shortageTestTable->item(row, 3)->setText(
            station.boxQuantity > 0 ? QString::number(station.boxQuantity) : QStringLiteral("0"));
        m_shortageTestTable->item(row, 4)->setText(stationStatusText(station));
    }
}

QString ShortageTestPanel::stationStatusText(const StationConsumption &station) const
{
    if (!station.reason.isEmpty()) {
        return station.reason;
    }
    if (!station.configured) {
        return QStringLiteral("配置缺失");
    }
    if (station.shortage) {
        return QStringLiteral("缺料（仅测试）");
    }
    if (m_currentStatusText == QStringLiteral("等待首轮基线")
        || m_currentStatusText == QStringLiteral("等待生产信号稳定")
        || m_currentStatusText == QStringLiteral("通信中断")
        || m_currentStatusText == QStringLiteral("产品信号不唯一")
        || m_currentStatusText == QStringLiteral("生产方式信号不唯一")
        || m_currentStatusText == QStringLiteral("产量异常回退")) {
        return m_currentStatusText;
    }
    return QStringLiteral("正常");
}

QString ShortageTestPanel::usageSummaryText(ProductionMode mode) const
{
    const UsageStrategy strategy = SHORTAGE_USAGE_STRATEGY;
    if (strategy == UsageStrategy::Spreadsheet) {
        return QStringLiteral("按表格工位用量");
    }
    return QStringLiteral("按方式 %1").arg(productionModeText(mode));
}

void ShortageTestPanel::initializeTable()
{
    for (int row = 0; row < 12; ++row) {
        m_shortageTestTable->setItem(row, 0, new QTableWidgetItem(QStringLiteral("工位%1").arg(row + 1)));
        m_shortageTestTable->setItem(row, 1, new QTableWidgetItem(QStringLiteral("0")));
        m_shortageTestTable->setItem(row, 2, new QTableWidgetItem(QStringLiteral("0")));
        m_shortageTestTable->setItem(row, 3, new QTableWidgetItem(QStringLiteral("0")));
        m_shortageTestTable->setItem(row, 4, new QTableWidgetItem(QStringLiteral("等待首轮基线")));
    }
}
