#ifndef SHORTAGETESTSESSION_H
#define SHORTAGETESTSESSION_H

#include <QHash>
#include <QList>
#include <QObject>
#include <QTimer>

#include "customSysScheduler.h"
#include "shortagecalculator.h"

/**
 * @brief 缺料信号测试会话。
 *
 * 该类只用于测试面板：
 * - 只轮询 MES/PLC 并驱动纯计算器；
 * - 只发出测试状态、样本和库存快照；
 * - 不派单、不持久化、不接入主调度。
 */
class ShortageTestSession : public QObject
{
    Q_OBJECT
public:
    explicit ShortageTestSession(CustomSysScheduler *client, QObject *parent = nullptr);

    bool isRunning() const;
    QList<StationConsumption> snapshot() const;

public slots:
    void start();
    void stop();

signals:
    void statusChanged(QString text, bool healthy);
    void sampleUpdated(qint64 actualQty,
                       qint64 delta,
                       ProductModel product,
                       ProductionMode mode,
                       QHash<QString, bool> bits);
    void inventoryUpdated(QList<StationConsumption> stations);

private slots:
    void onPollTimerTimeout();
    void onRoundTimeout();
    void onMesReplyReady(quint64 roundId, bool ok, qint64 actualQty, QString error);
    void onPlcReplyReady(quint64 roundId,
                         int startAddress,
                         CustomSysScheduler::PlcBitReply result);

private:
    enum class StableState {
        Unknown,
        WaitingSecondRound,
        Confirmed
    };

    struct RoundState {
        quint64 roundId = 0;          ///< 当前测试轮次号；旧轮次响应必须丢弃。
        quint64 sessionId = 0;        ///< 当前 start/stop 会话号；stop 后旧轮次失效。
        bool mesReceived = false;     ///< 本轮是否收到 MES 响应。
        qint64 actualQty = 0;         ///< 本轮 actualQty 原值。
        bool plc68Received = false;   ///< 本轮是否收到 L68/L69 分片。
        bool plc71Received = false;   ///< 本轮是否收到 L71/L72/L73 分片。
        bool plc1998Received = false; ///< 本轮是否收到 L1998 分片。
        CustomSysScheduler::PlcBitReply plc68;
        CustomSysScheduler::PlcBitReply plc71;
        CustomSysScheduler::PlcBitReply plc1998;
    };

    void beginRound();
    void completeRoundIfReady();
    void failRound(const QString &reason);
    bool hasIncompleteCurrentRound() const;
    void emitInventory();
    bool chooseProduct(const QHash<QString, bool> &bits,
                       ProductModel *product,
                       QString *error) const;
    bool chooseMode(const QHash<QString, bool> &bits,
                    ProductionMode *mode,
                    QString *error) const;

    CustomSysScheduler *m_client = nullptr; ///< 非拥有指针；只发起测试 HTTP/PLC 请求。
    QTimer *m_pollTimer = nullptr;          ///< 5 秒测试轮询定时器。
    QTimer *m_roundTimeout = nullptr;       ///< 单轮 3 秒超时定时器。
    ShortageCalculator m_calculator;        ///< 测试专用纯计算器，不含派单语义。
    RoundState m_round;                     ///< 当前轮次聚合状态。
    bool m_running = false;                 ///< true 表示测试会话已启动。
    quint64 m_sessionId = 0;                ///< 每次 start 递增，用于隔离 stop 前旧响应。
    qint64 m_lastAcceptedActualQty = 0;     ///< 最近一次完成轮次的 actualQty。
    bool m_hasAcceptedActualQty = false;    ///< 是否已有上一轮 actualQty 可计算 delta。
    StableState m_stableState = StableState::Unknown; ///< 当前产品/方式稳定确认状态。
    ProductModel m_candidateProduct = ProductModel::Model88; ///< 等待第二轮确认的候选产品。
    ProductionMode m_candidateMode = ProductionMode::L68;    ///< 等待第二轮确认的候选方式。
    ProductModel m_confirmedProduct = ProductModel::Model88; ///< 已确认产品。
    ProductionMode m_confirmedMode = ProductionMode::L68;    ///< 已确认方式。
};

#endif // SHORTAGETESTSESSION_H
