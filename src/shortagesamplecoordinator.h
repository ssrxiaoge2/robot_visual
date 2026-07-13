#ifndef SHORTAGE_SAMPLE_COORDINATOR_H
#define SHORTAGE_SAMPLE_COORDINATOR_H

#include "customSysScheduler.h"
#include "shortagetypes.h"

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QTimer>
#include <initializer_list>

/**
 * @brief 正式缺料同轮采样协调器。
 *
 * 该对象只消费唯一真实通信对象的 MES/PLC 响应，负责 roundId 隔离、
 * 四响应聚合、三选一校验、两轮稳定确认和通信状态上报；不会调用
 * ShortageEngine，也不会修改库存、补料单或 FIFO。
 */
class ShortageSampleCoordinator final : public QObject
{
    Q_OBJECT
public:
    /// scheduler 必须是唯一真实通信对象；工程中已不存在诊断对象或第二通信路径。
    explicit ShortageSampleCoordinator(CustomSysScheduler *scheduler,
                                        QObject *parent = nullptr);

    /// 更新后续轮次使用的通信参数；正在等待的轮次继续使用启动时快照。
    void setParameters(const ShortageParameters &parameters);

    /// 测试钩子：立即触发下一轮，避免自动化测试依赖真实 sleep。
    void triggerNextRoundForTest();
    /// 测试钩子：立即触发当前轮超时，失败语义与真实 QTimer 超时一致。
    void triggerRoundTimeoutForTest();
    /// 测试钩子：把连续失败起点向过去推进，用于验证红色报警边界。
    void advanceFailureDurationForTest(int seconds);
    /// 测试钩子：返回当前轮启动时锁定的超时秒数，确认参数只影响下一轮。
    int activeRoundTimeoutSecondsForTest() const { return m_activeRoundTimeoutSeconds; }
    /// 测试钩子：返回当前轮启动时锁定的间隔秒数，确认下一轮使用新参数。
    int activeRoundIntervalSecondsForTest() const { return m_activeRoundIntervalSeconds; }

public slots:
    void start(); ///< 立即开始首轮，此后按 sampleIntervalSeconds 启动。
    void stop();  ///< 终止当前轮并使迟到响应失效，不清除上层账本。

signals:
    /// 两轮稳定或既有稳定上下文下完成的样本；接收方自行选择目标 Engine。
    void stableSampleReady(ShortageSample sample);
    /// 当前整轮被拒绝；reason 必须说明字段、原值和维护/重试语义。
    void sampleRejected(quint64 roundId, QString reason);
    /// 通信可用性变化；只表达采样健康度，不直接改变库存。
    void communicationStateChanged(ShortageCommunicationState state, QString reason);
    /// 产品/模式两轮稳定确认；旧任务排空和是否激活由上层协调器决定。
    void contextChangeConfirmed(ProductModel product, ProductionMode mode);

private slots:
    void onMesReply(quint64 roundId, CustomSysScheduler::LiveMesDayReply reply);
    void onPlcReply(quint64 roundId, int startAddress, CustomSysScheduler::PlcBitReply reply);
    void onRoundTimeout();

private:
    struct RoundAccumulator {
        quint64 roundId = 0;                ///< 当前有效 roundId，迟到响应必须匹配才接收。
        bool active = false;                ///< false 表示已完成/拒绝，后续响应全部丢弃。
        bool hasMes = false;                ///< MES 日产量是否已到达。
        bool hasMode68 = false;             ///< L68/L69 模式位响应是否已到达。
        bool hasProduct71 = false;          ///< L71/L72/L73 产品位响应是否已到达。
        bool hasMode1998 = false;           ///< L1998 模式位响应是否已到达。
        qint64 actualQty = 0;               ///< 本轮 MES actualQty，必须非负且保持 64 位。
        bool l68 = false;                   ///< L/R 模式真实位。
        bool l69 = false;                   ///< L/L 模式真实位。
        bool l71 = false;                   ///< 88 产品真实位。
        bool l72 = false;                   ///< 88R 产品真实位。
        bool l73 = false;                   ///< 92 产品真实位。
        bool l1998 = false;                 ///< R/H 模式真实位。
        QDateTime startedAtUtc;             ///< 轮次启动时间，用于现场追踪。
    };

    void beginRound();
    void requestRound(quint64 roundId);
    void tryCompleteRound();
    void rejectActiveRound(const QString &reason);
    void rejectRecoveryNeedsReview(const QString &reason);
    void markCommunicationHealthy(const QString &reason);
    void markCommunicationFailed(const QString &reason);
    void scheduleNextRound();
    void handleCompletedRound(ProductModel product, ProductionMode mode);
    void emitStableSample(ProductModel product, ProductionMode mode, bool recoveredAfterInterruption);
    bool validatePlcRange(const CustomSysScheduler::PlcBitReply &reply,
                          int startAddress,
                          std::initializer_list<int> addresses,
                          QString *reason) const;
    bool resolveProduct(ProductModel *product, QString *reason) const;
    bool resolveMode(ProductionMode *mode, QString *reason) const;
    QString allBitValuesText() const;
    static QString bitText(const QString &name, bool value);
    static int boundedSampleIntervalSeconds(int value);
    static int boundedRoundTimeoutSeconds(int value, int intervalSeconds);
    static int boundedAlarmMinutes(int value);

    CustomSysScheduler *m_scheduler = nullptr; ///< 唯一真实通信对象，生命周期由 DeviceManager/测试持有。
    ShortageParameters m_parameters;           ///< 下一轮参数快照来源。
    QTimer m_roundTimeoutTimer;                ///< 当前轮等待上限，超时整轮失败。
    QTimer m_nextRoundTimer;                   ///< 成功/失败后按间隔启动下一轮。
    RoundAccumulator m_round;                  ///< 当前轮同 roundId 聚合状态。
    quint64 m_nextRoundId = 1;                 ///< 单调递增轮次号。
    int m_activeRoundTimeoutSeconds = 0;       ///< 当前轮锁定的超时秒数。
    int m_activeRoundIntervalSeconds = 0;      ///< 当前轮锁定的下一轮间隔秒数。
    bool m_running = false;                    ///< stop 后迟到响应失效且不再自动采样。
    bool m_hasCandidateContext = false;        ///< 第一轮合法组合候选是否存在。
    ProductModel m_candidateProduct = ProductModel::Model88; ///< 候选产品。
    ProductionMode m_candidateMode = ProductionMode::LeftRight; ///< 候选模式。
    int m_candidateCount = 0;                  ///< 连续相同候选轮数，达到 2 才确认。
    bool m_hasStableContext = false;           ///< 是否已有两轮稳定上下文。
    ProductModel m_stableProduct = ProductModel::Model88; ///< 当前已确认产品。
    ProductionMode m_stableMode = ProductionMode::LeftRight; ///< 当前已确认模式。
    bool m_hasSampleBaseline = false;          ///< 是否已有用于断线恢复比较的样本基线。
    ProductModel m_baselineProduct = ProductModel::Model88; ///< 基线所属产品。
    ProductionMode m_baselineMode = ProductionMode::LeftRight; ///< 基线所属模式。
    qint64 m_lastStableActualQty = 0;          ///< 最近一次已输出稳定样本 actualQty。
    bool m_interruptedSinceLastSample = false; ///< true 时恢复回退必须进入维护确认。
    QDateTime m_failureStartedAtUtc;           ///< 连续通信失败起点，达到配置分钟数转报警。
    ShortageCommunicationState m_state = ShortageCommunicationState::Stopped; ///< 最近发布状态。
};

Q_DECLARE_METATYPE(ShortageSample)
Q_DECLARE_METATYPE(ShortageCommunicationState)
Q_DECLARE_METATYPE(ProductModel)
Q_DECLARE_METATYPE(ProductionMode)

#endif // SHORTAGE_SAMPLE_COORDINATOR_H
