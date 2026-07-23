#ifndef SETTINGSMANAGER_H
#define SETTINGSMANAGER_H

#include <QObject>
#include <QString>

#include "runtimesettings.h"

struct SettingsLoadResult
{
    RuntimeSettings settings;
    QStringList warnings;
};

class SettingsManager : public QObject
{
    Q_OBJECT
public:
    explicit SettingsManager(QString iniPath, QObject *parent = nullptr);

    SettingsLoadResult load();
    const RuntimeSettings &current() const { return m_current; }

    bool stageCandidate(const RuntimeSettings &candidate, QString *error);
    bool commitStaged(const RuntimeSettings &candidate, QString *error);
    void discardStaged();

private:
    QString m_iniPath;
    QString m_stagedPath;
    RuntimeSettings m_current = RuntimeSettings::defaults();
};

#endif // SETTINGSMANAGER_H
