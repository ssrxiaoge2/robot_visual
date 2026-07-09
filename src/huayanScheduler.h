#ifndef HUAYANSCHEDULER_H
#define HUAYANSCHEDULER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include "lineconfig.h"
#include "palletscheduler.h"

class QTimer;

/**
 * @brief 华研机械臂高层动作调度器。
 *
 * 对外暴露“取料、收姿态、倒料、码垛”等业务动作，对内把示教器函数、相对运动
 * 和异步到位轮询组织成状态机。上层不得绕过本类直接并发调用 HRIF 运动原语。
 * 本对象工作在 UI 主线程；SDK 指令非阻塞，下发后由定时器轮询运动状态。
 */
class HuayanScheduler : public QObject
{
    Q_OBJECT

public:
    /// 可独立启动的机械臂业务阶段；任一时刻最多运行一个 Stage。
    enum class Stage {
        None,       ///< 空闲，没有阶段动作。
        StageOne,   ///< 视觉定位、扫码暂停、夹紧并抬升料箱。
        StageTwo,   ///< 旧流程的组合卸料阶段，保留给测试面板。
        StageThree, ///< 旧流程的码垛脚本阶段，保留兼容性。
        Stow,       ///< 调用运行中安全姿态函数。
        Unload      ///< 调用工位倒料点和倒料函数。
    };

    /// 笛卡尔位姿，平移单位 mm、旋转单位 deg。
    struct Pose {
        double x = 0;
        double y = 0;
        double z = 0;
        double rx = 0;
        double ry = 0;
        double rz = 0;
    };

    /// 当前任务按工位注入的示教器函数名。
    struct StationArmFunctions {
        QString captureFunc;     ///< 拍照初始位；CaptureFunc 模式下也作为夹后安全离开位。
        AfterGripMode afterGripMode = AfterGripMode::CaptureFunc; ///< 夹紧后离开策略；默认兼容旧逻辑。
        QString afterGripFunc;                                    ///< CustomFunc 模式下使用的夹后安全离开函数。
        double grabZClearance = 425.0;                            ///< 本工位 Z 下探余量(mm)。
        QString unloadPointFunc; ///< 倒料前准备点。
        QString unloadFunc;      ///< 实际翻转/倾倒动作。
    };

    /// 当前码垛区注入的示教器函数名。
    struct PalletArmFunctions {
        QString palletBaseFunc; ///< 码垛相对偏移的零点/基准位。
        QString releaseFunc;    ///< 空箱到位后的松爪动作。
    };

    explicit HuayanScheduler(QObject *parent = nullptr);
    ~HuayanScheduler() override;

    void setConnectionParams(const QString &ip,
                             unsigned short port = 10003,
                             unsigned int boxID = 0,
                             unsigned int rbtID = 0);

    bool connectRobot();
    bool disconnectRobot();
    bool isConnected() const;

    void setStackingFunction(const QString &funcName,
                             const QStringList &params = QStringList());

    bool startRobotScript();
    bool stopRobotScript();

    /// 仅覆盖非空字段，保留默认函数供单机调试。
    void setStationFunctions(const StationArmFunctions &funcs);
    /// 注入当前任务使用的码垛基准与松爪函数。
    void setPalletFunctions(const PalletArmFunctions &funcs);
    /// 控制 StageOne 是否在夹紧前停住并请求扫码。
    void setPreGripScanEnabled(bool enabled);
    /// 仅在 StageOne/WaitPreGripScan 生效，继续夹紧和抬升。
    void continueAfterPreGripScan();
    /// 扫码补救独立动作：工具坐标系 Rz 相对旋转 180°，不丢失暂停中的 StageOne。
    void rotateToolRz180();
    /// 夹紧前扫码失败时，沿工具系 Y 轴移动到指定搜索偏移。
    void movePreGripScanSearchTo(double targetYOffsetMm);
    /// 扫码搜索全部失败时，回到拍照位，后续失败收尾由上层决定。
    void returnToCaptureForScanFailure();
    /**
     * @brief 执行一次标准空箱码垛动作。
     *
     * 标准动作链：安全位夹紧 -> 码垛基准点 -> XY 到目标上方 -> Z 到释放高度
     * -> 松爪 -> Z 抬升 -> Func_yun_xing_zhong 回运行安全位。
     * 本函数只执行机械臂动作，不更新 PalletScheduler 缓存；调用方必须在
     * palletPlaceCompleted() 后再 commitPlaced()。
     */
    void startPalletPlace(const PalletPose &targetOffset, double releaseZOffsetMm);

    void startStageOne();
    void startStageTwo();
    void startStageThree();
    void startStow();    // 收运行姿态（Func_yun_xing_zhong）
    /// 只覆盖下一次 startStow() 调用使用的函数；执行后自动恢复默认收姿态函数。
    void setNextStowFunction(const QString &funcName);
    void startUnload();  // 倒料（Func_daoliao_1_point → Func_daoliao）
    void stop(bool emitStoppedLog = true);

    void setSurveyPose(const Pose &p);
    void releaseGripper();           // 手动松开夹爪（UI 按钮调用）
    void setSpeedOverride(int percent);  // 运动速度倍率 1~100(%)，可经 UI 实时调整
    void resetArm();                     // 机械臂复位（调用 Func_fuwei）

    /// 计算抓取 Z 下探量。
    ///
    /// 公式：下探量 = 视觉深度 - 工位余量；结果被限制在 0 到 maxDescend。
    /// 余量越大，下探越少；余量越小，下探越多。
    static double calculateGrabDescend(double visionDepth,
                                       double grabZClearance,
                                       double maxDescend)
    {
        return qBound(0.0, visionDepth - grabZClearance, maxDescend);
    }

    /// 根据夹后策略解析实际要调用的函数名。
    ///
    /// shouldRun 返回 false 表示夹紧后不调用任何函数，阶段一可直接完成。
    /// CaptureFunc 返回拍照函数；CustomFunc 返回独立夹后安全离开函数。
    static QString resolveAfterGripFunction(AfterGripMode mode,
                                            const QString &captureFunc,
                                            const QString &afterGripFunc,
                                            bool *shouldRun)
    {
        if (mode == AfterGripMode::None) {
            if (shouldRun)
                *shouldRun = false;
            return QString();
        }
        if (shouldRun)
            *shouldRun = true;
        return mode == AfterGripMode::CustomFunc ? afterGripFunc : captureFunc;
    }

    /// 命令门控判定结果。
    enum class CommandReadiness {
        ReadyToDispatch, ///< 当前状态允许立即下发下一条命令。
        Wait,            ///< 当前状态暂不允许下发，继续等待。
        Error            ///< 状态信息本身不可用，必须 fail-closed。
    };

    /// 纯判定：根据机器人状态决定当前命令是否允许下发。
    static CommandReadiness evaluateCommandReadiness(int movingState,
                                                     int pauseState,
                                                     int fsmRet,
                                                     const QString &fsmText)
    {
        if (movingState != 0 || pauseState != 0)
            return CommandReadiness::Wait;
        if (fsmRet != 0)
            return CommandReadiness::Error;
        const QString normalizedFsm = fsmText.trimmed().toLower();
        if (normalizedFsm.contains(QStringLiteral("programstopped"))
            || normalizedFsm.contains(QStringLiteral("robotinmoving")))
            return CommandReadiness::Wait;
        if (normalizedFsm.contains(QStringLiteral("auto"))
            || normalizedFsm.contains(QStringLiteral("standby"))
            || normalizedFsm.contains(QStringLiteral("ready"))
            || normalizedFsm.contains(QStringLiteral("idle")))
            return CommandReadiness::ReadyToDispatch;
        return CommandReadiness::Wait;
    }

    /// 纯判定：统一命令门控是否还能登记新的待执行命令。
    ///
    /// 这里显式拒绝覆盖旧待命令，避免 UI 手动命令或阶段切换边缘把上一条还未真正下发的命令挤掉。
    static bool canQueuePendingCommand(bool hasPendingCommand, bool hasActiveCommand)
    {
        return !hasPendingCommand && !hasActiveCommand;
    }

public slots:
    void setGrabOffset(double x, double y, double z, double rz);
    void onVisionNoObject();
    void onVisionErrorForPickup(const QString &msg);

signals:
    void connected();
    void disconnected();
    void stageStarted(const QString &stageName);
    void stageCompleted(const QString &stageName);
    void stageError(const QString &msg);
    void logMessage(const QString &msg);
    void schedulerStopped();
    void surveyReady();             ///< 已稳定到拍照位，请 VisionHttpClient 发起推理。
    void preGripScanRequested();    ///< 已到夹紧前安全暂停点，请上层异步扫码。
    void toolRotationCompleted();
    void toolRotationError(const QString &reason);
    void preGripScanSearchMoveCompleted(double currentYOffsetMm);
    void preGripScanSearchMoveError(const QString &reason);
    void preGripScanCaptureReturnCompleted();
    void preGripScanCaptureReturnError(const QString &reason);
    void palletPlaceCompleted();
    void palletPlaceError(const QString &reason);

    /// 流程图步骤索引变化（0-4，对应 WorkflowWidget 节点），供 UI 高亮
    void stepChanged(int stepIdx);

private slots:
    void onPollTick();
    void onStepTimeout();

private:
    /// Stage 内部步骤；advanceStep() 定义每个阶段允许的唯一前进方向。
    enum class StageStep {
        None,                    ///< 当前阶段步骤耗尽，下一次执行将完成阶段。
        MoveToSurvey,            ///< 调用拍照位示教函数。
        WaitForVision,           ///< 机械臂静止，等待视觉成功/无目标/错误回调。
        SearchDescend,           ///< 无目标时沿 Z 搜索下移。
        MoveToGrab,              ///< 按 X/Y/Rz 分轴执行视觉闭环偏移。
        DescendZ,                ///< 根据视觉深度向夹取高度下探。
        WaitPreGripScan,         ///< 夹紧前安全暂停，等待扫码决策。
        MoveToPickup,            ///< 旧步骤名，保留枚举兼容性。
        CloseGripper,            ///< 调用夹紧示教函数。
        LiftLoad,                ///< 按工位夹后策略离开抓取位，必要时回安全高度。
        MoveToUnload,            ///< 旧 StageTwo 的卸料位移动。
        FlipUnload,              ///< 旧 StageTwo 翻转动作。
        ReleaseLoad,             ///< 旧 StageTwo 松爪动作。
        MoveEmptyBox,            ///< 旧 StageTwo 空箱移动。
        ExecuteStackingFunction, ///< 旧 StageThree 码垛脚本。
        StowArm,                 ///< 调用运行中安全姿态函数。
        MoveToUnloadPoint,       ///< 新主流程调用工位倒料准备点。
        RunUnloadFunc            ///< 新主流程调用工位倒料函数。
    };

    // 工具坐标系单轴相对运动（HRIF_MoveRelL），用于视觉偏移的分轴串联微调
    struct RelMove {
        int poseId;     // 0~5 = X/Y/Z/Rx/Ry/Rz
        int direction;  // 0=负向, 1=正向
        double distance;
    };

    /// 可在 StageOne 扫码暂停期间运行的独立动作状态机。
    enum class Action {
        None,                           ///< 无独立动作。
        RotateTool,                     ///< 扫码补救旋转。
        PalletPlace,                    ///< 空箱码垛放置。
        PreGripScanSearchMove,          ///< 夹紧前扫码搜索位移动。
        ReturnToCaptureForScanFailure   ///< 扫码搜索失败后回拍照位。
    };

    enum class ActionStep {
        None,
        RotateTool,
        ClampPalletAtSafety,
        RunPalletBase,
        MovePalletXY,
        DescendPalletZ,
        ReleasePallet,
        LiftAfterPalletRelease,
        StowAfterPalletRelease,
        MovePreGripScanSearchY,
        RunCaptureForScanFailure
    };

    void proceedStage();
    void advanceStep();
    void executeCurrentStep();
    bool executeNextGrabMove();
    void proceedAction();
    void advanceActionStep();
    bool executeNextPalletMove();
    bool rejectStageStartWhileActionRunning(const QString &stageName);
    void clearActionState();
    void finishAction();
    void actionError(const QString &msg, bool stopRobot = false);
    void stopPollingAndTimers();
    void requestRobotStop();
    void emitOperationError(const QString &msg);

    void setPickupPose(const Pose &p);
    Pose pickupPose() const;

    void setPickupLiftPose(const Pose &p);
    Pose pickupLiftPose() const;

    void setUnloadPose(const Pose &p);
    Pose unloadPose() const;

    void setEmptyBoxPose(const Pose &p);
    Pose emptyBoxPose() const;

    bool ensureConnected();
    bool executeMoveJ(double x, double y, double z,
                      double rx, double ry, double rz,
                      const QString &cmdId   = QStringLiteral("0"),
                      const QString &ucsName = QStringLiteral("Base"));
    bool setGripper(bool open);

    /// 待下发的 SDK 运动命令类型。
    ///
    /// RunFunc：调用示教器函数，如 Func_captureX / Func_daoliaoX。
    /// MoveRelTool：工具坐标系相对移动，视觉微调、搜索、Z 下探使用。
    /// MoveRelBase：基坐标系相对移动，码垛 offset 使用。
    /// MoveJ：绝对笛卡尔 MoveJ，倒料位/空箱位等示教点使用。
    enum class PendingCommandKind {
        None,
        RunFunc,
        MoveRelTool,
        MoveRelBase,
        MoveJ
    };

    /// 统一命令门控使用的待执行命令。
    ///
    /// timeoutMs 是命令执行后的到位等待超时，不是状态门控超时。
    struct PendingCommand {
        PendingCommandKind kind = PendingCommandKind::None;
        QString label;        ///< 日志标签，说明阶段和动作，便于现场追踪 20018。
        QString funcName;     ///< kind=RunFunc 时使用。
        int poseId = 0;       ///< kind=MoveRelTool/MoveRelBase 时使用，0~5=X/Y/Z/Rx/Ry/Rz。
        int direction = 1;    ///< 相对移动方向，0=负向，1=正向。
        double distance = 0;  ///< 相对移动距离(mm或deg，取决于 poseId)。
        Pose targetPose;      ///< kind=MoveJ 时使用的绝对目标位姿。
        QString cmdId = QStringLiteral("0");      ///< kind=MoveJ 时透传给 SDK 的命令编号。
        QString ucsName = QStringLiteral("Base"); ///< kind=MoveJ 时使用的用户坐标系。
        int timeoutMs = 30000; ///< 到位等待超时，单位 ms，默认 30000ms。
        QStringList params;   ///< 兼容旧码垛脚本等带参数的 RunFunc。
    };

    struct RobotStateSnapshot {
        // 现场诊断用快照，不参与运动决策本身。
        int movingState = 0;
        int pauseState = 0;
        int errorState = 0;
        int errorCode = 0;
        int nCurFSM = 0;
        QString strCurFSM;
        bool valid = false;
    };

    /// 在真正下发 SDK 命令前先做一次控制器状态门控。
    ///
    /// 该入口只负责登记待执行命令并启动/立即执行状态检查，不会直接调用 SDK 运动原语；
    /// 若控制器仍处于 moving/pause/ProgramStopped，则延后到可执行时再下发，避免串行命令撞 20018。
    bool beginCommandWhenReady(const PendingCommand &cmd);
    /// 轮询控制器当前是否允许下发下一条命令。
    ///
    /// 检查 moving/pause/error/FSM 状态；若发现 ProgramStopped，仅在本轮命令门控中执行一次 GrpReset，
    /// 并持续等待可执行状态。出现错误或超时会清空待命令并走统一错误出口。
    bool pollCommandReady();
    /// 在命令门控通过后，执行真实的 SDK 下发。
    ///
    /// 只有这个入口允许真正调用 HRIF_RunFunc / HRIF_MoveRelL；成功后接管到位轮询，
    /// 失败则立即走统一错误处理，确保所有运动命令的安全语义一致。
    bool dispatchReadyCommand(const PendingCommand &cmd);
    bool hasActiveRobotCommand() const; ///< 当前是否仍有已下发但尚未完成的 SDK 命令。
    void stopVisionWaitTimeout();       ///< 收到视觉结果后关闭 WaitForVision 的超时保护，避免误判为执行中命令。
    RobotStateSnapshot readRobotStateSnapshot() const;
    QString formatRobotStateSnapshot(const RobotStateSnapshot &snapshot) const;

    bool executeRunFunc(const QString &funcName, int timeoutMs = 30000);
    bool executeGripFunc();
    bool executeFlipUnload();
    bool executeStackingFunction();

    void startWaitForIdle(int timeoutMs = 30000);
    void resetAndProceed();  // GrpReset 后延时再下发首条指令，避开 20018 ProgramStopped
    void completeStage();    // 阶段收尾：先 stop() 再发完成信号，避免重置刚启动的下一阶段
    quint64 nextCallbackSeq(); ///< 生成延迟回调序号，避免旧 singleShot 推进新阶段。

    QString stageName(Stage stage) const;
    static int stepIndexFor(StageStep step);

    QTimer *m_pollTimer    = nullptr; ///< 每 100ms 查询机器人运动状态。
    QTimer *m_timeoutTimer = nullptr; ///< 当前单条 SDK 动作的超时保护。
    QTimer *m_commandReadyTimer = nullptr; ///< SDK 命令下发前的状态门控轮询定时器。
    int     m_pollCount    = 0;       ///< 当前动作已轮询次数，用于极短动作兜底。
    bool    m_hasSeenMoving = false;  // 是否已观察到运动真正开始（避免启动延迟误判完成）
    PendingCommand m_pendingCommand;       ///< 当前等待状态可执行后再下发的命令。
    PendingCommandKind m_activeCommandKind = PendingCommandKind::None;
    QString m_activeCommandLabel;
    bool m_loggedRunFuncScriptRunning = false;
    int m_commandReadyElapsedMs = 0;       ///< 已等待可执行状态的时间(ms)。
    bool m_commandResetIssued = false;     ///< 本轮门控是否已对 ProgramStopped 执行过 GrpReset。
    quint64 m_commandSeq = 0;              ///< 命令序号，防止旧 singleShot 回调推进新阶段。

    Stage m_stage = Stage::None;              ///< 当前业务阶段。
    StageStep m_stageStep = StageStep::None;  ///< 当前阶段内步骤。

    Pose m_surveyPose;             ///< 旧硬编码拍照位，主流程优先使用示教函数。
    Pose m_grabOffset;             ///< 最近视觉结果转换后的工具系偏移（mm/deg）。
    QList<RelMove> m_grabMoves;    ///< 本轮视觉微调拆分出的单轴动作序列。
    int m_grabMoveIdx = 0;         ///< 下一条待执行视觉微调索引。
    int m_grabIterations = 0;   // 闭环视觉矫正的迭代计数
    int m_searchDescendCount = 0;   // 找目标保护搜索的次数，不参与抓取闭环迭代
    double m_searchDescendedMm = 0.0;  // 找目标累计下移量(mm)，不是抓取 Z 下探量
    bool m_pendingLargeRzConfirmation = false; ///< 上一帧是否出现待确认的 Rz 大角度跳变。
    double m_pendingLargeRz = 0.0;             ///< 待确认的 Rz 大角度跳变值(deg)。
    bool m_preGripScanEnabled = false; ///< 是否启用夹紧前扫码暂停。
    bool m_waitingPreGripScan = false; ///< 已发扫码请求且尚未收到继续指令。
    double m_preGripScanSearchCurrentY = 0.0; ///< 当前夹紧前扫码搜索 Y 偏移(mm)。
    double m_preGripScanSearchTargetY = 0.0;  ///< 本轮夹紧前扫码搜索目标 Y 偏移(mm)。
    Pose m_pickupPose;
    Pose m_pickupLiftPose;
    Pose m_unloadPose;
    Pose m_emptyBoxPose;
    QList<RelMove> m_palletMoves;          ///< X/Y/Z/Rz 顺序的码垛相对动作。
    int m_palletMoveIdx = 0;               ///< 下一条待执行码垛偏移索引。
    PalletPose m_pendingPalletTargetOffset; ///< 本次码垛目标层中心偏移。
    double m_pendingPalletReleaseZ = 0.0;   ///< 本次真实松爪相对 Z。
    Action m_action = Action::None;         ///< 当前独立动作。
    ActionStep m_actionStep = ActionStep::None; ///< 独立动作内步骤。

    QString m_ip;                       ///< 华研控制箱 IP。
    unsigned short m_port = 10003;      ///< 华研 SDK 固定服务端口。
    unsigned int m_boxID = 0;           ///< SDK 控制箱编号。
    unsigned int m_rbtID = 0;           ///< SDK 机器人组编号。
    bool m_connected = false;           ///< SDK 会话状态，不代表机器人无报警。
    int  m_speedPercent = 100;   // 运动速度倍率(%)，连接时应用，可经 UI 实时调整

    QString m_stackingFuncName;
    QStringList m_stackingParams;

    // 示教器脚本与函数名（方案 C：上位机调用示教器内已示教好的函数）
    QString m_scriptName      = QStringLiteral("dashiceshi");
    QString m_captureFuncName = QStringLiteral("Func_capture");
    QString m_gripFuncName    = QStringLiteral("Func_jiajin");
    QString m_releaseFuncName = QStringLiteral("Func_songzhua");
    QString m_palletBaseFuncName;

    QString m_stowFuncName        = QStringLiteral("Func_yun_xing_zhong");
    QString m_nextStowFuncName;
    QString m_unloadPointFuncName = QStringLiteral("Func_daoliao_1_point");
    QString m_unloadFuncName      = QStringLiteral("Func_daoliao");
    AfterGripMode m_afterGripMode = AfterGripMode::CaptureFunc; ///< 当前任务夹紧后离开策略，由 lineconfig 注入。
    QString m_afterGripFuncName;                                ///< 当前任务夹后安全离开函数名，CustomFunc 时必须非空。
    double m_grabZClearance = 425.0;                            ///< 当前任务 Z 下探余量(mm)，替代全局固定值。
};

#endif // HUAYANSCHEDULER_H
