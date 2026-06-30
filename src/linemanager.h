#ifndef LINEMANAGER_H
#define LINEMANAGER_H

#include <QObject>

#include "agvcontroller.h"
#include "lineconfig.h"
#include "nscanscheduler.h"
#include "taskqueue.h"

class QTimer;
class HuayanScheduler;
class PalletScheduler;
class TaskExecutor;

class LineManager : public QObject
{
    Q_OBJECT

public:
    explicit LineManager(AgvController *agv,
                         HuayanScheduler *arm,
                         NScanScheduler *scanner,
                         PalletScheduler *pallet,
                         QObject *parent = nullptr);

    LineSystemState state() const;
    QList<Task> queueSnapshot() const;
    Task currentTask() const;

public slots:
    void start();
    void stop();
    void resetError();
    void reportShortage(int stationId);
    void onScanFinished(const NScanScheduler::ScanResult &result);

signals:
    void systemStateChanged(LineSystemState state, const QString &text);
    void queueChanged(QList<Task> tasks);
    void currentTaskChanged(Task task);
    void alarmRaised(QString reason);
    void logMessage(QString message);
    void agvDispatchRequested(int lm);
    void scanRequested(NScanScheduler::ScanOptions options);

private slots:
    void onExecutorTaskUpdated(const Task &task);
    void onExecutorTaskSucceeded(const Task &task);
    void onExecutorTaskFailed(const Task &task, const QString &reason);
    void onExecutorSystemError(const Task &task, const QString &reason);
    void onAgvMonitorUpdated(const AgvMonitorData &data);
    void onAgvErrorOccurred(const QString &message);
    void onReturnHomeTimeout();

private:
    static constexpr int kHomeLm = 1;
    static constexpr int kReturnHomeTimeoutMs = 120000;

    void setState(LineSystemState state, const QString &text);
    void setCurrentTask(const Task &task);
    void clearCurrentTask();
    void emitQueueChanged();
    void tryStartNext();
    void returnHomeIfNeeded();
    void cancelReturnHomeForNewTask();
    void stopReturnHomeTracking();
    void enterError(const QString &reason);
    void clearPendingForError(const QString &reason);

    AgvController *m_agv = nullptr;
    HuayanScheduler *m_arm = nullptr;
    TaskExecutor *m_executor = nullptr;
    QTimer *m_returnHomeTimeout = nullptr;

    TaskQueue m_queue;
    Task m_currentTask;
    LineSystemState m_state = LineSystemState::Idle;
    QString m_stateText = QStringLiteral("未启动");

    bool m_manualStopInProgress = false;
    bool m_returnHomeActive = false;
    bool m_returnHomeSeenMoving = false;
    bool m_hasAgvMonitor = false;
    AgvMonitorData m_lastAgvMonitor;
};

#endif // LINEMANAGER_H
