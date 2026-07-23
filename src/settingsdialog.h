#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include <QHash>

#include "runtimesettings.h"

class QCheckBox;
class QDoubleSpinBox;
class QSpinBox;
class QLabel;
class QListWidget;
class QStackedWidget;

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(const RuntimeSettings &current,
                            bool runtimeLocked,
                            QWidget *parent = nullptr);

    RuntimeSettings candidate() const;

signals:
    void saveRequested(const RuntimeSettings &candidate);

private:
    QWidget *createCategoryPage(SettingsCategory category);
    QDoubleSpinBox *addDouble(QWidget *page, const QString &key,
                              const QString &label, double min, double max,
                              const QString &unit, bool safety = false);
    QSpinBox *addInt(QWidget *page, const QString &key,
                     const QString &label, int min, int max,
                     const QString &unit, bool safety = false);
    QCheckBox *addBool(QWidget *page, const QString &key, const QString &label);
    void writeSettings(const RuntimeSettings &settings);
    QStringList changedValues(const RuntimeSettings &value) const;
    bool safetyChanged(const RuntimeSettings &value) const;
    SettingsCategory currentCategory() const;

    RuntimeSettings m_original;
    bool m_runtimeLocked = false;
    QListWidget *m_categories = nullptr;
    QStackedWidget *m_stack = nullptr;
    QLabel *m_preview = nullptr;
    QCheckBox *m_safetyAcknowledgement = nullptr;
    QHash<QString, QDoubleSpinBox *> m_doubles;
    QHash<QString, QSpinBox *> m_ints;
    QHash<QString, QCheckBox *> m_bools;
};

Q_DECLARE_METATYPE(RuntimeSettings)

#endif // SETTINGSDIALOG_H
