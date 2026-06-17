#ifndef CUSTOM_SYS_SCHEDULER_H
#define CUSTOM_SYS_SCHEDULER_H

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>

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

    explicit CustomSysScheduler(QObject *parent = nullptr);

    QUrl endpoint() const { return m_endpoint; }
    void setEndpoint(const QUrl &endpoint);

    static QUrl defaultEndpoint();
    static ParseResult parseDayReply(const QByteArray &payload);

public slots:
    void testConnectivity();
    void fetchDayData();

signals:
    void requestStarted(const QString &operation);
    void connectivityChecked(bool ok, const QString &statusText, int httpStatus);
    void dayDataReady(const CustomSysScheduler::DayRecord &record,
                      const QString &rawJson);
    void requestFailed(const QString &operation,
                       const QString &errorMessage,
                       const QString &rawJson);
    void logMessage(const QString &message);

private:
    enum class Operation {
        Connectivity,
        FetchDayData
    };

    void sendGet(Operation operation);
    void handleReply(QNetworkReply *reply, Operation operation);
    static QString operationText(Operation operation);
    static bool readIntField(const QJsonObject &obj,
                             const QString &key,
                             int *value,
                             QString *errorMessage,
                             bool required);

    QNetworkAccessManager *m_nam = nullptr;
    QUrl m_endpoint;
};

Q_DECLARE_METATYPE(CustomSysScheduler::DayRecord)

#endif // CUSTOM_SYS_SCHEDULER_H
