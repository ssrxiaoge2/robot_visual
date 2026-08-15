#include "settingsdialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {

QFormLayout *pageForm(QWidget *page)
{
    return page->findChild<QFormLayout *>(QStringLiteral("settingsForm"));
}

bool changed(double a, double b)
{
    return !qFuzzyCompare(a + 1.0, b + 1.0);
}

} // namespace

SettingsDialog::SettingsDialog(const RuntimeSettings &current,
                               bool runtimeLocked,
                               QWidget *parent)
    : QDialog(parent)
    , m_original(current)
    , m_runtimeLocked(runtimeLocked)
{
    qRegisterMetaType<RuntimeSettings>("RuntimeSettings");
    setWindowTitle(QStringLiteral("运行参数设置"));
    resize(900, 680);
    setMinimumSize(760, 560);

    auto *root = new QVBoxLayout(this);
    auto *lockedBanner = new QLabel(
        QStringLiteral("当前任务正在运行：参数仅供查看，请停止任务后再修改。"), this);
    lockedBanner->setObjectName(QStringLiteral("runtimeLockedBanner"));
    lockedBanner->setWordWrap(true);
    lockedBanner->setStyleSheet(QStringLiteral(
        "QLabel { color:#8a5200; background:#fff3cd; border:1px solid #e4b85a;"
        " padding:8px; border-radius:4px; }"));
    lockedBanner->setVisible(runtimeLocked);
    root->addWidget(lockedBanner);

    auto *content = new QHBoxLayout();
    m_categories = new QListWidget(this);
    m_categories->setObjectName(QStringLiteral("settingsCategoryList"));
    m_categories->addItems({
        QStringLiteral("抓取与箱型"),
        QStringLiteral("视觉闭环"),
        QStringLiteral("深度自动下探"),
        QStringLiteral("未识别目标搜索"),
        QStringLiteral("运动控制"),
        QStringLiteral("安全限制与超时")
    });
    m_categories->setFixedWidth(175);
    content->addWidget(m_categories);

    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(QStringLiteral("settingsStack"));
    const SettingsCategory categories[] = {
        SettingsCategory::Pickup,
        SettingsCategory::VisionClosedLoop,
        SettingsCategory::DepthDescent,
        SettingsCategory::Search,
        SettingsCategory::Motion,
        SettingsCategory::SafetyAndTimeouts
    };
    for (SettingsCategory category : categories) {
        auto *scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(createCategoryPage(category));
        m_stack->addWidget(scroll);
    }
    m_stack->setEnabled(!runtimeLocked);
    content->addWidget(m_stack, 1);
    root->addLayout(content, 1);

    m_preview = new QLabel(this);
    m_preview->setObjectName(QStringLiteral("settingsChangePreview"));
    m_preview->setWordWrap(true);
    m_preview->setText(QStringLiteral("尚无变更"));
    m_preview->setStyleSheet(QStringLiteral(
        "QLabel { background:#f4f6f8; border:1px solid #ccd2d8; padding:8px; }"));
    root->addWidget(m_preview);

    m_safetyAcknowledgement =
        new QCheckBox(QStringLiteral("我已确认安全限制或超时变更的风险"), this);
    m_safetyAcknowledgement->setObjectName(
        QStringLiteral("safetyAcknowledgementCheckBox"));
    m_safetyAcknowledgement->setStyleSheet(QStringLiteral("QCheckBox { color:#a33; }"));
    root->addWidget(m_safetyAcknowledgement);

    auto *buttons = new QHBoxLayout();
    auto *restore = new QPushButton(QStringLiteral("恢复本类默认值"), this);
    restore->setObjectName(QStringLiteral("restoreCategoryDefaultsButton"));
    restore->setEnabled(!runtimeLocked);
    buttons->addWidget(restore);
    buttons->addStretch();
    auto *cancel = new QPushButton(QStringLiteral("取消"), this);
    auto *save = new QPushButton(QStringLiteral("保存并应用"), this);
    save->setObjectName(QStringLiteral("saveAndApplyButton"));
    save->setEnabled(!runtimeLocked);
    save->setDefault(true);
    buttons->addWidget(cancel);
    buttons->addWidget(save);
    root->addLayout(buttons);

    connect(m_categories, &QListWidget::currentRowChanged,
            m_stack, &QStackedWidget::setCurrentIndex);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(restore, &QPushButton::clicked, this, [this] {
        writeSettings(restoreCategoryDefaults(candidate(), currentCategory()));
        m_safetyAcknowledgement->setChecked(false);
    });
    connect(save, &QPushButton::clicked, this, [this] {
        const RuntimeSettings value = candidate();
        const SettingsValidation validation = validateRuntimeSettings(value);
        if (!validation.ok) {
            m_preview->setText(QStringLiteral("无法保存：%1")
                                   .arg(validation.errors.join(QStringLiteral("；"))));
            return;
        }
        const QStringList changes = changedValues(value);
        m_preview->setText(changes.isEmpty()
            ? QStringLiteral("无参数变更")
            : QStringLiteral("变更预览：\n%1").arg(changes.join(QLatin1Char('\n'))));
        if (safetyChanged(value) && !m_safetyAcknowledgement->isChecked()) {
            m_preview->setText(m_preview->text()
                + QStringLiteral("\n请勾选安全风险确认后再保存。"));
            return;
        }
        emit saveRequested(value);
    });

    writeSettings(current);
    m_categories->setCurrentRow(0);
}

QWidget *SettingsDialog::createCategoryPage(SettingsCategory category)
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    auto *form = new QFormLayout();
    form->setObjectName(QStringLiteral("settingsForm"));
    form->setLabelAlignment(Qt::AlignRight);
    layout->addLayout(form);
    layout->addStretch();

    switch (category) {
    case SettingsCategory::Pickup:
        addDouble(page, "largeBasketClearance", QStringLiteral("大篮筐抓取 Z 余量"),
                  -1000, 1000, "mm");
        addDouble(page, "purpleBasketClearance", QStringLiteral("紫筐抓取 Z 余量"),
                  -1000, 1000, "mm");
        addDouble(page, "grabXCompensation", QStringLiteral("抓取 X 补偿"),
                  -500, 500, "mm");
        addDouble(page, "grabYCompensation", QStringLiteral("抓取 Y 补偿"),
                  -500, 500, "mm");
        addBool(page, "zDescendInvert", QStringLiteral("反转 Z 下探方向"));
        break;
    case SettingsCategory::VisionClosedLoop:
        addDouble(page, "visionXyTolerance", QStringLiteral("XY 收敛阈值"), 0.1, 100, "mm");
        addDouble(page, "visionRzTolerance", QStringLiteral("Rz 收敛阈值"), 0.1, 90, "°");
        addInt(page, "visionMaxFineCorrections", QStringLiteral("联合精修正次数"),
               0, 2, "次");
        addInt(page, "visionSettleMs", QStringLiteral("视觉稳定等待"), 0, 60000, "ms");
        addDouble(page, "largeRzThreshold", QStringLiteral("Rz 大角度阈值"), 0, 180, "°");
        addDouble(page, "largeRzTolerance", QStringLiteral("连续帧 Rz 容差"), 0, 180, "°");
        addInt(page, "maxLargeRz", QStringLiteral("大角度 Rz 最大执行次数"), 0, 20, "次");
        addDouble(page, "roiHalfX", QStringLiteral("工位 ROI 半宽 X"), 1, 5000, "mm");
        addDouble(page, "roiHalfY", QStringLiteral("工位 ROI 半宽 Y"), 1, 5000, "mm");
        addDouble(page, "anchorTrustX", QStringLiteral("锚点可信边界 X"), 1, 5000, "mm");
        addDouble(page, "anchorTrustY", QStringLiteral("锚点可信边界 Y"), 1, 5000, "mm");
        addDouble(page, "anchorLayerTolerance", QStringLiteral("锚点同层容差"), 0, 1000, "mm");
        addDouble(page, "anchorSwitchXy", QStringLiteral("锚点切换最大距离"), 0, 5000, "mm");
        addInt(page, "lockMissingFrames", QStringLiteral("锁定最大丢帧"), 0, 100, "帧");
        addDouble(page, "lockRadius", QStringLiteral("锁定跟踪半径"), 1, 5000, "mm");
        addDouble(page, "lockLayerTolerance", QStringLiteral("锁定同层容差"), 0, 1000, "mm");
        break;
    case SettingsCategory::DepthDescent:
        addBool(page, "depthEnabled", QStringLiteral("启用深度自动下探"));
        addDouble(page, "depthTrigger", QStringLiteral("视觉深度触发阈值"), 1, 5000, "mm");
        addDouble(page, "depthStep", QStringLiteral("单次下探距离"), 1, 2000, "mm");
        addDouble(page, "depthMax", QStringLiteral("最大累计下探"), 1, 5000, "mm");
        break;
    case SettingsCategory::Search:
        addDouble(page, "searchStep", QStringLiteral("单次搜索下移"), 1, 1000, "mm");
        addDouble(page, "searchMax", QStringLiteral("最大累计搜索下移"), 1, 5000, "mm");
        addInt(page, "searchSettleMs", QStringLiteral("搜索后视觉等待"), 0, 60000, "ms");
        break;
    case SettingsCategory::Motion:
        addInt(page, "speedPercent", QStringLiteral("机械臂速度倍率"), 1, 100, "%");
        addDouble(page, "velocity", QStringLiteral("相对移动速度"), 0.1, 5000, "mm/s");
        addDouble(page, "acceleration", QStringLiteral("相对移动加速度"), 0.1, 10000, "mm/s²");
        addDouble(page, "radius", QStringLiteral("运动过渡半径"), 0, 1000, "mm");
        addDouble(page, "scanRotation", QStringLiteral("扫码补救旋转角度"), 0, 360, "°");
        break;
    case SettingsCategory::SafetyAndTimeouts:
        addDouble(page, "safetyMaxXy", QStringLiteral("单次 XY 最大调整"), 1, 5000, "mm", true);
        addDouble(page, "safetyMaxZ", QStringLiteral("Z 最大下探"), 1, 5000, "mm", true);
        addInt(page, "normalTimeout", QStringLiteral("普通运动超时"), 1, 600000, "ms", true);
        addInt(page, "longZTimeout", QStringLiteral("长 Z 运动超时"), 1, 600000, "ms", true);
        addInt(page, "commandReadyTimeout", QStringLiteral("命令就绪超时"), 1, 600000, "ms", true);
        addInt(page, "resetSettle", QStringLiteral("复位稳定等待"), 0, 60000, "ms", true);
        addInt(page, "pollInterval", QStringLiteral("状态轮询间隔"), 1, 10000, "ms", true);
        addInt(page, "shortFallback", QStringLiteral("短动作完成兜底"), 1, 60000, "ms", true);
        break;
    }
    return page;
}

QDoubleSpinBox *SettingsDialog::addDouble(QWidget *page, const QString &key,
                                          const QString &label, double min,
                                          double max, const QString &unit,
                                          bool safety)
{
    auto *spin = new QDoubleSpinBox(page);
    spin->setObjectName(key + QStringLiteral("Spin"));
    spin->setRange(min, max);
    spin->setDecimals(1);
    spin->setSingleStep(1.0);
    spin->setSuffix(QStringLiteral(" ") + unit);
    if (safety)
        spin->setStyleSheet(QStringLiteral("QDoubleSpinBox { color:#a33; }"));
    pageForm(page)->addRow(label + QStringLiteral("："), spin);
    m_doubles.insert(key, spin);
    return spin;
}

QSpinBox *SettingsDialog::addInt(QWidget *page, const QString &key,
                                 const QString &label, int min, int max,
                                 const QString &unit, bool safety)
{
    auto *spin = new QSpinBox(page);
    spin->setObjectName(key + QStringLiteral("Spin"));
    spin->setRange(min, max);
    spin->setSuffix(QStringLiteral(" ") + unit);
    if (safety)
        spin->setStyleSheet(QStringLiteral("QSpinBox { color:#a33; }"));
    pageForm(page)->addRow(label + QStringLiteral("："), spin);
    m_ints.insert(key, spin);
    return spin;
}

QCheckBox *SettingsDialog::addBool(QWidget *page, const QString &key,
                                   const QString &label)
{
    auto *check = new QCheckBox(label, page);
    check->setObjectName(key + QStringLiteral("CheckBox"));
    pageForm(page)->addRow(check);
    m_bools.insert(key, check);
    return check;
}

RuntimeSettings SettingsDialog::candidate() const
{
    RuntimeSettings s;
    s.pickup.largeBasketGrabZClearanceMm = m_doubles["largeBasketClearance"]->value();
    s.pickup.purpleBasketGrabZClearanceMm = m_doubles["purpleBasketClearance"]->value();
    s.pickup.grabXCompensationMm = m_doubles["grabXCompensation"]->value();
    s.pickup.grabYCompensationMm = m_doubles["grabYCompensation"]->value();
    s.pickup.zDescendInvert = m_bools["zDescendInvert"]->isChecked();
    s.vision.xyToleranceMm = m_doubles["visionXyTolerance"]->value();
    s.vision.rzToleranceDeg = m_doubles["visionRzTolerance"]->value();
    s.vision.maxFineCorrectionCount =
        m_ints["visionMaxFineCorrections"]->value();
    s.vision.settleMs = m_ints["visionSettleMs"]->value();
    s.vision.largeRzJumpThresholdDeg = m_doubles["largeRzThreshold"]->value();
    s.vision.largeRzDeltaToleranceDeg = m_doubles["largeRzTolerance"]->value();
    s.vision.maxLargeRzExecutions = m_ints["maxLargeRz"]->value();
    s.vision.stationRoiHalfXmm = m_doubles["roiHalfX"]->value();
    s.vision.stationRoiHalfYmm = m_doubles["roiHalfY"]->value();
    s.vision.anchorMaxTrustXmm = m_doubles["anchorTrustX"]->value();
    s.vision.anchorMaxTrustYmm = m_doubles["anchorTrustY"]->value();
    s.vision.anchorSameLayerToleranceMm = m_doubles["anchorLayerTolerance"]->value();
    s.vision.anchorSwitchMaxXyMm = m_doubles["anchorSwitchXy"]->value();
    s.vision.lockMaxMissingFrames = m_ints["lockMissingFrames"]->value();
    s.vision.lockTrackRadiusMm = m_doubles["lockRadius"]->value();
    s.vision.lockSameLayerToleranceMm = m_doubles["lockLayerTolerance"]->value();
    s.depthDescent.enabled = m_bools["depthEnabled"]->isChecked();
    s.depthDescent.triggerDepthMm = m_doubles["depthTrigger"]->value();
    s.depthDescent.stepMm = m_doubles["depthStep"]->value();
    s.depthDescent.maxAccumulatedMm = m_doubles["depthMax"]->value();
    s.search.descendStepMm = m_doubles["searchStep"]->value();
    s.search.maxAccumulatedMm = m_doubles["searchMax"]->value();
    s.search.settleMs = m_ints["searchSettleMs"]->value();
    s.motion.speedPercent = m_ints["speedPercent"]->value();
    s.motion.velocity = m_doubles["velocity"]->value();
    s.motion.acceleration = m_doubles["acceleration"]->value();
    s.motion.radius = m_doubles["radius"]->value();
    s.motion.scanRecoveryRotationDeg = m_doubles["scanRotation"]->value();
    s.safety.maxSingleXyAdjustMm = m_doubles["safetyMaxXy"]->value();
    s.safety.maxZDescendMm = m_doubles["safetyMaxZ"]->value();
    s.safety.normalMotionTimeoutMs = m_ints["normalTimeout"]->value();
    s.safety.longZMotionTimeoutMs = m_ints["longZTimeout"]->value();
    s.safety.commandReadyTimeoutMs = m_ints["commandReadyTimeout"]->value();
    s.safety.resetSettleMs = m_ints["resetSettle"]->value();
    s.safety.pollIntervalMs = m_ints["pollInterval"]->value();
    s.safety.shortMotionFallbackMs = m_ints["shortFallback"]->value();
    return s;
}

void SettingsDialog::writeSettings(const RuntimeSettings &s)
{
    m_doubles["largeBasketClearance"]->setValue(s.pickup.largeBasketGrabZClearanceMm);
    m_doubles["purpleBasketClearance"]->setValue(s.pickup.purpleBasketGrabZClearanceMm);
    m_doubles["grabXCompensation"]->setValue(s.pickup.grabXCompensationMm);
    m_doubles["grabYCompensation"]->setValue(s.pickup.grabYCompensationMm);
    m_bools["zDescendInvert"]->setChecked(s.pickup.zDescendInvert);
    m_doubles["visionXyTolerance"]->setValue(s.vision.xyToleranceMm);
    m_doubles["visionRzTolerance"]->setValue(s.vision.rzToleranceDeg);
    m_ints["visionMaxFineCorrections"]->setValue(
        s.vision.maxFineCorrectionCount);
    m_ints["visionSettleMs"]->setValue(s.vision.settleMs);
    m_doubles["largeRzThreshold"]->setValue(s.vision.largeRzJumpThresholdDeg);
    m_doubles["largeRzTolerance"]->setValue(s.vision.largeRzDeltaToleranceDeg);
    m_ints["maxLargeRz"]->setValue(s.vision.maxLargeRzExecutions);
    m_doubles["roiHalfX"]->setValue(s.vision.stationRoiHalfXmm);
    m_doubles["roiHalfY"]->setValue(s.vision.stationRoiHalfYmm);
    m_doubles["anchorTrustX"]->setValue(s.vision.anchorMaxTrustXmm);
    m_doubles["anchorTrustY"]->setValue(s.vision.anchorMaxTrustYmm);
    m_doubles["anchorLayerTolerance"]->setValue(s.vision.anchorSameLayerToleranceMm);
    m_doubles["anchorSwitchXy"]->setValue(s.vision.anchorSwitchMaxXyMm);
    m_ints["lockMissingFrames"]->setValue(s.vision.lockMaxMissingFrames);
    m_doubles["lockRadius"]->setValue(s.vision.lockTrackRadiusMm);
    m_doubles["lockLayerTolerance"]->setValue(s.vision.lockSameLayerToleranceMm);
    m_bools["depthEnabled"]->setChecked(s.depthDescent.enabled);
    m_doubles["depthTrigger"]->setValue(s.depthDescent.triggerDepthMm);
    m_doubles["depthStep"]->setValue(s.depthDescent.stepMm);
    m_doubles["depthMax"]->setValue(s.depthDescent.maxAccumulatedMm);
    m_doubles["searchStep"]->setValue(s.search.descendStepMm);
    m_doubles["searchMax"]->setValue(s.search.maxAccumulatedMm);
    m_ints["searchSettleMs"]->setValue(s.search.settleMs);
    m_ints["speedPercent"]->setValue(s.motion.speedPercent);
    m_doubles["velocity"]->setValue(s.motion.velocity);
    m_doubles["acceleration"]->setValue(s.motion.acceleration);
    m_doubles["radius"]->setValue(s.motion.radius);
    m_doubles["scanRotation"]->setValue(s.motion.scanRecoveryRotationDeg);
    m_doubles["safetyMaxXy"]->setValue(s.safety.maxSingleXyAdjustMm);
    m_doubles["safetyMaxZ"]->setValue(s.safety.maxZDescendMm);
    m_ints["normalTimeout"]->setValue(s.safety.normalMotionTimeoutMs);
    m_ints["longZTimeout"]->setValue(s.safety.longZMotionTimeoutMs);
    m_ints["commandReadyTimeout"]->setValue(s.safety.commandReadyTimeoutMs);
    m_ints["resetSettle"]->setValue(s.safety.resetSettleMs);
    m_ints["pollInterval"]->setValue(s.safety.pollIntervalMs);
    m_ints["shortFallback"]->setValue(s.safety.shortMotionFallbackMs);
}

QStringList SettingsDialog::changedValues(const RuntimeSettings &s) const
{
    QStringList result;
    auto addDouble = [&result](const QString &name, double oldValue, double newValue,
                               const QString &unit) {
        if (changed(oldValue, newValue)) {
            result << QStringLiteral("%1：%2 → %3 %4")
                          .arg(name)
                          .arg(oldValue, 0, 'f', 1)
                          .arg(newValue, 0, 'f', 1)
                          .arg(unit);
        }
    };
    auto addInt = [&result](const QString &name, int oldValue, int newValue,
                            const QString &unit) {
        if (oldValue != newValue)
            result << QStringLiteral("%1：%2 → %3 %4")
                          .arg(name).arg(oldValue).arg(newValue).arg(unit);
    };
    auto addBool = [&result](const QString &name, bool oldValue, bool newValue) {
        if (oldValue != newValue)
            result << QStringLiteral("%1：%2 → %3")
                          .arg(name,
                               oldValue ? QStringLiteral("启用") : QStringLiteral("禁用"),
                               newValue ? QStringLiteral("启用") : QStringLiteral("禁用"));
    };

    addDouble(QStringLiteral("大篮筐抓取 Z 余量"),
              m_original.pickup.largeBasketGrabZClearanceMm,
              s.pickup.largeBasketGrabZClearanceMm, "mm");
    addDouble(QStringLiteral("紫筐抓取 Z 余量"),
              m_original.pickup.purpleBasketGrabZClearanceMm,
              s.pickup.purpleBasketGrabZClearanceMm, "mm");
    addDouble(QStringLiteral("抓取 X 补偿"), m_original.pickup.grabXCompensationMm,
              s.pickup.grabXCompensationMm, "mm");
    addDouble(QStringLiteral("抓取 Y 补偿"), m_original.pickup.grabYCompensationMm,
              s.pickup.grabYCompensationMm, "mm");
    addBool(QStringLiteral("Z 下探方向反转"), m_original.pickup.zDescendInvert,
            s.pickup.zDescendInvert);
    addDouble(QStringLiteral("XY 收敛阈值"), m_original.vision.xyToleranceMm,
              s.vision.xyToleranceMm, "mm");
    addDouble(QStringLiteral("Rz 收敛阈值"), m_original.vision.rzToleranceDeg,
              s.vision.rzToleranceDeg, "°");
    addInt(QStringLiteral("联合精修正次数"),
           m_original.vision.maxFineCorrectionCount,
           s.vision.maxFineCorrectionCount, "次");
    addInt(QStringLiteral("视觉稳定等待"), m_original.vision.settleMs,
           s.vision.settleMs, "ms");
    addBool(QStringLiteral("深度自动下探"), m_original.depthDescent.enabled,
            s.depthDescent.enabled);
    addDouble(QStringLiteral("深度触发阈值"), m_original.depthDescent.triggerDepthMm,
              s.depthDescent.triggerDepthMm, "mm");
    addDouble(QStringLiteral("单次深度下探"), m_original.depthDescent.stepMm,
              s.depthDescent.stepMm, "mm");
    addDouble(QStringLiteral("最大累计深度下探"),
              m_original.depthDescent.maxAccumulatedMm,
              s.depthDescent.maxAccumulatedMm, "mm");
    addDouble(QStringLiteral("搜索下移步长"), m_original.search.descendStepMm,
              s.search.descendStepMm, "mm");
    addDouble(QStringLiteral("搜索累计上限"), m_original.search.maxAccumulatedMm,
              s.search.maxAccumulatedMm, "mm");
    addInt(QStringLiteral("速度倍率"), m_original.motion.speedPercent,
           s.motion.speedPercent, "%");
    addDouble(QStringLiteral("相对移动速度"), m_original.motion.velocity,
              s.motion.velocity, "mm/s");
    addDouble(QStringLiteral("相对移动加速度"), m_original.motion.acceleration,
              s.motion.acceleration, "mm/s²");
    addDouble(QStringLiteral("单次 XY 安全上限"),
              m_original.safety.maxSingleXyAdjustMm,
              s.safety.maxSingleXyAdjustMm, "mm");
    addDouble(QStringLiteral("Z 安全上限"), m_original.safety.maxZDescendMm,
              s.safety.maxZDescendMm, "mm");
    addInt(QStringLiteral("普通运动超时"), m_original.safety.normalMotionTimeoutMs,
           s.safety.normalMotionTimeoutMs, "ms");
    addInt(QStringLiteral("长 Z 运动超时"), m_original.safety.longZMotionTimeoutMs,
           s.safety.longZMotionTimeoutMs, "ms");
    return result;
}

bool SettingsDialog::safetyChanged(const RuntimeSettings &s) const
{
    return changed(m_original.safety.maxSingleXyAdjustMm, s.safety.maxSingleXyAdjustMm)
        || changed(m_original.safety.maxZDescendMm, s.safety.maxZDescendMm)
        || m_original.safety.normalMotionTimeoutMs != s.safety.normalMotionTimeoutMs
        || m_original.safety.longZMotionTimeoutMs != s.safety.longZMotionTimeoutMs
        || m_original.safety.commandReadyTimeoutMs != s.safety.commandReadyTimeoutMs
        || m_original.safety.resetSettleMs != s.safety.resetSettleMs
        || m_original.safety.pollIntervalMs != s.safety.pollIntervalMs
        || m_original.safety.shortMotionFallbackMs != s.safety.shortMotionFallbackMs;
}

SettingsCategory SettingsDialog::currentCategory() const
{
    static const SettingsCategory categories[] = {
        SettingsCategory::Pickup,
        SettingsCategory::VisionClosedLoop,
        SettingsCategory::DepthDescent,
        SettingsCategory::Search,
        SettingsCategory::Motion,
        SettingsCategory::SafetyAndTimeouts
    };
    return categories[qBound(0, m_categories->currentRow(), 5)];
}
