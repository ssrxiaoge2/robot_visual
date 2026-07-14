#ifndef SHORTAGECONFIGDIALOG_H
#define SHORTAGECONFIGDIALOG_H

#include "shortageconfigstore.h"
#include "shortagetestcontroller.h"

#include <QDialog>
#include <QList>
#include <QMap>
#include <functional>

class QButtonGroup;
class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QTextEdit;
class QVBoxLayout;

/// 宽屏缺料配置与独立全逻辑测试弹窗；只持有配置副本、测试控制器和只读快照，不访问生产 Engine。
class ShortageConfigDialog final : public QDialog
{
    Q_OBJECT
public:
    using EditGateProvider = std::function<ShortageEditConditions()>;

    /// testController 为非拥有依赖；为空时仍可展示配置和静态测试契约。
    explicit ShortageConfigDialog(ShortageConfiguration configuration,
                                  EditGateProvider editGateProvider,
                                  ShortageTestController *testController = nullptr,
                                  ShortageUiSnapshot snapshot = {},
                                  QWidget *parent = nullptr);

    /// 返回最近一次通过门禁和 ShortageConfigStore::validate() 的配置副本，供集成层持久化。
    /// 保存被门禁/校验拦截时保持上一次成功值，Dialog 不直接写生产 Engine 或文件。
    ShortageConfiguration validatedConfiguration() const;

signals:
    /// 配置通过校验并完成保存动作后通知外层刷新只读视图。
    void configurationSaved();

private:
    void buildUi();
    QWidget *buildConfigurationPage();
    QWidget *buildTestPage();
    QTableWidget *createStationTable(ProductModel product, const QString &objectName);
    void populateStationTable(QTableWidget *table, ProductModel product);
    void createParameterEditors(QVBoxLayout *layout);
    void createTestActionButton(QGridLayout *layout,
                                const QString &text,
                                const QString &objectName,
                                int row,
                                int column,
                                void (ShortageTestController::*slot)());
    ShortageConfiguration configurationFromUi() const;
    bool focusValidationFailure(const QString &messageZh);
    bool focusStationValidationFailure(const QString &messageZh);
    void focusConfigurationWidget(QWidget *widget);
    void saveConfiguration();
    /// 按当前选择把手工产品、模式和 qint64 actualQty 一次提交给测试控制器。
    void submitManualSample();
    /// 只更新输入源控件门禁；业务层仍在 ShortageTestController 二次校验。
    void refreshTestSourceControls();
    /// 用控制器权威快照刷新 12 工位、基线、等待顺序和当前补料单。
    void refreshTestSnapshot(const ShortageUiSnapshot &snapshot);
    /// 用控制器计算的动作能力刷新事件按钮，不在 UI 复制状态机规则。
    void refreshTestActionAvailability(const ShortageTestActionAvailability &availability);

    ShortageConfiguration m_configuration; ///< UI 编辑副本，保存前统一交给 ShortageConfigStore 校验。
    ShortageConfiguration m_validatedConfiguration; ///< 最近一次可安全交给外层保存的已校验配置。
    EditGateProvider m_editGateProvider;   ///< 非拥有运行门禁查询，避免 Dialog 读取 Line/Engine。
    ShortageTestController *m_testController = nullptr; ///< 非拥有测试控制器，只连接测试页按钮。
    ShortageUiSnapshot m_snapshot; ///< 打开弹窗时的只读快照，用于摘要展示。
    QTabWidget *m_mainTabs = nullptr;
    QTabWidget *m_productTabs = nullptr;
    QMap<ProductModel, QTableWidget *> m_tables;
    QLineEdit *m_endpointEdit = nullptr;
    QSpinBox *m_intervalSpin = nullptr;
    QSpinBox *m_timeoutSpin = nullptr;
    QSpinBox *m_alarmSpin = nullptr;
    QSpinBox *m_failureLimitSpin = nullptr;
    QTextEdit *m_eventLog = nullptr;
    QRadioButton *m_manualSourceRadio = nullptr; ///< Dialog 拥有；默认选中且不访问真实系统。
    QRadioButton *m_fieldSourceRadio = nullptr;  ///< Dialog 拥有；选中后仍需明确点击启动采样。
    QComboBox *m_manualProductCombo = nullptr;   ///< Dialog 拥有；data 保存 ProductModel。
    QComboBox *m_manualModeCombo = nullptr;      ///< Dialog 拥有；data 保存 ProductionMode。
    QLineEdit *m_manualActualQtyEdit = nullptr;  ///< Dialog 拥有；验证完整非负 qint64 文本。
    QPushButton *m_manualSampleSubmitButton = nullptr; ///< Dialog 拥有；只在手工源可用。
    QTableWidget *m_testRuntimeTable = nullptr;  ///< Dialog 拥有；只读显示 12 工位权威快照。
    QLabel *m_testPlanSummaryLabel = nullptr;    ///< Dialog 拥有；显示基线、活动工位和等待顺序。
    QLabel *m_testOrderSummaryLabel = nullptr;   ///< Dialog 拥有；显示当前测试补料单状态。
    QMap<QString, QPushButton *> m_testActionButtons; ///< objectName 到按钮，仅用于门禁刷新。
};

#endif // SHORTAGECONFIGDIALOG_H
