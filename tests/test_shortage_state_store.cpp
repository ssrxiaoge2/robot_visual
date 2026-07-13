#include "shortagestatestore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

namespace {

QString productionMain(const QTemporaryDir &dir)
{
    return dir.filePath(QStringLiteral("production-state.json"));
}

QString productionBackup(const QTemporaryDir &dir)
{
    return dir.filePath(QStringLiteral("production-state.backup.json"));
}

QString productionJournal(const QTemporaryDir &dir)
{
    return dir.filePath(QStringLiteral("production-events.jsonl"));
}

QString testMain(const QTemporaryDir &dir)
{
    return dir.filePath(QStringLiteral("test-state.json"));
}

ShortageRuntimeState sampleState(quint64 nextSequence = 2)
{
    ShortageRuntimeState state;
    state.configurationRevision = 42;
    state.initialized = true;
    state.operatorConfirmedRestore = true;
    state.hasStableContext = true;
    state.product = ProductModel::Model88R;
    state.mode = ProductionMode::LeftOnly;
    state.actualQty.hasBaseline = true;
    state.actualQty.baseline = 1000;
    state.nextAuditSequence = nextSequence;
    state.nextReplenishmentOrderNo = 9;
    state.stations.reserve(12);
    for (int i = 1; i <= 12; ++i) {
        ShortageStationRuntime station;
        station.stationId = i;
        station.stock = 1000 + i;
        state.stations.append(station);
    }
    state.waitingStationIds = {3, 5};
    state.activeStationId = 3;
    return state;
}

ShortageAuditEvent auditEvent(quint64 sequence,
                         const QString &eventType = QStringLiteral("sample_applied"))
{
    ShortageAuditEvent audit;
    audit.sequence = sequence;
    audit.occurredAtUtc = QDateTime::fromString(QStringLiteral("2026-07-13T01:02:03Z"),
                                                Qt::ISODate);
    audit.eventType = eventType;
    audit.messageZh = QStringLiteral("测试流水 %1").arg(sequence);
    audit.details = {{QStringLiteral("stationId"), 3},
                     {QStringLiteral("newStock"), QString::number(900 - int(sequence))}};
    return audit;
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

int nonEmptyLineCount(const QString &path)
{
    int count = 0;
    const QList<QByteArray> lines = readAll(path).split('\n');
    for (const QByteArray &line : lines) {
        if (!line.trimmed().isEmpty())
            ++count;
    }
    return count;
}

void overwrite(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(bytes), qint64(bytes.size()));
}

void refreshChecksum(QJsonObject *root)
{
    root->remove(QStringLiteral("checksum"));
    const QByteArray bytes = QJsonDocument(*root).toJson(QJsonDocument::Compact);
    const QByteArray checksum =
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    root->insert(QStringLiteral("checksum"), QString::fromLatin1(checksum));
}

QJsonObject snapshotRoot(const QString &path)
{
    return QJsonDocument::fromJson(readAll(path)).object();
}

void overwriteSnapshotState(const QString &path, const QJsonObject &stateObject)
{
    QJsonObject root = snapshotRoot(path);
    root[QStringLiteral("state")] = stateObject;
    refreshChecksum(&root);
    overwrite(path, QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void overwriteProductionSnapshotOnly(const QTemporaryDir &dir, const QJsonObject &stateObject)
{
    overwriteSnapshotState(productionMain(dir), stateObject);
    QVERIFY(QFile::remove(productionJournal(dir)));
}

QDateTime utcFromSeconds(qint64 seconds)
{
    return QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::UTC);
}

} // namespace

class ShortageStateStoreTest final : public QObject
{
    Q_OBJECT
private slots:
    void unchangedStateDoesNotWrite();
    void oneSampleWritesOneAggregateEvent();
    void periodicSnapshotIsLimitedToSixtySeconds();
    void criticalEventsForceImmediateSnapshot();
    void interruptedTemporaryWriteKeepsMainFile();
    void corruptedMainFallsBackToBackup();
    void journalReplayRestoresPostSnapshotState();
    void journalGapOrDuplicateRejectsBareSnapshot();
    void checksumAndVersionErrorsAreRejected();
    void semanticSnapshotErrorsAreRejected();
    void allCorruptSourcesLockAutomaticMode();
    void testNamespaceNeverTouchesProductionFiles();
    void persistenceFailureAfterUnloadKeepsMemoryFact();
    void safeRestoreRequiresOperatorConfirmation();
    void zeroLedgerRequiresSiteClearConfirmation();
};

void ShortageStateStoreTest::unchangedStateDoesNotWrite()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ShortageStateStore store(dir.path(), ShortageStateNamespace::Production);
    ShortageRuntimeState state = sampleState(1);

    const ShortageOperationResult result =
        store.savePeriodic(state, ShortageAuditEvent{}, QDateTime::currentDateTimeUtc());

    QVERIFY2(result.ok, qPrintable(result.messageZh));
    QVERIFY(!QFile::exists(productionMain(dir)));
    QVERIFY(!QFile::exists(productionJournal(dir)));
}

void ShortageStateStoreTest::oneSampleWritesOneAggregateEvent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ShortageStateStore store(dir.path(), ShortageStateNamespace::Production);
    ShortageRuntimeState state = sampleState(2);

    const ShortageOperationResult result =
        store.savePeriodic(state, auditEvent(1), utcFromSeconds(1000));

    QVERIFY2(result.ok, qPrintable(result.messageZh));
    QCOMPARE(nonEmptyLineCount(productionJournal(dir)), 1);
    QVERIFY(readAll(productionJournal(dir)).contains("sample_applied"));
}

void ShortageStateStoreTest::periodicSnapshotIsLimitedToSixtySeconds()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ShortageStateStore store(dir.path(), ShortageStateNamespace::Production);

    ShortageRuntimeState first = sampleState(2);
    first.lastSavedAtUtc = utcFromSeconds(1000);
    QVERIFY(store.savePeriodic(first, auditEvent(1), first.lastSavedAtUtc).ok);
    const QByteArray firstSnapshot = readAll(productionMain(dir));
    QVERIFY(!firstSnapshot.isEmpty());

    const ShortageStateLoadResult firstLoaded = store.load();
    QVERIFY2(firstLoaded.ok, qPrintable(firstLoaded.messageZh));
    QCOMPARE(firstLoaded.state.lastSavedAtUtc, first.lastSavedAtUtc);

    ShortageRuntimeState second = first;
    second.nextAuditSequence = 3;
    second.stations[0].stock = 777;
    second.lastSavedAtUtc = first.lastSavedAtUtc;
    QVERIFY(store.savePeriodic(second, auditEvent(2),
                                first.lastSavedAtUtc.addSecs(59)).ok);

    QCOMPARE(readAll(productionMain(dir)), firstSnapshot);
    QCOMPARE(nonEmptyLineCount(productionJournal(dir)), 2);

    ShortageRuntimeState third = second;
    third.nextAuditSequence = 4;
    third.lastSavedAtUtc = first.lastSavedAtUtc;
    const QDateTime thirdSaveTime = first.lastSavedAtUtc.addSecs(60);
    QVERIFY(store.savePeriodic(third, auditEvent(3), thirdSaveTime).ok);

    const ShortageStateLoadResult loaded = store.load();
    QVERIFY2(loaded.ok, qPrintable(loaded.messageZh));
    QCOMPARE(loaded.state.lastSavedAtUtc, thirdSaveTime);
}

void ShortageStateStoreTest::criticalEventsForceImmediateSnapshot()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ShortageStateStore store(dir.path(), ShortageStateNamespace::Production);

    ShortageRuntimeState state = sampleState(2);
    state.stations[2].stock = -5;
    const ShortageOperationResult result =
        store.saveCritical(state, auditEvent(1, QStringLiteral("box_unloaded")));

    QVERIFY2(result.ok, qPrintable(result.messageZh));
    QVERIFY(QFile::exists(productionMain(dir)));
    QCOMPARE(nonEmptyLineCount(productionJournal(dir)), 1);
    QVERIFY(readAll(productionMain(dir)).contains("\"-5\""));
}

void ShortageStateStoreTest::interruptedTemporaryWriteKeepsMainFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ShortageStateStore store(dir.path(), ShortageStateNamespace::Production);
    ShortageRuntimeState state = sampleState(2);
    QVERIFY(store.saveCritical(state, auditEvent(1)).ok);
    const QByteArray mainBefore = readAll(productionMain(dir));

    overwrite(dir.filePath(QStringLiteral("production-state.json.tmp")),
              QByteArrayLiteral("{\"partial\":true}"));

    QCOMPARE(readAll(productionMain(dir)), mainBefore);
    const ShortageStateLoadResult loaded = store.load();
    QVERIFY2(loaded.ok, qPrintable(loaded.messageZh));
    QCOMPARE(loaded.state.nextAuditSequence, quint64{2});
}

void ShortageStateStoreTest::corruptedMainFallsBackToBackup()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ShortageStateStore store(dir.path(), ShortageStateNamespace::Production);

    ShortageRuntimeState first = sampleState(2);
    first.stations[0].stock = 111;
    QVERIFY(store.saveCritical(first, auditEvent(1)).ok);
    ShortageRuntimeState second = first;
    second.nextAuditSequence = 3;
    second.stations[0].stock = 222;
    QVERIFY(store.saveCritical(second, auditEvent(2)).ok);
    overwrite(productionMain(dir), QByteArrayLiteral("{broken"));

    const ShortageStateLoadResult loaded = store.load();
    QVERIFY2(loaded.ok, qPrintable(loaded.messageZh));
    QCOMPARE(loaded.source, ShortageRestoreSource::Journal);
    QCOMPARE(loaded.state.stations[0].stock, qint64{222});
}

void ShortageStateStoreTest::journalReplayRestoresPostSnapshotState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ShortageStateStore store(dir.path(), ShortageStateNamespace::Production);

    ShortageRuntimeState first = sampleState(2);
    first.stations[0].stock = 123;
    first.lastSavedAtUtc = utcFromSeconds(1000);
    QVERIFY(store.savePeriodic(first, auditEvent(1), first.lastSavedAtUtc).ok);

    ShortageRuntimeState second = first;
    second.nextAuditSequence = 3;
    second.stations[0].stock = 321;
    second.lastSavedAtUtc = first.lastSavedAtUtc;
    QVERIFY(store.savePeriodic(second, auditEvent(2),
                                first.lastSavedAtUtc.addSecs(10)).ok);

    const ShortageStateLoadResult loaded = store.load();
    QVERIFY2(loaded.ok, qPrintable(loaded.messageZh));
    QCOMPARE(loaded.source, ShortageRestoreSource::Journal);
    QCOMPARE(loaded.state.stations[0].stock, qint64{321});
    QCOMPARE(loaded.state.nextAuditSequence, quint64{3});
}

void ShortageStateStoreTest::journalGapOrDuplicateRejectsBareSnapshot()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ShortageStateStore store(dir.path(), ShortageStateNamespace::Production);
    ShortageRuntimeState state = sampleState(2);
    QVERIFY(store.saveCritical(state, auditEvent(1)).ok);
    overwrite(productionJournal(dir), readAll(productionJournal(dir))
                                    + readAll(productionJournal(dir)));

    const ShortageStateLoadResult duplicate = store.load();
    QVERIFY(!duplicate.ok);
    QVERIFY(duplicate.requiresMaintenance);

    QVERIFY(store.saveCritical(state, auditEvent(1)).ok);
    overwrite(productionJournal(dir), QByteArrayLiteral("{\"sequence\":\"3\"}\n"));
    const ShortageStateLoadResult gap = store.load();
    QVERIFY(!gap.ok);
    QVERIFY(gap.requiresMaintenance);
}

void ShortageStateStoreTest::checksumAndVersionErrorsAreRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ShortageStateStore store(dir.path(), ShortageStateNamespace::Production);
    ShortageRuntimeState state = sampleState(2);
    QVERIFY(store.saveCritical(state, auditEvent(1)).ok);

    QJsonObject root = QJsonDocument::fromJson(readAll(productionMain(dir))).object();
    root[QStringLiteral("state")] = QJsonObject{{QStringLiteral("formatVersion"), 999}};
    overwrite(productionMain(dir), QJsonDocument(root).toJson(QJsonDocument::Compact));
    ShortageStateLoadResult badChecksum = store.load();
    QVERIFY(!badChecksum.ok);
    QVERIFY(badChecksum.messageZh.contains(QStringLiteral("校验")));

    QVERIFY(store.saveCritical(state, auditEvent(1)).ok);
    root = QJsonDocument::fromJson(readAll(productionMain(dir))).object();
    QJsonObject stateObject = root.value(QStringLiteral("state")).toObject();
    stateObject[QStringLiteral("formatVersion")] = 999;
    root[QStringLiteral("state")] = stateObject;
    refreshChecksum(&root);
    overwrite(productionMain(dir), QJsonDocument(root).toJson(QJsonDocument::Compact));
    ShortageStateLoadResult badVersion = store.load();
    QVERIFY(!badVersion.ok);
    QVERIFY(badVersion.messageZh.contains(QStringLiteral("版本")));
}

void ShortageStateStoreTest::semanticSnapshotErrorsAreRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ShortageStateStore store(dir.path(), ShortageStateNamespace::Production);
    ShortageRuntimeState state = sampleState(1);
    QVERIFY(store.saveCritical(state, auditEvent(1)).ok);

    QJsonObject stateObject = snapshotRoot(productionMain(dir)).value(QStringLiteral("state")).toObject();
    QJsonArray stations = stateObject.value(QStringLiteral("stations")).toArray();
    stations.removeLast();
    stateObject[QStringLiteral("stations")] = stations;
    overwriteProductionSnapshotOnly(dir, stateObject);
    ShortageStateLoadResult missingStation = store.load();
    QVERIFY(!missingStation.ok);
    QVERIFY(missingStation.requiresMaintenance);
    QVERIFY(missingStation.messageZh.contains(QStringLiteral("工位")));

    QVERIFY(store.saveCritical(state, auditEvent(1)).ok);
    stateObject = snapshotRoot(productionMain(dir)).value(QStringLiteral("state")).toObject();
    stations = stateObject.value(QStringLiteral("stations")).toArray();
    QJsonObject duplicateStation = stations.at(1).toObject();
    duplicateStation[QStringLiteral("stationId")] = 1;
    stations[1] = duplicateStation;
    stateObject[QStringLiteral("stations")] = stations;
    overwriteProductionSnapshotOnly(dir, stateObject);
    ShortageStateLoadResult duplicateStationLoad = store.load();
    QVERIFY(!duplicateStationLoad.ok);
    QVERIFY(duplicateStationLoad.requiresMaintenance);
    QVERIFY(duplicateStationLoad.messageZh.contains(QStringLiteral("工位")));

    QVERIFY(store.saveCritical(state, auditEvent(1)).ok);
    stateObject = snapshotRoot(productionMain(dir)).value(QStringLiteral("state")).toObject();
    stations = stateObject.value(QStringLiteral("stations")).toArray();
    QJsonObject outOfRangeStation = stations.at(11).toObject();
    outOfRangeStation[QStringLiteral("stationId")] = 13;
    stations[11] = outOfRangeStation;
    stateObject[QStringLiteral("stations")] = stations;
    overwriteProductionSnapshotOnly(dir, stateObject);
    ShortageStateLoadResult outOfRangeStationLoad = store.load();
    QVERIFY(!outOfRangeStationLoad.ok);
    QVERIFY(outOfRangeStationLoad.requiresMaintenance);
    QVERIFY(outOfRangeStationLoad.messageZh.contains(QStringLiteral("工位")));

    QVERIFY(store.saveCritical(state, auditEvent(1)).ok);
    stateObject = snapshotRoot(productionMain(dir)).value(QStringLiteral("state")).toObject();
    stateObject.remove(QStringLiteral("configurationRevision"));
    overwriteProductionSnapshotOnly(dir, stateObject);
    ShortageStateLoadResult missingScalar = store.load();
    QVERIFY(!missingScalar.ok);
    QVERIFY(missingScalar.requiresMaintenance);
    QVERIFY(missingScalar.messageZh.contains(QStringLiteral("字段")));

    QVERIFY(store.saveCritical(state, auditEvent(1)).ok);
    stateObject = snapshotRoot(productionMain(dir)).value(QStringLiteral("state")).toObject();
    stateObject[QStringLiteral("activeStationId")] = QStringLiteral("3");
    overwriteProductionSnapshotOnly(dir, stateObject);
    ShortageStateLoadResult malformedScalar = store.load();
    QVERIFY(!malformedScalar.ok);
    QVERIFY(malformedScalar.requiresMaintenance);
    QVERIFY(malformedScalar.messageZh.contains(QStringLiteral("字段")));

    QVERIFY(store.saveCritical(state, auditEvent(1)).ok);
    stateObject = snapshotRoot(productionMain(dir)).value(QStringLiteral("state")).toObject();
    stateObject[QStringLiteral("activeStationId")] = 13;
    overwriteProductionSnapshotOnly(dir, stateObject);
    ShortageStateLoadResult activeOutOfRange = store.load();
    QVERIFY(!activeOutOfRange.ok);
    QVERIFY(activeOutOfRange.requiresMaintenance);
    QVERIFY(activeOutOfRange.messageZh.contains(QStringLiteral("活动工位")));

    QVERIFY(store.saveCritical(state, auditEvent(1)).ok);
    stateObject = snapshotRoot(productionMain(dir)).value(QStringLiteral("state")).toObject();
    stateObject[QStringLiteral("waitingStationIds")] = QJsonArray{99};
    overwriteProductionSnapshotOnly(dir, stateObject);
    ShortageStateLoadResult waitingOutOfRange = store.load();
    QVERIFY(!waitingOutOfRange.ok);
    QVERIFY(waitingOutOfRange.requiresMaintenance);
    QVERIFY(waitingOutOfRange.messageZh.contains(QStringLiteral("等待工位")));

    QVERIFY(store.saveCritical(state, auditEvent(1)).ok);
    stateObject = snapshotRoot(productionMain(dir)).value(QStringLiteral("state")).toObject();
    stateObject[QStringLiteral("waitingStationIds")] = QJsonArray{3, 3};
    overwriteProductionSnapshotOnly(dir, stateObject);
    ShortageStateLoadResult duplicateWaiting = store.load();
    QVERIFY(!duplicateWaiting.ok);
    QVERIFY(duplicateWaiting.requiresMaintenance);
    QVERIFY(duplicateWaiting.messageZh.contains(QStringLiteral("等待工位")));

    ShortageRuntimeState orderState = state;
    ReplenishmentOrder order;
    order.orderNo = 1;
    order.stationId = 3;
    order.taskId = 0;
    order.createdAtUtc = utcFromSeconds(2000);
    orderState.orders = {order};
    QVERIFY(store.saveCritical(orderState, auditEvent(1)).ok);
    stateObject = snapshotRoot(productionMain(dir)).value(QStringLiteral("state")).toObject();
    QJsonArray orders = stateObject.value(QStringLiteral("orders")).toArray();
    QJsonObject storedOrder = orders.at(0).toObject();
    storedOrder[QStringLiteral("stationId")] = 99;
    orders[0] = storedOrder;
    stateObject[QStringLiteral("orders")] = orders;
    overwriteProductionSnapshotOnly(dir, stateObject);
    ShortageStateLoadResult orderStationOutOfRange = store.load();
    QVERIFY(!orderStationOutOfRange.ok);
    QVERIFY(orderStationOutOfRange.requiresMaintenance);
    QVERIFY(orderStationOutOfRange.messageZh.contains(QStringLiteral("补料单")));

    QVERIFY(store.saveCritical(orderState, auditEvent(1)).ok);
    stateObject = snapshotRoot(productionMain(dir)).value(QStringLiteral("state")).toObject();
    stateObject[QStringLiteral("lastSavedAtUtc")] = QStringLiteral("not-a-date");
    overwriteProductionSnapshotOnly(dir, stateObject);
    ShortageStateLoadResult badRequiredTimestamp = store.load();
    QVERIFY(!badRequiredTimestamp.ok);
    QVERIFY(badRequiredTimestamp.requiresMaintenance);
    QVERIFY(badRequiredTimestamp.messageZh.contains(QStringLiteral("时间")));

    QVERIFY(store.saveCritical(orderState, auditEvent(1)).ok);
    stateObject = snapshotRoot(productionMain(dir)).value(QStringLiteral("state")).toObject();
    orders = stateObject.value(QStringLiteral("orders")).toArray();
    storedOrder = orders.at(0).toObject();
    storedOrder[QStringLiteral("createdAtUtc")] = QStringLiteral("not-a-date");
    orders[0] = storedOrder;
    stateObject[QStringLiteral("orders")] = orders;
    overwriteProductionSnapshotOnly(dir, stateObject);
    ShortageStateLoadResult badOrderTimestamp = store.load();
    QVERIFY(!badOrderTimestamp.ok);
    QVERIFY(badOrderTimestamp.requiresMaintenance);
    QVERIFY(badOrderTimestamp.messageZh.contains(QStringLiteral("时间")));

    QVERIFY(store.saveCritical(state, auditEvent(1)).ok);
    stateObject = snapshotRoot(productionMain(dir)).value(QStringLiteral("state")).toObject();
    stations = stateObject.value(QStringLiteral("stations")).toArray();
    QJsonObject firstStation = stations.at(0).toObject();
    firstStation[QStringLiteral("firstLowAtUtc")] = QStringLiteral("not-a-date");
    stations[0] = firstStation;
    stateObject[QStringLiteral("stations")] = stations;
    overwriteProductionSnapshotOnly(dir, stateObject);
    ShortageStateLoadResult badOptionalTimestamp = store.load();
    QVERIFY(!badOptionalTimestamp.ok);
    QVERIFY(badOptionalTimestamp.requiresMaintenance);
    QVERIFY(badOptionalTimestamp.messageZh.contains(QStringLiteral("时间")));

    ShortageRuntimeState replayState = sampleState(2);
    QVERIFY(store.saveCritical(replayState, auditEvent(1)).ok);
    QJsonObject replaySnapshot = snapshotRoot(productionMain(dir)).value(QStringLiteral("state")).toObject();
    replaySnapshot[QStringLiteral("nextAuditSequence")] = QStringLiteral("1");
    overwriteSnapshotState(productionMain(dir), replaySnapshot);
    const QJsonObject journalEvent =
        QJsonDocument::fromJson(readAll(productionJournal(dir)).trimmed()).object();
    QJsonObject badJournalEvent = journalEvent;
    badJournalEvent[QStringLiteral("occurredAtUtc")] = QStringLiteral("not-a-date");
    overwrite(productionJournal(dir),
              QJsonDocument(badJournalEvent).toJson(QJsonDocument::Compact) + '\n');
    overwrite(productionBackup(dir), QByteArrayLiteral("{broken-backup"));
    ShortageStateLoadResult badJournalTimestamp = store.load();
    QVERIFY(!badJournalTimestamp.ok);
    QVERIFY(badJournalTimestamp.requiresMaintenance);
    QVERIFY(badJournalTimestamp.messageZh.contains(QStringLiteral("时间")));
}

void ShortageStateStoreTest::allCorruptSourcesLockAutomaticMode()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ShortageStateStore store(dir.path(), ShortageStateNamespace::Production);
    QVERIFY(store.saveCritical(sampleState(2), auditEvent(1)).ok);
    overwrite(productionMain(dir), QByteArrayLiteral("{broken-main"));
    overwrite(productionBackup(dir), QByteArrayLiteral("{broken-backup"));
    overwrite(productionJournal(dir), QByteArrayLiteral("{broken-journal"));

    const ShortageStateLoadResult loaded = store.load();
    QVERIFY(!loaded.ok);
    QVERIFY(loaded.requiresMaintenance);
    QVERIFY(loaded.state.criticalLock);
    QVERIFY(loaded.messageZh.contains(QStringLiteral("维护")));
}

void ShortageStateStoreTest::testNamespaceNeverTouchesProductionFiles()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ShortageStateStore store(dir.path(), ShortageStateNamespace::StandaloneTest);
    QVERIFY(store.saveCritical(sampleState(2), auditEvent(1)).ok);

    QVERIFY(QFile::exists(testMain(dir)));
    QVERIFY(!QFile::exists(productionMain(dir)));
    QVERIFY(!QFile::exists(productionBackup(dir)));
    QVERIFY(!QFile::exists(productionJournal(dir)));
}

void ShortageStateStoreTest::persistenceFailureAfterUnloadKeepsMemoryFact()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString fileAsBase = dir.filePath(QStringLiteral("not-a-directory"));
    overwrite(fileAsBase, QByteArrayLiteral("blocks mkdir"));
    ShortageStateStore store(fileAsBase, ShortageStateNamespace::Production);

    ShortageRuntimeState state = sampleState(2);
    state.stations[0].stock = 2000;
    const ShortageOperationResult result =
        store.saveCritical(state, auditEvent(1, QStringLiteral("box_unloaded")));

    QVERIFY(!result.ok);
    QCOMPARE(state.stations[0].stock, qint64{2000});
}

void ShortageStateStoreTest::safeRestoreRequiresOperatorConfirmation()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ShortageStateStore store(dir.path(), ShortageStateNamespace::Production);
    ShortageRuntimeState state = sampleState(2);
    state.operatorConfirmedRestore = false;
    QVERIFY(store.saveCritical(state, auditEvent(1)).ok);

    const ShortageStateLoadResult loaded = store.load();
    QVERIFY2(loaded.ok, qPrintable(loaded.messageZh));
    QVERIFY(loaded.state.criticalLock);
    QVERIFY(loaded.messageZh.contains(QStringLiteral("人工确认")));
}

void ShortageStateStoreTest::zeroLedgerRequiresSiteClearConfirmation()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ShortageStateStore store(dir.path(), ShortageStateNamespace::Production);

    const ShortageStateLoadResult loaded = store.load();
    QVERIFY(!loaded.ok);
    QVERIFY(!loaded.stateFound);
    QVERIFY(loaded.requiresMaintenance);
    QVERIFY(loaded.messageZh.contains(QStringLiteral("现场清空确认")));
}

QTEST_MAIN(ShortageStateStoreTest)
#include "test_shortage_state_store.moc"
