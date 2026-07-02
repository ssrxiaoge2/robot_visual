#include "customSysScheduler.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QVariant>

namespace {
// 客户现场 HTTP 请求统一使用 3 秒超时，避免轮询线程长时间挂起。
constexpr int kRequestTimeoutMs = 3000;

// MES/PLC 固定端点集中定义在解析层，主线程只传递轮次和地址范围。
const char *kMesDayEndpoint = "http://192.168.115.229:5084/api/MesData/day";
const char *kPlcBitEndpoint = "http://192.168.115.228:5084/api/PlcData/GetLBitRegister";

// 这些固定地址属于协议层常量，供主线程后续接线时直接复用。
[[maybe_unused]] constexpr int kPlcModeStartAddress = 68;
[[maybe_unused]] constexpr int kPlcModeLength = 2;
[[maybe_unused]] constexpr int kPlcProductStartAddress = 71;
[[maybe_unused]] constexpr int kPlcProductLength = 3;
[[maybe_unused]] constexpr int kPlcFallbackModeAddress = 1998;
[[maybe_unused]] constexpr int kPlcFallbackModeLength = 1;

bool isHttpOk(int status)
{
    return status >= 200 && status < 300;
}

QString firstNonEmptyString(const QJsonObject &obj, const QStringList &keys)
{
    for (const QString &key : keys) {
        const QString value = obj.value(key).toString().trimmed();
        if (!value.isEmpty())
            return value;
    }
    return QString();
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
    return defaultMesDayEndpoint();
}

QUrl CustomSysScheduler::defaultMesDayEndpoint()
{
    return QUrl(QString::fromLatin1(kMesDayEndpoint));
}

QUrl CustomSysScheduler::defaultPlcBitEndpoint()
{
    return QUrl(QString::fromLatin1(kPlcBitEndpoint));
}

void CustomSysScheduler::testConnectivity()
{
    sendGet(Operation::Connectivity);
}

void CustomSysScheduler::fetchDayData()
{
    sendGet(Operation::FetchDayData);
}

void CustomSysScheduler::fetchMesDayData(quint64 roundId)
{
    RequestContext context;
    context.operation = Operation::FetchMesDayData;
    context.roundId = roundId;
    context.url = m_endpoint;
    sendGet(context);
}

void CustomSysScheduler::fetchPlcBits(quint64 roundId, int startAddress, int length)
{
    if (startAddress < 0 || length <= 0) {
        // 本地参数无效时直接返回中文错误，避免把非法地址发到现场 PLC。
        PlcBitReply result;
        result.errorMessage = QStringLiteral("PLC 地址范围无效：start=%1, length=%2")
                                  .arg(startAddress)
                                  .arg(length);
        emit plcReplyReady(roundId, startAddress, result);
        emit requestFailed(QStringLiteral("读取 PLC 位"),
                           result.errorMessage,
                           QString());
        emit logMessage(QStringLiteral("[客户系统] 读取 PLC 位失败：%1")
                            .arg(result.errorMessage));
        return;
    }

    RequestContext context;
    context.operation = Operation::FetchPlcBits;
    context.roundId = roundId;
    context.startAddress = startAddress;
    context.length = length;
    context.requiredAddresses = buildRequiredAddresses(startAddress, length);
    context.url = defaultPlcBitEndpoint();

    QUrlQuery query(context.url);
    query.addQueryItem(QStringLiteral("StartAddress"), QString::number(startAddress));
    query.addQueryItem(QStringLiteral("length"), QString::number(length));
    context.url.setQuery(query);

    sendGet(context);
}

void CustomSysScheduler::sendGet(Operation operation)
{
    RequestContext context;
    context.operation = operation;
    context.url = m_endpoint;
    sendGet(context);
}

void CustomSysScheduler::sendGet(const RequestContext &context)
{
    const QString opText = operationText(context);
    if (!context.url.isValid() || context.url.scheme().isEmpty()
        || context.url.host().isEmpty()) {
        // URL 本身不合法时，不进入网络层，直接按不同契约回传错误。
        const QString msg = QStringLiteral("接口地址无效：%1").arg(context.url.toString());
        emit requestFailed(opText, msg, QString());
        emit logMessage(QStringLiteral("[客户系统] %1失败：%2").arg(opText, msg));
        switch (context.operation) {
        case Operation::Connectivity:
            emit connectivityChecked(false, QStringLiteral("地址无效"), 0);
            break;
        case Operation::FetchMesDayData:
            emit mesReplyReady(context.roundId, false, 0, msg);
            break;
        case Operation::FetchPlcBits: {
            PlcBitReply result;
            result.errorMessage = msg;
            emit plcReplyReady(context.roundId, context.startAddress, result);
            break;
        }
        case Operation::FetchDayData:
            break;
        }
        return;
    }

    QNetworkRequest request(context.url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("wh-robot-visual/custom-system-test"));
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(kRequestTimeoutMs);
#endif

    emit requestStarted(opText);
    emit logMessage(QStringLiteral("[客户系统] %1：GET %2")
                        .arg(opText, context.url.toString()));

    QNetworkReply *reply = m_nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, context]() {
        handleReply(reply, context);
        reply->deleteLater();
    });
}

void CustomSysScheduler::handleReply(QNetworkReply *reply, const RequestContext &context)
{
    const QString opText = operationText(context);
    const QByteArray payload = reply->readAll();
    const QString rawJson = QString::fromUtf8(payload);
    const int httpStatus = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();

    auto emitFailure = [this, &context, &opText, &rawJson, httpStatus](
                           const QString &msg, const QString &connectivityStatusText) {
        emit requestFailed(opText, msg, rawJson);
        emit logMessage(QStringLiteral("[客户系统] %1失败：%2，HTTP=%3")
                            .arg(opText, msg)
                            .arg(httpStatus));

        switch (context.operation) {
        case Operation::Connectivity:
            emit connectivityChecked(false, connectivityStatusText, httpStatus);
            break;
        case Operation::FetchMesDayData:
            emit mesReplyReady(context.roundId, false, 0, msg);
            break;
        case Operation::FetchPlcBits: {
            PlcBitReply result;
            result.errorMessage = msg;
            emit plcReplyReady(context.roundId, context.startAddress, result);
            break;
        }
        case Operation::FetchDayData:
            break;
        }
    };

    if (reply->error() != QNetworkReply::NoError) {
        // 现场 HTTP/网络错误统一转换成中文错误路径，供上层决定是否跳过本轮。
        emitFailure(QStringLiteral("HTTP 请求失败：%1").arg(reply->errorString()),
                    QStringLiteral("连接失败"));
        return;
    }

    if (!isHttpOk(httpStatus)) {
        // 非 2xx 状态直接视为协议失败，避免把 HTML/错误页继续当 JSON 解析。
        emitFailure(QStringLiteral("HTTP 状态码异常：%1").arg(httpStatus),
                    QStringLiteral("HTTP %1").arg(httpStatus));
        return;
    }

    if (context.operation == Operation::Connectivity) {
        emit connectivityChecked(true,
                                 QStringLiteral("可达 HTTP %1").arg(httpStatus),
                                 httpStatus);
        emit logMessage(QStringLiteral("[客户系统] 连通性测试成功，HTTP=%1").arg(httpStatus));
        return;
    }

    if (context.operation == Operation::FetchPlcBits) {
        const PlcBitReply parsed = parsePlcBitReply(payload, context.requiredAddresses);
        if (!parsed.ok) {
            // PLC 契约错误仍按当前轮次和地址透传给上层，便于主线程聚合同轮结果。
            emit requestFailed(opText, parsed.errorMessage, rawJson);
            emit logMessage(QStringLiteral("[客户系统] PLC 位解析失败：%1")
                                .arg(parsed.errorMessage));
            emit plcReplyReady(context.roundId, context.startAddress, parsed);
            return;
        }

        emit plcReplyReady(context.roundId, context.startAddress, parsed);
        emit logMessage(QStringLiteral("[客户系统] PLC 位读取成功：round=%1，start=%2，count=%3")
                            .arg(context.roundId)
                            .arg(plcAddressText(context.startAddress))
                            .arg(parsed.bits.size()));
        return;
    }

    const ParseResult parsed = parseDayReply(payload);
    if (!parsed.ok) {
        // MES 契约失败要同时保留旧 requestFailed 和新 roundId 回传。
        emit requestFailed(opText, parsed.errorMessage, rawJson);
        emit logMessage(QStringLiteral("[客户系统] 数据解析失败：%1").arg(parsed.errorMessage));
        if (context.operation == Operation::FetchMesDayData)
            emit mesReplyReady(context.roundId, false, 0, parsed.errorMessage);
        return;
    }

    if (context.operation == Operation::FetchMesDayData) {
        emit mesReplyReady(context.roundId, true, parsed.record.actualQty, QString());
        emit logMessage(QStringLiteral("[客户系统] MES 读取成功：round=%1，actualQty=%2")
                            .arg(context.roundId)
                            .arg(parsed.record.actualQty));
        return;
    }

    emit dayDataReady(parsed.record, rawJson);
    emit logMessage(QStringLiteral("[客户系统] 读取成功：ActualQty=%1，line=%2")
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
    record.id = obj.value(QStringLiteral("id")).toInteger();
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

CustomSysScheduler::PlcBitReply CustomSysScheduler::parsePlcBitReply(
    const QByteArray &payload, const QStringList &requiredAddresses)
{
    PlcBitReply result;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        result.errorMessage = QStringLiteral("JSON 解析失败：%1").arg(parseError.errorString());
        return result;
    }

    if (!doc.isObject()) {
        result.errorMessage = QStringLiteral("PLC 响应根节点不是对象");
        return result;
    }

    const QJsonObject obj = doc.object();
    const QJsonValue successValue = obj.value(QStringLiteral("success"));
    if (!successValue.isBool()) {
        result.errorMessage = QStringLiteral("PLC success 字段不是 bool");
        return result;
    }

    if (!successValue.toBool()) {
        result.errorMessage = firstNonEmptyString(
            obj,
            {QStringLiteral("errorMessage"),
             QStringLiteral("message"),
             QStringLiteral("error")});
        if (result.errorMessage.isEmpty())
            result.errorMessage = QStringLiteral("PLC 响应 success=false");
        return result;
    }

    const QDateTime timestamp = parseIsoTimestamp(
        obj.value(QStringLiteral("timestamp")).toString());
    if (!timestamp.isValid()) {
        // timestamp 无效时直接失败，避免上层把过期或不可判定时间的响应混入同轮。
        result.errorMessage = QStringLiteral("PLC timestamp 无效");
        return result;
    }

    const QJsonValue dataValue = obj.value(QStringLiteral("data"));
    if (!dataValue.isArray()) {
        result.errorMessage = QStringLiteral("PLC data 字段不是数组");
        return result;
    }

    const QJsonArray array = dataValue.toArray();
    QHash<QString, bool> bits;
    for (int i = 0; i < array.size(); ++i) {
        if (!array.at(i).isObject()) {
            result.errorMessage = QStringLiteral("PLC data[%1] 不是对象").arg(i);
            return result;
        }

        const QJsonObject item = array.at(i).toObject();
        const QJsonValue addressValue = item.value(QStringLiteral("address"));
        if (!addressValue.isString() || addressValue.toString().trimmed().isEmpty()) {
            result.errorMessage = QStringLiteral("PLC data[%1] 缺少 address 字段").arg(i);
            return result;
        }

        const QString address = addressValue.toString().trimmed();
        const QJsonValue bitValue = item.value(QStringLiteral("value"));
        if (bitValue.isUndefined()) {
            result.errorMessage = QStringLiteral("PLC data[%1] 缺少 value 字段").arg(i);
            return result;
        }

        if (!bitValue.isBool()) {
            result.errorMessage = QStringLiteral("PLC 地址 %1 的 value 不是 bool").arg(address);
            return result;
        }

        if (bits.contains(address)) {
            result.errorMessage = QStringLiteral("PLC 地址重复：%1").arg(address);
            return result;
        }

        bits.insert(address, bitValue.toBool());
    }

    for (const QString &address : requiredAddresses) {
        if (!bits.contains(address)) {
            result.errorMessage = QStringLiteral("PLC 响应缺少必需地址：%1").arg(address);
            return result;
        }
    }

    result.ok = true;
    result.bits = bits;
    result.timestamp = timestamp;
    return result;
}

QString CustomSysScheduler::operationText(const RequestContext &context)
{
    switch (context.operation) {
    case Operation::Connectivity:
        return QStringLiteral("连通性测试");
    case Operation::FetchDayData:
        return QStringLiteral("读取日统计");
    case Operation::FetchMesDayData:
        return QStringLiteral("读取 MES 日统计");
    case Operation::FetchPlcBits:
        return QStringLiteral("读取 PLC 位");
    }
    return QStringLiteral("客户系统请求");
}

QString CustomSysScheduler::plcAddressText(int address)
{
    return QStringLiteral("L%1").arg(address);
}

QStringList CustomSysScheduler::buildRequiredAddresses(int startAddress, int length)
{
    QStringList addresses;
    addresses.reserve(length);
    for (int offset = 0; offset < length; ++offset)
        addresses.append(plcAddressText(startAddress + offset));
    return addresses;
}

QDateTime CustomSysScheduler::parseIsoTimestamp(const QString &timestampText)
{
    QDateTime timestamp = QDateTime::fromString(timestampText, Qt::ISODateWithMs);
    if (timestamp.isValid())
        return timestamp;

    timestamp = QDateTime::fromString(timestampText, Qt::ISODate);
    if (timestamp.isValid())
        return timestamp;

    // Qt 对高于毫秒精度的 ISO 字符串兼容性不一致，这里裁到毫秒再校验。
    static const QRegularExpression kTimestampPattern(
        QStringLiteral(R"(^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})(\.\d+)?(Z|[+-]\d{2}:\d{2})$)"));
    const QRegularExpressionMatch match = kTimestampPattern.match(timestampText);
    if (!match.hasMatch())
        return QDateTime();

    QString fraction = match.captured(2);
    if (fraction.size() > 4)
        fraction = fraction.left(4);

    const QString normalized = match.captured(1) + fraction + match.captured(3);
    if (fraction.isEmpty())
        return QDateTime::fromString(normalized, Qt::ISODate);
    return QDateTime::fromString(normalized, Qt::ISODateWithMs);
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
