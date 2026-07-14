#include "shortageconfigdialog.h"
#include "shortagevalidationdialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
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

/// 返回测试页手工样本允许选择的全部生产模式；只用于 UI 下拉框，不改变业务枚举定义。
QList<ProductionMode> productionModes()
{
    return {ProductionMode::LeftRight, ProductionMode::LeftOnly, ProductionMode::RightOnly};
}

/// 将生产模式映射为现场沿用的显示短码；未知分支仅作为防御性中文兜底。
QString modeName(ProductionMode mode)
{
    switch (mode) {
    case ProductionMode::LeftRight:
        return QStringLiteral("L/R");
    case ProductionMode::LeftOnly:
        return QStringLiteral("L/L");
    case ProductionMode::RightOnly:
        return QStringLiteral("R/H");
    }
    return QStringLiteral("未知模式");
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

/// 根据当前生产模式读取配置表用量；仅用于测试页只读展示，不写回 Engine。
qint64 usageForMode(const ShortageStationConfig &station, ProductionMode mode)
{
    switch (mode) {
    case ProductionMode::LeftRight:
        return station.usageLeftRight;
    case ProductionMode::LeftOnly:
        return station.usageLeftOnly;
    case ProductionMode::RightOnly:
        return station.usageRightOnly;
    }
    return 0;
}

/// 将补料单状态完整映射为中文界面文案；不使用整数强转暴露枚举值。
QString replenishmentOrderStateText(ReplenishmentOrderState state)
{
    switch (state) {
    case ReplenishmentOrderState::AwaitingDispatch:
        return QStringLiteral("等待派单");
    case ReplenishmentOrderState::Queued:
        return QStringLiteral("已入队");
    case ReplenishmentOrderState::Running:
        return QStringLiteral("运行中");
    case ReplenishmentOrderState::Unloaded:
        return QStringLiteral("已倒料");
    case ReplenishmentOrderState::Succeeded:
        return QStringLiteral("已成功");
    case ReplenishmentOrderState::FailedBeforeUnload:
        return QStringLiteral("倒料前失败");
    case ReplenishmentOrderState::FailedAfterUnload:
        return QStringLiteral("倒料后失败");
    case ReplenishmentOrderState::Canceled:
        return QStringLiteral("已取消");
    }
    return QStringLiteral("未知状态");
}

/// 判断补料单是否仍应作为“当前测试补料单”展示；终态只留在事件日志和历史状态。
bool isNonTerminalOrder(ReplenishmentOrderState state)
{
    switch (state) {
    case ReplenishmentOrderState::AwaitingDispatch:
    case ReplenishmentOrderState::Queued:
    case ReplenishmentOrderState::Running:
    case ReplenishmentOrderState::Unloaded:
        return true;
    case ReplenishmentOrderState::Succeeded:
    case ReplenishmentOrderState::FailedBeforeUnload:
    case ReplenishmentOrderState::FailedAfterUnload:
    case ReplenishmentOrderState::Canceled:
        return false;
    }
    return false;
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
    // 修改前 QDialog 默认标题栏缺少最小化/最大化；增加窗口按钮只改变窗口管理，不影响测试状态。
    setWindowFlag(Qt::Window, true);
    setWindowFlags(windowFlags()
                   | Qt::WindowMinimizeButtonHint
                   | Qt::WindowMaximizeButtonHint
                   | Qt::WindowCloseButtonHint);
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
    if (kShowShortageValidationTab) {
        // 验证业务位于独立 Dialog；第三 Tab 只保留可后续隐藏的入口。
        m_mainTabs->addTab(buildValidationEntryPage(), QStringLiteral("验证向导"));
    }
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
    m_manualSourceRadio = new QRadioButton(QStringLiteral("手工源"), sourceBox);
    m_manualSourceRadio->setObjectName(QStringLiteral("manualSourceRadio"));
    m_fieldSourceRadio = new QRadioButton(QStringLiteral("现场源"), sourceBox);
    m_fieldSourceRadio->setObjectName(QStringLiteral("fieldSourceRadio"));
    m_manualSourceRadio->setChecked(true);
    group->addButton(m_manualSourceRadio, 0);
    group->addButton(m_fieldSourceRadio, 1);
    sourceLayout->addWidget(m_manualSourceRadio);
    sourceLayout->addWidget(m_fieldSourceRadio);
    sourceLayout->addStretch();
    layout->addWidget(sourceBox);

    auto *manualBox = new QGroupBox(QStringLiteral("手工样本"), page);
    auto *manualLayout = new QFormLayout(manualBox);
    m_manualProductCombo = new QComboBox(manualBox);
    m_manualProductCombo->setObjectName(QStringLiteral("manualProductCombo"));
    for (ProductModel product : products())
        m_manualProductCombo->addItem(productName(product), QVariant::fromValue(product));
    manualLayout->addRow(QStringLiteral("产品"), m_manualProductCombo);

    m_manualModeCombo = new QComboBox(manualBox);
    m_manualModeCombo->setObjectName(QStringLiteral("manualModeCombo"));
    for (ProductionMode mode : productionModes())
        m_manualModeCombo->addItem(modeName(mode), QVariant::fromValue(mode));
    manualLayout->addRow(QStringLiteral("模式"), m_manualModeCombo);

    m_manualActualQtyEdit = new QLineEdit(QStringLiteral("0"), manualBox);
    m_manualActualQtyEdit->setObjectName(QStringLiteral("manualActualQtyEdit"));
    // 手工 actualQty 修改前没有完整输入；现场问题是无法复现真实 qint64 产量边界；
    // 修改后正则只接受非负 qint64 十进制范围，不影响控制器对业务状态的二次校验。
    m_manualActualQtyEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral(
            "^(0|[1-9][0-9]{0,17}|[1-8][0-9]{18}|9[0-1][0-9]{17}|92[0-1][0-9]{16}|922[0-2][0-9]{15}|9223[0-2][0-9]{14}|92233[0-6][0-9]{13}|922337[0-1][0-9]{12}|92233720[0-2][0-9]{10}|922337203[0-5][0-9]{9}|9223372036[0-7][0-9]{8}|92233720368[0-4][0-9]{7}|922337203685[0-3][0-9]{6}|9223372036854[0-6][0-9]{5}|92233720368547[0-6][0-9]{4}|922337203685477[0-4][0-9]{3}|9223372036854775[0-7][0-9]{2}|922337203685477580[0-7])$")),
        m_manualActualQtyEdit));
    manualLayout->addRow(QStringLiteral("actualQty"), m_manualActualQtyEdit);

    m_manualSampleSubmitButton = new QPushButton(QStringLiteral("提交手工样本"), manualBox);
    m_manualSampleSubmitButton->setObjectName(QStringLiteral("manualSampleSubmitButton"));
    connect(m_manualSampleSubmitButton, &QPushButton::clicked, this,
            &ShortageConfigDialog::submitManualSample);
    manualLayout->addRow(QString(), m_manualSampleSubmitButton);
    layout->addWidget(manualBox);

    connect(m_manualSourceRadio, &QRadioButton::clicked, this, [this] {
        // 修改前单选框只改变外观；现在先通知控制器停止现场采样，再刷新输入门禁。
        if (m_testController != nullptr)
            m_testController->selectInputSource(ShortageTestInputSource::Manual);
        refreshTestSourceControls();
    });
    connect(m_fieldSourceRadio, &QRadioButton::clicked, this, [this] {
        // 选择现场源不自动启动 MES/PLC，仍需操作员明确点击“启动现场采样”。
        if (m_testController != nullptr)
            m_testController->selectInputSource(ShortageTestInputSource::Field);
        refreshTestSourceControls();
    });

    auto *content = new QHBoxLayout;
    m_testRuntimeTable = new QTableWidget(12, 7, page);
    m_testRuntimeTable->setObjectName(QStringLiteral("testStationRuntimeTable"));
    m_testRuntimeTable->setHorizontalHeaderLabels({QStringLiteral("工位"), QStringLiteral("库存"),
                                                   QStringLiteral("最低"), QStringLiteral("最高"),
                                                   QStringLiteral("当前用量"), QStringLiteral("状态"),
                                                   QStringLiteral("连续失败")});
    m_testRuntimeTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_testRuntimeTable->verticalHeader()->setVisible(false);
    for (int row = 0; row < 12; ++row) {
        for (int column = 0; column < 7; ++column)
            m_testRuntimeTable->setItem(row, column, readOnlyItem(QString()));
        m_testRuntimeTable->item(row, 0)->setText(QString::number(row + 1));
    }
    content->addWidget(m_testRuntimeTable, 1);

    auto *right = new QVBoxLayout;
    m_testPlanSummaryLabel = new QLabel(page);
    m_testPlanSummaryLabel->setObjectName(QStringLiteral("testPlanSummaryLabel"));
    m_testPlanSummaryLabel->setWordWrap(true);
    right->addWidget(m_testPlanSummaryLabel);

    m_testOrderSummaryLabel = new QLabel(page);
    m_testOrderSummaryLabel->setObjectName(QStringLiteral("testOrderSummaryLabel"));
    m_testOrderSummaryLabel->setWordWrap(true);
    right->addWidget(m_testOrderSummaryLabel);

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
    createTestActionButton(actions, QStringLiteral("保存测试状态"), QStringLiteral("testSaveStateButton"),
                           4, 0, &ShortageTestController::saveTestState);
    createTestActionButton(actions, QStringLiteral("重新加载测试状态"), QStringLiteral("testReloadStateButton"),
                           4, 1, &ShortageTestController::reloadTestState);
    createTestActionButton(actions, QStringLiteral("倒料后失败"), QStringLiteral("testFailureAfterUnloadButton"),
                           5, 0, &ShortageTestController::simulateFailureAfterUnload);
    createTestActionButton(actions, QStringLiteral("重复发送倒料事件"), QStringLiteral("testResendUnloadButton"),
                           5, 1, &ShortageTestController::resendLastUnloadFact);
    createTestActionButton(actions, QStringLiteral("模拟程序重启"), QStringLiteral("testSimulateRestartButton"),
                           6, 0, &ShortageTestController::simulateRestart);
    auto *clear = new QPushButton(QStringLiteral("清空测试状态"), this);
    clear->setObjectName(QStringLiteral("testClearStateButton"));
    m_testActionButtons.insert(clear->objectName(), clear);
    connect(clear, &QPushButton::clicked, this, [this] {
        // 清空修改前会直接调用控制器；现场问题是误点会删除独立测试文件；
        // 修改后必须中文二次确认，取消时不调用控制器，不影响确认后的 StandaloneTest 清理范围。
        if (m_testController == nullptr)
            return;
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this, QStringLiteral("确认清空测试状态"),
            QStringLiteral("确认清空独立测试状态？正式 production-* 文件不会被触碰。"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes)
            m_testController->clearTestStateAfterConfirmation();
    });
    actions->addWidget(clear, 6, 1);
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
        // snapshotChanged 是测试运行状态进入旧完整逻辑页面的唯一连接点；只刷新只读显示。
        connect(m_testController, &ShortageTestController::snapshotChanged, this,
                &ShortageConfigDialog::refreshTestSnapshot);
        // actionAvailabilityChanged 是事件按钮门禁的唯一连接点；UI 不复制控制器状态机。
        connect(m_testController, &ShortageTestController::actionAvailabilityChanged, this,
                &ShortageConfigDialog::refreshTestActionAvailability);
        refreshTestActionAvailability(m_testController->actionAvailability());
    }
    refreshTestSnapshot(m_testController != nullptr ? m_testController->currentSnapshot()
                                                    : m_snapshot);
    refreshTestSourceControls();

    return page;
}

QWidget *ShortageConfigDialog::buildValidationEntryPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *summary = new QLabel(
        QStringLiteral("验证业务固定在独立缺料验证控制台内执行；本页只提供可隐藏入口，不承载验证步骤、日志或现场证据控件。"),
        page);
    summary->setWordWrap(true);
    summary->setStyleSheet(QStringLiteral("color:#ffd166; font-weight:600;"));
    layout->addWidget(summary);

    auto *scope = new QLabel(
        QStringLiteral("关闭或重复打开验证控制台不会启动/停止采样，也不会修改 StandaloneTest 状态。"),
        page);
    scope->setWordWrap(true);
    layout->addWidget(scope);

    auto *buttonRow = new QHBoxLayout;
    auto *openButton = new QPushButton(QStringLiteral("打开验证控制台"), page);
    openButton->setObjectName(QStringLiteral("openShortageValidationDialogButton"));
    buttonRow->addWidget(openButton);
    buttonRow->addStretch();
    layout->addLayout(buttonRow);
    layout->addStretch();

    // 第三 Tab 的唯一信号连接点：只打开或恢复独立验证窗口，不执行业务动作。
    connect(openButton, &QPushButton::clicked, this, &ShortageConfigDialog::openValidationDialog);
    return page;
}

void ShortageConfigDialog::openValidationDialog()
{
    if (m_shortageValidationDialog != nullptr) {
        // 已存在窗口由 QPointer 自动跟踪；重复点击只恢复、显示并置顶，不创建第二个会话。
        if (m_shortageValidationDialog->isMinimized())
            m_shortageValidationDialog->showNormal();
        m_shortageValidationDialog->show();
        m_shortageValidationDialog->raise();
        m_shortageValidationDialog->activateWindow();
        return;
    }

    auto *dialog = new ShortageValidationDialog(m_testController, this);
    // Qt 父子关系负责窗口生命周期，WA_DeleteOnClose 关闭后让 QPointer 自动归零。
    m_shortageValidationDialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
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
    m_testActionButtons.insert(objectName, button);
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

void ShortageConfigDialog::submitManualSample()
{
    if (m_testController == nullptr)
        return;

    bool ok = false;
    const qint64 actualQty = m_manualActualQtyEdit->text().toLongLong(&ok);
    if (!ok || actualQty < 0) {
        // 手工提交失败分支修改前没有 qint64 解析失败提示；现在只记录中文原因，
        // 不写 Engine，不影响合法 actualQty 的既有完整逻辑链路。
        const QString reason = QStringLiteral("手工样本提交失败：actualQty 不是非负 qint64");
        if (m_eventLog != nullptr)
            m_eventLog->append(reason);
        return;
    }

    const ProductModel product =
        qvariant_cast<ProductModel>(m_manualProductCombo->currentData());
    const ProductionMode mode =
        qvariant_cast<ProductionMode>(m_manualModeCombo->currentData());
    m_testController->applyManualSample(product, mode, actualQty);
}

void ShortageConfigDialog::refreshTestSourceControls()
{
    const bool controllerAvailable = m_testController != nullptr;
    // 修改前无控制器测试页总被当作手工源；现场问题是旧单选互斥契约被刷新覆盖；
    // 修改后无控制器时尊重当前控件状态，不影响有控制器时由控制器决定的权威来源。
    const ShortageTestInputSource source =
        controllerAvailable ? m_testController->inputSource()
                            : ((m_fieldSourceRadio != nullptr && m_fieldSourceRadio->isChecked())
                                   ? ShortageTestInputSource::Field
                                   : ShortageTestInputSource::Manual);
    const ShortageTestActionAvailability availability =
        controllerAvailable ? m_testController->actionAvailability()
                            : ShortageTestActionAvailability {};
    const bool manualSource = source == ShortageTestInputSource::Manual;

    if (m_manualSourceRadio != nullptr)
        m_manualSourceRadio->setChecked(manualSource);
    if (m_fieldSourceRadio != nullptr)
        m_fieldSourceRadio->setChecked(source == ShortageTestInputSource::Field);

    const bool manualInputsEnabled =
        !controllerAvailable || (manualSource && availability.canSubmitManualSample);
    if (m_manualProductCombo != nullptr)
        m_manualProductCombo->setEnabled(manualInputsEnabled);
    if (m_manualModeCombo != nullptr)
        m_manualModeCombo->setEnabled(manualInputsEnabled);
    if (m_manualActualQtyEdit != nullptr)
        m_manualActualQtyEdit->setEnabled(manualInputsEnabled);
    if (m_manualSampleSubmitButton != nullptr)
        m_manualSampleSubmitButton->setEnabled(manualInputsEnabled);
}

void ShortageConfigDialog::refreshTestSnapshot(const ShortageUiSnapshot &snapshot)
{
    m_snapshot = snapshot;
    if (m_testRuntimeTable == nullptr)
        return;

    const ProductModel product = snapshot.runtime.product;
    const ProductionMode mode = snapshot.runtime.mode;
    for (int row = 0; row < 12; ++row) {
        const int stationId = row + 1;
        const ShortageStationRuntime *runtimeStation = nullptr;
        for (const ShortageStationRuntime &station : snapshot.runtime.stations) {
            if (station.stationId == stationId) {
                runtimeStation = &station;
                break;
            }
        }

        const ShortageStationConfig *configStation = nullptr;
        for (const ShortageStationConfig &station : m_configuration.stations) {
            if (station.product == product && station.stationId == stationId) {
                configStation = &station;
                break;
            }
        }

        const qint64 stock = runtimeStation == nullptr ? 0 : runtimeStation->stock;
        const qint64 minimum = configStation == nullptr ? 0 : configStation->minimumStock;
        const qint64 maximum = configStation == nullptr ? 0 : configStation->maximumStock;
        const qint64 usage = configStation == nullptr ? 0 : usageForMode(*configStation, mode);
        const int failures =
            runtimeStation == nullptr ? 0 : runtimeStation->consecutivePreUnloadFailures;

        QString status = QStringLiteral("正常");
        if (runtimeStation != nullptr && runtimeStation->automaticPaused) {
            status = runtimeStation->pauseReasonZh.isEmpty()
                ? QStringLiteral("已暂停")
                : QStringLiteral("已暂停：%1").arg(runtimeStation->pauseReasonZh);
        } else if (snapshot.runtime.activeStationId == stationId) {
            status = QStringLiteral("活动");
        } else if (snapshot.runtime.waitingStationIds.contains(stationId)) {
            status = QStringLiteral("等待");
        } else if (!snapshot.runtime.initialized) {
            status = QStringLiteral("未建账");
        }

        m_testRuntimeTable->item(row, 0)->setText(QString::number(stationId));
        m_testRuntimeTable->item(row, 1)->setText(QString::number(stock));
        m_testRuntimeTable->item(row, 2)->setText(QString::number(minimum));
        m_testRuntimeTable->item(row, 3)->setText(QString::number(maximum));
        m_testRuntimeTable->item(row, 4)->setText(QString::number(usage));
        m_testRuntimeTable->item(row, 5)->setText(status);
        m_testRuntimeTable->item(row, 6)->setText(QString::number(failures));
    }

    QStringList waitingTexts;
    for (int stationId : snapshot.runtime.waitingStationIds)
        waitingTexts.append(QString::number(stationId));
    const QString waitingSummary =
        waitingTexts.isEmpty() ? QStringLiteral("无") : waitingTexts.join(QStringLiteral("、"));
    const QString baseline =
        snapshot.runtime.actualQty.hasBaseline
        ? QString::number(snapshot.runtime.actualQty.baseline)
        : QStringLiteral("未建立");
    if (m_testPlanSummaryLabel != nullptr) {
        m_testPlanSummaryLabel->setText(
            QStringLiteral("%1\n%2\n基线=%3，活动工位=%4，等待顺序=%5")
                .arg(snapshot.summaryLine1Zh,
                     snapshot.summaryLine2Zh,
                     baseline)
                .arg(snapshot.runtime.activeStationId)
                .arg(waitingSummary));
    }

    const ReplenishmentOrder *currentOrder = nullptr;
    for (int index = snapshot.runtime.orders.size() - 1; index >= 0; --index) {
        const ReplenishmentOrder &order = snapshot.runtime.orders.at(index);
        if (isNonTerminalOrder(order.state)) {
            currentOrder = &order;
            break;
        }
    }
    if (m_testOrderSummaryLabel != nullptr) {
        if (currentOrder == nullptr) {
            m_testOrderSummaryLabel->setText(QStringLiteral("当前测试补料单：无"));
        } else {
            m_testOrderSummaryLabel->setText(
                QStringLiteral("当前测试补料单：单号=%1，工位=%2，状态=%3，taskId=%4")
                    .arg(currentOrder->orderNo)
                    .arg(currentOrder->stationId)
                    .arg(replenishmentOrderStateText(currentOrder->state))
                    .arg(currentOrder->taskId));
        }
    }
}

void ShortageConfigDialog::refreshTestActionAvailability(
    const ShortageTestActionAvailability &availability)
{
    auto applyButton = [this](const QString &objectName, bool enabled) {
        QPushButton *button = m_testActionButtons.value(objectName, nullptr);
        if (button == nullptr)
            return;
        button->setEnabled(enabled);
        button->setToolTip(enabled
                               ? QStringLiteral("可执行：控制器仍会二次校验当前测试状态")
                               : QStringLiteral("当前测试状态不允许：控制器仍会二次校验"));
    };

    if (m_manualSampleSubmitButton != nullptr) {
        m_manualSampleSubmitButton->setEnabled(availability.canSubmitManualSample);
        m_manualSampleSubmitButton->setToolTip(
            availability.canSubmitManualSample
                ? QStringLiteral("可提交手工样本：控制器仍会二次校验输入源")
                : QStringLiteral("当前测试状态不允许提交手工样本"));
    }
    applyButton(QStringLiteral("testStartFieldButton"), availability.canStartFieldSampling);
    applyButton(QStringLiteral("testStopButton"), availability.canStopFieldSampling);
    applyButton(QStringLiteral("testDispatchAcceptedButton"), availability.canAcceptDispatch);
    applyButton(QStringLiteral("testDispatchRejectedButton"), availability.canRejectDispatch);
    applyButton(QStringLiteral("testFailureBeforeUnloadButton"),
                availability.canFailBeforeUnload);
    applyButton(QStringLiteral("testMaterialUnloadedButton"), availability.canRecordUnload);
    applyButton(QStringLiteral("testFailureAfterUnloadButton"), availability.canFailAfterUnload);
    applyButton(QStringLiteral("testTaskSucceededButton"), availability.canSucceed);
    applyButton(QStringLiteral("testResendUnloadButton"), availability.canResendUnload);
    refreshTestSourceControls();
}
