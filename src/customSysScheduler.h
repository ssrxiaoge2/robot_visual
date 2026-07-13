#ifndef CUSTOM_SYS_SCHEDULER_H
#define CUSTOM_SYS_SCHEDULER_H

#include <QMap>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>

/**
 * @brief 客户现场 MES/PLC 真实缺料通信协议层。
 *
 * 该类只负责一次性 HTTP GET、严格 JSON 解析和 roundId 透传；
 * 业务采样、库存账本和 FIFO 任务由后续协调器/引擎接入。
 */
class CustomSysScheduler : public QObject
{
    Q_OBJECT
public:
    /// MES 日数据解析结果；真实账本数量必须使用 64 位有符号整数。
    struct LiveMesDayReply {
        bool ok = false;       ///< true 表示 HTTP 和 JSON 契约完整。
        qint64 actualQty = 0;  ///< 非负累计产量；负值或溢出文本解析失败。
        QString errorMessage;  ///< 失败字段、原值和原因；成功时为空。
    };

    /// PLC 位读取结果；values 的 key 必须完整覆盖请求地址范围。
    struct PlcBitReply {
        bool ok = false;       ///< true 表示 success/timestamp/data 均合法。
        QString timestamp;     ///< 客户返回时间文本，只记录不参与库存判断。
        QMap<int, bool> values;///< address 到布尔位；缺失/重复/越界整包失败。
        QString errorMessage;  ///< 失败字段、地址和处理动作。
    };

    explicit CustomSysScheduler(QObject *parent = nullptr);
    /// 返回真实缺料 `.228` 日数据默认值；工程中不再提供 `.229` 默认函数。
    static QUrl defaultLiveMesDayEndpoint();
    /// 仅替换固定 PLC path，保留真实 MES URL 的 scheme/host/port。
    static QUrl livePlcBitEndpointFor(const QUrl &liveMesDayEndpoint);
    /// 只接受完整 HTTP/HTTPS 日数据 URL；失败不覆盖当前有效地址。
    bool setLiveMesDayEndpoint(const QUrl &endpoint, QString *errorMessage = nullptr);
    /// 请求级超时必须为正数；默认 5000 ms，失败不覆盖当前有效值。
    bool setRequestTimeoutMs(int timeoutMs, QString *errorMessage = nullptr);
    /// 纯函数解析 MES payload，供协议测试和网络完成回调共同复用。
    static LiveMesDayReply parseMesDayReply(const QByteArray &payload);
    /// 纯函数解析 PLC payload，并严格校验请求范围内地址唯一且完整。
    static PlcBitReply parsePlcBitReply(const QByteArray &payload,
                                        int startAddress,
                                        int length);

public slots:
    /// roundId 由采样层生成；协议层只透传并执行一次 MES GET。
    virtual void fetchMesDayData(quint64 roundId);
    /// 读取连续 PLC 位并带回相同 roundId/startAddress；不解释产品或模式。
    virtual void fetchPlcBits(quint64 roundId, int startAddress, int length);

signals:
    /// MES 成功和失败都回传原 roundId，便于采样层丢弃迟到响应。
    void mesReplyReady(quint64 roundId,
                       CustomSysScheduler::LiveMesDayReply reply);
    /// PLC 成功和失败都回传原 roundId 和起始地址。
    void plcReplyReady(quint64 roundId,
                       int startAddress,
                       CustomSysScheduler::PlcBitReply reply);
    /// 结构化中文日志只描述通信，不修改库存或任务。
    void logMessage(QString messageZh);

private:
    enum class Operation {
        FetchMesDayData,
        FetchPlcBits
    };

    struct RequestContext {
        Operation operation = Operation::FetchMesDayData;
        quint64 roundId = 0;
        int startAddress = 0;
        int length = 0;
        QUrl url;
    };

    void sendGet(const RequestContext &context);
    void handleReply(QNetworkReply *reply, const RequestContext &context);
    static QString operationText(Operation operation);
    static bool validateEndpoint(const QUrl &endpoint, QString *errorMessage);

    QNetworkAccessManager *m_nam = nullptr;
    QUrl m_liveMesDayEndpoint;
    int m_requestTimeoutMs = 5000;
};

// 信号参数需要被 QSignalSpy 和潜在 queued connection 安全识别。
Q_DECLARE_METATYPE(CustomSysScheduler::LiveMesDayReply)
Q_DECLARE_METATYPE(CustomSysScheduler::PlcBitReply)

#endif // CUSTOM_SYS_SCHEDULER_H
