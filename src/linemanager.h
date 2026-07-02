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

/**
 * @brief 十二工位连续补料的整线状态机和 FIFO 入口。
 *
 * LineManager 负责 Start/Stop/Reset、FIFO 启停、回 LM1 和 Error 边界。
 * 单任务内部动作继续委托给 TaskExecutor；reportShortage() 的返回值只表示
 * FIFO 是否接受该缺料事件，不表示送料是否完成。
 */
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
    bool reportShortage(int stationId, TaskSource source = TaskSource::UiMock);
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

    AgvController *m_agv = nullptr;        ///< 非拥有指针；由 DeviceManager 统一持有。
    HuayanScheduler *m_arm = nullptr;      ///< 非拥有指针；Stop/Error 时立即停机械臂。
    TaskExecutor *m_executor = nullptr;    ///< 子对象；一次只执行一个任务。
    QTimer *m_returnHomeTimeout = nullptr; ///< ReturningHome 阶段的独立超时器。

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
