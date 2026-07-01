#ifndef HUAYANSCHEDULER_H
#define HUAYANSCHEDULER_H

#include <QObject>
#include <QString>
#include <QStringList>

class QTimer;
struct PalletPose;

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
        QString captureFunc;     ///< 拍照初始位，同时用作夹取后的安全抬升位。
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
    /// 执行“码垛基准位→相对偏移→松爪”；不更新 PalletScheduler 缓存。
    void startPalletPlace(const PalletPose &offset);

    void startStageOne();
    void startStageTwo();
    void startStageThree();
    void startStow();    // 收运行姿态（Func_yun_xing_zhong）
    void startUnload();  // 倒料（Func_daoliao_1_point → Func_daoliao）
    void stop(bool emitStoppedLog = true);

    void setSurveyPose(const Pose &p);
    void releaseGripper();           // 手动松开夹爪（UI 按钮调用）
    void setSpeedOverride(int percent);  // 运动速度倍率 1~100(%)，可经 UI 实时调整
    void resetArm();                     // 机械臂复位（调用 Func_fuwei）

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
        LiftLoad,                ///< 回拍照位，将料箱抬到运输安全高度。
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
        RunPalletBase,
        MovePalletOffset,
        ReleasePallet,
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
    bool executeRunFunc(const QString &funcName, int timeoutMs = 30000);
    bool executeGripFunc();
    bool executeFlipUnload();
    bool executeStackingFunction();

    void startWaitForIdle(int timeoutMs = 30000);
    void resetAndProceed();  // GrpReset 后延时再下发首条指令，避开 20018 ProgramStopped
    void completeStage();    // 阶段收尾：先 stop() 再发完成信号，避免重置刚启动的下一阶段

    QString stageName(Stage stage) const;
    static int stepIndexFor(StageStep step);

    QTimer *m_pollTimer    = nullptr; ///< 每 100ms 查询机器人运动状态。
    QTimer *m_timeoutTimer = nullptr; ///< 当前单条 SDK 动作的超时保护。
    int     m_pollCount    = 0;       ///< 当前动作已轮询次数，用于极短动作兜底。
    bool    m_hasSeenMoving = false;  // 是否已观察到运动真正开始（避免启动延迟误判完成）

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
    QString m_unloadPointFuncName = QStringLiteral("Func_daoliao_1_point");
    QString m_unloadFuncName      = QStringLiteral("Func_daoliao");
};

#endif // HUAYANSCHEDULER_H
