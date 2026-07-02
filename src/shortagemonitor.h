#ifndef SHORTAGEMONITOR_H
#define SHORTAGEMONITOR_H

#include <QHash>
#include <QList>
#include <QObject>
#include <QTimer>

#include "customSysScheduler.h"
#include "shortagecalculator.h"

/**
 * @brief 现场缺料信号轮询聚合器。
 *
 * 该对象运行在 UI 主线程，持有 5 秒轮询定时器和 3 秒单轮超时定时器。
 * 它只负责聚合四个 HTTP 结果、驱动 ShortageCalculator，并把“是否接受 FIFO”
 * 的确认语义同步回写；它不等待送料完成。
 */
class ShortageMonitor : public QObject
{
    Q_OBJECT
public:
    explicit ShortageMonitor(CustomSysScheduler *client,
                             QObject *parent = nullptr);

    bool isRunning() const;
    QList<StationConsumption> snapshot() const;

public slots:
    void start();
    void stop();
    void confirmShortageAccepted(int stationId, bool accepted);

signals:
    void shortageRequested(int stationId);
    void statusChanged(QString text, bool healthy);
    void sampleUpdated(qint64 actualQty, ProductModel product,
                       ProductionMode mode, QHash<QString, bool> bits);
    void consumptionUpdated(QList<StationConsumption> stations);
    void logMessage(QString message);

private slots:
    void onPollTimerTimeout();
    void onRoundTimeout();
    void onMesReplyReady(quint64 roundId, bool ok, qint64 actualQty, QString error);
    void onPlcReplyReady(quint64 roundId, int startAddress,
                         CustomSysScheduler::PlcBitReply result);

private:
    struct RoundState {
        quint64 roundId = 0;                     ///< 递增轮次号，防止旧响应混入新轮次。
        bool mesReceived = false;                ///< 当前轮次是否已收到 MES 结果。
        bool mesOk = false;                      ///< MES 结果是否成功可用。
        qint64 actualQty = 0;                    ///< MES 返回的 actualQty。
        bool plc68Received = false;              ///< L68/L69 分片是否已完成。
        bool plc71Received = false;              ///< L71/L72/L73 分片是否已完成。
        bool plc1998Received = false;            ///< L1998 分片是否已完成。
        CustomSysScheduler::PlcBitReply plc68;   ///< StartAddress=68 分片结果。
        CustomSysScheduler::PlcBitReply plc71;   ///< StartAddress=71 分片结果。
        CustomSysScheduler::PlcBitReply plc1998; ///< StartAddress=1998 分片结果。
    };

    void beginRound();
    void failRound(const QString &reason);
    void completeRoundIfReady();
    bool chooseProduct(const QHash<QString, bool> &bits,
                       ProductModel *product,
                       QString *error) const;
    bool chooseMode(const QHash<QString, bool> &bits,
                    ProductionMode *mode,
                    QString *warning,
                    QString *error) const;
    void emitPendingStations(const QList<int> &stations);
    void emitSnapshot();
    bool hasIncompleteCurrentRound() const;

    CustomSysScheduler *m_client = nullptr; ///< 非拥有指针；网络请求由 DeviceManager 统一持有。
    QTimer *m_pollTimer = nullptr;          ///< 5 秒轮询定时器；未完成轮次不会重叠启动。
    QTimer *m_roundTimeout = nullptr;       ///< 3 秒单轮超时定时器。
    ShortageCalculator m_calculator;        ///< 纯计算器；不触网、不关心送料完成。
    RoundState m_round;                     ///< 当前正在等待的四请求聚合状态。
    bool m_running = false;
    bool m_hasLastSample = false;
    ShortageSample m_lastSample;
    QString m_lastRejectedReason;           ///< 同一拒收原因去重，避免日志刷屏。
    QString m_lastModeWarning;              ///< 多生产方式 true 的警告去重。
};

#endif // SHORTAGEMONITOR_H
