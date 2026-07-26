#pragma once

#include "chargesettings.h"

#include <QDialog>

class QCheckBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QWidget;

/**
 * @brief 充电桩通信、电气和超时参数的独立设置窗口。
 *
 * 自动充电授权不属于 ChargeSettings，且刻意不在本窗口中提供控件，保证“恢复
 * 默认值”和配置持久化都不能意外开启自动模式。locked 表示控制器存在活动会话
 * 或安全状态不允许改参；锁定时仍可查看当前已生效值，但不能编辑或发出保存信号。
 */
class ChargePileSettingsDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit ChargePileSettingsDialog(const ChargeSettings &settings,
                                      bool locked,
                                      QWidget *parent = nullptr);

    /**
     * @brief 动态切换整个参数窗口的只读锁定状态。
     *
     * 对话框 exec() 期间仍会处理控制器和协调器信号，因此锁定不能只依赖构造时
     * 快照。重复传入相同值是安全的；通信、充电、超时分组、恢复默认和保存按钮
     * 会在一次调用中同步刷新，取消按钮始终可用。
     */
    void setLocked(bool locked);

    /// 从当前控件一次性组装完整候选快照；可选项未勾选时保持 std::nullopt。
    ChargeSettings candidate() const;

signals:
    /// 仅在完整候选参数通过统一 validateChargeSettings() 后发出。
    void saveRequested(const ChargeSettings &candidate);

private:
    void writeSettings(const ChargeSettings &settings);
    void updateOptionalControls();
    QWidget *createCommunicationGroup();
    QWidget *createChargeGroup();
    QWidget *createTimeoutGroup();

    bool m_locked = false; ///< 锁定态只读，禁止恢复默认和保存。
    QLabel *m_lockedBanner = nullptr; ///< 动态提示活动会话期间参数仅供查看。

    QLineEdit *m_hostEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QSpinBox *m_slaveIdSpin = nullptr;

    QDoubleSpinBox *m_voltageSpin = nullptr;
    QDoubleSpinBox *m_currentSpin = nullptr;
    QCheckBox *m_cutoffCurrentCheck = nullptr;
    QDoubleSpinBox *m_cutoffCurrentSpin = nullptr;
    QCheckBox *m_maxChargeSecondsCheck = nullptr;
    QSpinBox *m_maxChargeSecondsSpin = nullptr;
    QDoubleSpinBox *m_safeCurrentSpin = nullptr;
    QDoubleSpinBox *m_chargeDetectCurrentSpin = nullptr;
    QSpinBox *m_startChargePercentSpin = nullptr;
    QSpinBox *m_dispatchReadyPercentSpin = nullptr;
    QSpinBox *m_stopChargePercentSpin = nullptr;

    QSpinBox *m_responseTimeoutSpin = nullptr;
    QSpinBox *m_connectTimeoutSpin = nullptr;
    QSpinBox *m_pollIntervalSpin = nullptr;
    QSpinBox *m_startTimeoutSpin = nullptr;
    QSpinBox *m_monitorTimeoutSpin = nullptr;
    QSpinBox *m_stopTimeoutSpin = nullptr;
    QSpinBox *m_motionTimeoutSpin = nullptr;

    QPushButton *m_restoreDefaultsButton = nullptr;
    QDialogButtonBox *m_buttonBox = nullptr;
};
