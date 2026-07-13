#include "shortageconfigdialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>

namespace {

QString productName(ProductModel product)
{
    switch (product) {
    case ProductModel::Model88:
        return QStringLiteral("88");
    case ProductModel::Model88R:
        return QStringLiteral("88R");
    case ProductModel::Model92:
        return QStringLiteral("92");
    }
    return QStringLiteral("未知");
}

QList<ProductModel> products()
{
    return {ProductModel::Model88, ProductModel::Model88R, ProductModel::Model92};
}

QTableWidgetItem *readOnlyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

qint64 tableNumber(const QTableWidget *table, int row, int column)
{
    const QTableWidgetItem *item = table->item(row, column);
    return item == nullptr ? 0 : item->text().toLongLong();
}

QString tableText(const QTableWidget *table, int row, int column)
{
    const QTableWidgetItem *item = table->item(row, column);
    return item == nullptr ? QString() : item->text();
}

int productTabIndex(ProductModel product)
{
    return products().indexOf(product);
}

bool productFromName(const QString &name, ProductModel *product)
{
    if (name == QStringLiteral("88")) {
        *product = ProductModel::Model88;
        return true;
    }
    if (name == QStringLiteral("88R")) {
        *product = ProductModel::Model88R;
        return true;
    }
    if (name == QStringLiteral("92")) {
        *product = ProductModel::Model92;
        return true;
    }
    return false;
}

int stationFieldColumn(const QString &field)
{
    static const QHash<QString, int> kColumns {
        {QStringLiteral("stationId"), 0},
        {QStringLiteral("temporaryNo"), 1},
        {QStringLiteral("sitePosition"), 2},
        {QStringLiteral("partNumber"), 3},
        {QStringLiteral("boxQuantity"), 5},
        {QStringLiteral("minimumStock"), 6},
        {QStringLiteral("maximumStock"), 7},
        {QStringLiteral("usageLeftRight"), 8},
        {QStringLiteral("usageLeftOnly"), 9},
        {QStringLiteral("usageRightOnly"), 10},
    };
    return kColumns.value(field, -1);
}

} // namespace

ShortageConfigDialog::ShortageConfigDialog(ShortageConfiguration configuration,
                                           EditGateProvider editGateProvider,
                                           ShortageTestController *testController,
                                           ShortageUiSnapshot snapshot,
                                           QWidget *parent)
    : QDialog(parent),
      m_configuration(std::move(configuration)),
      m_validatedConfiguration(ShortageConfigStore::validate(m_configuration).ok
                                   ? m_configuration
                                   : ShortageConfigStore::sheet3Defaults()),
      m_editGateProvider(std::move(editGateProvider)),
      m_testController(testController),
      m_snapshot(std::move(snapshot))
{
    buildUi();
}

ShortageConfiguration ShortageConfigDialog::validatedConfiguration() const
{
    return m_validatedConfiguration;
}

void ShortageConfigDialog::buildUi()
{
    setWindowTitle(QStringLiteral("缺料配置与完整逻辑测试"));
    setMinimumSize(1200, 720);
    resize(1320, 780);
    setStyleSheet(QStringLiteral(
        "QDialog { background:#20242b; color:#f0f3f6; }"
        "QGroupBox { border:1px solid #3c4654; border-radius:6px; margin-top:12px; padding:8px; }"
        "QGroupBox::title { subcontrol-origin: margin; left:10px; padding:0 4px; }"
        "QTableWidget, QTextEdit, QLineEdit, QSpinBox { background:#111820; color:#f4f7fb;"
        " border:1px solid #3b4654; selection-background-color:#2f6fed; }"
        "QHeaderView::section { background:#2b3440; color:#f4f7fb; padding:4px; border:0; }"
        "QPushButton { background:#2f6fed; color:white; border:0; border-radius:4px; padding:6px 12px; }"
        "QPushButton:disabled { background:#555b66; }"));

    auto *root = new QVBoxLayout(this);
    auto *boundaryWarning = new QLabel(
        QStringLiteral("独立测试只连接 ShortageTestController，使用 StandaloneTest 状态文件，不写入生产 Engine/FIFO。"),
        this);
    boundaryWarning->setObjectName(QStringLiteral("testBoundaryWarningLabel"));
    boundaryWarning->setWordWrap(true);
    boundaryWarning->setStyleSheet(QStringLiteral("color:#ffd166; font-weight:600;"));
    root->addWidget(boundaryWarning);

    m_mainTabs = new QTabWidget(this);
    m_mainTabs->setObjectName(QStringLiteral("shortageMainTabs"));
    m_mainTabs->addTab(buildConfigurationPage(), QStringLiteral("宽屏配置"));
    m_mainTabs->addTab(buildTestPage(), QStringLiteral("完整逻辑测试"));
    root->addWidget(m_mainTabs);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *save = buttons->addButton(QStringLiteral("保存配置"), QDialogButtonBox::AcceptRole);
    save->setObjectName(QStringLiteral("saveConfigurationButton"));
    connect(save, &QPushButton::clicked, this, &ShortageConfigDialog::saveConfiguration);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

QWidget *ShortageConfigDialog::buildConfigurationPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    m_productTabs = new QTabWidget(page);
    m_productTabs->setObjectName(QStringLiteral("shortageProductTabs"));
    for (ProductModel product : products()) {
        const QString name = productName(product);
        auto *table = createStationTable(product, QStringLiteral("stationTable_%1").arg(name));
        m_tables.insert(product, table);
        m_productTabs->addTab(table, name);
    }
    layout->addWidget(m_productTabs, 1);

    auto *parameters = new QGroupBox(QStringLiteral("真实缺料 MES 与保护参数"), page);
    auto *parameterLayout = new QVBoxLayout(parameters);
    createParameterEditors(parameterLayout);
    layout->addWidget(parameters);
    return page;
}

QWidget *ShortageConfigDialog::buildTestPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *warning = new QLabel(
        QStringLiteral("独立测试只连接 ShortageTestController，使用 StandaloneTest 状态文件，不写入生产 Engine/FIFO。"),
        page);
    warning->setWordWrap(true);
    warning->setStyleSheet(QStringLiteral("color:#ffd166; font-weight:600;"));
    layout->addWidget(warning);

    auto *sourceBox = new QGroupBox(QStringLiteral("测试输入源"), page);
    auto *sourceLayout = new QHBoxLayout(sourceBox);
    auto *group = new QButtonGroup(sourceBox);
    group->setObjectName(QStringLiteral("testSourceButtonGroup"));
    group->setExclusive(true);
    auto *manual = new QRadioButton(QStringLiteral("手工源"), sourceBox);
    manual->setObjectName(QStringLiteral("manualSourceRadio"));
    auto *field = new QRadioButton(QStringLiteral("现场源"), sourceBox);
    field->setObjectName(QStringLiteral("fieldSourceRadio"));
    manual->setChecked(true);
    group->addButton(manual, 0);
    group->addButton(field, 1);
    sourceLayout->addWidget(manual);
    sourceLayout->addWidget(field);
    sourceLayout->addStretch();
    layout->addWidget(sourceBox);

    auto *content = new QHBoxLayout;
    auto *leftTable = new QTableWidget(12, 3, page);
    leftTable->setObjectName(QStringLiteral("testStationRuntimeTable"));
    leftTable->setHorizontalHeaderLabels({QStringLiteral("工位"), QStringLiteral("库存"),
                                          QStringLiteral("暂停")});
    leftTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    leftTable->verticalHeader()->setVisible(false);
    for (int row = 0; row < 12; ++row) {
        leftTable->setItem(row, 0, readOnlyItem(QString::number(row + 1)));
        leftTable->setItem(row, 1, readOnlyItem(QStringLiteral("0")));
        leftTable->setItem(row, 2, readOnlyItem(QStringLiteral("-")));
    }
    content->addWidget(leftTable, 1);

    auto *right = new QVBoxLayout;
    auto *summary = new QLabel(
        QStringLiteral("%1\n%2").arg(m_snapshot.summaryLine1Zh, m_snapshot.summaryLine2Zh), page);
    summary->setObjectName(QStringLiteral("testPlanSummaryLabel"));
    summary->setWordWrap(true);
    right->addWidget(summary);

    auto *actions = new QGridLayout;
    createTestActionButton(actions, QStringLiteral("现场清零建账"), QStringLiteral("testInitZeroButton"),
                           0, 0, &ShortageTestController::initializeZeroAfterConfirmation);
    createTestActionButton(actions, QStringLiteral("启动现场采样"), QStringLiteral("testStartFieldButton"),
                           0, 1, &ShortageTestController::startFieldSampling);
    createTestActionButton(actions, QStringLiteral("停止测试"), QStringLiteral("testStopButton"),
                           1, 0, &ShortageTestController::stop);
    createTestActionButton(actions, QStringLiteral("派单接受"), QStringLiteral("testDispatchAcceptedButton"),
                           1, 1, &ShortageTestController::simulateDispatchAccepted);
    createTestActionButton(actions, QStringLiteral("派单拒收"), QStringLiteral("testDispatchRejectedButton"),
                           2, 0, &ShortageTestController::simulateDispatchRejected);
    createTestActionButton(actions, QStringLiteral("倒料前失败"), QStringLiteral("testFailureBeforeUnloadButton"),
                           2, 1, &ShortageTestController::simulateFailureBeforeUnload);
    createTestActionButton(actions, QStringLiteral("倒料完成"), QStringLiteral("testMaterialUnloadedButton"),
                           3, 0, &ShortageTestController::simulateMaterialUnloaded);
    createTestActionButton(actions, QStringLiteral("任务成功"), QStringLiteral("testTaskSucceededButton"),
                           3, 1, &ShortageTestController::simulateTaskSucceeded);
    right->addLayout(actions);
    right->addStretch();
    content->addLayout(right, 1);
    layout->addLayout(content, 1);

    m_eventLog = new QTextEdit(page);
    m_eventLog->setObjectName(QStringLiteral("testEventLog"));
    m_eventLog->setReadOnly(true);
    layout->addWidget(m_eventLog);
    if (m_testController != nullptr) {
        connect(m_testController, &ShortageTestController::eventLogged, m_eventLog,
                &QTextEdit::append);
        connect(m_testController, &ShortageTestController::operationRejected, m_eventLog,
                &QTextEdit::append);
    }

    return page;
}

QTableWidget *ShortageConfigDialog::createStationTable(ProductModel product,
                                                       const QString &objectName)
{
    auto *table = new QTableWidget(12, 11, this);
    table->setObjectName(objectName);
    table->setHorizontalHeaderLabels({QStringLiteral("代码工位"), QStringLiteral("NO"),
                                      QStringLiteral("现场位置"), QStringLiteral("品号"),
                                      QStringLiteral("启用"), QStringLiteral("每箱"),
                                      QStringLiteral("最低"), QStringLiteral("最高"),
                                      QStringLiteral("L/R"), QStringLiteral("L/L"),
                                      QStringLiteral("R/H")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->setVisible(false);
    table->setAlternatingRowColors(true);
    populateStationTable(table, product);
    return table;
}

void ShortageConfigDialog::populateStationTable(QTableWidget *table, ProductModel product)
{
    int row = 0;
    for (const ShortageStationConfig &station : m_configuration.stations) {
        if (station.product != product)
            continue;
        table->setItem(row, 0, readOnlyItem(QString::number(station.stationId)));
        table->setItem(row, 1, new QTableWidgetItem(station.temporaryNo));
        table->setItem(row, 2, new QTableWidgetItem(station.sitePosition));
        table->setItem(row, 3, new QTableWidgetItem(station.partNumber));
        table->setItem(row, 4, new QTableWidgetItem(station.enabled ? QStringLiteral("1")
                                                                     : QStringLiteral("0")));
        table->setItem(row, 5, new QTableWidgetItem(QString::number(station.boxQuantity)));
        table->setItem(row, 6, new QTableWidgetItem(QString::number(station.minimumStock)));
        table->setItem(row, 7, new QTableWidgetItem(QString::number(station.maximumStock)));
        table->setItem(row, 8, new QTableWidgetItem(QString::number(station.usageLeftRight)));
        table->setItem(row, 9, new QTableWidgetItem(QString::number(station.usageLeftOnly)));
        table->setItem(row, 10, new QTableWidgetItem(QString::number(station.usageRightOnly)));
        ++row;
    }
}

void ShortageConfigDialog::createParameterEditors(QVBoxLayout *layout)
{
    auto *form = new QFormLayout;
    m_endpointEdit = new QLineEdit(m_configuration.parameters.liveMesDayEndpoint, this);
    m_endpointEdit->setObjectName(QStringLiteral("liveMesDayEndpointEdit"));
    form->addRow(QStringLiteral("真实缺料 MES 地址"), m_endpointEdit);

    m_intervalSpin = new QSpinBox(this);
    m_intervalSpin->setObjectName(QStringLiteral("sampleIntervalSecondsSpin"));
    m_intervalSpin->setRange(5, 300);
    m_intervalSpin->setValue(m_configuration.parameters.sampleIntervalSeconds);
    form->addRow(QStringLiteral("采样间隔（秒）"), m_intervalSpin);

    m_timeoutSpin = new QSpinBox(this);
    m_timeoutSpin->setObjectName(QStringLiteral("roundTimeoutSecondsSpin"));
    m_timeoutSpin->setRange(1, 30);
    m_timeoutSpin->setValue(m_configuration.parameters.roundTimeoutSeconds);
    form->addRow(QStringLiteral("单轮超时（秒）"), m_timeoutSpin);

    m_alarmSpin = new QSpinBox(this);
    m_alarmSpin->setObjectName(QStringLiteral("communicationAlarmMinutesSpin"));
    m_alarmSpin->setRange(1, 60);
    m_alarmSpin->setValue(m_configuration.parameters.communicationAlarmMinutes);
    form->addRow(QStringLiteral("通信报警（分钟）"), m_alarmSpin);

    m_failureLimitSpin = new QSpinBox(this);
    m_failureLimitSpin->setObjectName(QStringLiteral("preUnloadFailureLimitSpin"));
    m_failureLimitSpin->setRange(1, 20);
    m_failureLimitSpin->setValue(m_configuration.parameters.preUnloadFailureLimit);
    form->addRow(QStringLiteral("连续倒料前失败阈值"), m_failureLimitSpin);
    layout->addLayout(form);
}

void ShortageConfigDialog::createTestActionButton(QGridLayout *layout,
                                                  const QString &text,
                                                  const QString &objectName,
                                                  int row,
                                                  int column,
                                                  void (ShortageTestController::*slot)())
{
    auto *button = new QPushButton(text, this);
    button->setObjectName(objectName);
    if (m_testController != nullptr)
        connect(button, &QPushButton::clicked, m_testController, slot);
    layout->addWidget(button, row, column);
}

ShortageConfiguration ShortageConfigDialog::configurationFromUi() const
{
    ShortageConfiguration configuration;
    configuration.revision = m_configuration.revision;
    for (ProductModel product : products()) {
        const QTableWidget *table = m_tables.value(product);
        for (int row = 0; row < table->rowCount(); ++row) {
            ShortageStationConfig station;
            station.product = product;
            station.stationId = tableNumber(table, row, 0);
            station.temporaryNo = tableText(table, row, 1);
            station.sitePosition = tableText(table, row, 2);
            station.partNumber = tableText(table, row, 3);
            station.enabled = tableText(table, row, 4).trimmed() != QStringLiteral("0");
            station.boxQuantity = tableNumber(table, row, 5);
            station.minimumStock = tableNumber(table, row, 6);
            station.maximumStock = tableNumber(table, row, 7);
            station.usageLeftRight = tableNumber(table, row, 8);
            station.usageLeftOnly = tableNumber(table, row, 9);
            station.usageRightOnly = tableNumber(table, row, 10);
            configuration.stations.append(station);
        }
    }
    configuration.parameters.liveMesDayEndpoint = m_endpointEdit->text();
    configuration.parameters.sampleIntervalSeconds = m_intervalSpin->value();
    configuration.parameters.roundTimeoutSeconds = m_timeoutSpin->value();
    configuration.parameters.communicationAlarmMinutes = m_alarmSpin->value();
    configuration.parameters.preUnloadFailureLimit = m_failureLimitSpin->value();
    return configuration;
}

void ShortageConfigDialog::focusConfigurationWidget(QWidget *widget)
{
    if (widget == nullptr)
        return;
    if (m_mainTabs != nullptr)
        m_mainTabs->setCurrentIndex(0);
    raise();
    activateWindow();
    widget->setFocus(Qt::OtherFocusReason);
}

bool ShortageConfigDialog::focusStationValidationFailure(const QString &messageZh)
{
    static const QRegularExpression stationError(
        QStringLiteral("行\\d+ 产品(88R|88|92) 工位(\\d+) 字段([A-Za-z]+)"));
    const QRegularExpressionMatch match = stationError.match(messageZh);
    if (!match.hasMatch())
        return false;

    ProductModel product = ProductModel::Model88;
    if (!productFromName(match.captured(1), &product))
        return false;

    QTableWidget *table = m_tables.value(product, nullptr);
    if (table == nullptr)
        return false;

    const int column = stationFieldColumn(match.captured(3));
    if (column < 0)
        return false;

    const qint64 stationId = match.captured(2).toLongLong();
    int targetRow = -1;
    for (int row = 0; row < table->rowCount(); ++row) {
        if (tableNumber(table, row, 0) == stationId) {
            targetRow = row;
            break;
        }
    }
    if (targetRow < 0)
        return false;

    if (m_mainTabs != nullptr)
        m_mainTabs->setCurrentIndex(0);
    if (m_productTabs != nullptr)
        m_productTabs->setCurrentIndex(productTabIndex(product));
    table->setCurrentCell(targetRow, column);
    table->scrollToItem(table->item(targetRow, column), QAbstractItemView::PositionAtCenter);
    raise();
    activateWindow();
    table->setFocus(Qt::OtherFocusReason);
    return true;
}

bool ShortageConfigDialog::focusValidationFailure(const QString &messageZh)
{
    if (focusStationValidationFailure(messageZh))
        return true;

    if (messageZh.contains(QStringLiteral("liveMesDayEndpoint"))) {
        focusConfigurationWidget(m_endpointEdit);
        return true;
    }
    if (messageZh.contains(QStringLiteral("sampleIntervalSeconds"))
        || messageZh.contains(QStringLiteral("timeout < interval"))) {
        focusConfigurationWidget(m_intervalSpin);
        return true;
    }
    if (messageZh.contains(QStringLiteral("roundTimeoutSeconds"))) {
        focusConfigurationWidget(m_timeoutSpin);
        return true;
    }
    if (messageZh.contains(QStringLiteral("communicationAlarmMinutes"))) {
        focusConfigurationWidget(m_alarmSpin);
        return true;
    }
    if (messageZh.contains(QStringLiteral("preUnloadFailureLimit"))) {
        focusConfigurationWidget(m_failureLimitSpin);
        return true;
    }
    return false;
}

void ShortageConfigDialog::saveConfiguration()
{
    // UI 状态分支：保存先查外部运行门禁，再做配置校验；失败只定位控件，不改生产 Engine。
    const ShortageEditConditions conditions =
        m_editGateProvider ? m_editGateProvider() : ShortageEditConditions {};
    const ShortageOperationResult gate = ShortageConfigStore::canEditConfiguration(conditions);
    if (!gate.ok) {
        QMessageBox::warning(this, QStringLiteral("禁止保存"), gate.messageZh);
        return;
    }

    ShortageConfiguration candidate = configurationFromUi();
    const ShortageOperationResult validation = ShortageConfigStore::validate(candidate);
    if (!validation.ok) {
        QMessageBox::warning(this, QStringLiteral("配置错误"), validation.messageZh);
        focusValidationFailure(validation.messageZh);
        return;
    }

    m_configuration = candidate;
    m_validatedConfiguration = candidate;
    emit configurationSaved();
}
