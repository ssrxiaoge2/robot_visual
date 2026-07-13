#include "shortagestatestore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QtGlobal>

#include <limits>

namespace {

struct StatePaths {
    QString mainPath;        ///< 当前命名空间主快照。
    QString backupPath;      ///< 当前命名空间上一次主快照备份。
    QString journalPath;     ///< 当前命名空间 JSONL 审计流水。
    QString maintenancePath; ///< 维护前人工备份目录。
};

QString u64(quint64 value)
{
    return QString::number(value);
}

QString i64(qint64 value)
{
    return QString::number(value);
}

bool parseU64(const QJsonValue &value, quint64 *out)
{
    if (value.isUndefined() || value.isNull())
        return false;
    bool ok = false;
    const QString text = value.isString() ? value.toString() : QString();
    if (text.isEmpty())
        return false;
    const quint64 parsed = text.toULongLong(&ok);
    if (ok)
        *out = parsed;
    return ok;
}

bool parseI64(const QJsonValue &value, qint64 *out)
{
    if (value.isUndefined() || value.isNull())
        return false;
    bool ok = false;
    const QString text = value.isString() ? value.toString() : QString();
    if (text.isEmpty())
        return false;
    const qint64 parsed = text.toLongLong(&ok);
    if (ok)
        *out = parsed;
    return ok;
}

bool requireBool(const QJsonObject &object, const QString &field, bool *out, QString *errorZh)
{
    const QJsonValue value = object.value(field);
    if (!value.isBool()) {
        *errorZh = QStringLiteral("运行状态字段 %1 缺失或格式非法").arg(field);
        return false;
    }
    *out = value.toBool();
    return true;
}

bool requireInt(const QJsonObject &object, const QString &field, int *out, QString *errorZh)
{
    const QJsonValue value = object.value(field);
    if (!value.isDouble()) {
        *errorZh = QStringLiteral("运行状态字段 %1 缺失或格式非法").arg(field);
        return false;
    }
    const double number = value.toDouble();
    if (!qIsFinite(number)
        || number < std::numeric_limits<int>::min()
        || number > std::numeric_limits<int>::max()
        || number != int(number)) {
        *errorZh = QStringLiteral("运行状态字段 %1 缺失或格式非法").arg(field);
        return false;
    }
    *out = int(number);
    return true;
}

bool requireString(const QJsonObject &object, const QString &field, QString *out, QString *errorZh)
{
    const QJsonValue value = object.value(field);
    if (!value.isString()) {
        *errorZh = QStringLiteral("运行状态字段 %1 缺失或格式非法").arg(field);
        return false;
    }
    *out = value.toString();
    return true;
}

bool requireObject(const QJsonObject &object, const QString &field, QJsonObject *out, QString *errorZh)
{
    const QJsonValue value = object.value(field);
    if (!value.isObject()) {
        *errorZh = QStringLiteral("运行状态字段 %1 缺失或格式非法").arg(field);
        return false;
    }
    *out = value.toObject();
    return true;
}

bool requireArray(const QJsonObject &object, const QString &field, QJsonArray *out, QString *errorZh)
{
    const QJsonValue value = object.value(field);
    if (!value.isArray()) {
        *errorZh = QStringLiteral("运行状态字段 %1 缺失或格式非法").arg(field);
        return false;
    }
    *out = value.toArray();
    return true;
}

QString productToString(ProductModel product)
{
    switch (product) {
    case ProductModel::Model88:
        return QStringLiteral("88");
    case ProductModel::Model88R:
        return QStringLiteral("88R");
    case ProductModel::Model92:
        return QStringLiteral("92");
    }
    return QStringLiteral("unknown");
}

bool productFromString(const QString &value, ProductModel *product)
{
    if (value == QStringLiteral("88")) {
        *product = ProductModel::Model88;
        return true;
    }
    if (value == QStringLiteral("88R")) {
        *product = ProductModel::Model88R;
        return true;
    }
    if (value == QStringLiteral("92")) {
        *product = ProductModel::Model92;
        return true;
    }
    return false;
}

QString modeToString(ProductionMode mode)
{
    switch (mode) {
    case ProductionMode::LeftRight:
        return QStringLiteral("left_right");
    case ProductionMode::LeftOnly:
        return QStringLiteral("left_only");
    case ProductionMode::RightOnly:
        return QStringLiteral("right_only");
    }
    return QStringLiteral("unknown");
}

bool modeFromString(const QString &value, ProductionMode *mode)
{
    if (value == QStringLiteral("left_right")) {
        *mode = ProductionMode::LeftRight;
        return true;
    }
    if (value == QStringLiteral("left_only")) {
        *mode = ProductionMode::LeftOnly;
        return true;
    }
    if (value == QStringLiteral("right_only")) {
        *mode = ProductionMode::RightOnly;
        return true;
    }
    return false;
}

QString originToString(ReplenishmentOrigin origin)
{
    return origin == ReplenishmentOrigin::Automatic
        ? QStringLiteral("automatic")
        : QStringLiteral("manual");
}

bool originFromString(const QString &value, ReplenishmentOrigin *origin)
{
    if (value == QStringLiteral("automatic")) {
        *origin = ReplenishmentOrigin::Automatic;
        return true;
    }
    if (value == QStringLiteral("manual")) {
        *origin = ReplenishmentOrigin::Manual;
        return true;
    }
    return false;
}

QString orderStateToString(ReplenishmentOrderState state)
{
    switch (state) {
    case ReplenishmentOrderState::AwaitingDispatch:
        return QStringLiteral("awaiting_dispatch");
    case ReplenishmentOrderState::Queued:
        return QStringLiteral("queued");
    case ReplenishmentOrderState::Running:
        return QStringLiteral("running");
    case ReplenishmentOrderState::Unloaded:
        return QStringLiteral("unloaded");
    case ReplenishmentOrderState::Succeeded:
        return QStringLiteral("succeeded");
    case ReplenishmentOrderState::FailedBeforeUnload:
        return QStringLiteral("failed_before_unload");
    case ReplenishmentOrderState::FailedAfterUnload:
        return QStringLiteral("failed_after_unload");
    case ReplenishmentOrderState::Canceled:
        return QStringLiteral("canceled");
    }
    return QStringLiteral("unknown");
}

bool orderStateFromString(const QString &value, ReplenishmentOrderState *state)
{
    if (value == QStringLiteral("awaiting_dispatch"))
        *state = ReplenishmentOrderState::AwaitingDispatch;
    else if (value == QStringLiteral("queued"))
        *state = ReplenishmentOrderState::Queued;
    else if (value == QStringLiteral("running"))
        *state = ReplenishmentOrderState::Running;
    else if (value == QStringLiteral("unloaded"))
        *state = ReplenishmentOrderState::Unloaded;
    else if (value == QStringLiteral("succeeded"))
        *state = ReplenishmentOrderState::Succeeded;
    else if (value == QStringLiteral("failed_before_unload"))
        *state = ReplenishmentOrderState::FailedBeforeUnload;
    else if (value == QStringLiteral("failed_after_unload"))
        *state = ReplenishmentOrderState::FailedAfterUnload;
    else if (value == QStringLiteral("canceled"))
        *state = ReplenishmentOrderState::Canceled;
    else
        return false;
    return true;
}

QString dateToString(const QDateTime &value)
{
    return value.isValid()
        ? value.toUTC().toString(Qt::ISODateWithMs)
        : QString();
}

QDateTime dateFromString(const QString &value)
{
    return value.isEmpty() ? QDateTime() : QDateTime::fromString(value, Qt::ISODateWithMs).toUTC();
}

StatePaths pathsFor(const QString &baseDirectory, ShortageStateNamespace stateNamespace)
{
    const QDir dir(baseDirectory);
    const bool production = stateNamespace == ShortageStateNamespace::Production;
    return {
        dir.filePath(production ? QStringLiteral("production-state.json")
                                : QStringLiteral("test-state.json")),
        dir.filePath(production ? QStringLiteral("production-state.backup.json")
                                : QStringLiteral("test-state.backup.json")),
        dir.filePath(production ? QStringLiteral("production-events.jsonl")
                                : QStringLiteral("test-events.jsonl")),
        dir.filePath(QStringLiteral("maintenance-backups")),
    };
}

QJsonObject stationToJson(const ShortageStationRuntime &station)
{
    return {
        {QStringLiteral("stationId"), station.stationId},
        {QStringLiteral("stock"), i64(station.stock)},
        {QStringLiteral("firstLowAtUtc"), dateToString(station.firstLowAtUtc)},
        {QStringLiteral("consecutivePreUnloadFailures"), station.consecutivePreUnloadFailures},
        {QStringLiteral("automaticPaused"), station.automaticPaused},
        {QStringLiteral("pauseReasonZh"), station.pauseReasonZh},
    };
}

bool stationFromJson(const QJsonObject &object, ShortageStationRuntime *station, QString *errorZh)
{
    if (!requireInt(object, QStringLiteral("stationId"), &station->stationId, errorZh))
        return false;
    if (station->stationId < 1 || station->stationId > 12) {
        *errorZh = QStringLiteral("工位编号%1超出1到12范围").arg(station->stationId);
        return false;
    }
    if (!parseI64(object.value(QStringLiteral("stock")), &station->stock)) {
        *errorZh = QStringLiteral("工位%1库存格式非法").arg(station->stationId);
        return false;
    }
    QString firstLowAtUtc;
    if (!requireString(object, QStringLiteral("firstLowAtUtc"), &firstLowAtUtc, errorZh)
        || !requireInt(object, QStringLiteral("consecutivePreUnloadFailures"),
                       &station->consecutivePreUnloadFailures, errorZh)
        || !requireBool(object, QStringLiteral("automaticPaused"), &station->automaticPaused, errorZh)
        || !requireString(object, QStringLiteral("pauseReasonZh"), &station->pauseReasonZh, errorZh)) {
        return false;
    }
    station->firstLowAtUtc = dateFromString(firstLowAtUtc);
    return true;
}

QJsonObject actualQtyToJson(const ActualQtyRuntime &actualQty)
{
    return {
        {QStringLiteral("hasBaseline"), actualQty.hasBaseline},
        {QStringLiteral("baseline"), i64(actualQty.baseline)},
        {QStringLiteral("hasResetCandidate"), actualQty.hasResetCandidate},
        {QStringLiteral("resetCandidate"), i64(actualQty.resetCandidate)},
        {QStringLiteral("interrupted"), actualQty.interrupted},
    };
}

bool actualQtyFromJson(const QJsonObject &object, ActualQtyRuntime *actualQty, QString *errorZh)
{
    if (!requireBool(object, QStringLiteral("hasBaseline"), &actualQty->hasBaseline, errorZh)
        || !requireBool(object, QStringLiteral("hasResetCandidate"),
                        &actualQty->hasResetCandidate, errorZh)
        || !requireBool(object, QStringLiteral("interrupted"), &actualQty->interrupted, errorZh)) {
        return false;
    }
    if (!parseI64(object.value(QStringLiteral("baseline")), &actualQty->baseline)
        || !parseI64(object.value(QStringLiteral("resetCandidate")), &actualQty->resetCandidate)) {
        *errorZh = QStringLiteral("actualQty 64位字段格式非法");
        return false;
    }
    return true;
}

QJsonObject orderToJson(const ReplenishmentOrder &order)
{
    return {
        {QStringLiteral("orderNo"), u64(order.orderNo)},
        {QStringLiteral("stationId"), order.stationId},
        {QStringLiteral("origin"), originToString(order.origin)},
        {QStringLiteral("state"), orderStateToString(order.state)},
        {QStringLiteral("taskId"), u64(order.taskId)},
        {QStringLiteral("unloadAccounted"), order.unloadAccounted},
        {QStringLiteral("createdAtUtc"), dateToString(order.createdAtUtc)},
        {QStringLiteral("lastReasonZh"), order.lastReasonZh},
    };
}

bool orderFromJson(const QJsonObject &object, ReplenishmentOrder *order, QString *errorZh)
{
    if (!parseU64(object.value(QStringLiteral("orderNo")), &order->orderNo)
        || !parseU64(object.value(QStringLiteral("taskId")), &order->taskId)) {
        *errorZh = QStringLiteral("补料单64位编号格式非法");
        return false;
    }
    if (!requireInt(object, QStringLiteral("stationId"), &order->stationId, errorZh))
        return false;
    if (!originFromString(object.value(QStringLiteral("origin")).toString(), &order->origin)
        || !orderStateFromString(object.value(QStringLiteral("state")).toString(), &order->state)) {
        *errorZh = QStringLiteral("补料单枚举字段非法");
        return false;
    }
    QString createdAtUtc;
    if (!requireBool(object, QStringLiteral("unloadAccounted"), &order->unloadAccounted, errorZh)
        || !requireString(object, QStringLiteral("createdAtUtc"), &createdAtUtc, errorZh)
        || !requireString(object, QStringLiteral("lastReasonZh"), &order->lastReasonZh, errorZh)) {
        return false;
    }
    order->createdAtUtc = dateFromString(createdAtUtc);
    return true;
}

QJsonObject stateToJson(const ShortageRuntimeState &state)
{
    QJsonArray stations;
    for (const ShortageStationRuntime &station : state.stations)
        stations.append(stationToJson(station));

    QJsonArray waitingStationIds;
    for (int stationId : state.waitingStationIds)
        waitingStationIds.append(stationId);

    QJsonArray orders;
    for (const ReplenishmentOrder &order : state.orders)
        orders.append(orderToJson(order));

    return {
        {QStringLiteral("formatVersion"), state.formatVersion},
        {QStringLiteral("configurationRevision"), u64(state.configurationRevision)},
        {QStringLiteral("initialized"), state.initialized},
        {QStringLiteral("operatorConfirmedRestore"), state.operatorConfirmedRestore},
        {QStringLiteral("hasStableContext"), state.hasStableContext},
        {QStringLiteral("product"), productToString(state.product)},
        {QStringLiteral("mode"), modeToString(state.mode)},
        {QStringLiteral("hasPendingContext"), state.hasPendingContext},
        {QStringLiteral("pendingProduct"), productToString(state.pendingProduct)},
        {QStringLiteral("pendingMode"), modeToString(state.pendingMode)},
        {QStringLiteral("hasPendingActualQty"), state.hasPendingActualQty},
        {QStringLiteral("pendingActualQty"), i64(state.pendingActualQty)},
        {QStringLiteral("actualQty"), actualQtyToJson(state.actualQty)},
        {QStringLiteral("stations"), stations},
        {QStringLiteral("waitingStationIds"), waitingStationIds},
        {QStringLiteral("activeStationId"), state.activeStationId},
        {QStringLiteral("orders"), orders},
        {QStringLiteral("nextReplenishmentOrderNo"), u64(state.nextReplenishmentOrderNo)},
        {QStringLiteral("nextAuditSequence"), u64(state.nextAuditSequence)},
        {QStringLiteral("criticalLock"), state.criticalLock},
        {QStringLiteral("criticalReasonZh"), state.criticalReasonZh},
        {QStringLiteral("lastSavedAtUtc"), dateToString(state.lastSavedAtUtc)},
    };
}

bool stateFromJson(const QJsonObject &object, ShortageRuntimeState *state, QString *errorZh)
{
    ShortageRuntimeState parsed;
    if (!requireInt(object, QStringLiteral("formatVersion"), &parsed.formatVersion, errorZh))
        return false;
    if (parsed.formatVersion != kShortageStateFormatVersion) {
        *errorZh = QStringLiteral("状态格式版本%1不兼容，当前仅支持%2")
                       .arg(parsed.formatVersion)
                       .arg(kShortageStateFormatVersion);
        return false;
    }

    if (!parseU64(object.value(QStringLiteral("configurationRevision")),
                  &parsed.configurationRevision)
        || !parseI64(object.value(QStringLiteral("pendingActualQty")),
                     &parsed.pendingActualQty)
        || !parseU64(object.value(QStringLiteral("nextReplenishmentOrderNo")),
                     &parsed.nextReplenishmentOrderNo)
        || !parseU64(object.value(QStringLiteral("nextAuditSequence")),
                     &parsed.nextAuditSequence)) {
        *errorZh = QStringLiteral("运行状态64位字段格式非法");
        return false;
    }

    if (!requireBool(object, QStringLiteral("initialized"), &parsed.initialized, errorZh)
        || !requireBool(object, QStringLiteral("operatorConfirmedRestore"),
                        &parsed.operatorConfirmedRestore, errorZh)
        || !requireBool(object, QStringLiteral("hasStableContext"), &parsed.hasStableContext, errorZh)
        || !requireBool(object, QStringLiteral("hasPendingContext"), &parsed.hasPendingContext, errorZh)
        || !requireBool(object, QStringLiteral("hasPendingActualQty"),
                        &parsed.hasPendingActualQty, errorZh)
        || !requireInt(object, QStringLiteral("activeStationId"), &parsed.activeStationId, errorZh)
        || !requireBool(object, QStringLiteral("criticalLock"), &parsed.criticalLock, errorZh)
        || !requireString(object, QStringLiteral("criticalReasonZh"),
                          &parsed.criticalReasonZh, errorZh)) {
        return false;
    }
    QString lastSavedAtUtc;
    if (!requireString(object, QStringLiteral("lastSavedAtUtc"), &lastSavedAtUtc, errorZh))
        return false;
    parsed.lastSavedAtUtc = dateFromString(lastSavedAtUtc);

    QString product;
    QString pendingProduct;
    QString mode;
    QString pendingMode;
    QJsonObject actualQty;
    if (!requireString(object, QStringLiteral("product"), &product, errorZh)
        || !requireString(object, QStringLiteral("pendingProduct"), &pendingProduct, errorZh)
        || !requireString(object, QStringLiteral("mode"), &mode, errorZh)
        || !requireString(object, QStringLiteral("pendingMode"), &pendingMode, errorZh)
        || !requireObject(object, QStringLiteral("actualQty"), &actualQty, errorZh)) {
        return false;
    }

    if (!productFromString(product, &parsed.product)
        || !productFromString(pendingProduct, &parsed.pendingProduct)
        || !modeFromString(mode, &parsed.mode)
        || !modeFromString(pendingMode, &parsed.pendingMode)
        || !actualQtyFromJson(actualQty, &parsed.actualQty, errorZh)) {
        if (errorZh->isEmpty())
            *errorZh = QStringLiteral("运行状态枚举字段非法");
        return false;
    }

    QJsonArray stations;
    if (!requireArray(object, QStringLiteral("stations"), &stations, errorZh))
        return false;
    if (stations.size() != 12) {
        *errorZh = QStringLiteral("工位状态数量必须恰好为12，当前为%1").arg(stations.size());
        return false;
    }
    QSet<int> stationIds;
    for (const QJsonValue &value : stations) {
        if (!value.isObject()) {
            *errorZh = QStringLiteral("工位状态格式非法");
            return false;
        }
        ShortageStationRuntime station;
        if (!stationFromJson(value.toObject(), &station, errorZh))
            return false;
        if (stationIds.contains(station.stationId)) {
            *errorZh = QStringLiteral("工位%1状态重复").arg(station.stationId);
            return false;
        }
        stationIds.insert(station.stationId);
        parsed.stations.append(station);
    }
    for (int stationId = 1; stationId <= 12; ++stationId) {
        if (!stationIds.contains(stationId)) {
            *errorZh = QStringLiteral("工位%1状态缺失").arg(stationId);
            return false;
        }
    }

    QJsonArray waitingStationIds;
    if (!requireArray(object, QStringLiteral("waitingStationIds"), &waitingStationIds, errorZh))
        return false;
    for (const QJsonValue &value : waitingStationIds) {
        if (!value.isDouble()) {
            *errorZh = QStringLiteral("等待工位字段格式非法");
            return false;
        }
        parsed.waitingStationIds.append(value.toInt());
    }

    QJsonArray orders;
    if (!requireArray(object, QStringLiteral("orders"), &orders, errorZh))
        return false;
    for (const QJsonValue &value : orders) {
        if (!value.isObject()) {
            *errorZh = QStringLiteral("补料单字段格式非法");
            return false;
        }
        ReplenishmentOrder order;
        if (!orderFromJson(value.toObject(), &order, errorZh))
            return false;
        parsed.orders.append(order);
    }

    *state = parsed;
    return true;
}

QByteArray checksumForEnvelope(QJsonObject envelope)
{
    envelope.remove(QStringLiteral("checksum"));
    const QByteArray bytes = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

QJsonObject snapshotEnvelope(const ShortageRuntimeState &state)
{
    QJsonObject envelope {
        {QStringLiteral("state"), stateToJson(state)},
    };
    envelope.insert(QStringLiteral("checksum"), QString::fromLatin1(checksumForEnvelope(envelope)));
    return envelope;
}

bool readSnapshot(const QString &path, ShortageRuntimeState *state, QString *errorZh)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *errorZh = QStringLiteral("无法读取快照 %1：%2").arg(path, file.errorString());
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *errorZh = QStringLiteral("快照 JSON 损坏：%1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject envelope = document.object();
    const QString expected = QString::fromLatin1(checksumForEnvelope(envelope));
    const QString actual = envelope.value(QStringLiteral("checksum")).toString();
    if (actual.isEmpty() || actual != expected) {
        *errorZh = QStringLiteral("快照校验和不一致，拒绝进入自动模式");
        return false;
    }

    return stateFromJson(envelope.value(QStringLiteral("state")).toObject(), state, errorZh);
}

QJsonObject eventToJson(const ShortageAuditEvent &event,
                        const ShortageRuntimeState &stateAfter)
{
    return {
        {QStringLiteral("sequence"), u64(event.sequence)},
        {QStringLiteral("occurredAtUtc"), dateToString(event.occurredAtUtc)},
        {QStringLiteral("eventType"), event.eventType},
        {QStringLiteral("messageZh"), event.messageZh},
        {QStringLiteral("details"), event.details},
        {QStringLiteral("stateAfter"), stateToJson(stateAfter)},
    };
}

ShortageOperationResult appendJournal(const QString &path,
                                      const ShortageRuntimeState &state,
                                      const ShortageAuditEvent &event)
{
    if (event.sequence == 0 || event.eventType.isEmpty())
        return {true, QStringLiteral("状态未变化，未写入流水")};
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return {false, QStringLiteral("无法创建状态目录 %1").arg(QFileInfo(path).absolutePath())};
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return {false, QStringLiteral("无法追加状态流水 %1：%2").arg(path, file.errorString())};
    }
    const QByteArray line =
        QJsonDocument(eventToJson(event, state)).toJson(QJsonDocument::Compact) + '\n';
    if (file.write(line) != line.size()) {
        return {false, QStringLiteral("写入状态流水 %1 失败：%2").arg(path, file.errorString())};
    }
    return {true, QStringLiteral("状态流水已写入")};
}

ShortageOperationResult writeSnapshot(const QString &mainPath,
                                      const QString &backupPath,
                                      const ShortageRuntimeState &state)
{
    if (!QDir().mkpath(QFileInfo(mainPath).absolutePath())) {
        return {false, QStringLiteral("无法创建状态目录 %1").arg(QFileInfo(mainPath).absolutePath())};
    }

    if (QFile::exists(mainPath)) {
        QFile mainFile(mainPath);
        if (!mainFile.open(QIODevice::ReadOnly)) {
            return {false, QStringLiteral("写入主快照前读取现有快照 %1 失败：%2")
                               .arg(mainPath, mainFile.errorString())};
        }
        const QByteArray backupBytes = mainFile.readAll();
        QSaveFile backupFile(backupPath);
        if (!backupFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return {false, QStringLiteral("写入主快照前打开备份 %1 失败：%2")
                               .arg(backupPath, backupFile.errorString())};
        }
        if (backupFile.write(backupBytes) != backupBytes.size()) {
            return {false, QStringLiteral("写入主快照前写入备份 %1 失败：%2")
                               .arg(backupPath, backupFile.errorString())};
        }
        if (!backupFile.commit()) {
            return {false, QStringLiteral("写入主快照前创建备份 %1 失败").arg(backupPath)};
        }
    }

    QSaveFile file(mainPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return {false, QStringLiteral("无法打开状态快照 %1：%2").arg(mainPath, file.errorString())};
    }
    const QByteArray bytes =
        QJsonDocument(snapshotEnvelope(state)).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size()) {
        return {false, QStringLiteral("写入状态快照 %1 失败：%2").arg(mainPath, file.errorString())};
    }
    if (!file.commit()) {
        return {false, QStringLiteral("原子替换状态快照 %1 失败：%2").arg(mainPath, file.errorString())};
    }
    return {true, QStringLiteral("状态快照已保存")};
}

struct ReplayResult {
    bool ok = false;             ///< true 表示快照后流水连续完整。
    bool applied = false;        ///< true 表示至少重放一条快照后的流水。
    ShortageRuntimeState state;  ///< 重放后的状态。
    QString errorZh;             ///< 不完整或损坏的中文原因。
};

ReplayResult replayJournal(const QString &journalPath, const ShortageRuntimeState &snapshot)
{
    ReplayResult result;
    result.state = snapshot;
    result.ok = true;

    QFile file(journalPath);
    if (!file.exists())
        return result;
    if (!file.open(QIODevice::ReadOnly)) {
        result.ok = false;
        result.errorZh = QStringLiteral("无法读取流水 %1：%2").arg(journalPath, file.errorString());
        return result;
    }

    quint64 expected = snapshot.nextAuditSequence;
    QSet<quint64> seen;
    int lineNo = 0;
    while (!file.atEnd()) {
        ++lineNo;
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            result.ok = false;
            result.errorZh = QStringLiteral("流水第%1行 JSON 损坏：%2")
                                 .arg(lineNo)
                                 .arg(parseError.errorString());
            return result;
        }
        const QJsonObject object = document.object();
        quint64 sequence = 0;
        if (!parseU64(object.value(QStringLiteral("sequence")), &sequence) || sequence == 0) {
            result.ok = false;
            result.errorZh = QStringLiteral("流水第%1行缺少合法 sequence").arg(lineNo);
            return result;
        }
        if (seen.contains(sequence)) {
            result.ok = false;
            result.errorZh = QStringLiteral("流水 sequence %1 重复，拒绝使用裸快照").arg(sequence);
            return result;
        }
        seen.insert(sequence);

        if (sequence < expected)
            continue;
        if (sequence > expected) {
            result.ok = false;
            result.errorZh = QStringLiteral("流水 sequence %1 前存在缺口，应为 %2")
                                 .arg(sequence)
                                 .arg(expected);
            return result;
        }

        ShortageRuntimeState stateAfter;
        QString errorZh;
        if (!stateFromJson(object.value(QStringLiteral("stateAfter")).toObject(),
                           &stateAfter, &errorZh)) {
            result.ok = false;
            result.errorZh = QStringLiteral("流水 sequence %1 无法恢复状态：%2")
                                 .arg(sequence)
                                 .arg(errorZh);
            return result;
        }
        if (stateAfter.nextAuditSequence != sequence + 1) {
            result.ok = false;
            result.errorZh = QStringLiteral("流水 sequence %1 与状态 nextAuditSequence 不连续")
                                 .arg(sequence);
            return result;
        }
        result.state = stateAfter;
        expected = stateAfter.nextAuditSequence;
        result.applied = true;
    }
    return result;
}

ShortageStateLoadResult loadFromSnapshot(const QString &snapshotPath,
                                         const QString &journalPath,
                                         ShortageRestoreSource snapshotSource)
{
    ShortageRuntimeState snapshot;
    QString snapshotError;
    if (!readSnapshot(snapshotPath, &snapshot, &snapshotError)) {
        return {false, QFile::exists(snapshotPath), false,
                ShortageRestoreSource::None, {}, snapshotError};
    }

    const ReplayResult replay = replayJournal(journalPath, snapshot);
    if (!replay.ok) {
        return {false, true, true, ShortageRestoreSource::None, {},
                QStringLiteral("快照可读但后续流水不完整：%1").arg(replay.errorZh)};
    }

    ShortageStateLoadResult loaded;
    loaded.ok = true;
    loaded.stateFound = true;
    loaded.requiresMaintenance = false;
    loaded.source = replay.applied ? ShortageRestoreSource::Journal : snapshotSource;
    loaded.state = replay.state;
    loaded.messageZh = replay.applied
        ? QStringLiteral("已从快照和连续流水安全恢复")
        : QStringLiteral("已从快照安全恢复");

    // 启动恢复后的人工确认是安全门禁；账本可读但自动派单保持锁定。
    if (!loaded.state.operatorConfirmedRestore) {
        loaded.state.criticalLock = true;
        loaded.state.criticalReasonZh = QStringLiteral("恢复后尚未人工确认，禁止自动派单");
        loaded.messageZh += QStringLiteral("，等待人工确认后才能自动派单");
    }
    return loaded;
}

} // namespace

ShortageStateStore::ShortageStateStore(QString baseDirectory,
                                       ShortageStateNamespace stateNamespace)
    : baseDirectory_(std::move(baseDirectory)),
      stateNamespace_(stateNamespace)
{
}

ShortageStateLoadResult ShortageStateStore::load() const
{
    const StatePaths paths = pathsFor(baseDirectory_, stateNamespace_);
    if (!QFile::exists(paths.mainPath)
        && !QFile::exists(paths.backupPath)
        && !QFile::exists(paths.journalPath)) {
        ShortageRuntimeState locked;
        locked.criticalLock = true;
        locked.criticalReasonZh = QStringLiteral("没有状态账本，必须现场清空确认后才能建账");
        return {false, false, true, ShortageRestoreSource::None, locked,
                QStringLiteral("未找到状态账本；需要现场清空确认后新建，自动模式保持锁定")};
    }

    const ShortageStateLoadResult main =
        loadFromSnapshot(paths.mainPath, paths.journalPath, ShortageRestoreSource::Main);
    if (main.ok)
        return main;

    const ShortageStateLoadResult backup =
        loadFromSnapshot(paths.backupPath, paths.journalPath, ShortageRestoreSource::Backup);
    if (backup.ok)
        return backup;

    ShortageRuntimeState locked;
    locked.criticalLock = true;
    locked.criticalReasonZh = QStringLiteral("主快照、备份或流水损坏，需要维护人员处理");
    return {false, main.stateFound || backup.stateFound || QFile::exists(paths.journalPath),
            true, ShortageRestoreSource::None, locked,
            QStringLiteral("状态恢复失败：主快照=%1；备份=%2；请联系维护人员")
                .arg(main.messageZh, backup.messageZh)};
}

ShortageOperationResult ShortageStateStore::saveCritical(const ShortageRuntimeState &state,
                                                          const ShortageAuditEvent &event)
{
    const StatePaths paths = pathsFor(baseDirectory_, stateNamespace_);
    const ShortageOperationResult journal = appendJournal(paths.journalPath, state, event);
    if (!journal.ok)
        return journal;
    if (event.sequence == 0 || event.eventType.isEmpty())
        return journal;
    ShortageRuntimeState snapshot = state;
    if (!snapshot.lastSavedAtUtc.isValid())
        snapshot.lastSavedAtUtc = event.occurredAtUtc.toUTC();
    return writeSnapshot(paths.mainPath, paths.backupPath, snapshot);
}

ShortageOperationResult ShortageStateStore::savePeriodic(const ShortageRuntimeState &state,
                                                          const ShortageAuditEvent &event,
                                                          const QDateTime &nowUtc)
{
    const StatePaths paths = pathsFor(baseDirectory_, stateNamespace_);
    const ShortageOperationResult journal = appendJournal(paths.journalPath, state, event);
    if (!journal.ok)
        return journal;
    if (event.sequence == 0 || event.eventType.isEmpty())
        return journal;

    if (!state.lastSavedAtUtc.isValid()
        || state.lastSavedAtUtc.secsTo(nowUtc.toUTC()) >= 60
        || !QFile::exists(paths.mainPath)) {
        ShortageRuntimeState snapshot = state;
        snapshot.lastSavedAtUtc = nowUtc.toUTC();
        return writeSnapshot(paths.mainPath, paths.backupPath, snapshot);
    }
    return {true, QStringLiteral("状态流水已写入，距离上次快照不足60秒")};
}

ShortageOperationResult ShortageStateStore::backupBeforeMaintenance(
    const ShortageRuntimeState &state)
{
    const StatePaths paths = pathsFor(baseDirectory_, stateNamespace_);
    if (!QDir().mkpath(paths.maintenancePath)) {
        return {false, QStringLiteral("无法创建维护备份目录 %1").arg(paths.maintenancePath)};
    }
    const QString stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    const QString fileName = stateNamespace_ == ShortageStateNamespace::Production
        ? QStringLiteral("production-state-%1.json").arg(stamp)
        : QStringLiteral("test-state-%1.json").arg(stamp);
    const QString targetPath = QDir(paths.maintenancePath).filePath(fileName);

    QSaveFile file(targetPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return {false, QStringLiteral("无法打开维护备份 %1：%2").arg(targetPath, file.errorString())};
    }
    const QByteArray bytes =
        QJsonDocument(snapshotEnvelope(state)).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size()) {
        return {false, QStringLiteral("写入维护备份 %1 失败：%2").arg(targetPath, file.errorString())};
    }
    if (!file.commit()) {
        return {false, QStringLiteral("原子保存维护备份 %1 失败：%2").arg(targetPath, file.errorString())};
    }
    return {true, QStringLiteral("维护前状态备份已保存")};
}
