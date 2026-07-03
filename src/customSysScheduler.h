#ifndef CUSTOM_SYS_SCHEDULER_H
#define CUSTOM_SYS_SCHEDULER_H

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

class QNetworkReply;

/**
 * @brief 客户现场系统 REST API 通信调度器。
 *
 * 该类只负责客户系统 HTTP 请求、连通性判断和 /api/MesData/day
 * JSON 解析；UI 展示和按钮状态由 MainWindow 处理，入口由
 * DeviceManager 统一调度。当前业务只使用 actualQty，其余字段用于现场排查。
 */
class CustomSysScheduler : public QObject
{
    Q_OBJECT
public:
    struct DayRecord {
        qint64 id = 0;
        QDateTime statDate;
        QString lineId;
        QString lineName;
        int planQty = 0;
        int actualQty = 0;
        int okQty = 0;
        int ngQty = 0;
    };

    struct ParseResult {
        bool ok = false;
        DayRecord record;
        QString errorMessage;
    };

    // PLC 位读取结果只承载协议层数据，不在此处做业务判断。
    struct PlcBitReply {
        bool ok = false;
        QHash<QString, bool> bits;
        QDateTime timestamp;
        QString errorMessage;
    };

    explicit CustomSysScheduler(QObject *parent = nullptr);

    QUrl endpoint() const { return m_endpoint; }
    void setEndpoint(const QUrl &endpoint);

    /// 缺料测试与客户系统面板共用的默认入口，当前固定指向现场实测 MES 日产量接口。
    static QUrl defaultEndpoint();
    /// .228 是现场实测通过的 MES 日产量接口地址，所有 MES 请求必须统一复用。
    static QUrl mesDayEndpoint();
    /// 兼容旧调用点的包装接口，始终返回 mesDayEndpoint()。
    static QUrl defaultMesDayEndpoint();
    /// PLC 位读取固定入口；与 MES 地址分离，避免上层拼接协议细节。
    static QUrl defaultPlcBitEndpoint();
    static ParseResult parseDayReply(const QByteArray &payload);
    static PlcBitReply parsePlcBitReply(const QByteArray &payload,
                                        const QStringList &requiredAddresses);

public slots:
    void testConnectivity();
    void fetchDayData();
    /// roundId 由上层轮询会话生成并原样透传，便于同轮聚合和丢弃迟到响应。
    virtual void fetchMesDayData(quint64 roundId);
    /// 读取 PLC 位寄存器并把 roundId、地址范围原样带回给上层聚合同轮结果。
    virtual void fetchPlcBits(quint64 roundId, int startAddress, int length);

signals:
    void requestStarted(const QString &operation);
    void connectivityChecked(bool ok, const QString &statusText, int httpStatus);
    void dayDataReady(const CustomSysScheduler::DayRecord &record,
                      const QString &rawJson);
    void mesReplyReady(quint64 roundId, bool ok, qint64 actualQty, QString error);
    void plcReplyReady(quint64 roundId,
                       int startAddress,
                       CustomSysScheduler::PlcBitReply result);
    void requestFailed(const QString &operation,
                       const QString &errorMessage,
                       const QString &rawJson);
    void logMessage(const QString &message);

private:
    enum class Operation {
        Connectivity,
        FetchDayData,
        FetchMesDayData,
        FetchPlcBits
    };

    // 用请求上下文区分 legacy 接口和 roundId 新契约，避免解析层掺入业务状态。
    struct RequestContext {
        Operation operation = Operation::Connectivity;
        // roundId 属于上层会话契约数据，这里只负责透传，不重新解释业务含义。
        quint64 roundId = 0;
        int startAddress = 0;
        int length = 0;
        QStringList requiredAddresses;
        QUrl url;
    };

    void sendGet(Operation operation);
    void sendGet(const RequestContext &context);
    void handleReply(QNetworkReply *reply, const RequestContext &context);
    static QString operationText(const RequestContext &context);
    static QString plcAddressText(int address);
    static QStringList buildRequiredAddresses(int startAddress, int length);
    static QDateTime parseIsoTimestamp(const QString &timestampText);
    static bool readIntField(const QJsonObject &obj,
                             const QString &key,
                             int *value,
                             QString *errorMessage,
                             bool required);

    QNetworkAccessManager *m_nam = nullptr;
    QUrl m_endpoint;
};

Q_DECLARE_METATYPE(CustomSysScheduler::DayRecord)
Q_DECLARE_METATYPE(CustomSysScheduler::PlcBitReply)

#endif // CUSTOM_SYS_SCHEDULER_H
