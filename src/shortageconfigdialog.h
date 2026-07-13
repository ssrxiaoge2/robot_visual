#ifndef SHORTAGECONFIGDIALOG_H
#define SHORTAGECONFIGDIALOG_H

#include "shortageconfigstore.h"
#include "shortagetestcontroller.h"

#include <QDialog>
#include <QList>
#include <QMap>
#include <functional>

class QButtonGroup;
class QGridLayout;
class QLineEdit;
class QPushButton;
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
    void saveConfiguration();

    ShortageConfiguration m_configuration; ///< UI 编辑副本，保存前统一交给 ShortageConfigStore 校验。
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
};

#endif // SHORTAGECONFIGDIALOG_H
