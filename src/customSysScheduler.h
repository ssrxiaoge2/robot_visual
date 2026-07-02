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

    static QUrl defaultEndpoint();
    static QUrl defaultMesDayEndpoint();
    static QUrl defaultPlcBitEndpoint();
    static ParseResult parseDayReply(const QByteArray &payload);
    static PlcBitReply parsePlcBitReply(const QByteArray &payload,
                                        const QStringList &requiredAddresses);

public slots:
    void testConnectivity();
    void fetchDayData();
    virtual void fetchMesDayData(quint64 roundId);
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
