#include "palletparamdialog.h"

#include "visionclient.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIntValidator>
#include <QLineEdit>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {
QString fmt(double v) { return QString::number(v, 'f', 1); }

class NoWheelDoubleSpinBox : public QDoubleSpinBox
{
public:
    explicit NoWheelDoubleSpinBox(QWidget *parent = nullptr)
        : QDoubleSpinBox(parent)
    {
        setFocusPolicy(Qt::StrongFocus);
    }

protected:
    void wheelEvent(QWheelEvent *event) override
    {
        event->ignore();
    }
};

QLabel *wrapLabel(const QString &text = QString())
{
    auto *label = new QLabel(text);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

void addTripleRow(QFormLayout *form,
                  const QString &label,
                  QDoubleSpinBox *a,
                  QDoubleSpinBox *b,
                  QDoubleSpinBox *c,
                  const QStringList &names)
{
    auto *row = new QHBoxLayout();
    row->addWidget(new QLabel(names.value(0)));
    row->addWidget(a);
    row->addWidget(new QLabel(names.value(1)));
    row->addWidget(b);
    row->addWidget(new QLabel(names.value(2)));
    row->addWidget(c);
    form->addRow(label, row);
}
}

PalletParamDialog::PalletParamDialog(PalletScheduler *scheduler,
                                     VisionHttpClient *visionClient,
                                     QWidget *parent)
    : QDialog(parent)
    , m_scheduler(scheduler)
    , m_visionClient(visionClient)
{
    setWindowTitle(QStringLiteral("空箱码垛配置"));
    resize(980, 720);
    setMinimumSize(860, 620);

    auto *root = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    m_tabs->addTab(createAreaPage(PalletArea::LargeBox), QStringLiteral("大箱码垛"));
    m_tabs->addTab(createAreaPage(PalletArea::SmallBox), QStringLiteral("小箱码垛"));
    root->addWidget(m_tabs, 1);

    auto *bottom = new QHBoxLayout();
    bottom->addStretch();
    auto *closeBtn = new QPushButton(QStringLiteral("关闭"));
    closeBtn->setFixedHeight(28);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
    bottom->addWidget(closeBtn);
    root->addLayout(bottom);

    if (m_scheduler) {
        connect(m_scheduler, &PalletScheduler::stateChanged,
                this, &PalletParamDialog::refreshPage);
        connect(m_scheduler, &PalletScheduler::areaFull, this, [this](PalletArea area) {
            if (auto *w = widgets(area))
                setStatus(w, QStringLiteral("码垛区已满，请搬运"), QStringLiteral("error"));
        });
        connect(m_scheduler, &PalletScheduler::areaAutoReset,
                this, [this](PalletArea area, const QString &reason) {
            if (auto *w = widgets(area))
                setStatus(w, reason, QStringLiteral("warning"));
            refreshPage(area);
        });
    }

    if (m_visionClient) {
        connect(m_visionClient, &VisionHttpClient::palletOccupancyReady,
                this, [this](const QString &requestId, bool occupied,
                             int objectCount, double confidence, const QString &summary) {
            if (!m_pendingRequests.contains(requestId))
                return;
            const PalletArea area = m_pendingRequests.take(requestId);
            PageWidgets *w = widgets(area);
            if (!w || !m_scheduler)
                return;
            if (occupied) {
                m_scheduler->markAreaObservedOccupied(area);
                w->detectLabel->setText(QStringLiteral("检测到目标：%1").arg(summary));
                setStatus(w, QStringLiteral("视觉检测到目标，沿用当前缓存"), QStringLiteral("ok"));
            } else {
                m_scheduler->markAreaObservedEmpty(area);
                w->detectLabel->setText(QStringLiteral(
                    "未检测到目标：object_count=%1, confidence=%2")
                    .arg(objectCount).arg(confidence, 0, 'f', 3));
                setStatus(w, QStringLiteral("视觉检测为空，已累计空区计数"), QStringLiteral("warning"));
            }
            w->detectBtn->setEnabled(true);
            refreshPage(area);
        });
        connect(m_visionClient, &VisionHttpClient::palletOccupancyError,
                this, [this](const QString &requestId, const QString &msg) {
            if (!m_pendingRequests.contains(requestId))
                return;
            const PalletArea area = m_pendingRequests.take(requestId);
            PageWidgets *w = widgets(area);
            if (!w)
                return;
            w->detectBtn->setEnabled(true);
            w->detectLabel->setText(msg);
            setStatus(w, QStringLiteral("视觉检测失败，缓存未修改"), QStringLiteral("error"));
        });
    }

    refreshPage(PalletArea::LargeBox);
    refreshPage(PalletArea::SmallBox);
}

QWidget *PalletParamDialog::createAreaPage(PalletArea area)
{
    auto *w = new PageWidgets();
    m_pages.insert(areaKey(area), w);

    auto *page = new QWidget();
    w->page = page;
    auto *pageLayout = new QHBoxLayout(page);

    auto *leftWidget = new QWidget();
    auto *left = new QVBoxLayout(leftWidget);
    left->setContentsMargins(4, 4, 4, 4);

    auto addGroup = [left](const QString &title) {
        auto *group = new QGroupBox(title);
        auto *form = new QFormLayout(group);
        form->setLabelAlignment(Qt::AlignRight);
        left->addWidget(group);
        return form;
    };

    auto *palletForm = addGroup(QStringLiteral("码垛区域尺寸 (mm)"));
    w->palletX = createDistanceSpin(); w->palletY = createDistanceSpin(); w->palletZ = createDistanceSpin();
    addTripleRow(palletForm, QStringLiteral("托盘/区域:"), w->palletX, w->palletY, w->palletZ,
                 {QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")});

    auto *boxForm = addGroup(QStringLiteral("空箱尺寸 (mm)"));
    w->boxX = createDistanceSpin(); w->boxY = createDistanceSpin(); w->boxZ = createDistanceSpin();
    addTripleRow(boxForm, QStringLiteral("空箱:"), w->boxX, w->boxY, w->boxZ,
                 {QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")});

    auto *gapForm = addGroup(QStringLiteral("间距与边缘 (mm)"));
    w->gapX = createDistanceSpin(); w->gapY = createDistanceSpin();
    w->marginX = createDistanceSpin(); w->marginY = createDistanceSpin();
    auto *gapRow = new QHBoxLayout();
    gapRow->addWidget(new QLabel(QStringLiteral("X"))); gapRow->addWidget(w->gapX);
    gapRow->addWidget(new QLabel(QStringLiteral("Y"))); gapRow->addWidget(w->gapY);
    gapForm->addRow(QStringLiteral("箱间距:"), gapRow);
    auto *marginRow = new QHBoxLayout();
    marginRow->addWidget(new QLabel(QStringLiteral("X"))); marginRow->addWidget(w->marginX);
    marginRow->addWidget(new QLabel(QStringLiteral("Y"))); marginRow->addWidget(w->marginY);
    gapForm->addRow(QStringLiteral("边缘预留:"), marginRow);

    auto *heightForm = addGroup(QStringLiteral("层数与高度"));
    w->maxLayers = new QLineEdit();
    w->maxLayers->setAlignment(Qt::AlignRight);
    w->maxLayers->setValidator(new QIntValidator(1, 8, w->maxLayers));
    w->releaseZOffset = createDistanceSpin();
    w->maxRobotZ = createDistanceSpin();
    heightForm->addRow(QStringLiteral("最大层数:"), w->maxLayers);
    heightForm->addRow(QStringLiteral("释放高度参考:"), w->releaseZOffset);
    heightForm->addRow(QStringLiteral("最高安全 Z:"), w->maxRobotZ);

    auto *originForm = addGroup(QStringLiteral("机械臂初始点位（绝对预览可选）"));
    w->originX = createPoseSpin(); w->originY = createPoseSpin(); w->originZ = createPoseSpin();
    w->originRx = createAngleSpin(); w->originRy = createAngleSpin(); w->originRz = createAngleSpin();
    addTripleRow(originForm, QStringLiteral("XYZ:"), w->originX, w->originY, w->originZ,
                 {QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")});
    addTripleRow(originForm, QStringLiteral("姿态:"), w->originRx, w->originRy, w->originRz,
                 {QStringLiteral("Rx"), QStringLiteral("Ry"), QStringLiteral("Rz")});

    auto *dirForm = addGroup(QStringLiteral("方向修正"));
    w->invertX = new QCheckBox(QStringLiteral("反转 X"));
    w->invertY = new QCheckBox(QStringLiteral("反转 Y"));
    auto *dirRow = new QHBoxLayout();
    dirRow->addWidget(w->invertX); dirRow->addWidget(w->invertY); dirRow->addStretch();
    dirForm->addRow(dirRow);

    auto *stateForm = addGroup(QStringLiteral("状态修正"));
    w->placedCount = new QLineEdit();
    w->placedCount->setAlignment(Qt::AlignRight);
    w->placedCount->setValidator(new QIntValidator(0, 9999, w->placedCount));
    w->applyPlaced = new QPushButton(QStringLiteral("应用数量修正"));
    auto *stateRow = new QHBoxLayout();
    stateRow->addWidget(w->placedCount); stateRow->addWidget(w->applyPlaced);
    stateForm->addRow(QStringLiteral("已放数量:"), stateRow);

    auto *visionForm = addGroup(QStringLiteral("视觉检测"));
    w->detectBtn = new QPushButton(QStringLiteral("检测当前区域"));
    w->detectLabel = wrapLabel(QStringLiteral("未检测"));
    w->emptyCountLabel = wrapLabel(QStringLiteral("连续空计数：0/3"));
    visionForm->addRow(w->detectBtn);
    visionForm->addRow(QStringLiteral("结果:"), w->detectLabel);
    visionForm->addRow(QStringLiteral("计数:"), w->emptyCountLabel);

    auto *buttonGroup = new QGroupBox(QStringLiteral("操作"));
    auto *buttonGrid = new QGridLayout(buttonGroup);
    auto *saveBtn = new QPushButton(QStringLiteral("保存配置"));
    auto *validateBtn = new QPushButton(QStringLiteral("校验配置"));
    auto *fieldBtn = new QPushButton(QStringLiteral("填入默认现场值"));
    auto *releaseBtn = new QPushButton(QStringLiteral("推荐释放高度"));
    auto *nextBtn = new QPushButton(QStringLiteral("计算下一点"));
    auto *commitBtn = new QPushButton(QStringLiteral("模拟放置完成"));
    auto *simBtn = new QPushButton(QStringLiteral("仿真 8 层"));
    auto *clearBtn = new QPushButton(QStringLiteral("清除显示结果"));
    auto *resetBtn = new QPushButton(QStringLiteral("清零当前区域"));

    fieldBtn->setToolTip(QStringLiteral("把托盘/箱体/间距/层数恢复为默认现场值；只写入输入框，不保存配置。"));
    nextBtn->setToolTip(QStringLiteral("只按当前已放数量预览下一点，不推进缓存。"));
    commitBtn->setToolTip(QStringLiteral("仿真用：代替机械臂松爪完成信号，把已放数量推进 1。"));
    simBtn->setToolTip(QStringLiteral("从空托盘开始生成完整 8 层点位表，不修改真实缓存。"));
    clearBtn->setToolTip(QStringLiteral("清空右侧下一点、校验结果和仿真表格；不修改配置或已放数量。"));

    buttonGrid->addWidget(saveBtn, 0, 0);
    buttonGrid->addWidget(validateBtn, 0, 1);
    buttonGrid->addWidget(fieldBtn, 0, 2);
    buttonGrid->addWidget(releaseBtn, 1, 0);
    buttonGrid->addWidget(nextBtn, 1, 1);
    buttonGrid->addWidget(commitBtn, 1, 2);
    buttonGrid->addWidget(simBtn, 2, 0);
    buttonGrid->addWidget(clearBtn, 2, 1);
    buttonGrid->addWidget(resetBtn, 2, 2);
    left->addWidget(buttonGroup);
    left->addStretch();

    auto *scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setWidget(leftWidget);
    scroll->setMinimumWidth(390);
    pageLayout->addWidget(scroll, 0);

    auto *right = new QVBoxLayout();
    w->capacityLabel = wrapLabel();
    w->nextPoseLabel = wrapLabel();
    w->statusLabel = wrapLabel();
    w->validationText = new QPlainTextEdit();
    w->validationText->setReadOnly(true);
    w->validationText->setMaximumHeight(110);
    w->simulationTable = new QTableWidget(0, 13);
    w->simulationTable->setHorizontalHeaderLabels({
        QStringLiteral("序号"), QStringLiteral("层"), QStringLiteral("行"), QStringLiteral("列"),
        QStringLiteral("offsetX"), QStringLiteral("offsetY"), QStringLiteral("offsetZ"),
        QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z"),
        QStringLiteral("Rx"), QStringLiteral("Ry"), QStringLiteral("Rz")});
    w->simulationTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    w->simulationTable->horizontalHeader()->setStretchLastSection(true);
    w->simulationTable->verticalHeader()->setVisible(false);

    auto *summaryGroup = new QGroupBox(QStringLiteral("结果"));
    auto *summaryLayout = new QVBoxLayout(summaryGroup);
    summaryLayout->addWidget(w->capacityLabel);
    summaryLayout->addWidget(w->nextPoseLabel);
    summaryLayout->addWidget(w->statusLabel);
    right->addWidget(summaryGroup);

    auto *validationGroup = new QGroupBox(QStringLiteral("校验结果"));
    auto *validationLayout = new QVBoxLayout(validationGroup);
    validationLayout->addWidget(w->validationText);
    right->addWidget(validationGroup);

    auto *tableGroup = new QGroupBox(QStringLiteral("8 层仿真"));
    auto *tableLayout = new QVBoxLayout(tableGroup);
    tableLayout->addWidget(w->simulationTable);
    right->addWidget(tableGroup, 1);
    pageLayout->addLayout(right, 1);

    connect(saveBtn, &QPushButton::clicked, this, [this, area] { savePage(area); });
    connect(validateBtn, &QPushButton::clicked, this, [this, area] { validatePage(area); });
    connect(fieldBtn, &QPushButton::clicked, this, [this, area] { fillFieldDefaults(area); });
    connect(releaseBtn, &QPushButton::clicked, this, [this, area] { recommendReleaseHeight(area); });
    connect(nextBtn, &QPushButton::clicked, this, [this, area] { showNextPose(area); });
    connect(commitBtn, &QPushButton::clicked, this, [this, area] { simulatePlaced(area); });
    connect(simBtn, &QPushButton::clicked, this, [this, area] { simulateArea(area); });
    connect(clearBtn, &QPushButton::clicked, this, [this, area] { clearDisplayResults(area); });
    connect(resetBtn, &QPushButton::clicked, this, [this, area] { resetArea(area); });
    connect(w->applyPlaced, &QPushButton::clicked, this, [this, area] { applyPlacedCount(area); });
    connect(w->detectBtn, &QPushButton::clicked, this, [this, area] { detectArea(area); });

    if (m_scheduler)
        writeConfig(area, m_scheduler->config(area));
    return page;
}

PalletParamDialog::PageWidgets *PalletParamDialog::widgets(PalletArea area) const
{
    return m_pages.value(areaKey(area), nullptr);
}

QDoubleSpinBox *PalletParamDialog::createDistanceSpin(double max) const
{
    auto *spin = new NoWheelDoubleSpinBox();
    spin->setRange(0.0, max);
    spin->setDecimals(1);
    spin->setSingleStep(1.0);
    spin->setSuffix(QStringLiteral(" mm"));
    return spin;
}

QDoubleSpinBox *PalletParamDialog::createPoseSpin() const
{
    auto *spin = new NoWheelDoubleSpinBox();
    spin->setRange(-100000.0, 100000.0);
    spin->setDecimals(1);
    spin->setSingleStep(1.0);
    spin->setSuffix(QStringLiteral(" mm"));
    return spin;
}

QDoubleSpinBox *PalletParamDialog::createAngleSpin() const
{
    auto *spin = new NoWheelDoubleSpinBox();
    spin->setRange(-360.0, 360.0);
    spin->setDecimals(2);
    spin->setSingleStep(1.0);
    spin->setSuffix(QStringLiteral(" °"));
    return spin;
}

PalletConfig PalletParamDialog::readConfig(PalletArea area) const
{
    PageWidgets *w = widgets(area);
    PalletConfig cfg;
    if (!w)
        return cfg;
    cfg.palletSize = {w->palletX->value(), w->palletY->value(), w->palletZ->value()};
    cfg.boxSize = {w->boxX->value(), w->boxY->value(), w->boxZ->value()};
    cfg.gapX = w->gapX->value();
    cfg.gapY = w->gapY->value();
    cfg.marginX = w->marginX->value();
    cfg.marginY = w->marginY->value();
    cfg.maxLayers = w->maxLayers->text().trimmed().toInt();
    cfg.releaseZOffset = w->releaseZOffset->value();
    cfg.maxRobotZ = w->maxRobotZ->value();
    cfg.invertX = w->invertX->isChecked();
    cfg.invertY = w->invertY->isChecked();
    cfg.originPose = {w->originX->value(), w->originY->value(), w->originZ->value(),
                      w->originRx->value(), w->originRy->value(), w->originRz->value()};
    return cfg;
}

void PalletParamDialog::writeConfig(PalletArea area, const PalletConfig &cfg)
{
    PageWidgets *w = widgets(area);
    if (!w)
        return;
    w->palletX->setValue(cfg.palletSize.x);
    w->palletY->setValue(cfg.palletSize.y);
    w->palletZ->setValue(cfg.palletSize.z);
    w->boxX->setValue(cfg.boxSize.x);
    w->boxY->setValue(cfg.boxSize.y);
    w->boxZ->setValue(cfg.boxSize.z);
    w->gapX->setValue(cfg.gapX);
    w->gapY->setValue(cfg.gapY);
    w->marginX->setValue(cfg.marginX);
    w->marginY->setValue(cfg.marginY);
    w->maxLayers->setText(QString::number(cfg.maxLayers));
    w->releaseZOffset->setValue(cfg.releaseZOffset);
    w->maxRobotZ->setValue(cfg.maxRobotZ);
    w->invertX->setChecked(cfg.invertX);
    w->invertY->setChecked(cfg.invertY);
    w->originX->setValue(cfg.originPose.x);
    w->originY->setValue(cfg.originPose.y);
    w->originZ->setValue(cfg.originPose.z);
    w->originRx->setValue(cfg.originPose.rx);
    w->originRy->setValue(cfg.originPose.ry);
    w->originRz->setValue(cfg.originPose.rz);
}

void PalletParamDialog::refreshPage(PalletArea area)
{
    PageWidgets *w = widgets(area);
    if (!w || !m_scheduler)
        return;
    const int cols = m_scheduler->columns(area);
    const int rows = m_scheduler->rows(area);
    const int perLayer = m_scheduler->perLayerCapacity(area);
    const int total = m_scheduler->totalCapacity(area);
    const int placed = m_scheduler->placedCount(area);
    Q_UNUSED(total)
    w->placedCount->setText(QString::number(placed));
    w->capacityLabel->setText(QStringLiteral(
        "列=%1 行=%2 单层=%3 总容量=%4 当前已放=%5")
        .arg(cols).arg(rows).arg(perLayer).arg(total).arg(placed));
    w->emptyCountLabel->setText(QStringLiteral("连续空计数：%1/3")
        .arg(m_scheduler->emptyObserveCount(area)));
    const QDateTime resetAt = m_scheduler->lastAutoResetTime(area);
    if (resetAt.isValid()) {
        w->statusLabel->setToolTip(QStringLiteral("最近自动清零：%1")
            .arg(resetAt.toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"))));
    }
}

void PalletParamDialog::savePage(PalletArea area)
{
    if (!m_scheduler)
        return;
    m_scheduler->setConfig(area, readConfig(area));
    validatePage(area);
    refreshPage(area);
}

void PalletParamDialog::validatePage(PalletArea area)
{
    if (!m_scheduler)
        return;
    m_scheduler->setConfig(area, readConfig(area));
    QStringList errors;
    QStringList suggestions;
    const bool ok = m_scheduler->validateConfig(area, &errors, &suggestions);
    PageWidgets *w = widgets(area);
    if (!w)
        return;
    QStringList lines;
    lines << (ok ? QStringLiteral("配置校验通过") : QStringLiteral("配置校验失败"));
    for (const QString &e : errors)
        lines << QStringLiteral("错误：%1").arg(e);
    for (const QString &s : suggestions)
        lines << QStringLiteral("建议：%1").arg(s);
    w->validationText->setPlainText(lines.join('\n'));
    setStatus(w, ok ? QStringLiteral("配置正常") : QStringLiteral("配置存在错误"),
              ok ? QStringLiteral("ok") : QStringLiteral("error"));
    refreshPage(area);
}

void PalletParamDialog::fillFieldDefaults(PalletArea area)
{
    writeConfig(area, area == PalletArea::LargeBox
        ? PalletScheduler::defaultLargeBoxConfig()
        : PalletScheduler::defaultSmallBoxConfig());
    if (auto *w = widgets(area))
        setStatus(w, QStringLiteral("已填入默认现场值；仅写入输入框，确认后请保存配置"), QStringLiteral("warning"));
}

void PalletParamDialog::recommendReleaseHeight(PalletArea area)
{
    PageWidgets *w = widgets(area);
    if (!w)
        return;
    w->releaseZOffset->setValue(w->boxZ->value() * 1.2);
    setStatus(w, QStringLiteral("已按 1.2 倍箱高推荐释放高度记录值"), QStringLiteral("warning"));
}

void PalletParamDialog::showNextPose(PalletArea area)
{
    if (!m_scheduler)
        return;
    // 这里只做预览，不推进 placedCount；真实流程应在机械臂松爪完成后调用 commitPlaced()。
    savePage(area);
    PalletPose pose;
    PalletPose offset;
    QString error;
    PageWidgets *w = widgets(area);
    if (!w)
        return;
    if (!m_scheduler->nextPose(area, &pose, &error)
        || !m_scheduler->nextRelativeOffset(area, &offset, &error)) {
        w->nextPoseLabel->setText(QStringLiteral("下一点计算失败：%1").arg(error));
        setStatus(w, error, QStringLiteral("error"));
        return;
    }
    const int perLayer = m_scheduler->perLayerCapacity(area);
    const int idx = m_scheduler->placedCount(area);
    const int layer = perLayer > 0 ? idx / perLayer + 1 : 0;
    const int row = (perLayer > 0 && m_scheduler->columns(area) > 0)
        ? (idx % perLayer) / m_scheduler->columns(area) + 1 : 0;
    const int col = m_scheduler->columns(area) > 0 ? (idx % m_scheduler->columns(area)) + 1 : 0;
    w->nextPoseLabel->setText(QStringLiteral(
        "下一箱：第%1层 第%2行 第%3列\n"
        "给机械臂的相对偏移: X=%4 Y=%5 Z=%6\n"
        "绝对预览: X=%7 Y=%8 Z=%9 Rx=%10 Ry=%11 Rz=%12")
        .arg(layer).arg(row).arg(col)
        .arg(fmt(offset.x)).arg(fmt(offset.y)).arg(fmt(offset.z))
        .arg(fmt(pose.x)).arg(fmt(pose.y)).arg(fmt(pose.z))
        .arg(fmt(pose.rx)).arg(fmt(pose.ry)).arg(fmt(pose.rz)));
    setStatus(w, QStringLiteral("下一点已计算；主输出是相对初始点位的偏移"), QStringLiteral("ok"));
}

void PalletParamDialog::simulatePlaced(PalletArea area)
{
    if (!m_scheduler)
        return;

    // 仿真模式下用按钮代替“机械臂已运动到点位并松爪”的完成信号。
    // 真实接入时不要在计算点位后立即提交，应由机械臂完成回调触发 commitPlaced()。
    savePage(area);
    PageWidgets *w = widgets(area);
    if (!w)
        return;

    QString error;
    if (!m_scheduler->commitPlaced(area, &error)) {
        setStatus(w, error, QStringLiteral("error"));
        return;
    }

    refreshPage(area);
    showNextPose(area);
    setStatus(w, QStringLiteral("已模拟完成一次放置，缓存数量已推进"), QStringLiteral("ok"));
}

void PalletParamDialog::simulateArea(PalletArea area)
{
    if (!m_scheduler)
        return;
    savePage(area);
    QString error;
    const QList<PalletSimulationItem> items = m_scheduler->simulate(area, 0, &error);
    PageWidgets *w = widgets(area);
    if (!w)
        return;
    w->simulationTable->setRowCount(0);
    if (items.isEmpty() && !error.isEmpty()) {
        setStatus(w, error, QStringLiteral("error"));
        return;
    }
    for (const PalletSimulationItem &item : items) {
        const int row = w->simulationTable->rowCount();
        w->simulationTable->insertRow(row);
        const QStringList vals = {
            QString::number(item.index), QString::number(item.layer),
            QString::number(item.row), QString::number(item.column),
            fmt(item.offset.x), fmt(item.offset.y), fmt(item.offset.z),
            fmt(item.pose.x), fmt(item.pose.y), fmt(item.pose.z),
            fmt(item.pose.rx), fmt(item.pose.ry), fmt(item.pose.rz)};
        for (int c = 0; c < vals.size(); ++c)
            w->simulationTable->setItem(row, c, new QTableWidgetItem(vals[c]));
    }
    setStatus(w, QStringLiteral("仿真完成，共 %1 个点位").arg(items.size()), QStringLiteral("ok"));
}


void PalletParamDialog::clearDisplayResults(PalletArea area)
{
    PageWidgets *w = widgets(area);
    if (!w)
        return;

    w->nextPoseLabel->clear();
    w->validationText->clear();
    w->simulationTable->setRowCount(0);
    setStatus(w, QStringLiteral("已清除右侧显示结果，配置和已放数量未修改"), QStringLiteral("warning"));
}

void PalletParamDialog::resetArea(PalletArea area)
{
    if (!m_scheduler)
        return;
    if (QMessageBox::question(this, QStringLiteral("确认清零"),
        QStringLiteral("确认清零%1的已放数量？").arg(PalletScheduler::areaName(area)))
        != QMessageBox::Yes) {
        return;
    }
    m_scheduler->reset(area);
    refreshPage(area);
}

void PalletParamDialog::applyPlacedCount(PalletArea area)
{
    PageWidgets *w = widgets(area);
    if (!w || !m_scheduler)
        return;
    if (QMessageBox::question(this, QStringLiteral("确认修正"),
        QStringLiteral("确认把%1已放数量修正为 %2？")
            .arg(PalletScheduler::areaName(area))
            .arg(w->placedCount->text().trimmed()))
        != QMessageBox::Yes) {
        return;
    }
    QString error;
    bool ok = false;
    const int correctedCount = w->placedCount->text().trimmed().toInt(&ok);
    if (!ok) {
        setStatus(w, QStringLiteral("已放数量必须是整数"), QStringLiteral("error"));
        return;
    }
    if (!m_scheduler->setPlacedCount(area, correctedCount, &error)) {
        setStatus(w, error, QStringLiteral("error"));
        return;
    }
    refreshPage(area);
}

void PalletParamDialog::detectArea(PalletArea area)
{
    PageWidgets *w = widgets(area);
    if (!w || !m_visionClient)
        return;
    const QString requestId = QStringLiteral("%1-%2")
        .arg(areaKey(area)).arg(QDateTime::currentMSecsSinceEpoch());
    m_pendingRequests.insert(requestId, area);
    w->detectBtn->setEnabled(false);
    w->detectLabel->setText(QStringLiteral("检测中..."));
    m_visionClient->fetchPalletOccupancy(requestId);
}

void PalletParamDialog::setStatus(PageWidgets *w, const QString &text, const QString &state)
{
    if (!w)
        return;
    w->statusLabel->setText(text);
    QString color = QStringLiteral("#b8b8b8");
    if (state == QStringLiteral("ok")) color = QStringLiteral("#2da44e");
    else if (state == QStringLiteral("warning")) color = QStringLiteral("#b7791f");
    else if (state == QStringLiteral("error")) color = QStringLiteral("#cf222e");
    w->statusLabel->setStyleSheet(QStringLiteral("color:%1; font-weight:bold;").arg(color));
}

int PalletParamDialog::areaKey(PalletArea area)
{
    return area == PalletArea::LargeBox ? 0 : 1;
}
