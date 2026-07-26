#ifndef LINEMANAGER_H
#define LINEMANAGER_H

#include <QElapsedTimer>
#include <QObject>
#include <functional>

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
    /// 返回自动充电是否正在保持 FIFO 派单；默认 false 时不改变原调度路径。
    bool chargeDispatchHeld() const;
    void applyRuntimeSettings(const RuntimeSettings &settings);
    /// 注入兼容旧整线是否运行的只读判定；用于拒绝两个顶层调度同时控制 AGV/机械臂。
    void setExternalWorkflowRunning(std::function<bool()> predicate);

public slots:
    /// Idle -> Running；有 Pending 时直接执行，无任务时确保 AGV 回 LM1。
    void start();
    /// 人工急停语义：取消设备、清 Pending、当前任务 Canceled，并进入 Error。
    void stop();
    /// 仅 Error 状态有效；回 Idle，但不自动重新启动任务。
    void resetError();
    /**
     * @brief 设置自动充电派单保持电平。
     *
     * 保持只阻止取出新的 FIFO 队首，不中断正在执行的任务，也不清空 Pending。
     * 从 true 解除为 false 时，仅在主调度 Running 且执行器空闲时恢复取队首。
     */
    void setChargeDispatchHold(bool hold, const QString &reason);
    /**
     * @brief 由自动充电协调器请求使用 LineManager 的唯一返航入口回 LM1。
     *
     * 只有已建立派单保持且主调度 Running 时才接受；实际导航仍由既有
     * agvDispatchRequested 信号下发，充电模块不能绕过调度直接控制 AGV。
     */
    void requestChargeReturnHome();
    /// 将充电等外部子系统的致命故障汇入既有 Error/清队列/停止设备语义。
    void raiseExternalSystemError(const QString &reason);
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
    /**
     * @brief 通知派单保持的实际电平已经变化。
     *
     * Stop、Error、Reset 和协调器请求都必须发布同一确认，使自动协调器不会
     * 只保存自己最后发出的边沿而忽略 LineManager 的本地安全清理。
     */
    void chargeDispatchHoldChanged(bool hold, const QString &reason);
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
    static constexpr int kReturnHomeTimeoutMs = 300000; ///< 回 LM1 超时，单位 ms（5 分钟）。

    void setState(LineSystemState state, const QString &text);
    void setCurrentTask(const Task &task);
    void clearCurrentTask();
    void emitQueueChanged();
    /// 更新实际保持电平；Stop/Error 传 resumePending=false，禁止清理时瞬间取新单。
    void updateChargeDispatchHold(bool hold, const QString &reason,
                                  bool resumePending);
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
    QElapsedTimer m_returnHomeElapsed;         ///< 本轮独立返航的实际等待计时，只用于超时日志。

    TaskQueue m_queue;                                  ///< 仅含 Pending 的 FIFO。
    Task m_currentTask;                                 ///< 当前 Running/终态任务快照。
    LineSystemState m_state = LineSystemState::Idle;    ///< 整线状态。
    QString m_stateText = QStringLiteral("未启动");     ///< 面向现场 UI 的状态文案。

    bool m_chargeDispatchHold = false; ///< 自动充电保持；false 时所有既有派单分支原样执行。
    QString m_chargeDispatchHoldReason; ///< 最近保持原因，仅用于现场日志，不参与状态判断。
    bool m_manualStopInProgress = false; ///< 防止 Stop 内同步信号重复处理任务终态。
    bool m_returnHomeActive = false;     ///< 当前是否由 LineManager 独立跟踪回 LM1。
    bool m_returnHomeSeenMoving = false; ///< 已看到回站导航进入 Waiting/Running。
    bool m_hasAgvMonitor = false;        ///< m_lastAgvMonitor 是否至少更新过一次。
    AgvMonitorData m_lastAgvMonitor;     ///< 最近 AGV 快照，仅供回 LM1 状态机使用。

    std::function<bool()> m_externalWorkflowRunning; ///< 兼容旧整线运行判定；不拥有对象，只在 start() 入口读取。
};

#endif // LINEMANAGER_H
