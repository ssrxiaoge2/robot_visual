#include "shortageconfigstore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <limits>

namespace {

struct ExpectedSheet3Row {
    int stationId;           ///< 代码工位。
    QString temporaryNo;     ///< Sheet3 NO。
    QString sitePosition;    ///< Sheet3 现场位置。
    QString partNumber;      ///< 完整显示品号。
    qint64 boxQuantity;      ///< 每箱数量。
    qint64 minimumStock;     ///< 最低安全位。
    qint64 maximumStock;     ///< 最高安全位。
    qint64 usageLeftRight;   ///< L/R 用量。
    qint64 usageLeftOnly;    ///< L/L 用量。
    qint64 usageRightOnly;   ///< R/H 用量。
};

const QList<ExpectedSheet3Row> expectedRows = {
    {11, "1",  "2",          "18112-RM8S0WS",       114,  600,  800,  1, 1, 1},
    {10, "2",  "3",          "18115-RM8S0DE（左）", 2000, 2000, 4000, 1, 1, 0},
    {9,  "3",  "4",          "18125-RM8S0BR（左）", 2000, 2000, 4000, 1, 1, 0},
    {8,  "4",  "5",          "18215-RM8S0DE（右）", 2000, 2000, 4000, 1, 0, 1},
    {7,  "5",  "6",          "18225-RM8S0BR（右）", 2000, 2000, 4000, 1, 0, 1},
    {6,  "6",  "7",          "18114-RM8S0BR（左）", 2000, 2000, 4000, 1, 1, 0},
    {5,  "7",  "8",          "18214-RM8S0BR（右）", 2000, 2000, 4000, 1, 0, 1},
    {4,  "8",  "9",          "18116-RM8S0（左）",    836,  800, 1600, 1, 1, 0},
    {3,  "T",  "暂未确定",    "18116-RM8S0（右）",    836,  800, 1600, 1, 0, 1},
    {2,  "9",  "10",         "18167-RM700HT（右）",  250,  600, 1200, 1, 0, 1},
    {1,  "10", "11",         "18117-RM8S0HT（左）",  250,  600, 1200, 1, 1, 0},
    {12, "11", "13",         "18118-RM8S0",          300,  600, 1200, 1, 1, 1},
};

const QList<ProductModel> expectedProducts = {
    ProductModel::Model88,
    ProductModel::Model88R,
    ProductModel::Model92,
};

QString productName(ProductModel product)
{
    switch (product) {
    case ProductModel::Model88:
        return QStringLiteral("88");
    case ProductModel::Model88R:
        return QStringLiteral("88R");
    case ProductModel::Model92:
        return QStringLiteral("92");
    }
    return QStringLiteral("未知");
}

qint64 usageForMode(const ShortageStationConfig &station, ProductionMode mode)
{
    switch (mode) {
    case ProductionMode::LeftRight:
        return station.usageLeftRight;
    case ProductionMode::LeftOnly:
        return station.usageLeftOnly;
    case ProductionMode::RightOnly:
        return station.usageRightOnly;
    }
    return -1;
}

ShortageStationConfig findStation(const ShortageConfiguration &configuration,
                                  ProductModel product,
                                  int stationId)
{
    for (const ShortageStationConfig &station : configuration.stations) {
        if (station.product == product && station.stationId == stationId)
            return station;
    }
    return {};
}

bool containsAll(const QString &text, const QStringList &needles)
{
    for (const QString &needle : needles) {
        if (!text.contains(needle))
            return false;
    }
    return true;
}

} // namespace

class ShortageConfigTest final : public QObject
{
    Q_OBJECT
private slots:
    void sheet3DefaultsContainExactlyThirtySixRows(); // CF-01：全表逐字段核对。
    void plcBitsMapToConfirmedModesOnly();             // CF-02：L68/L69/L1998 映射。
    void parameterBoundariesAreValidated();            // CF-03：所有边界和越界。
    void stationRowsRejectInvalidFields();             // CF-04：逐字段错误原因。
    void saveLoadAndBackupAreAtomic();                 // CF-05：保存、损坏、备份。
    void saveLoadAndExportPreserveLargeQuantitiesExactly(); // CF-05：64 位数量精确持久化。
    void savePersistsNextRevisionWithoutMutatingInput();     // CF-05：正式保存修订号单调递增。
    void repeatedSaveOfSameObjectAdvancesRevision();         // CF-05：重复保存同一内存对象仍单调递增。
    void saveRejectsRevisionOverflowFromCurrentMain();        // CF-05：主文件 revision 已满时拒绝覆盖。
    void loadRejectsUnknownProductText();                    // CF-05：未知产品文本不得默认为 88。
    void loadRejectsMalformedNumericStrings();               // CF-05：畸形数字字符串不得退回 double。
    void loadRejectsMalformedRevisionString();               // CF-05：畸形 revision 字符串不得退回 double。
    void loadRejectsOverflowRevisionString();                 // CF-05：溢出 revision 字符串不得退回 double。
    void busyGuardListsEveryBlockingCondition();       // CF-06：五类忙碌门禁。
    void allNineProductModeCombinationsResolveUsage(); // CH-05：3×3 用量选择。
};

void ShortageConfigTest::sheet3DefaultsContainExactlyThirtySixRows()
{
    const ShortageConfiguration configuration = ShortageConfigStore::sheet3Defaults();

    QCOMPARE(configuration.stations.size(), 36);
    QCOMPARE(configuration.revision, quint64{1});
    QCOMPARE(configuration.parameters.liveMesDayEndpoint,
             QStringLiteral("http://192.168.115.228:5084/api/MesData/day"));

    int assertionRows = 0;
    for (ProductModel product : expectedProducts) {
        for (const ExpectedSheet3Row &expected : expectedRows) {
            const ShortageStationConfig station = findStation(configuration, product, expected.stationId);
            QVERIFY2(station.stationId != 0,
                     qPrintable(QStringLiteral("%1 工位 %2 缺失")
                                    .arg(productName(product))
                                    .arg(expected.stationId)));
            QCOMPARE(station.product, product);
            QCOMPARE(station.temporaryNo, expected.temporaryNo);
            QCOMPARE(station.sitePosition, expected.sitePosition);
            QCOMPARE(station.partNumber, expected.partNumber);
            QVERIFY(station.enabled);
            QCOMPARE(station.boxQuantity, expected.boxQuantity);
            QCOMPARE(station.minimumStock, expected.minimumStock);
            QCOMPARE(station.maximumStock, expected.maximumStock);
            QCOMPARE(station.usageLeftRight, expected.usageLeftRight);
            QCOMPARE(station.usageLeftOnly, expected.usageLeftOnly);
            QCOMPARE(station.usageRightOnly, expected.usageRightOnly);
            ++assertionRows;
        }
    }
    QCOMPARE(assertionRows, 36);
}

void ShortageConfigTest::plcBitsMapToConfirmedModesOnly()
{
    QCOMPARE(static_cast<int>(ProductionMode::LeftRight), 0);
    QCOMPARE(static_cast<int>(ProductionMode::LeftOnly), 1);
    QCOMPARE(static_cast<int>(ProductionMode::RightOnly), 2);

    const ShortageConfiguration configuration = ShortageConfigStore::sheet3Defaults();
    const ShortageStationConfig left = findStation(configuration, ProductModel::Model88, 10);
    const ShortageStationConfig right = findStation(configuration, ProductModel::Model88, 8);
    const ShortageStationConfig shared = findStation(configuration, ProductModel::Model88, 11);

    QCOMPARE(usageForMode(left, ProductionMode::LeftRight), qint64{1});  // L68：左右生产。
    QCOMPARE(usageForMode(left, ProductionMode::LeftOnly), qint64{1});   // L69：只有左。
    QCOMPARE(usageForMode(left, ProductionMode::RightOnly), qint64{0});  // L1998：只有右。
    QCOMPARE(usageForMode(right, ProductionMode::LeftOnly), qint64{0});
    QCOMPARE(usageForMode(right, ProductionMode::RightOnly), qint64{1});
    QCOMPARE(usageForMode(shared, ProductionMode::LeftOnly), qint64{1});
    QCOMPARE(usageForMode(shared, ProductionMode::RightOnly), qint64{1});
}

void ShortageConfigTest::parameterBoundariesAreValidated()
{
    ShortageConfiguration configuration = ShortageConfigStore::sheet3Defaults();
    QVERIFY2(ShortageConfigStore::validate(configuration).ok,
             qPrintable(ShortageConfigStore::validate(configuration).messageZh));

    const QString defaultEndpoint = configuration.parameters.liveMesDayEndpoint;
    QVERIFY(defaultEndpoint.contains(QStringLiteral(".228")));
    QVERIFY(!defaultEndpoint.contains(QStringLiteral(".229")));

    auto expectRejected = [](ShortageConfiguration invalid, const QStringList &needles) {
        const ShortageOperationResult result = ShortageConfigStore::validate(invalid);
        QVERIFY(!result.ok);
        QVERIFY2(containsAll(result.messageZh, needles), qPrintable(result.messageZh));
    };

    configuration.parameters.liveMesDayEndpoint.clear();
    expectRejected(configuration, {QStringLiteral("MES"), QStringLiteral("空")});

    configuration = ShortageConfigStore::sheet3Defaults();
    configuration.parameters.liveMesDayEndpoint = QStringLiteral("192.168.115.228:5084/api/MesData/day");
    expectRejected(configuration, {QStringLiteral("MES"), QStringLiteral("scheme")});

    configuration = ShortageConfigStore::sheet3Defaults();
    configuration.parameters.liveMesDayEndpoint = QStringLiteral("http:///api/MesData/day");
    expectRejected(configuration, {QStringLiteral("MES"), QStringLiteral("host")});

    configuration = ShortageConfigStore::sheet3Defaults();
    configuration.parameters.liveMesDayEndpoint = QStringLiteral("ftp://192.168.115.228/api/MesData/day");
    expectRejected(configuration, {QStringLiteral("MES"), QStringLiteral("HTTP")});

    configuration = ShortageConfigStore::sheet3Defaults();
    configuration.parameters.sampleIntervalSeconds = 5;
    configuration.parameters.roundTimeoutSeconds = 1;
    configuration.parameters.communicationAlarmMinutes = 1;
    configuration.parameters.preUnloadFailureLimit = 1;
    QVERIFY2(ShortageConfigStore::validate(configuration).ok,
             qPrintable(ShortageConfigStore::validate(configuration).messageZh));

    configuration.parameters.sampleIntervalSeconds = 300;
    configuration.parameters.roundTimeoutSeconds = 30;
    configuration.parameters.communicationAlarmMinutes = 60;
    configuration.parameters.preUnloadFailureLimit = 20;
    QVERIFY2(ShortageConfigStore::validate(configuration).ok,
             qPrintable(ShortageConfigStore::validate(configuration).messageZh));

    configuration.parameters.sampleIntervalSeconds = 4;
    configuration.parameters.roundTimeoutSeconds = 31;
    configuration.parameters.communicationAlarmMinutes = 0;
    configuration.parameters.preUnloadFailureLimit = 0;
    expectRejected(configuration, {
        QStringLiteral("sampleIntervalSeconds"),
        QStringLiteral("roundTimeoutSeconds"),
        QStringLiteral("communicationAlarmMinutes"),
        QStringLiteral("preUnloadFailureLimit"),
    });

    configuration = ShortageConfigStore::sheet3Defaults();
    configuration.parameters.sampleIntervalSeconds = 10;
    configuration.parameters.roundTimeoutSeconds = 10;
    expectRejected(configuration, {QStringLiteral("timeout < interval")});
}

void ShortageConfigTest::stationRowsRejectInvalidFields()
{
    ShortageConfiguration invalid = ShortageConfigStore::sheet3Defaults();
    invalid.stations.removeLast();
    invalid.stations[0].stationId = 13;
    invalid.stations[1].stationId = invalid.stations[2].stationId;
    invalid.stations[3].temporaryNo.clear();
    invalid.stations[4].sitePosition.clear();
    invalid.stations[5].partNumber.clear();
    invalid.stations[6].boxQuantity = 0;
    invalid.stations[7].minimumStock = -1;
    invalid.stations[8].maximumStock = invalid.stations[8].minimumStock;
    invalid.stations[9].usageLeftRight = -1;
    invalid.stations[10].usageLeftOnly = -1;
    invalid.stations[11].usageRightOnly = -1;

    const ShortageOperationResult result = ShortageConfigStore::validate(invalid);
    QVERIFY(!result.ok);
    QVERIFY2(containsAll(result.messageZh, {
                 QStringLiteral("36"),
                 QStringLiteral("行1"),
                 QStringLiteral("产品"),
                 QStringLiteral("工位"),
                 QStringLiteral("13"),
                 QStringLiteral("重复"),
                 QStringLiteral("temporaryNo"),
                 QStringLiteral("sitePosition"),
                 QStringLiteral("partNumber"),
                 QStringLiteral("boxQuantity"),
                 QStringLiteral("minimumStock"),
                 QStringLiteral("maximumStock"),
                 QStringLiteral("usageLeftRight"),
                 QStringLiteral("usageLeftOnly"),
                 QStringLiteral("usageRightOnly"),
             }),
             qPrintable(result.messageZh));
}

void ShortageConfigTest::saveLoadAndBackupAreAtomic()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString configPath = dir.filePath(QStringLiteral("test-configuration.json"));
    const QString backupPath = dir.filePath(QStringLiteral("test-configuration.backup.json"));

    ShortageConfiguration first = ShortageConfigStore::sheet3Defaults();
    first.revision = 7;
    first.stations[0].minimumStock = 701;
    const ShortageOperationResult saveFirst = ShortageConfigStore::save(configPath, first);
    QVERIFY2(saveFirst.ok, qPrintable(saveFirst.messageZh));
    QVERIFY(QFile::exists(configPath));
    QVERIFY(!QFile::exists(backupPath));

    ShortageConfiguration second = first;
    second.revision = 8;
    second.stations[0].minimumStock = 702;
    const ShortageOperationResult saveSecond = ShortageConfigStore::save(configPath, second);
    QVERIFY2(saveSecond.ok, qPrintable(saveSecond.messageZh));
    QVERIFY(QFile::exists(backupPath));

    QFile broken(configPath);
    QVERIFY(broken.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(broken.write("{broken json") > 0);
    broken.close();

    ShortageConfiguration loaded;
    loaded.revision = 999;
    const ShortageOperationResult loadResult = ShortageConfigStore::load(configPath, &loaded);
    QVERIFY2(loadResult.ok, qPrintable(loadResult.messageZh));
    QVERIFY2(loadResult.messageZh.contains(QStringLiteral("来自备份")), qPrintable(loadResult.messageZh));
    QCOMPARE(loaded.revision, quint64{8});
    QCOMPARE(loaded.stations[0].minimumStock, qint64{701});

    const QString exportPath = dir.filePath(QStringLiteral("export.json"));
    const ShortageOperationResult exportResult = ShortageConfigStore::exportCopy(exportPath, second);
    QVERIFY2(exportResult.ok, qPrintable(exportResult.messageZh));
    QVERIFY(QFile::exists(exportPath));
    QCOMPARE(loaded.revision, quint64{8});
}

void ShortageConfigTest::saveLoadAndExportPreserveLargeQuantitiesExactly()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString configPath = dir.filePath(QStringLiteral("large-quantity.json"));
    const QString exportPath = dir.filePath(QStringLiteral("large-quantity-export.json"));

    const qint64 aboveDoubleExactRange = (qint64{1} << 53) + 123;
    ShortageConfiguration configuration = ShortageConfigStore::sheet3Defaults();
    configuration.stations[0].boxQuantity = aboveDoubleExactRange;
    configuration.stations[0].minimumStock = aboveDoubleExactRange - 2;
    configuration.stations[0].maximumStock = aboveDoubleExactRange + 2;
    configuration.stations[0].usageLeftRight = aboveDoubleExactRange - 1;
    configuration.stations[0].usageLeftOnly = aboveDoubleExactRange;
    configuration.stations[0].usageRightOnly = aboveDoubleExactRange + 1;

    const ShortageOperationResult saveResult = ShortageConfigStore::save(configPath, configuration);
    QVERIFY2(saveResult.ok, qPrintable(saveResult.messageZh));

    ShortageConfiguration loaded;
    const ShortageOperationResult loadResult = ShortageConfigStore::load(configPath, &loaded);
    QVERIFY2(loadResult.ok, qPrintable(loadResult.messageZh));
    QCOMPARE(loaded.stations[0].boxQuantity, aboveDoubleExactRange);
    QCOMPARE(loaded.stations[0].minimumStock, aboveDoubleExactRange - 2);
    QCOMPARE(loaded.stations[0].maximumStock, aboveDoubleExactRange + 2);
    QCOMPARE(loaded.stations[0].usageLeftRight, aboveDoubleExactRange - 1);
    QCOMPARE(loaded.stations[0].usageLeftOnly, aboveDoubleExactRange);
    QCOMPARE(loaded.stations[0].usageRightOnly, aboveDoubleExactRange + 1);

    const ShortageOperationResult exportResult =
        ShortageConfigStore::exportCopy(exportPath, configuration);
    QVERIFY2(exportResult.ok, qPrintable(exportResult.messageZh));

    ShortageConfiguration exported;
    const ShortageOperationResult exportLoadResult =
        ShortageConfigStore::load(exportPath, &exported);
    QVERIFY2(exportLoadResult.ok, qPrintable(exportLoadResult.messageZh));
    QCOMPARE(exported.stations[0].boxQuantity, aboveDoubleExactRange);
    QCOMPARE(exported.stations[0].minimumStock, aboveDoubleExactRange - 2);
    QCOMPARE(exported.stations[0].maximumStock, aboveDoubleExactRange + 2);
    QCOMPARE(exported.stations[0].usageLeftRight, aboveDoubleExactRange - 1);
    QCOMPARE(exported.stations[0].usageLeftOnly, aboveDoubleExactRange);
    QCOMPARE(exported.stations[0].usageRightOnly, aboveDoubleExactRange + 1);
}

void ShortageConfigTest::savePersistsNextRevisionWithoutMutatingInput()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString configPath = dir.filePath(QStringLiteral("revision.json"));

    ShortageConfiguration configuration = ShortageConfigStore::sheet3Defaults();
    configuration.revision = 41;
    const ShortageOperationResult saveResult = ShortageConfigStore::save(configPath, configuration);
    QVERIFY2(saveResult.ok, qPrintable(saveResult.messageZh));
    QCOMPARE(configuration.revision, quint64{41});

    ShortageConfiguration loaded;
    const ShortageOperationResult loadResult = ShortageConfigStore::load(configPath, &loaded);
    QVERIFY2(loadResult.ok, qPrintable(loadResult.messageZh));
    QCOMPARE(loaded.revision, quint64{42});
}

void ShortageConfigTest::repeatedSaveOfSameObjectAdvancesRevision()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString configPath = dir.filePath(QStringLiteral("repeated-revision.json"));

    ShortageConfiguration configuration = ShortageConfigStore::sheet3Defaults();
    configuration.revision = 100;
    const ShortageOperationResult firstSave = ShortageConfigStore::save(configPath, configuration);
    QVERIFY2(firstSave.ok, qPrintable(firstSave.messageZh));

    ShortageConfiguration firstLoaded;
    const ShortageOperationResult firstLoad = ShortageConfigStore::load(configPath, &firstLoaded);
    QVERIFY2(firstLoad.ok, qPrintable(firstLoad.messageZh));

    const ShortageOperationResult secondSave = ShortageConfigStore::save(configPath, configuration);
    QVERIFY2(secondSave.ok, qPrintable(secondSave.messageZh));

    ShortageConfiguration secondLoaded;
    const ShortageOperationResult secondLoad = ShortageConfigStore::load(configPath, &secondLoaded);
    QVERIFY2(secondLoad.ok, qPrintable(secondLoad.messageZh));
    QVERIFY2(secondLoaded.revision > firstLoaded.revision,
             qPrintable(QStringLiteral("first=%1 second=%2")
                            .arg(firstLoaded.revision)
                            .arg(secondLoaded.revision)));
    QCOMPARE(firstLoaded.revision, quint64{101});
    QCOMPARE(secondLoaded.revision, quint64{102});
    QCOMPARE(configuration.revision, quint64{100});
}

void ShortageConfigTest::saveRejectsRevisionOverflowFromCurrentMain()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString configPath = dir.filePath(QStringLiteral("revision-overflow.json"));

    ShortageConfiguration maximumRevision = ShortageConfigStore::sheet3Defaults();
    maximumRevision.revision = std::numeric_limits<quint64>::max();
    const ShortageOperationResult exportResult =
        ShortageConfigStore::exportCopy(configPath, maximumRevision);
    QVERIFY2(exportResult.ok, qPrintable(exportResult.messageZh));

    ShortageConfiguration lowerRevision = ShortageConfigStore::sheet3Defaults();
    lowerRevision.revision = 1;
    const ShortageOperationResult saveResult =
        ShortageConfigStore::save(configPath, lowerRevision);
    QVERIFY(!saveResult.ok);
    QVERIFY2(saveResult.messageZh.contains(QStringLiteral("revision")),
             qPrintable(saveResult.messageZh));

    ShortageConfiguration loaded;
    const ShortageOperationResult loadResult = ShortageConfigStore::load(configPath, &loaded);
    QVERIFY2(loadResult.ok, qPrintable(loadResult.messageZh));
    QCOMPARE(loaded.revision, std::numeric_limits<quint64>::max());
}

void ShortageConfigTest::loadRejectsUnknownProductText()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString configPath = dir.filePath(QStringLiteral("unknown-product.json"));

    ShortageConfiguration configuration = ShortageConfigStore::sheet3Defaults();
    const ShortageOperationResult saveResult = ShortageConfigStore::exportCopy(configPath, configuration);
    QVERIFY2(saveResult.ok, qPrintable(saveResult.messageZh));

    QFile file(configPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    file.close();
    QJsonObject root = document.object();
    QJsonArray stations = root.value(QStringLiteral("stations")).toArray();
    QJsonObject firstStation = stations.at(0).toObject();
    firstStation[QStringLiteral("product")] = QStringLiteral("bad-product");
    stations[0] = firstStation;
    root[QStringLiteral("stations")] = stations;
    document.setObject(root);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.write(document.toJson(QJsonDocument::Indented)) > 0);
    file.close();

    ShortageConfiguration loaded = ShortageConfigStore::sheet3Defaults();
    const ShortageOperationResult loadResult = ShortageConfigStore::load(configPath, &loaded);
    QVERIFY(!loadResult.ok);
    QVERIFY2(containsAll(loadResult.messageZh, {
                 QStringLiteral("bad-product"),
                 QStringLiteral("产品"),
             }),
             qPrintable(loadResult.messageZh));
}

void ShortageConfigTest::loadRejectsMalformedNumericStrings()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString configPath = dir.filePath(QStringLiteral("bad-number.json"));

    ShortageConfiguration configuration = ShortageConfigStore::sheet3Defaults();
    const ShortageOperationResult saveResult =
        ShortageConfigStore::exportCopy(configPath, configuration);
    QVERIFY2(saveResult.ok, qPrintable(saveResult.messageZh));

    QFile file(configPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    file.close();
    QJsonObject root = document.object();
    QJsonArray stations = root.value(QStringLiteral("stations")).toArray();
    QJsonObject firstStation = stations.at(0).toObject();
    firstStation[QStringLiteral("boxQuantity")] = QStringLiteral("123abc");
    stations[0] = firstStation;
    root[QStringLiteral("stations")] = stations;
    document.setObject(root);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.write(document.toJson(QJsonDocument::Indented)) > 0);
    file.close();

    ShortageConfiguration loaded = ShortageConfigStore::sheet3Defaults();
    const ShortageOperationResult loadResult = ShortageConfigStore::load(configPath, &loaded);
    QVERIFY(!loadResult.ok);
    QVERIFY2(containsAll(loadResult.messageZh, {
                 QStringLiteral("boxQuantity"),
                 QStringLiteral("123abc"),
             }),
             qPrintable(loadResult.messageZh));
}

void ShortageConfigTest::loadRejectsMalformedRevisionString()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString configPath = dir.filePath(QStringLiteral("bad-revision.json"));

    ShortageConfiguration configuration = ShortageConfigStore::sheet3Defaults();
    const ShortageOperationResult saveResult =
        ShortageConfigStore::exportCopy(configPath, configuration);
    QVERIFY2(saveResult.ok, qPrintable(saveResult.messageZh));

    QFile file(configPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    file.close();
    QJsonObject root = document.object();
    root[QStringLiteral("revision")] = QStringLiteral("123abc");
    document.setObject(root);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.write(document.toJson(QJsonDocument::Indented)) > 0);
    file.close();

    ShortageConfiguration loaded = ShortageConfigStore::sheet3Defaults();
    const ShortageOperationResult loadResult = ShortageConfigStore::load(configPath, &loaded);
    QVERIFY(!loadResult.ok);
    QVERIFY2(containsAll(loadResult.messageZh, {
                 QStringLiteral("revision"),
                 QStringLiteral("123abc"),
             }),
             qPrintable(loadResult.messageZh));
}

void ShortageConfigTest::loadRejectsOverflowRevisionString()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString configPath = dir.filePath(QStringLiteral("overflow-revision.json"));

    ShortageConfiguration configuration = ShortageConfigStore::sheet3Defaults();
    const ShortageOperationResult saveResult =
        ShortageConfigStore::exportCopy(configPath, configuration);
    QVERIFY2(saveResult.ok, qPrintable(saveResult.messageZh));

    QFile file(configPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    file.close();
    QJsonObject root = document.object();
    root[QStringLiteral("revision")] = QStringLiteral("18446744073709551616");
    document.setObject(root);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.write(document.toJson(QJsonDocument::Indented)) > 0);
    file.close();

    ShortageConfiguration loaded = ShortageConfigStore::sheet3Defaults();
    const ShortageOperationResult loadResult = ShortageConfigStore::load(configPath, &loaded);
    QVERIFY(!loadResult.ok);
    QVERIFY2(containsAll(loadResult.messageZh, {
                 QStringLiteral("revision"),
                 QStringLiteral("18446744073709551616"),
             }),
             qPrintable(loadResult.messageZh));
}

void ShortageConfigTest::busyGuardListsEveryBlockingCondition()
{
    ShortageEditConditions allClear;
    QVERIFY2(ShortageConfigStore::canEditConfiguration(allClear).ok,
             qPrintable(ShortageConfigStore::canEditConfiguration(allClear).messageZh));

    ShortageEditConditions blocked;
    blocked.standaloneTestStopped = false;
    blocked.liveSamplingStopped = false;
    blocked.lineStopped = false;
    blocked.currentTaskEmpty = false;
    blocked.fifoEmpty = false;

    const ShortageOperationResult result = ShortageConfigStore::canEditConfiguration(blocked);
    QVERIFY(!result.ok);
    QVERIFY2(containsAll(result.messageZh, {
                 QStringLiteral("独立测试"),
                 QStringLiteral("正式采样"),
                 QStringLiteral("LineManager"),
                 QStringLiteral("当前执行任务"),
                 QStringLiteral("Pending FIFO"),
             }),
             qPrintable(result.messageZh));
}

void ShortageConfigTest::allNineProductModeCombinationsResolveUsage()
{
    const ShortageConfiguration configuration = ShortageConfigStore::sheet3Defaults();
    for (ProductModel product : expectedProducts) {
        const ShortageStationConfig left = findStation(configuration, product, 10);
        const ShortageStationConfig right = findStation(configuration, product, 8);
        const ShortageStationConfig shared = findStation(configuration, product, 11);

        QCOMPARE(usageForMode(left, ProductionMode::LeftRight), qint64{1});
        QCOMPARE(usageForMode(left, ProductionMode::LeftOnly), qint64{1});
        QCOMPARE(usageForMode(left, ProductionMode::RightOnly), qint64{0});

        QCOMPARE(usageForMode(right, ProductionMode::LeftRight), qint64{1});
        QCOMPARE(usageForMode(right, ProductionMode::LeftOnly), qint64{0});
        QCOMPARE(usageForMode(right, ProductionMode::RightOnly), qint64{1});

        QCOMPARE(usageForMode(shared, ProductionMode::LeftRight), qint64{1});
        QCOMPARE(usageForMode(shared, ProductionMode::LeftOnly), qint64{1});
        QCOMPARE(usageForMode(shared, ProductionMode::RightOnly), qint64{1});
    }
}

QTEST_MAIN(ShortageConfigTest)
#include "test_shortage_config.moc"
