#pragma once

#include "agvcontroller.h"
#include "chargepilecontroller.h"
#include "chargesettings.h"
#include "lineconfig.h"

#include <QList>
#include <QObject>
#include <QString>

/**
 * @brief 自动充电策略的一次完整输入快照。
 *
 * 该结构只承载已由各设备所有者采集的业务状态，不持有控制器、队列或套接字。
 * automaticSessionActive 必须只由自动启动请求被控制器接受后置位，手动查询和
 * 手动充电即使让控制器处于忙碌状态，也不得写入该字段。
 */
struct AutoChargeInputs {
    bool enabled = false;                ///< 本进程自动授权；软件启动时固定为 false。
    bool automaticSessionActive = false; ///< 已被业务层接受且尚未安全结束的自动会话。
    LineSystemState lineState = LineSystemState::Idle; ///< 主调度生命周期快照。
    bool hasAgvMonitor = false;          ///< agv 是否来自仍在新鲜期内的完整轮询。
    AgvMonitorData agv;                  ///< 同一轮电量、站点与导航状态快照。
    bool currentTaskRunning = false;     ///< 当前是否仍有必须先完成的送料任务。
    int pendingCount = 0;                ///< FIFO 中尚未执行的任务数量。
    bool chargeControllerBusy = false;   ///< 控制器处于查询或充电通信阶段。
    bool chargeControllerUnknown = false; ///< 控制器处于 Unknown/Fault 安全不确定态。
};
/**
 * @brief 无副作用策略函数输出的可组合动作集合。
 *
 * 布尔动作可以同时成立，例如低电量空闲时同时保持派单并请求返回 LM1。
 * statusText 用于界面解释当前决策；errorText 用于严重电量报警或系统故障详情。
 */
struct AutoChargeDecision {
    bool holdDispatch = false;      ///< 禁止 LineManager 从 FIFO 取下一单。
    bool requestReturnHome = false; ///< 使用主调度唯一导航入口返回 LM1。
    bool requestStartCharge = false; ///< 在 LM1 且导航空闲时提交自动开始意图。
    bool requestSafeStop = false;   ///< 将活动会话汇入控制器安全收尾。
    bool releaseDispatch = false;   ///< 安全结束且满足接单电量后允许恢复派单。
    bool raiseLineError = false;    ///< 设备状态无法安全解释时升级主调度 Error。
    QString statusText;             ///< 面板显示的当前策略原因。
    QString errorText;              ///< Error 或临界电量报警的详细上下文。
};

/**
 * @brief 根据同一时刻的业务快照计算自动充电意图。
 *
 * 函数不修改输入、不发送信号且不访问设备，可由测试穷举阈值边界。自动开关关闭
 * 且没有活动自动会话时严格返回全假动作，确保已有主调度逻辑完全旁路。
 */
AutoChargeDecision decideAutoCharge(const AutoChargeInputs &inputs,
                                    const ChargeSettings &settings);

/**
 * @brief 自动充电阈值策略与现有业务对象之间的解耦协调器。
 *
 * 协调器只保存最新快照并发出“意图”信号，不直接调用 LineManager、不读写任务
 * 队列，也不发送充电桩报文。DeviceManager 在任务六中负责把这些意图连接到唯一
 * 的调度和充电控制入口。
 */
class AutoChargeCoordinator : public QObject
{
    Q_OBJECT

public:
    explicit AutoChargeCoordinator(QObject *parent = nullptr);

    bool isEnabled() const;
    /// 返回协调器当前用于阈值决策的设置副本，不允许调用方直接修改内部状态。
    ChargeSettings appliedSettings() const { return m_settings; }

    /**
     * @brief 返回是否存在尚未安全完成的自动充电会话。
     *
     * 只有 automaticChargeStartRequested 被业务层明确接受后才返回 true；
     * 手动会话、只读查询及仅仅观察到控制器忙碌均不会改变该值。
     */
    bool automaticSessionActive() const;

public slots:
    /// 自动授权默认关闭；关闭活动自动会话时只进入安全收尾，不立即释放派单保持。
    void setEnabled(bool enabled);
    /// 应用已经由配置层校验并持久化成功的参数快照。
    void applySettings(const ChargeSettings &settings);
    /// 接收一轮字段一致的 AGV 监控快照。
    void onAgvMonitorUpdated(const AgvMonitorData &data);
    /// 显式使旧 AGV 快照失效；活动自动会话会因此安全收尾并上报系统故障。
    void onAgvMonitorLost(const QString &reason);
    /// 接收主调度生命周期状态；text 仅用于日志上下文。
    void onLineStateChanged(LineSystemState state, const QString &text);
    /// 从快照中分别统计 Running 当前任务和 Pending 待执行任务。
    void onQueueChanged(const QList<Task> &tasks);
    /// 控制器状态只用于抑制重复开始和识别未知风险，不用于推断自动会话来源。
    void onChargeControllerStateChanged(ChargePileController::State state,
                                        const QString &text);
    /**
     * @brief 接收 LineManager 对派单保持实际电平的排队确认。
     *
     * DeviceManager 必须使用 QueuedConnection，避免 Stop/enterError 在主调度
     * 状态尚未完成切换时同步触发策略重入。
     */
    void onDispatchHoldChanged(bool hold, const QString &reason);

    /**
     * @brief 业务层反馈自动启动意图是否被唯一控制器接受。
     *
     * 协调器在 accepted=true 后才拥有该自动会话；同步拒绝会保持派单，但等待
     * 下一次真实输入变化后再评估，避免直接连接时形成拒绝/重试递归。
     */
    void onAutomaticChargeStartResult(bool accepted, const QString &message);

    /**
     * @brief 接收控制器会话终态，并仅处理 Automatic 来源。
     *
     * safe=false 时仍保持 automaticSessionActive，直到后续确认完整安全收尾；
     * Manual 来源始终忽略，防止手动调试误释放自动派单保持。
     */
    void onChargeSessionFinished(bool safe,
                                 ChargePileController::SessionOrigin origin,
                                 const QString &message);

signals:
    /// 请求 LineManager 设置或解除“因充电暂缓派单”；协调器不直接接触队列。
    void dispatchHoldRequested(bool hold, const QString &reason);
    /// 请求 LineManager 使用其唯一导航所有权返回 LM1。
    void returnHomeRequested();
    /// 请求业务层向 ChargePileController 提交 Automatic 来源的开始会话。
    void automaticChargeStartRequested();
    /// 请求唯一控制器汇入安全收尾；原因用于保持控制器停止优先级。
    void automaticChargeSafeStopRequested(ChargePileController::StopReason reason);
    /// 不安全终态已结束原控制器会话时，请求建立一轮独立保守恢复会话。
    void automaticChargeConservativeRecoveryRequested(
        ChargePileController::SessionOrigin origin,
        ChargePileController::StopReason reason);
    /// 真正系统故障才请求主调度进入 Error；10%电量报警不通过此信号强停当前任务。
    void lineErrorRequested(const QString &reason);
    /// 电量小于等于 Roboshop 10%报警线时发出一次边沿报警。
    void criticalBatteryAlarm(const QString &reason);
    void decisionTextChanged(const QString &text);
    void logMessage(const QString &message);

private:
    /// 汇总最新输入并只在动作电平或一次性意图发生变化时发布信号。
    void evaluate();
    /// 根据关闭授权、任务到达阈值、正常目标或故障选择控制器停止原因。
    ChargePileController::StopReason currentStopReason() const;
    /// 将控制器阶段归并成策略层的“通信/会话忙碌”布尔量。
    static bool isControllerBusyState(ChargePileController::State state);

    ChargeSettings m_settings = ChargeSettings::defaults(); ///< 当前已生效阈值快照。
    AutoChargeInputs m_inputs;                              ///< 最近业务输入聚合。
    QString m_lineStateText;                                ///< 主调度文案，仅供日志。
    QString m_monitorLostReason;                            ///< 最近监控失效原因。
    QString m_lastDecisionText;                             ///< 抑制重复界面文案。

    bool m_dispatchHoldAsserted = false; ///< 已向调度发出的保持电平。
    bool m_returnIntentIssued = false;   ///< 同一返航条件只发一次边沿。
    bool m_startIntentPending = false;   ///< 等待业务层反馈启动是否被接受。
    bool m_startRejectedUntilInputChanges = false; ///< 防止同步拒绝递归重试。
    bool m_stopIntentIssued = false;     ///< 同一活动会话只提交一套安全收尾。
    bool m_lineErrorIssued = false;      ///< 同一故障条件只上报一次 Error。
    bool m_criticalAlarmIssued = false;  ///< 同一次 <=10%区间只报警一次。
    bool m_conservativeRecoveryIntentIssued = false; ///< 同一不安全终态只建一轮恢复。
    bool m_lowBatteryChargeRequired = false; ///< 当前任务低电后跨测量抖动保持充电需求。
};
