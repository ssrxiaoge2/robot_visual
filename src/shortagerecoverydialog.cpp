#include "shortagerecoverydialog.h"

#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

ShortageRecoveryDialog::ShortageRecoveryDialog(ShortageUiSnapshot snapshot,
                                               int stationId,
                                               QWidget *parent)
    : QDialog(parent), m_snapshot(std::move(snapshot))
{
    setWindowTitle(QStringLiteral("缺料异常库存恢复"));
    setMinimumSize(520, 320);
    setStyleSheet(QStringLiteral(
        "QDialog { background:#20242b; color:#f0f3f6; }"
        "QLineEdit, QSpinBox { background:#111820; color:#f4f7fb; border:1px solid #3b4654; }"
        "QPushButton { background:#2f6fed; color:white; border:0; border-radius:4px; padding:6px 12px; }"));

    auto *root = new QVBoxLayout(this);
    auto *notice = new QLabel(QStringLiteral(
                                  "普通恢复只允许提交一个代码工位、原因和复核输入；不提供全清零或批量任务决策。"),
                              this);
    notice->setWordWrap(true);
    root->addWidget(notice);

    auto *form = new QFormLayout;
    m_stationSpin = new QSpinBox(this);
    m_stationSpin->setObjectName(QStringLiteral("recoveryStationIdSpin"));
    m_stationSpin->setRange(1, 12);
    m_stationSpin->setValue(stationId < 1 || stationId > 12 ? 1 : stationId);
    form->addRow(QStringLiteral("代码工位"), m_stationSpin);

    m_newStockSpin = new QSpinBox(this);
    m_newStockSpin->setObjectName(QStringLiteral("recoveryNewStockSpin"));
    m_newStockSpin->setRange(-100000000, 100000000);
    m_newStockSpin->setValue(static_cast<int>(stockForStation(m_stationSpin->value())));
    form->addRow(QStringLiteral("修正后库存"), m_newStockSpin);

    m_reasonEdit = new QLineEdit(this);
    m_reasonEdit->setObjectName(QStringLiteral("recoveryReasonEdit"));
    form->addRow(QStringLiteral("维护原因"), m_reasonEdit);

    m_typedStationEdit = new QLineEdit(this);
    m_typedStationEdit->setObjectName(QStringLiteral("recoveryTypedStationIdEdit"));
    form->addRow(QStringLiteral("复核输入工位号"), m_typedStationEdit);
    root->addLayout(form);

    m_submitButton = new QPushButton(QStringLiteral("提交维护修正"), this);
    m_submitButton->setObjectName(QStringLiteral("recoverySubmitButton"));
    connect(m_submitButton, &QPushButton::clicked, this,
            &ShortageRecoveryDialog::submitCorrection);
    root->addWidget(m_submitButton);

    connect(m_stationSpin, &QSpinBox::valueChanged, this, [this](int value) {
        // UI 状态分支：切换唯一目标工位时同步只读快照里的旧库存提示，不批量编辑其他工位。
        m_newStockSpin->setValue(static_cast<int>(stockForStation(value)));
    });
}

qint64 ShortageRecoveryDialog::stockForStation(int stationId) const
{
    for (const ShortageStationRuntime &station : m_snapshot.runtime.stations) {
        if (station.stationId == stationId)
            return station.stock;
    }
    return 0;
}

void ShortageRecoveryDialog::submitCorrection()
{
    const int stationId = m_stationSpin->value();
    const QString reason = m_reasonEdit->text().trimmed();
    const QString typedStationId = m_typedStationEdit->text().trimmed();

    if (reason.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("缺少原因"), QStringLiteral("维护原因不能为空"));
        return;
    }
    if (typedStationId != QString::number(stationId)) {
        QMessageBox::warning(this, QStringLiteral("复核失败"),
                             QStringLiteral("复核输入的工位号必须与目标工位一致"));
        return;
    }

    ShortageMaintenanceCorrection correction;
    correction.stationId = stationId;
    correction.oldStock = stockForStation(stationId);
    correction.newStock = m_newStockSpin->value();
    correction.reason = reason;
    correction.typedStationId = typedStationId;
    emit maintenanceCorrectionRequested(correction);
}
