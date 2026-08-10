#include "customSysScheduler.h"
#include "networkcompat.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace {
constexpr int kRequestTimeoutMs = 5000;
const char *kDefaultEndpoint = "http://192.168.115.229:5084/api/MesData/day";

bool isHttpOk(int status)
{
    return status >= 200 && status < 300;
}

qint64 jsonInteger(const QJsonValue &value)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return value.toInteger();
#else
    // Qt 5.12 缺少 QJsonValue::toInteger()。先校验 JSON 类型，避免
    // QVariant 把字符串或布尔值额外转换为整数。
    return value.isDouble() ? static_cast<qint64>(value.toDouble()) : 0;
#endif
}

}

CustomSysScheduler::CustomSysScheduler(QObject *parent)
    : QObject(parent),
      m_nam(new QNetworkAccessManager(this)),
      m_endpoint(defaultEndpoint())
{
}

void CustomSysScheduler::setEndpoint(const QUrl &endpoint)
{
    m_endpoint = endpoint;
}

QUrl CustomSysScheduler::defaultEndpoint()
{
    return QUrl(QString::fromLatin1(kDefaultEndpoint));
}

void CustomSysScheduler::testConnectivity()
{
    sendGet(Operation::Connectivity);
}

void CustomSysScheduler::fetchDayData()
{
    sendGet(Operation::FetchDayData);
}

void CustomSysScheduler::sendGet(Operation operation)
{
    const QString opText = operationText(operation);
    if (!m_endpoint.isValid() || m_endpoint.scheme().isEmpty()
        || m_endpoint.host().isEmpty()) {
        const QString msg = QStringLiteral("接口地址无效：%1").arg(m_endpoint.toString());
        emit requestFailed(opText, msg, QString());
        emit logMessage(QStringLiteral("[客户系统] %1失败：%2").arg(opText, msg));
        if (operation == Operation::Connectivity)
            emit connectivityChecked(false, QStringLiteral("地址无效"), 0);
        return;
    }

    QNetworkRequest request(m_endpoint);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("wh-robot-visual/custom-system-test"));
    setNetworkTransferTimeout(request, kRequestTimeoutMs);

    emit requestStarted(opText);
    emit logMessage(QStringLiteral("[客户系统] %1：GET %2")
                        .arg(opText, m_endpoint.toString()));

    QNetworkReply *reply = m_nam->get(request);
    attachNetworkTransferTimeout(reply, kRequestTimeoutMs);
    connect(reply, &QNetworkReply::finished, this, [this, reply, operation]() {
        handleReply(reply, operation);
        reply->deleteLater();
    });
}

void CustomSysScheduler::handleReply(QNetworkReply *reply, Operation operation)
{
    const QString opText = operationText(operation);
    const QByteArray payload = reply->readAll();
    const QString rawJson = QString::fromUtf8(payload);
    const int httpStatus = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError) {
        const QString msg = QStringLiteral("HTTP 请求失败：%1").arg(reply->errorString());
        emit requestFailed(opText, msg, rawJson);
        emit logMessage(QStringLiteral("[客户系统] %1失败：%2，HTTP=%3")
                            .arg(opText, msg).arg(httpStatus));
        if (operation == Operation::Connectivity)
            emit connectivityChecked(false, QStringLiteral("连接失败"), httpStatus);
        return;
    }

    if (!isHttpOk(httpStatus)) {
        const QString msg = QStringLiteral("HTTP 状态码异常：%1").arg(httpStatus);
        emit requestFailed(opText, msg, rawJson);
        emit logMessage(QStringLiteral("[客户系统] %1失败：%2").arg(opText, msg));
        if (operation == Operation::Connectivity)
            emit connectivityChecked(false, QStringLiteral("HTTP %1").arg(httpStatus), httpStatus);
        return;
    }

    if (operation == Operation::Connectivity) {
        emit connectivityChecked(true, QStringLiteral("可达 HTTP %1").arg(httpStatus), httpStatus);
        emit logMessage(QStringLiteral("[客户系统] 连通性测试成功，HTTP=%1").arg(httpStatus));
        return;
    }

    ParseResult parsed = parseDayReply(payload);
    if (!parsed.ok) {
        emit requestFailed(opText, parsed.errorMessage, rawJson);
        emit logMessage(QStringLiteral("[客户系统] 数据解析失败：%1").arg(parsed.errorMessage));
        return;
    }

    emit dayDataReady(parsed.record, rawJson);
    emit logMessage(QStringLiteral("[客户系统] 读取成功：actualQty=%1，line=%2")
                        .arg(parsed.record.actualQty)
                        .arg(parsed.record.lineName.isEmpty()
                                 ? parsed.record.lineId
                                 : parsed.record.lineName));
}

CustomSysScheduler::ParseResult CustomSysScheduler::parseDayReply(const QByteArray &payload)
{
    ParseResult result;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        result.errorMessage = QStringLiteral("JSON 解析失败：%1").arg(parseError.errorString());
        return result;
    }

    if (!doc.isArray()) {
        result.errorMessage = QStringLiteral("返回不是 JSON 数组");
        return result;
    }

    const QJsonArray array = doc.array();
    if (array.isEmpty()) {
        result.errorMessage = QStringLiteral("返回数组为空");
        return result;
    }

    if (!array.first().isObject()) {
        result.errorMessage = QStringLiteral("首条记录不是 JSON 对象");
        return result;
    }

    const QJsonObject obj = array.first().toObject();
    DayRecord record;
    record.id = jsonInteger(obj.value(QStringLiteral("id")));
    record.statDate = QDateTime::fromString(
        obj.value(QStringLiteral("statDate")).toString(), Qt::ISODate);
    record.lineId = obj.value(QStringLiteral("lineId")).toString();
    record.lineName = obj.value(QStringLiteral("lineName")).toString();

    QString fieldError;
    if (!readIntField(obj, QStringLiteral("actualQty"), &record.actualQty,
                      &fieldError, true)
        || !readIntField(obj, QStringLiteral("planQty"), &record.planQty,
                         &fieldError, false)
        || !readIntField(obj, QStringLiteral("okQty"), &record.okQty,
                         &fieldError, false)
        || !readIntField(obj, QStringLiteral("ngQty"), &record.ngQty,
                         &fieldError, false)) {
        result.errorMessage = fieldError;
        return result;
    }

    result.ok = true;
    result.record = record;
    return result;
}

QString CustomSysScheduler::operationText(Operation operation)
{
    switch (operation) {
    case Operation::Connectivity:
        return QStringLiteral("连通性测试");
    case Operation::FetchDayData:
        return QStringLiteral("读取日统计");
    }
    return QStringLiteral("客户系统请求");
}

bool CustomSysScheduler::readIntField(const QJsonObject &obj,
                                      const QString &key,
                                      int *value,
                                      QString *errorMessage,
                                      bool required)
{
    const QJsonValue jsonValue = obj.value(key);
    if (jsonValue.isUndefined()) {
        if (required && errorMessage)
            *errorMessage = QStringLiteral("缺少 %1 字段").arg(key);
        return !required;
    }

    if (!jsonValue.isDouble()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("%1 字段不是数字").arg(key);
        return false;
    }

    if (value)
        *value = jsonValue.toInt();
    return true;
}
