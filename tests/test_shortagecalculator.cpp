#include <QtTest>

#include "shortagecalculator.h"

namespace {

QList<MaterialConfig> buildSyntheticConfigs()
{
    QList<MaterialConfig> configs;
    for (int stationId = 1; stationId <= 12; ++stationId) {
        for (ProductModel product : {ProductModel::Model88, ProductModel::Model88R, ProductModel::Model92}) {
            MaterialConfig config;
            config.stationId = stationId;
            config.product = product;
            config.partNumber = QStringLiteral("PART-%1").arg(stationId);
            config.boxQuantity = 10 + stationId;
            config.spreadsheetUsage = 1 + (stationId % 3);
            config.safetyStock = 6;
            configs.append(config);
        }
    }

    for (MaterialConfig &config : configs) {
        if (config.stationId == 3 || config.stationId == 4) {
            config.boxQuantity = 24;
            config.spreadsheetUsage = 3;
            config.safetyStock = 9;
            config.partNumber = QStringLiteral("PART-34");
        }
    }
    return configs;
}

} // namespace

class ShortageCalculatorTest : public QObject
{
    Q_OBJECT

private slots:
    void material_config_exists_for_all_stations_and_products();
    void station3_and_station4_share_values_but_keep_station_id();
    void default_plc_usage_strategy();
    void first_sample_builds_baseline_only();
    void threshold_and_acceptance_flow();
    void rejection_keeps_pending_state();
    void same_delta_is_not_double_counted();
    void multi_threshold_requires_multiple_accepts();
    void product_mode_or_counter_change_rebuilds_baseline();
    void communication_recovery_rebuilds_baseline_without_clearing_accumulated();
};

void ShortageCalculatorTest::material_config_exists_for_all_stations_and_products()
{
    for (int stationId = 1; stationId <= 12; ++stationId) {
        for (ProductModel product : {ProductModel::Model88, ProductModel::Model88R, ProductModel::Model92}) {
            QVERIFY2(materialConfig(stationId, product) != nullptr,
                     qPrintable(QStringLiteral("missing config for station=%1 product=%2")
                                    .arg(stationId)
                                    .arg(productModelText(product))));
        }
    }
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

void ShortageCalculatorTest::default_plc_usage_strategy()
{
    MaterialConfig config;
    config.spreadsheetUsage = 9;
    QCOMPARE(usagePerProduct(config, ProductionMode::L68), 2);
    QCOMPARE(usagePerProduct(config, ProductionMode::L69), 2);
    QCOMPARE(usagePerProduct(config, ProductionMode::L1998), 1);
}

void ShortageCalculatorTest::first_sample_builds_baseline_only()
{
    ShortageCalculator calculator(buildSyntheticConfigs());
    const IngestResult result = calculator.ingest({100, ProductModel::Model88, ProductionMode::L68});
    QVERIFY(result.ok);
    QVERIFY(result.pendingStations.isEmpty());
    QCOMPARE(calculator.snapshot().at(0).accumulated, 0);
}

void ShortageCalculatorTest::threshold_and_acceptance_flow()
{
    ShortageCalculator calculator(buildSyntheticConfigs());
    QVERIFY(calculator.ingest({100, ProductModel::Model88, ProductionMode::L68}).ok);

    IngestResult result = calculator.ingest({103, ProductModel::Model88, ProductionMode::L68});
    QVERIFY(result.ok);
    QVERIFY(result.pendingStations.contains(1));
    QCOMPARE(calculator.snapshot().at(0).accumulated, 6);

    QVERIFY(calculator.confirmAccepted(1));
    QCOMPARE(calculator.snapshot().at(0).accumulated, 0);
}

void ShortageCalculatorTest::rejection_keeps_pending_state()
{
    ShortageCalculator calculator(buildSyntheticConfigs());
    QVERIFY(calculator.ingest({100, ProductModel::Model88, ProductionMode::L68}).ok);
    const IngestResult result = calculator.ingest({103, ProductModel::Model88, ProductionMode::L68});
    QVERIFY(result.ok);
    QVERIFY(result.pendingStations.contains(1));
    QVERIFY(!calculator.confirmAccepted(2));
    QCOMPARE(calculator.snapshot().at(0).accumulated, 6);
}

void ShortageCalculatorTest::same_delta_is_not_double_counted()
{
    ShortageCalculator calculator(buildSyntheticConfigs());
    QVERIFY(calculator.ingest({100, ProductModel::Model88, ProductionMode::L68}).ok);
    QVERIFY(calculator.ingest({103, ProductModel::Model88, ProductionMode::L68}).ok);
    const qint64 accumulated = calculator.snapshot().at(0).accumulated;
    QVERIFY(calculator.ingest({103, ProductModel::Model88, ProductionMode::L68}).ok);
    QCOMPARE(calculator.snapshot().at(0).accumulated, accumulated);
}

void ShortageCalculatorTest::multi_threshold_requires_multiple_accepts()
{
    ShortageCalculator calculator(buildSyntheticConfigs());
    QVERIFY(calculator.ingest({100, ProductModel::Model88, ProductionMode::L68}).ok);

    IngestResult result = calculator.ingest({109, ProductModel::Model88, ProductionMode::L68});
    QVERIFY(result.ok);
    QVERIFY(result.pendingStations.contains(1));
    QCOMPARE(calculator.snapshot().at(0).accumulated, 18);

    QVERIFY(calculator.confirmAccepted(1));
    QCOMPARE(calculator.snapshot().at(0).accumulated, 12);
    result = calculator.ingest({109, ProductModel::Model88, ProductionMode::L68});
    QVERIFY(result.ok);
    QVERIFY(result.pendingStations.contains(1));
    QVERIFY(calculator.confirmAccepted(1));
    QCOMPARE(calculator.snapshot().at(0).accumulated, 6);
}

void ShortageCalculatorTest::product_mode_or_counter_change_rebuilds_baseline()
{
    ShortageCalculator calculator(buildSyntheticConfigs());
    QVERIFY(calculator.ingest({100, ProductModel::Model88, ProductionMode::L68}).ok);
    QVERIFY(calculator.ingest({103, ProductModel::Model88, ProductionMode::L68}).ok);
    QVERIFY(calculator.snapshot().at(0).accumulated > 0);

    QVERIFY(calculator.ingest({104, ProductModel::Model92, ProductionMode::L68}).ok);
    QCOMPARE(calculator.snapshot().at(0).accumulated, 0);

    QVERIFY(calculator.ingest({90, ProductModel::Model92, ProductionMode::L68}).ok);
    QCOMPARE(calculator.snapshot().at(0).accumulated, 0);
}

void ShortageCalculatorTest::communication_recovery_rebuilds_baseline_without_clearing_accumulated()
{
    ShortageCalculator calculator(buildSyntheticConfigs());
    QVERIFY(calculator.ingest({100, ProductModel::Model88, ProductionMode::L68}).ok);
    QVERIFY(calculator.ingest({102, ProductModel::Model88, ProductionMode::L68}).ok);
    const qint64 accumulated = calculator.snapshot().at(0).accumulated;

    calculator.markCommunicationInterrupted();
    QVERIFY(calculator.ingest({120, ProductModel::Model88, ProductionMode::L68}).ok);
    QCOMPARE(calculator.snapshot().at(0).accumulated, accumulated);
}

QTEST_MAIN(ShortageCalculatorTest)
#include "test_shortagecalculator.moc"
