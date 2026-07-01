#ifndef TASKEXECUTOR_H
#define TASKEXECUTOR_H

#include <QObject>

#include "agvcontroller.h"
#include "lineconfig.h"
#include "nscanscheduler.h"
#include "palletscheduler.h"

class QTimer;
class HuayanScheduler;

/**
 * @brief 串行执行一个补料任务的业务状态机。
 *
 * 本类只负责编排高层动作：向 AGV 请求导航、调用 HuayanScheduler 阶段、
 * 请求扫码、查询/提交码垛点位。它不直接调用 Modbus 或华沿 SDK 原语，
 * 对象与设备控制器同属 UI 主线程；扫码结果由 DeviceManager 从 worker
 * 线程排队转回。
 */
class TaskExecutor : public QObject
{
    Q_OBJECT

public:
    /// 依赖均由 DeviceManager/LineManager 持有，TaskExecutor 不负责释放。
    explicit TaskExecutor(AgvController *agv,
                          HuayanScheduler *arm,
                          NScanScheduler *scanner,
                          PalletScheduler *pallet,
                          QObject *parent = nullptr);

    bool isBusy() const;       ///< ExecState 非 Idle 即表示有任务占用设备。
    Task currentTask() const;  ///< 返回当前任务快照，供 LineManager/UI 只读展示。

public slots:
    /// 启动一个已出队任务；依赖或配置缺失会直接升级为系统级 ERROR。
    void start(const Task &task);
    /// 响应整线 Stop/致命错误，停止当前任务并发出 systemError。
    void stopForSystemError(const QString &reason);
    /// 接收调度专用扫码 worker 的结果；非扫码状态下的迟到结果会被忽略。
    void onScanFinished(const NScanScheduler::ScanResult &result);

signals:
    /// 任一任务步骤/文案变化时发出完整任务快照。
    void taskUpdated(const Task &task);
    /// 全流程成功完成（含码垛 commit 和最终收姿态）。
    void taskSucceeded(const Task &task);
    /// 任务级失败且安全收姿态成功；LineManager 可继续下一任务。
    void taskFailed(const Task &task, const QString &reason);
    /// 设备/配置/安全恢复失败；LineManager 必须进入 Error 并清 Pending。
    void systemError(const Task &task, const QString &reason);
    void logMessage(const QString &message);
    /// 请求上层把数字 LM 直接下发给 AgvController。
    void agvDispatchRequested(int lm);
    /// 请求 DeviceManager 在后台线程执行一次扫码。
    void scanRequested(const NScanScheduler::ScanOptions &options);

private slots:
    void onAgvMonitor(const AgvMonitorData &data);
    void onArmStageCompleted(const QString &stageName);
    void onArmStageError(const QString &reason);
    void onPreGripScanRequested();
    void onPreGripScanSearchMoveCompleted(double currentYOffsetMm);
    void onPreGripScanSearchMoveError(const QString &reason);
    void onPreGripScanCaptureReturnCompleted();
    void onPreGripScanCaptureReturnError(const QString &reason);
    void onToolRotationCompleted();
    void onToolRotationError(const QString &reason);
    void onPalletPlaceCompleted();
    void onPalletPlaceError(const QString &reason);
    void onAgvTimeout();

private:
    /// 单任务内部精细状态；任一时刻只允许处于其中一个状态，保证单设备严格串行。
    enum class ExecState {
        Idle,                    ///< 无活动任务，可接受 start()。
        AgvToPickup,             ///< 等待 AGV 到取料 LM。
        ArmPickup,               ///< 机械臂视觉定位、下探、夹取和抬升。
        PreGripScan,             ///< 机械臂停在夹紧前安全点，等待扫码结果。
        PreGripScanSearchMove,   ///< 首轮扫码失败后，等待机械臂移动到 Y 轴搜码位置。
        PreGripScanSearchReturn, ///< 搜码成功后，等待机械臂回到原夹取位置。
        PreGripScanReturnCapture,///< 搜码全部失败后，等待机械臂回拍照位。
        RotateForScan,           ///< 安全开关开启时，首轮扫码失败后的 180° 独立补救动作。
        StowAfterPickup,         ///< 取料完成后等待机械臂收姿态。
        AgvToUnload,             ///< 等待 AGV 到倒料 LM。
        ArmUnload,               ///< 等待倒料准备点和倒料函数完成。
        StowAfterUnload,         ///< 倒料后等待机械臂收姿态。
        AgvToPallet,             ///< 等待 AGV 到对应码垛区 LM。
        PreparePalletPoint,      ///< 校验并获取下一码垛偏移。
        ArmPalletPlace,          ///< 等待机械臂放置空箱并松爪。
        CommitPallet,            ///< 将已成功放置的点位提交到缓存。
        StowAfterPallet,         ///< 码垛后等待机械臂最终收姿态。
        CleanupStow              ///< 任务级失败后的安全恢复；失败则升级系统 ERROR。
    };

    static constexpr int kAgvTimeoutMs = 120000; ///< 每个 AGV 导航步骤上限，单位 ms。
    /**
     * @brief 是否允许扫码首轮失败后执行工具 Rz 旋转 180° 的补救动作。
     *
     * 默认必须保持 false：现场实测表明该旋转可能撞击机械臂旁边的人员或物体。
     * 只有完成旋转空间清理、人员隔离和现场安全确认后，才允许改为 true，并需
     * 重新编译部署。此开关只影响扫码失败补救，不影响扫码成功后的正常夹取流程。
     */
    static constexpr bool kEnableScanFailureRotationRecovery = false;
    static constexpr double kScanSearchYOffsetMm = 20.0;
    static constexpr int kScanSearchTargetCount = 2;

    /// 清除单任务运行字段并回 Idle；不会清理 m_task，以便随后发送终态快照。
    void resetRuntimeState();
    QString prefix(const QString &stage) const;
    void setTaskRunning(TaskStep step, const QString &statusText);
    void setTaskTerminal(TaskState state,
                         const QString &statusText,
                         const QString &lastError);
    TaskStep taskStepForState(ExecState state) const;
    int stepIndexForTaskStep(TaskStep step) const;
    bool isAgvNavigationState(ExecState state) const;

    /// 从静态配置表解析当前工位和码垛区；失败时内部已发 systemError。
    bool resolveTaskConfigs();
    /// 进入非 AGV 状态并立即启动该状态对应的高层动作。
    void enterState(ExecState state, const QString &statusText);
    /// 初始化 AGV 到站跟踪、启动 120s 超时并发出数字 LM。
    void startAgvStep(ExecState state, int lm, const QString &statusText);
    /// 进入 PreGripScan 并发出带默认参数的异步扫码请求。
    void requestScan(const QString &statusText);
    NScanScheduler::ScanOptions defaultScanOptions() const;

    /// 校验码垛配置并获取下一个偏移；这里只读取，不推进 placedCount。
    void beginPalletPreparation();
    /// 任务级失败统一入口：保存原因并尝试收安全姿态。
    void beginCleanupAfterTaskFailure(const QString &reason);
    void startNextScanSearchMove(const QString &failureDetail);
    void finishScanSearchFailureAtCapture(const QString &reason);
    void resetScanSearchState();
    void finalizeTaskFailureAfterCleanup();
    void finishTaskSuccess();
    void raiseSystemError(const QString &reason);
    void clearPalletStackingFlag();

    AgvController *m_agv = nullptr;       ///< 非拥有指针；AGV 导航与监控来源。
    HuayanScheduler *m_arm = nullptr;     ///< 非拥有指针；机械臂高层动作来源。
    NScanScheduler *m_scanner = nullptr;  ///< 非拥有指针；保留依赖身份，实际扫码由上层 worker 执行。
    PalletScheduler *m_pallet = nullptr;  ///< 非拥有指针；码垛配置和缓存唯一实例。
    QTimer *m_agvTimeout = nullptr;       ///< 当前一个 AGV 步骤的单次超时计时器。
    Task m_task;                          ///< 当前任务的权威运行快照。
    ExecState m_state = ExecState::Idle;  ///< 当前单任务精细状态。
    const StationTaskConfig *m_stationCfg = nullptr;   ///< 指向静态工位配置，任务结束时清空。
    const PalletAreaTaskConfig *m_palletCfg = nullptr; ///< 指向静态码垛区配置。
    PalletPose m_pendingPalletOffset;                  ///< 本次放置尚未 commit 的相对偏移。
    int m_expectedLm = 0;                              ///< 当前 AGV 步骤必须到达的数字 LM。
    int m_scanAttempt = 0;                             ///< 当前扫码轮次，用于日志与诊断。
    int m_scanSearchIndex = 0;                         ///< 当前进行到第几个 Y 轴搜码目标。
    bool m_agvSeenMoving = false;                      ///< 已观察到本次导航进入 Waiting/Running。
    bool m_cleanupAfterTaskFailure = false;            ///< 当前收姿态是否属于失败清理路径。
    QString m_pendingTaskFailureReason;                ///< 清理成功后写入 taskFailed 的原始原因。
    QString m_lastScanFailureDetail;                   ///< 最近一次扫码失败原因。
    QString m_pendingScanCodeAfterSearch;              ///< 搜码成功后，回原夹取位完成前暂存条码。
};

#endif // TASKEXECUTOR_H
