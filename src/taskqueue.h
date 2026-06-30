#ifndef TASKQUEUE_H
#define TASKQUEUE_H

#include <QList>
#include <QString>

#include "lineconfig.h"

/**
 * @brief 只保存待执行任务的进程内 FIFO 队列。
 *
 * 这里保存的是“任务实例”，不是工位状态表，因此同一工位允许连续多次入队。
 * Running 任务一旦由 takeNext() 取出便归 LineManager/TaskExecutor 管理，不再留在本容器中。
 * 本类只在 UI 主线程使用，不提供锁，也不负责持久化或断电恢复。
 */
class TaskQueue
{
public:
    /// 创建新任务并追加到队尾；返回副本供调用方立即刷新 UI/日志。
    Task enqueue(int stationId, TaskSource source)
    {
        Task task;
        task.taskId = m_nextTaskId++;
        task.stationId = stationId;
        task.source = source;
        task.state = TaskState::Pending;
        task.step = TaskStep::Waiting;
        task.stepIndex = 0;
        task.createdAt = QDateTime::currentDateTimeUtc();
        task.statusText.clear();
        task.lastError.clear();

        m_pending.append(task);
        return task;
    }

    /// 队列中是否还有 Pending 任务。
    bool hasPending() const
    {
        return !m_pending.isEmpty();
    }

    /// 当前 Pending 任务数，不包含正在运行的任务。
    int pendingCount() const
    {
        return m_pending.size();
    }

    /// 取出最早入队任务并标记 Running；空队列返回 taskId=0 的无效 Task。
    Task takeNext()
    {
        if (m_pending.isEmpty()) {
            return Task{};
        }

        Task task = m_pending.takeFirst();
        task.state = TaskState::Running;
        return task;
    }

    /// 返回队列副本，防止 UI 修改内部顺序或状态。
    QList<Task> pendingSnapshot() const
    {
        return m_pending;
    }

    /// 系统级错误/人工 Stop 时清空所有待执行任务；历史原因由上层日志保存。
    void clearPendingAsCanceled(const QString &reason)
    {
        Q_UNUSED(reason);
        m_pending.clear();
    }

    /// 查看下一任务号，主要用于诊断，不会消耗编号。
    quint64 nextTaskId() const
    {
        return m_nextTaskId;
    }

private:
    QList<Task> m_pending;      ///< 严格保持入队先后顺序的 Pending 列表。
    quint64 m_nextTaskId = 1;   ///< 进程内单调递增；程序重启后从 1 重新开始。
};

#endif // TASKQUEUE_H
