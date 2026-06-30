#ifndef TASKQUEUE_H
#define TASKQUEUE_H

#include <QList>
#include <QString>

#include "lineconfig.h"

/**
 * 任务实例队列，不是工位状态表；同一工位允许多次入队。
 */
class TaskQueue
{
public:
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

    bool hasPending() const
    {
        return !m_pending.isEmpty();
    }

    int pendingCount() const
    {
        return m_pending.size();
    }

    Task takeNext()
    {
        if (m_pending.isEmpty()) {
            return Task{};
        }

        Task task = m_pending.takeFirst();
        task.state = TaskState::Running;
        return task;
    }

    QList<Task> pendingSnapshot() const
    {
        return m_pending;
    }

    void clearPendingAsCanceled(const QString &reason)
    {
        Q_UNUSED(reason);
        m_pending.clear();
    }

    quint64 nextTaskId() const
    {
        return m_nextTaskId;
    }

private:
    QList<Task> m_pending;
    quint64 m_nextTaskId = 1;
};

#endif // TASKQUEUE_H
