#ifndef TASKEXECUTOR_H
#define TASKEXECUTOR_H

#include <QObject>

#include "agvcontroller.h"
#include "lineconfig.h"
#include "nscanscheduler.h"
#include "palletscheduler.h"

class QTimer;
class HuayanScheduler;

class TaskExecutor : public QObject
{
    Q_OBJECT

public:
    explicit TaskExecutor(AgvController *agv,
                          HuayanScheduler *arm,
                          NScanScheduler *scanner,
                          PalletScheduler *pallet,
                          QObject *parent = nullptr);

    bool isBusy() const;
    Task currentTask() const;

public slots:
    void start(const Task &task);
    void stopForSystemError(const QString &reason);
    void onScanFinished(const NScanScheduler::ScanResult &result);

signals:
    void taskUpdated(const Task &task);
    void taskSucceeded(const Task &task);
    void taskFailed(const Task &task, const QString &reason);
    void systemError(const Task &task, const QString &reason);
    void logMessage(const QString &message);
    void agvDispatchRequested(int lm);
    void scanRequested(const NScanScheduler::ScanOptions &options);

private slots:
    void onAgvMonitor(const AgvMonitorData &data);
    void onArmStageCompleted(const QString &stageName);
    void onArmStageError(const QString &reason);
    void onPreGripScanRequested();
    void onToolRotationCompleted();
    void onToolRotationError(const QString &reason);
    void onPalletPlaceCompleted();
    void onPalletPlaceError(const QString &reason);
    void onAgvTimeout();

private:
    enum class ExecState {
        Idle,
        AgvToPickup,
        ArmPickup,
        PreGripScan,
        RotateForScan,
        StowAfterPickup,
        AgvToUnload,
        ArmUnload,
        StowAfterUnload,
        AgvToPallet,
        PreparePalletPoint,
        ArmPalletPlace,
        CommitPallet,
        StowAfterPallet,
        CleanupStow
    };

    static constexpr int kAgvTimeoutMs = 120000;

    void resetRuntimeState();
    QString prefix(const QString &stage) const;
    void setTaskRunning(TaskStep step, const QString &statusText);
    void setTaskTerminal(TaskState state, const QString &statusText, const QString &lastError);
    TaskStep taskStepForState(ExecState state) const;
    int stepIndexForTaskStep(TaskStep step) const;
    bool isAgvNavigationState(ExecState state) const;

    bool resolveTaskConfigs();
    void enterState(ExecState state, const QString &statusText);
    void startAgvStep(ExecState state, int lm, const QString &statusText);
    void requestScan(const QString &statusText);
    NScanScheduler::ScanOptions defaultScanOptions() const;

    void beginPalletPreparation();
    void beginCleanupAfterTaskFailure(const QString &reason);
    void finalizeTaskFailureAfterCleanup();
    void finishTaskSuccess();
    void raiseSystemError(const QString &reason);
    void clearPalletStackingFlag();

    AgvController *m_agv = nullptr;
    HuayanScheduler *m_arm = nullptr;
    NScanScheduler *m_scanner = nullptr;
    PalletScheduler *m_pallet = nullptr;
    QTimer *m_agvTimeout = nullptr;

    Task m_task;
    ExecState m_state = ExecState::Idle;
    const StationTaskConfig *m_stationCfg = nullptr;
    const PalletAreaTaskConfig *m_palletCfg = nullptr;
    PalletPose m_pendingPalletOffset;
    int m_expectedLm = 0;
    int m_scanAttempt = 0;
    bool m_agvSeenMoving = false;
    bool m_cleanupAfterTaskFailure = false;
    QString m_pendingTaskFailureReason;
};

#endif // TASKEXECUTOR_H
