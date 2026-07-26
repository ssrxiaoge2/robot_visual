#include "chargepilesettingsdialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {

QSpinBox *makeIntegerSpin(const char *objectName, int minimum, int maximum,
                          const QString &suffix, QWidget *parent)
{
    auto *spin = new QSpinBox(parent);
    spin->setObjectName(QString::fromLatin1(objectName));
    spin->setRange(minimum, maximum);
    spin->setSuffix(suffix);
    spin->setAlignment(Qt::AlignRight);
    return spin;
}

QDoubleSpinBox *makeDecimalSpin(const char *objectName, double minimum,
                                double maximum, int decimals,
                                const QString &suffix, QWidget *parent)
{
    auto *spin = new QDoubleSpinBox(parent);
    spin->setObjectName(QString::fromLatin1(objectName));
    spin->setRange(minimum, maximum);
    spin->setDecimals(decimals);
    spin->setSingleStep(decimals == 1 ? 0.1 : 1.0);
    spin->setSuffix(suffix);
    spin->setAlignment(Qt::AlignRight);
    return spin;
}

} // namespace

ChargePileSettingsDialog::ChargePileSettingsDialog(const ChargeSettings &settings,
                                                   const bool locked,
                                                   QWidget *parent)
    : QDialog(parent)
    , m_locked(locked)
{
    qRegisterMetaType<ChargeSettings>("ChargeSettings");
    setWindowTitle(QStringLiteral("充电桩参数设置"));
    setMinimumWidth(520);

    auto *root = new QVBoxLayout(this);
    root->setSpacing(8);

    auto *lockedBanner = new QLabel(
        QStringLiteral("充电查询、会话或安全收尾正在执行：当前参数仅供查看。"),
        this);
    lockedBanner->setObjectName(QStringLiteral("chargeSettingsLockedBanner"));
    lockedBanner->setWordWrap(true);
    lockedBanner->setVisible(locked);
    lockedBanner->setStyleSheet(QStringLiteral(
        "QLabel { color:#8a5200; background:#fff3cd; border:1px solid #e4b85a;"
        " padding:6px; }"));
    root->addWidget(lockedBanner);

    root->addWidget(createCommunicationGroup());
    root->addWidget(createChargeGroup());
    root->addWidget(createTimeoutGroup());

    auto *buttonRow = new QHBoxLayout();
    m_restoreDefaultsButton = new QPushButton(QStringLiteral("恢复充电默认值"), this);
    m_restoreDefaultsButton->setObjectName(
        QStringLiteral("restoreChargeDefaultsButton"));
    m_restoreDefaultsButton->setEnabled(!locked);
    buttonRow->addWidget(m_restoreDefaultsButton);
    buttonRow->addStretch();

    m_buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    m_buttonBox->setObjectName(QStringLiteral("chargeSettingsButtonBox"));
    m_buttonBox->button(QDialogButtonBox::Save)->setText(
        QStringLiteral("保存并应用"));
    m_buttonBox->button(QDialogButtonBox::Cancel)->setText(
        QStringLiteral("取消"));
    m_buttonBox->button(QDialogButtonBox::Save)->setEnabled(!locked);
    buttonRow->addWidget(m_buttonBox);
    root->addLayout(buttonRow);

    connect(m_cutoffCurrentCheck, &QCheckBox::toggled,
            this, &ChargePileSettingsDialog::updateOptionalControls);
    connect(m_maxChargeSecondsCheck, &QCheckBox::toggled,
            this, &ChargePileSettingsDialog::updateOptionalControls);
    connect(m_restoreDefaultsButton, &QPushButton::clicked, this, [this] {
        // 自动授权不在 ChargeSettings 中；这里只重写对话框字段，不发保存信号，
        // 因而不会改变主面板开关，也不会在用户确认前触碰持久化文件。
        writeSettings(ChargeSettings::defaults());
    });
    connect(m_buttonBox, &QDialogButtonBox::rejected,
            this, &QDialog::reject);
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, [this] {
        if (m_locked)
            return;
        const ChargeSettings value = candidate();
        const ChargeSettingsValidation validation =
            validateChargeSettings(value);
        if (!validation.ok) {
            QMessageBox::warning(
                this,
                QStringLiteral("充电参数不合法"),
                validation.errors.join(QStringLiteral("\n")));
            return;
        }
        // DeviceManager 的完整候选事务决定是否真正保存并应用；对话框不能先
        // accept，否则持久化失败时用户看不到原值和错误原因。
        emit saveRequested(value);
    });

    writeSettings(settings);

    // 锁定时将三个分组整体设为只读，仍保留取消按钮供用户关闭窗口。
    for (QWidget *group : {
             findChild<QWidget *>(QStringLiteral("chargeCommunicationGroup")),
             findChild<QWidget *>(QStringLiteral("chargeElectricalGroup")),
             findChild<QWidget *>(QStringLiteral("chargeTimeoutGroup"))}) {
        if (group)
            group->setEnabled(!locked);
    }
}

QWidget *ChargePileSettingsDialog::createCommunicationGroup()
{
    auto *group = new QGroupBox(QStringLiteral("通信设置"), this);
    group->setObjectName(QStringLiteral("chargeCommunicationGroup"));
    auto *form = new QFormLayout(group);

    m_hostEdit = new QLineEdit(group);
    m_hostEdit->setObjectName(QStringLiteral("chargeHostEdit"));
    m_portSpin = makeIntegerSpin("chargePortSpin", 1, 65535, {}, group);
    m_slaveIdSpin = makeIntegerSpin("chargeSlaveIdSpin", 1, 255, {}, group);

    form->addRow(QStringLiteral("设备地址："), m_hostEdit);
    form->addRow(QStringLiteral("TCP 端口："), m_portSpin);
    form->addRow(QStringLiteral("充电站号："), m_slaveIdSpin);
    return group;
}

QWidget *ChargePileSettingsDialog::createChargeGroup()
{
    auto *group = new QGroupBox(QStringLiteral("充电设置"), this);
    group->setObjectName(QStringLiteral("chargeElectricalGroup"));
    auto *form = new QFormLayout(group);

    m_voltageSpin =
        makeDecimalSpin("chargeVoltageSpin", 0.1, 100.0, 1,
                        QStringLiteral(" V"), group);
    m_currentSpin =
        makeDecimalSpin("chargeCurrentSpin", 0.1, 120.0, 1,
                        QStringLiteral(" A"), group);

    m_cutoffCurrentCheck =
        new QCheckBox(QStringLiteral("启用截止电流寄存器"), group);
    m_cutoffCurrentCheck->setObjectName(QStringLiteral("cutoffCurrentCheck"));
    m_cutoffCurrentSpin =
        makeDecimalSpin("cutoffCurrentSpin", 0.1, 6553.5, 1,
                        QStringLiteral(" A"), group);
    auto *cutoffRow = new QWidget(group);
    auto *cutoffLayout = new QHBoxLayout(cutoffRow);
    cutoffLayout->setContentsMargins(0, 0, 0, 0);
    cutoffLayout->addWidget(m_cutoffCurrentCheck);
    cutoffLayout->addWidget(m_cutoffCurrentSpin);

    m_maxChargeSecondsCheck =
        new QCheckBox(QStringLiteral("启用充电桩最大时间寄存器"), group);
    m_maxChargeSecondsCheck->setObjectName(
        QStringLiteral("maxChargeSecondsCheck"));
    m_maxChargeSecondsSpin =
        makeIntegerSpin("maxChargeSecondsSpin", 1, 65535,
                        QStringLiteral(" s"), group);
    auto *maximumRow = new QWidget(group);
    auto *maximumLayout = new QHBoxLayout(maximumRow);
    maximumLayout->setContentsMargins(0, 0, 0, 0);
    maximumLayout->addWidget(m_maxChargeSecondsCheck);
    maximumLayout->addWidget(m_maxChargeSecondsSpin);

    m_safeCurrentSpin =
        makeDecimalSpin("safeCurrentSpin", 0.0, 120.0, 1,
                        QStringLiteral(" A"), group);
    m_chargeDetectCurrentSpin =
        makeDecimalSpin("chargeDetectCurrentSpin", 0.1, 120.0, 1,
                        QStringLiteral(" A"), group);
    m_startChargePercentSpin =
        makeIntegerSpin("startChargePercentSpin", 11, 99,
                        QStringLiteral(" %"), group);
    m_dispatchReadyPercentSpin =
        makeIntegerSpin("dispatchReadyPercentSpin", 11, 99,
                        QStringLiteral(" %"), group);
    m_stopChargePercentSpin =
        makeIntegerSpin("stopChargePercentSpin", 11, 100,
                        QStringLiteral(" %"), group);

    form->addRow(QStringLiteral("目标电压："), m_voltageSpin);
    form->addRow(QStringLiteral("目标电流："), m_currentSpin);
    form->addRow(QStringLiteral("截止电流："), cutoffRow);
    form->addRow(QStringLiteral("充电桩最大时间："), maximumRow);
    form->addRow(QStringLiteral("无输出安全电流："), m_safeCurrentSpin);
    form->addRow(QStringLiteral("充电检测电流："), m_chargeDetectCurrentSpin);
    form->addRow(QStringLiteral("启动充电阈值："), m_startChargePercentSpin);
    form->addRow(QStringLiteral("允许接单电量："), m_dispatchReadyPercentSpin);
    form->addRow(QStringLiteral("正常停止阈值："), m_stopChargePercentSpin);
    return group;
}

QWidget *ChargePileSettingsDialog::createTimeoutGroup()
{
    auto *group = new QGroupBox(QStringLiteral("超时与轮询"), this);
    group->setObjectName(QStringLiteral("chargeTimeoutGroup"));
    auto *form = new QFormLayout(group);

    constexpr int maximumMs = 3600000;
    m_responseTimeoutSpin =
        makeIntegerSpin("responseTimeoutSpin", 1, maximumMs,
                        QStringLiteral(" ms"), group);
    m_connectTimeoutSpin =
        makeIntegerSpin("connectTimeoutSpin", 1, maximumMs,
                        QStringLiteral(" ms"), group);
    m_pollIntervalSpin =
        makeIntegerSpin("pollIntervalSpin", 1, maximumMs,
                        QStringLiteral(" ms"), group);
    m_startTimeoutSpin =
        makeIntegerSpin("startTimeoutSpin", 1, maximumMs,
                        QStringLiteral(" ms"), group);
    m_monitorTimeoutSpin =
        makeIntegerSpin("monitorTimeoutSpin", 0, maximumMs,
                        QStringLiteral(" ms"), group);
    m_stopTimeoutSpin =
        makeIntegerSpin("stopTimeoutSpin", 1, maximumMs,
                        QStringLiteral(" ms"), group);
    m_motionTimeoutSpin =
        makeIntegerSpin("motionTimeoutSpin", 1, maximumMs,
                        QStringLiteral(" ms"), group);

    form->addRow(QStringLiteral("响应超时："), m_responseTimeoutSpin);
    form->addRow(QStringLiteral("连接超时："), m_connectTimeoutSpin);
    form->addRow(QStringLiteral("轮询间隔："), m_pollIntervalSpin);
    form->addRow(QStringLiteral("启动确认超时："), m_startTimeoutSpin);
    form->addRow(QStringLiteral("监控最大时长（0=关闭）："), m_monitorTimeoutSpin);
    form->addRow(QStringLiteral("停止确认超时："), m_stopTimeoutSpin);
    form->addRow(QStringLiteral("推杆动作超时："), m_motionTimeoutSpin);
    return group;
}

ChargeSettings ChargePileSettingsDialog::candidate() const
{
    ChargeSettings value;
    value.host = m_hostEdit->text().trimmed();
    value.port = static_cast<quint16>(m_portSpin->value());
    value.slaveId = static_cast<quint8>(m_slaveIdSpin->value());
    value.voltageV = m_voltageSpin->value();
    value.currentA = m_currentSpin->value();
    value.cutoffCurrentA = m_cutoffCurrentCheck->isChecked()
        ? std::optional<double>(m_cutoffCurrentSpin->value())
        : std::nullopt;
    value.maxChargeSeconds = m_maxChargeSecondsCheck->isChecked()
        ? std::optional<int>(m_maxChargeSecondsSpin->value())
        : std::nullopt;
    value.safeCurrentA = m_safeCurrentSpin->value();
    value.chargeDetectCurrentA = m_chargeDetectCurrentSpin->value();
    value.startChargePercent = m_startChargePercentSpin->value();
    value.dispatchReadyPercent = m_dispatchReadyPercentSpin->value();
    value.stopChargePercent = m_stopChargePercentSpin->value();
    value.responseTimeoutMs = m_responseTimeoutSpin->value();
    value.connectTimeoutMs = m_connectTimeoutSpin->value();
    value.pollIntervalMs = m_pollIntervalSpin->value();
    value.startTimeoutMs = m_startTimeoutSpin->value();
    value.monitorTimeoutMs = m_monitorTimeoutSpin->value();
    value.stopTimeoutMs = m_stopTimeoutSpin->value();
    value.motionTimeoutMs = m_motionTimeoutSpin->value();
    return value;
}

void ChargePileSettingsDialog::writeSettings(const ChargeSettings &settings)
{
    m_hostEdit->setText(settings.host);
    m_portSpin->setValue(settings.port);
    m_slaveIdSpin->setValue(settings.slaveId);
    m_voltageSpin->setValue(settings.voltageV);
    m_currentSpin->setValue(settings.currentA);
    m_cutoffCurrentCheck->setChecked(settings.cutoffCurrentA.has_value());
    m_cutoffCurrentSpin->setValue(settings.cutoffCurrentA.value_or(1.0));
    m_maxChargeSecondsCheck->setChecked(
        settings.maxChargeSeconds.has_value());
    m_maxChargeSecondsSpin->setValue(
        settings.maxChargeSeconds.value_or(3600));
    m_safeCurrentSpin->setValue(settings.safeCurrentA);
    m_chargeDetectCurrentSpin->setValue(settings.chargeDetectCurrentA);
    m_startChargePercentSpin->setValue(settings.startChargePercent);
    m_dispatchReadyPercentSpin->setValue(settings.dispatchReadyPercent);
    m_stopChargePercentSpin->setValue(settings.stopChargePercent);
    m_responseTimeoutSpin->setValue(settings.responseTimeoutMs);
    m_connectTimeoutSpin->setValue(settings.connectTimeoutMs);
    m_pollIntervalSpin->setValue(settings.pollIntervalMs);
    m_startTimeoutSpin->setValue(settings.startTimeoutMs);
    m_monitorTimeoutSpin->setValue(settings.monitorTimeoutMs);
    m_stopTimeoutSpin->setValue(settings.stopTimeoutMs);
    m_motionTimeoutSpin->setValue(settings.motionTimeoutMs);
    updateOptionalControls();
}

void ChargePileSettingsDialog::updateOptionalControls()
{
    // 未勾选时禁用数值编辑，candidate() 同时保持 nullopt，确保默认不会写入
    // 现场尚未确认的截止电流和充电桩最大时间寄存器。
    m_cutoffCurrentSpin->setEnabled(
        !m_locked && m_cutoffCurrentCheck->isChecked());
    m_maxChargeSecondsSpin->setEnabled(
        !m_locked && m_maxChargeSecondsCheck->isChecked());
}
