#ifndef SHORTAGERECOVERYDIALOG_H
#define SHORTAGERECOVERYDIALOG_H

#include "shortagetypes.h"

#include <QDialog>

class QLineEdit;
class QPushButton;
class QSpinBox;

/// 异常库存恢复弹窗；只接受只读快照并产出单工位维护修正命令。
class ShortageRecoveryDialog final : public QDialog
{
    Q_OBJECT
public:
    /// snapshot 按值保存，Dialog 生命周期内不持有 Engine/Ledger 指针。
    explicit ShortageRecoveryDialog(ShortageUiSnapshot snapshot,
                                    int stationId,
                                    QWidget *parent = nullptr);

signals:
    /// 维护修正请求交由外层生产协调器二次校验门禁后执行。
    void maintenanceCorrectionRequested(ShortageMaintenanceCorrection correction);

private:
    qint64 stockForStation(int stationId) const;
    void submitCorrection();

    ShortageUiSnapshot m_snapshot; ///< 打开窗口时的只读运行快照，用于 oldStock 并发校验。
    QSpinBox *m_stationSpin = nullptr;
    QSpinBox *m_newStockSpin = nullptr;
    QLineEdit *m_reasonEdit = nullptr;
    QLineEdit *m_typedStationEdit = nullptr;
    QPushButton *m_submitButton = nullptr;
};

Q_DECLARE_METATYPE(ShortageMaintenanceCorrection)

#endif // SHORTAGERECOVERYDIALOG_H
