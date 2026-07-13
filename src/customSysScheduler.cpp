#include "customSysScheduler.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QUrlQuery>
#include <QVariant>

namespace {
const char *kLiveMesDayEndpoint = "http://192.168.115.228:5084/api/MesData/day";
const char *kPlcBitPath = "/api/PlcData/GetLBitRegister";

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

bool parseAddress(const QString &raw, int *address)
{
    static const QRegularExpression pattern(QStringLiteral(R"(^L(\d+)$)"));
    const QRegularExpressionMatch match = pattern.match(raw.trimmed());
    if (!match.hasMatch())
        return false;

    bool ok = false;
    const int parsed = match.captured(1).toInt(&ok);
    if (!ok)
        return false;

    if (address)
        *address = parsed;
    return true;
}

bool readActualQty(const QJsonObject &obj,
                   const QByteArray &payload,
                   qint64 *value,
                   QString *errorMessage)
{
    const QString key = QStringLiteral("actualQty");
    const QJsonValue jsonValue = obj.value(key);
    if (jsonValue.isUndefined()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("缺少 actualQty 字段");
        return false;
    }

    QString token;
    if (jsonValue.isString()) {
        token = jsonValue.toString().trimmed();
    } else if (jsonValue.isDouble()) {
        static const QRegularExpression tokenPattern(
            QStringLiteral(R"("actualQty"\s*:\s*(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?))"));
        const QRegularExpressionMatch match = tokenPattern.match(QString::fromUtf8(payload));
        if (!match.hasMatch()) {
            if (errorMessage)
                *errorMessage = QStringLiteral("actualQty 原始数值无法定位");
            return false;
        }
        token = match.captured(1).trimmed();
    } else {
        if (errorMessage)
            *errorMessage = QStringLiteral("actualQty 字段不是数字或数字文本");
        return false;
    }

    if (token.contains(QLatin1Char('.')) || token.contains(QLatin1Char('e'), Qt::CaseInsensitive)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("actualQty 必须为整数：%1").arg(token);
        return false;
    }

    bool ok = false;
    const qint64 parsed = token.toLongLong(&ok);
    if (!ok || token.startsWith(QLatin1Char('-'))) {
        if (errorMessage)
            *errorMessage = QStringLiteral("actualQty 文本无效：%1").arg(token);
            return false;
    }

    if (!ok || parsed < 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("actualQty 必须为非负 64 位整数");
        return false;
    }

    if (value)
        *value = parsed;
    return true;
}

QString rangeText(int startAddress, int length)
{
    return QStringLiteral("L%1..L%2").arg(startAddress).arg(startAddress + length - 1);
}
} // namespace

CustomSysScheduler::CustomSysScheduler(QObject *parent)
    : QObject(parent),
      m_nam(new QNetworkAccessManager(this)),
      m_liveMesDayEndpoint(defaultLiveMesDayEndpoint())
{
    qRegisterMetaType<CustomSysScheduler::LiveMesDayReply>(
        "CustomSysScheduler::LiveMesDayReply");
    qRegisterMetaType<CustomSysScheduler::PlcBitReply>(
        "CustomSysScheduler::PlcBitReply");
}

QUrl CustomSysScheduler::defaultLiveMesDayEndpoint()
{
    return QUrl(QString::fromLatin1(kLiveMesDayEndpoint));
}

QUrl CustomSysScheduler::livePlcBitEndpointFor(const QUrl &liveMesDayEndpoint)
{
    QUrl endpoint = liveMesDayEndpoint;
    endpoint.setPath(QString::fromLatin1(kPlcBitPath));
    endpoint.setQuery(QString());
    endpoint.setFragment(QString());
    return endpoint;
}

bool CustomSysScheduler::setLiveMesDayEndpoint(const QUrl &endpoint,
                                               QString *errorMessage)
{
    QString validationError;
    if (!validateEndpoint(endpoint, &validationError)) {
        if (errorMessage)
            *errorMessage = validationError;
        return false;
    }

    m_liveMesDayEndpoint = endpoint;
    if (errorMessage)
        errorMessage->clear();
    return true;
}

bool CustomSysScheduler::setRequestTimeoutMs(int timeoutMs, QString *errorMessage)
{
    if (timeoutMs <= 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("请求超时必须为正数：%1 ms")
                                .arg(timeoutMs);
        }
        return false;
    }

    m_requestTimeoutMs = timeoutMs;
    if (errorMessage)
        errorMessage->clear();
    return true;
}

CustomSysScheduler::LiveMesDayReply CustomSysScheduler::parseMesDayReply(
    const QByteArray &payload)
{
    LiveMesDayReply result;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        result.errorMessage = QStringLiteral("MES JSON 解析失败：%1")
                                  .arg(parseError.errorString());
        return result;
    }

    if (!doc.isArray()) {
        result.errorMessage = QStringLiteral("MES 返回不是 JSON 数组");
        return result;
    }

    const QJsonArray array = doc.array();
    if (array.isEmpty()) {
        result.errorMessage = QStringLiteral("MES 返回数组为空");
        return result;
    }

    if (!array.first().isObject()) {
        result.errorMessage = QStringLiteral("MES 首条记录不是 JSON 对象");
        return result;
    }

    qint64 actualQty = 0;
    QString fieldError;
    if (!readActualQty(array.first().toObject(), payload, &actualQty, &fieldError)) {
        result.errorMessage = fieldError;
        return result;
    }

    result.ok = true;
    result.actualQty = actualQty;
    return result;
}

CustomSysScheduler::PlcBitReply CustomSysScheduler::parsePlcBitReply(
    const QByteArray &payload,
    int startAddress,
    int length)
{
    PlcBitReply result;
    if (startAddress < 0 || length <= 0) {
        result.errorMessage = QStringLiteral("PLC 地址范围无效：start=%1, length=%2")
                                  .arg(startAddress)
                                  .arg(length);
        return result;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        result.errorMessage = QStringLiteral("PLC JSON 解析失败：%1")
                                  .arg(parseError.errorString());
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

    const QJsonValue timestampValue = obj.value(QStringLiteral("timestamp"));
    if (!timestampValue.isString() || timestampValue.toString().trimmed().isEmpty()) {
        result.errorMessage = QStringLiteral("PLC timestamp 字段缺失或不是字符串");
        return result;
    }
    result.timestamp = timestampValue.toString();

    const QJsonValue dataValue = obj.value(QStringLiteral("data"));
    if (!dataValue.isArray()) {
        result.errorMessage = QStringLiteral("PLC data 字段不是数组");
        return result;
    }

    const int endAddress = startAddress + length - 1;
    QSet<int> seen;
    const QJsonArray array = dataValue.toArray();
    for (int i = 0; i < array.size(); ++i) {
        if (!array.at(i).isObject()) {
            result.errorMessage = QStringLiteral("PLC data[%1] 不是对象").arg(i);
            return result;
        }

        const QJsonObject item = array.at(i).toObject();
        const QJsonValue addressValue = item.value(QStringLiteral("address"));
        if (!addressValue.isString()) {
            result.errorMessage = QStringLiteral("PLC data[%1] address 缺失或不是字符串").arg(i);
            return result;
        }

        int address = 0;
        if (!parseAddress(addressValue.toString(), &address)) {
            result.errorMessage = QStringLiteral("PLC data[%1] address 格式无效：%2")
                                      .arg(i)
                                      .arg(addressValue.toString());
            return result;
        }

        if (address < startAddress || address > endAddress) {
            result.errorMessage = QStringLiteral("PLC 地址越界：L%1 不在 %2")
                                      .arg(address)
                                      .arg(rangeText(startAddress, length));
            return result;
        }

        if (seen.contains(address)) {
            result.errorMessage = QStringLiteral("PLC 地址重复：L%1").arg(address);
            return result;
        }
        seen.insert(address);

        const QJsonValue bitValue = item.value(QStringLiteral("value"));
        if (!bitValue.isBool()) {
            result.errorMessage = QStringLiteral("PLC 地址 L%1 的 value 不是 bool")
                                      .arg(address);
            return result;
        }

        result.values.insert(address, bitValue.toBool());
    }

    for (int address = startAddress; address <= endAddress; ++address) {
        if (!result.values.contains(address)) {
            result.values.clear();
            result.errorMessage = QStringLiteral("PLC 响应缺少必需地址：L%1")
                                      .arg(address);
            return result;
        }
    }

    result.ok = true;
    return result;
}

void CustomSysScheduler::fetchMesDayData(quint64 roundId)
{
    RequestContext context;
    context.operation = Operation::FetchMesDayData;
    context.roundId = roundId;
    context.url = m_liveMesDayEndpoint;
    sendGet(context);
}

void CustomSysScheduler::fetchPlcBits(quint64 roundId, int startAddress, int length)
{
    if (startAddress < 0 || length <= 0) {
        PlcBitReply reply;
        reply.errorMessage = QStringLiteral("PLC 地址范围无效：start=%1, length=%2")
                                 .arg(startAddress)
                                 .arg(length);
        emit plcReplyReady(roundId, startAddress, reply);
        emit logMessage(QStringLiteral("[缺料协议] 读取 PLC 位失败：%1")
                            .arg(reply.errorMessage));
        return;
    }

    QUrl url = livePlcBitEndpointFor(m_liveMesDayEndpoint);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("StartAddress"), QString::number(startAddress));
    query.addQueryItem(QStringLiteral("length"), QString::number(length));
    url.setQuery(query);

    RequestContext context;
    context.operation = Operation::FetchPlcBits;
    context.roundId = roundId;
    context.startAddress = startAddress;
    context.length = length;
    context.url = url;
    sendGet(context);
}

void CustomSysScheduler::sendGet(const RequestContext &context)
{
    QString validationError;
    if (!validateEndpoint(context.url, &validationError)) {
        if (context.operation == Operation::FetchMesDayData) {
            LiveMesDayReply reply;
            reply.errorMessage = validationError;
            emit mesReplyReady(context.roundId, reply);
        } else {
            PlcBitReply reply;
            reply.errorMessage = validationError;
            emit plcReplyReady(context.roundId, context.startAddress, reply);
        }
        emit logMessage(QStringLiteral("[缺料协议] %1失败：%2")
                            .arg(operationText(context.operation), validationError));
        return;
    }

    QNetworkRequest request(context.url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("wh-robot-visual/live-shortage-protocol"));
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(m_requestTimeoutMs);
#endif

    emit logMessage(QStringLiteral("[缺料协议] %1：GET %2")
                        .arg(operationText(context.operation), context.url.toString()));

    QNetworkReply *reply = m_nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, context] {
        handleReply(reply, context);
        reply->deleteLater();
    });
}

void CustomSysScheduler::handleReply(QNetworkReply *reply,
                                     const RequestContext &context)
{
    const QByteArray payload = reply->readAll();
    const int httpStatus = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();

    auto httpFailure = [this, &context, httpStatus](const QString &reason) {
        const QString msg = QStringLiteral("%1，HTTP=%2").arg(reason).arg(httpStatus);
        if (context.operation == Operation::FetchMesDayData) {
            LiveMesDayReply parsed;
            parsed.errorMessage = msg;
            emit mesReplyReady(context.roundId, parsed);
        } else {
            PlcBitReply parsed;
            parsed.errorMessage = msg;
            emit plcReplyReady(context.roundId, context.startAddress, parsed);
        }
        emit logMessage(QStringLiteral("[缺料协议] %1失败：%2")
                            .arg(operationText(context.operation), msg));
    };

    if (reply->error() != QNetworkReply::NoError) {
        httpFailure(QStringLiteral("HTTP 请求失败：%1").arg(reply->errorString()));
        return;
    }

    if (!isHttpOk(httpStatus)) {
        httpFailure(QStringLiteral("HTTP 状态码异常"));
        return;
    }

    if (context.operation == Operation::FetchMesDayData) {
        LiveMesDayReply parsed = parseMesDayReply(payload);
        emit mesReplyReady(context.roundId, parsed);
        emit logMessage(parsed.ok
            ? QStringLiteral("[缺料协议] MES 读取成功：round=%1，actualQty=%2")
                  .arg(context.roundId)
                  .arg(parsed.actualQty)
            : QStringLiteral("[缺料协议] MES 解析失败：round=%1，%2")
                  .arg(context.roundId)
                  .arg(parsed.errorMessage));
        return;
    }

    PlcBitReply parsed = parsePlcBitReply(payload,
                                          context.startAddress,
                                          context.length);
    emit plcReplyReady(context.roundId, context.startAddress, parsed);
    emit logMessage(parsed.ok
        ? QStringLiteral("[缺料协议] PLC 位读取成功：round=%1，start=L%2，count=%3")
              .arg(context.roundId)
              .arg(context.startAddress)
              .arg(parsed.values.size())
        : QStringLiteral("[缺料协议] PLC 位解析失败：round=%1，start=L%2，%3")
              .arg(context.roundId)
              .arg(context.startAddress)
              .arg(parsed.errorMessage));
}

QString CustomSysScheduler::operationText(Operation operation)
{
    switch (operation) {
    case Operation::FetchMesDayData:
        return QStringLiteral("读取 MES 日统计");
    case Operation::FetchPlcBits:
        return QStringLiteral("读取 PLC 位");
    }
    return QStringLiteral("缺料协议请求");
}

bool CustomSysScheduler::validateEndpoint(const QUrl &endpoint,
                                          QString *errorMessage)
{
    if (!endpoint.isValid() || endpoint.host().isEmpty()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("接口地址无效：%1").arg(endpoint.toString());
        return false;
    }

    const QString scheme = endpoint.scheme().toLower();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("接口地址只支持 HTTP/HTTPS：%1")
                                .arg(endpoint.toString());
        }
        return false;
    }

    if (endpoint.path().isEmpty() || endpoint.path() == QStringLiteral("/")) {
        if (errorMessage)
            *errorMessage = QStringLiteral("接口地址缺少完整 path：%1").arg(endpoint.toString());
        return false;
    }

    if (errorMessage)
        errorMessage->clear();
    return true;
}
