#include <QtTest>
#include <limits>

#include "shortagecalculator.h"

namespace {

struct ExpectedMaterialRow {
    int stationId;
    ProductModel product;
    const char *partNumber;
    int boxQuantity;
    int spreadsheetUsage;
    int safetyStock;
};

QList<ExpectedMaterialRow> expectedWorkbookRows()
{
    return {
        {1, ProductModel::Model88, "18117-RM8S0", 250, 1, 1000},
        {1, ProductModel::Model88R, "18117-RM8S0", 250, 1, 600},
        {1, ProductModel::Model92, "18117-RM8S0", 250, 1, 600},
        {2, ProductModel::Model88, "18167-RM700", 250, 1, 600},
        {2, ProductModel::Model88R, "18167-RM700", 250, 1, 1000},
        {2, ProductModel::Model92, "18167-RM700", 250, 1, 1000},
        {3, ProductModel::Model88, "18116-RM8S0", 836, 1, 800},
        {3, ProductModel::Model88R, "18116-RM8S0", 836, 1, 800},
        {3, ProductModel::Model92, "18116-RM8S0", 836, 1, 800},
        {4, ProductModel::Model88, "18116-RM8S0", 836, 1, 800},
        {4, ProductModel::Model88R, "18116-RM8S0", 836, 1, 800},
        {4, ProductModel::Model92, "18116-RM8S0", 836, 1, 800},
        {5, ProductModel::Model88, "18214-RM8S0", 2000, 1, 2000},
        {5, ProductModel::Model88R, "18214-RM8S0", 2000, 1, 2000},
        {5, ProductModel::Model92, "18214-RM8S0", 2000, 1, 2000},
        {6, ProductModel::Model88, "18114-RM8S0", 2000, 1, 2000},
        {6, ProductModel::Model88R, "18114-RM8S0", 2000, 1, 2000},
        {6, ProductModel::Model92, "18114-RM8S0", 2000, 1, 2000},
        {7, ProductModel::Model88, "18225-RM8S0", 2000, 1, 2000},
        {7, ProductModel::Model88R, "18225-RM8S0", 2000, 1, 2000},
        {7, ProductModel::Model92, "18225-RM8S0", 2000, 1, 2000},
        {8, ProductModel::Model88, "18215-RM8S0", 2000, 1, 2000},
        {8, ProductModel::Model88R, "18215-RM8S0", 2000, 1, 2000},
        {8, ProductModel::Model92, "18215-RM8S0", 2000, 1, 2000},
        {9, ProductModel::Model88, "18125-RM8S0", 2000, 1, 2000},
        {9, ProductModel::Model88R, "18125-RM8S0", 2000, 1, 2000},
        {9, ProductModel::Model92, "18125-RM8S0", 2000, 1, 2000},
        {10, ProductModel::Model88, "18115-RM8S0", 2000, 1, 2000},
        {10, ProductModel::Model88R, "18115-RM8S0", 2000, 1, 2000},
        {10, ProductModel::Model92, "18115-RM8S0", 2000, 1, 2000},
        {11, ProductModel::Model88, "18112-RM8S0", 114, 1, 600},
        {11, ProductModel::Model88R, "18112-RM8S0", 114, 1, 600},
        {11, ProductModel::Model92, "18112-RM8S0", 114, 1, 600},
        {12, ProductModel::Model88, "18118-RM8S0", 300, 1, 600},
        {12, ProductModel::Model88R, "18118-RM8S0", 300, 1, 600},
        {12, ProductModel::Model92, "18118-RM8S0", 300, 1, 600},
    };
}

QList<MaterialConfig> buildSyntheticConfigs()
{
    QList<MaterialConfig> configs;
    for (int stationId = 1; stationId <= 12; ++stationId) {
        configs.append(MaterialConfig{
            stationId,
            ProductModel::Model88,
            QStringLiteral("P%1").arg(stationId),
            100 + stationId,
            1,
            40 + stationId
        });
    }

    configs[0] = MaterialConfig{1, ProductModel::Model88, QStringLiteral("P1"), 100, 1, 40};
    configs[1] = MaterialConfig{2, ProductModel::Model88, QStringLiteral("P2"), 80, 3, 20};
    configs[2] = MaterialConfig{3, ProductModel::Model88, QStringLiteral("P34"), 60, 1, 10};
    configs[3] = MaterialConfig{4, ProductModel::Model88, QStringLiteral("P34"), 60, 1, 10};
    configs.append(MaterialConfig{1, ProductModel::Model92, QStringLiteral("P1-92"), 50, 1, 15});
    return configs;
}

} // namespace

class ShortageCalculatorTest : public QObject
{
    Q_OBJECT

private slots:
    void material_config_matches_workbook_examples();
    void material_config_exists_for_all_stations_and_products_data();
    void material_config_exists_for_all_stations_and_products();
    void station3_and_station4_share_values_but_keep_station_id();
    void default_usage_strategy_prefers_spreadsheet_usage();
    void plc_mode_usage_interface_remains_available();
    void initialize_for_product_sets_safety_plus_box_baseline();
    void ingest_deducts_each_station_independently();
    void exact_safety_line_is_shortage();
    void negative_inventory_is_preserved();
    void missing_configuration_is_not_shortage();
    void multiplication_overflow_is_rejected();
};

void ShortageCalculatorTest::material_config_matches_workbook_examples()
{
    const MaterialConfig *station11Model88 = materialConfig(11, ProductModel::Model88);
    QVERIFY(station11Model88);
    QCOMPARE(station11Model88->boxQuantity, 114);
    QCOMPARE(station11Model88->safetyStock, 600);

    const MaterialConfig *station1Model88 = materialConfig(1, ProductModel::Model88);
    QVERIFY(station1Model88);
    QCOMPARE(station1Model88->boxQuantity, 250);
    QCOMPARE(station1Model88->spreadsheetUsage, 1);
    QCOMPARE(station1Model88->safetyStock, 1000);
}

void ShortageCalculatorTest::material_config_exists_for_all_stations_and_products_data()
{
    QTest::addColumn<int>("stationId");
    QTest::addColumn<int>("productValue");
    QTest::addColumn<QString>("partNumber");
    QTest::addColumn<int>("boxQuantity");
    QTest::addColumn<int>("spreadsheetUsage");
    QTest::addColumn<int>("safetyStock");

    for (const ExpectedMaterialRow &row : expectedWorkbookRows()) {
        QTest::addRow("station_%d_%s",
                      row.stationId,
                      qPrintable(productModelText(row.product)))
            << row.stationId
            << static_cast<int>(row.product)
            << QString::fromLatin1(row.partNumber)
            << row.boxQuantity
            << row.spreadsheetUsage
            << row.safetyStock;
    }
}

void ShortageCalculatorTest::material_config_exists_for_all_stations_and_products()
{
    QFETCH(int, stationId);
    QFETCH(int, productValue);
    QFETCH(QString, partNumber);
    QFETCH(int, boxQuantity);
    QFETCH(int, spreadsheetUsage);
    QFETCH(int, safetyStock);

    const ProductModel product = static_cast<ProductModel>(productValue);
    const MaterialConfig *config = materialConfig(stationId, product);
    QVERIFY2(config != nullptr,
             qPrintable(QStringLiteral("missing config for station=%1 product=%2")
                            .arg(stationId)
                            .arg(productModelText(product))));
    QCOMPARE(config->partNumber, partNumber);
    QVERIFY(config->boxQuantity > 0);
    QVERIFY(config->spreadsheetUsage > 0);
    QVERIFY(config->safetyStock > 0);
    QCOMPARE(config->boxQuantity, boxQuantity);
    QCOMPARE(config->spreadsheetUsage, spreadsheetUsage);
    QCOMPARE(config->safetyStock, safetyStock);
}

void ShortageCalculatorTest::station3_and_station4_share_values_but_keep_station_id()
{
    const MaterialConfig *station3 = materialConfig(3, ProductModel::Model88);
    const MaterialConfig *station4 = materialConfig(4, ProductModel::Model88);
    QVERIFY(station3 != nullptr);
    QVERIFY(station4 != nullptr);
    QCOMPARE(station3->stationId, 3);
    QCOMPARE(station4->stationId, 4);
    QCOMPARE(station3->boxQuantity, station4->boxQuantity);
    QCOMPARE(station3->spreadsheetUsage, station4->spreadsheetUsage);
    QCOMPARE(station3->safetyStock, station4->safetyStock);
    QCOMPARE(station3->partNumber, station4->partNumber);
}

void ShortageCalculatorTest::default_usage_strategy_prefers_spreadsheet_usage()
{
    MaterialConfig config;
    config.spreadsheetUsage = 7;
    QCOMPARE(usagePerProduct(config, ProductionMode::L68), 7);
    QCOMPARE(usagePerProduct(config, ProductionMode::L1998), 7);
}

void ShortageCalculatorTest::plc_mode_usage_interface_remains_available()
{
    QCOMPARE(usagePerMode(ProductionMode::L68), 2);
    QCOMPARE(usagePerMode(ProductionMode::L69), 2);
    QCOMPARE(usagePerMode(ProductionMode::L1998), 1);
}

void ShortageCalculatorTest::initialize_for_product_sets_safety_plus_box_baseline()
{
    ShortageCalculator calculator(buildSyntheticConfigs());
    calculator.initializeForProduct(ProductModel::Model88);
    const StationConsumption station = calculator.snapshot().first();
    QCOMPARE(station.estimatedAvailable, 140);
    QCOMPARE(station.safetyStock, 40);
    QCOMPARE(station.boxQuantity, 100);
    QVERIFY(station.configured);
    QVERIFY(!station.shortage);
}

void ShortageCalculatorTest::ingest_deducts_each_station_independently()
{
    ShortageCalculator calculator(buildSyntheticConfigs());
    calculator.initializeForProduct(ProductModel::Model88);
    QVERIFY(calculator.ingest({1000, ProductModel::Model88, ProductionMode::L68}).ok);
    QVERIFY(calculator.ingest({1020, ProductModel::Model88, ProductionMode::L68}).ok);

    const QList<StationConsumption> stations = calculator.snapshot();
    QCOMPARE(stations.size(), 12);
    QCOMPARE(stations.at(0).estimatedAvailable, 120);
    QCOMPARE(stations.at(1).estimatedAvailable, 40);
    QCOMPARE(stations.at(2).estimatedAvailable, 50);
    QCOMPARE(stations.at(3).estimatedAvailable, 50);
    for (int index = 4; index < stations.size(); ++index) {
        const int stationId = index + 1;
        const qint64 expectedAvailable = qint64(40 + stationId) + (100 + stationId) - 20;
        QCOMPARE(stations.at(index).estimatedAvailable, expectedAvailable);
    }
    QVERIFY(!stations.at(0).shortage);
    QVERIFY(!stations.at(1).shortage);
}

void ShortageCalculatorTest::exact_safety_line_is_shortage()
{
    ShortageCalculator calculator(buildSyntheticConfigs());
    calculator.initializeForProduct(ProductModel::Model88);
    QVERIFY(calculator.ingest({1000, ProductModel::Model88, ProductionMode::L1998}).ok);
    QVERIFY(calculator.ingest({1100, ProductModel::Model88, ProductionMode::L1998}).ok);
    const StationConsumption station = calculator.snapshot().first();
    QCOMPARE(station.estimatedAvailable, 40);
    QVERIFY(station.shortage);
}

void ShortageCalculatorTest::negative_inventory_is_preserved()
{
    ShortageCalculator calculator(buildSyntheticConfigs());
    calculator.initializeForProduct(ProductModel::Model88);
    QVERIFY(calculator.ingest({1000, ProductModel::Model88, ProductionMode::L68}).ok);
    QVERIFY(calculator.ingest({1200, ProductModel::Model88, ProductionMode::L68}).ok);
    const StationConsumption station = calculator.snapshot().first();
    QVERIFY(station.estimatedAvailable < 0);
    QVERIFY(station.shortage);
}

void ShortageCalculatorTest::missing_configuration_is_not_shortage()
{
    MaterialConfig config{1, ProductModel::Model88, QStringLiteral("P1"), 0, 0, 0};
    ShortageCalculator calculator({config});
    calculator.initializeForProduct(ProductModel::Model88);
    const StationConsumption station = calculator.snapshot().first();
    QVERIFY(!station.configured);
    QVERIFY(!station.shortage);
    QCOMPARE(station.reason, QStringLiteral("配置缺失"));
}

void ShortageCalculatorTest::multiplication_overflow_is_rejected()
{
    MaterialConfig config{1, ProductModel::Model88, QStringLiteral("P1"),
                          100, std::numeric_limits<int>::max(), 40};
    ShortageCalculator calculator({config});
    calculator.initializeForProduct(ProductModel::Model88);
    QVERIFY(calculator.ingest({0, ProductModel::Model88, ProductionMode::L68}).ok);
    const IngestResult result = calculator.ingest({std::numeric_limits<qint64>::max(), ProductModel::Model88, ProductionMode::L68});
    QVERIFY(!result.ok);
    QVERIFY(result.errorMessage.contains(QStringLiteral("溢出")));
}

QTEST_MAIN(ShortageCalculatorTest)
#include "test_shortagecalculator.moc"
