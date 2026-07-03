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
    /**
     * @brief 兼容旧调用点的缺料入口，只返回“FIFO 是否接受该请求”。
     *
     * 修改前后该接口的队列校验、Idle/Running/ReturningHome 分支都保持原样；
     * 新增真实缺料接线后，上层若需要拿到任务号，应改调 reportShortageWithId()，
     * 而不是在这里推断是否入队成功。
     */
    bool reportShortage(int stationId, TaskSource source = TaskSource::UiMock);
    /**
     * @brief 在不改变既有入队/启动顺序的前提下，返回本次缺料事件对应的任务号。
     *
     * @param stationId 缺料工位，必须为 1..12。
     * @param source 任务来源；真实缺料接线使用 CustomerSystem，模拟按钮仍使用 UiMock。
     * @return 非 0 表示任务已成功创建并获得 taskId；0 表示参数非法或系统正处于 Error，
     *         此时不会新增任务，也不会改变 e1ffb3f 既有主流程。
     */
    quint64 reportShortageWithId(int stationId, TaskSource source);
    void onScanFinished(const NScanScheduler::ScanResult &result);

signals:
    void systemStateChanged(LineSystemState state, const QString &text);
    void queueChanged(QList<Task> tasks);
    void currentTaskChanged(Task task);
    /// 任务已创建并追加到 FIFO 队尾后发出；只增加通知，不改变队列算法。
    void taskEnqueued(Task task);
    /// 任务从 FIFO 取出并交给 TaskExecutor 启动后发出；对应既有 takeNext() 时刻。
    void taskStarted(Task task);
    /// TaskExecutor 报告真实倒料完成后原样向上转发；不参与库存计算。
    void materialUnloaded(Task task);
    /// 任务进入成功/失败/取消终态时发出；Stop/Error 仅补通知，不改变原清理顺序。
    void taskFinished(Task task);
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
