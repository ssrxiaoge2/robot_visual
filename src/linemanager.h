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
 * @brief 12 工位连续补料的整线状态机和 FIFO 调度入口。
 *
 * LineManager 决定何时启动下一任务、何时回 LM1、何时清空队列进入 Error；
 * 单个任务内部动作委托给 TaskExecutor。对象运行在 UI 主线程，不直接调用设备 SDK。
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
    /// Idle -> Running；有 Pending 时直接执行，无任务时确保 AGV 回 LM1。
    void start();
    /// 人工急停语义：取消设备、清 Pending、当前任务 Canceled，并进入 Error。
    void stop();
    /// 仅 Error 状态有效；回 Idle，但不自动重新启动任务。
    void resetError();
    /// 接收一次独立缺料事件；同一工位允许重复调用并生成不同 taskId。
    void reportShortage(int stationId);
    /// 仅转发调度专用扫码结果给当前 TaskExecutor。
    void onScanFinished(const NScanScheduler::ScanResult &result);

signals:
    /// 系统状态或客户显示文案变化。
    void systemStateChanged(LineSystemState state, const QString &text);
    /// 当前 Running 任务加全部 Pending 任务的 UI 快照。
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
    static constexpr int kHomeLm = 1;                 ///< 队列为空时的 AGV 待机点。
    static constexpr int kReturnHomeTimeoutMs = 120000; ///< 回 LM1 超时，单位 ms。

    void setState(LineSystemState state, const QString &text);
    void setCurrentTask(const Task &task);
    void clearCurrentTask();
    void emitQueueChanged();
    /// 条件允许时从 FIFO 取队首并启动；忙碌/Idle/Error 时无动作。
    void tryStartNext();
    /// 队列耗尽后的统一出口：已在 LM1 则等待，否则进入 ReturningHome。
    void returnHomeIfNeeded();
    /// 回站途中来新任务时的正常切换，不应把主动 cancel 当成 AGV fatal。
    void cancelReturnHomeForNewTask();
    void stopReturnHomeTracking();
    void enterError(const QString &reason);
    void clearPendingForError(const QString &reason);

    AgvController *m_agv = nullptr;            ///< 非拥有指针；用于回站监控和 Stop。
    HuayanScheduler *m_arm = nullptr;          ///< 非拥有指针；Stop 时立即停止机械臂。
    TaskExecutor *m_executor = nullptr;        ///< QObject 子对象；一次只执行一个任务。
    QTimer *m_returnHomeTimeout = nullptr;     ///< 仅 ReturningHome 期间启用。

    TaskQueue m_queue;                                  ///< 仅含 Pending 的 FIFO。
    Task m_currentTask;                                 ///< 当前 Running/终态任务快照。
    LineSystemState m_state = LineSystemState::Idle;    ///< 整线状态。
    QString m_stateText = QStringLiteral("未启动");     ///< 面向现场 UI 的状态文案。

    bool m_manualStopInProgress = false; ///< 防止 Stop 内同步信号重复处理任务终态。
    bool m_returnHomeActive = false;     ///< 当前是否由 LineManager 独立跟踪回 LM1。
    bool m_returnHomeSeenMoving = false; ///< 已看到回站导航进入 Waiting/Running。
    bool m_hasAgvMonitor = false;        ///< m_lastAgvMonitor 是否至少更新过一次。
    AgvMonitorData m_lastAgvMonitor;     ///< 最近 AGV 快照，仅供回 LM1 状态机使用。
};

#endif // LINEMANAGER_H
