#ifndef SHORTAGECONFIG_H
#define SHORTAGECONFIG_H

#include <QList>
#include <QString>

/**
 * @brief 客户现场 PLC 产型位映射的三种产品型号。
 *
 * 该枚举只表达 L71/L72/L73 三选一后的业务结果，不承载 HTTP、PLC 或 UI 逻辑。
 */
enum class ProductModel { Model88, Model88R, Model92 };

/**
 * @brief 客户现场 PLC 生产方式位映射。
 *
 * L68/L69/L1998 对应不同生产方式；开发版默认用 PLC 方式直接决定单件用量。
 */
enum class ProductionMode { L68, L69, L1998 };

/**
 * @brief 缺料累计时的单件用量策略。
 *
 * Spreadsheet 代表沿用表格“用量”列；PlcMode 代表按生产方式统一取值。
 * 该策略是编译期宏，修改后必须重新编译，运行期 UI 不得切换。
 */
enum class UsageStrategy { Spreadsheet, PlcMode };

#ifndef SHORTAGE_USAGE_STRATEGY
#define SHORTAGE_USAGE_STRATEGY UsageStrategy::PlcMode
#endif

/**
 * @brief 单工位、单产品的固定缺料配置。
 *
 * boxQuantity / spreadsheetUsage / safetyStock 来自
 * `xiancahngxitong/安全库存.xlsx`。当前仓库没有这 36 组确认数值，
 * 因此占位为 0；0 表示“尚未得到现场确认”，监控层不得据此自动派单。
 */
struct MaterialConfig {
    int stationId = 0;               ///< 代码工位号，合法范围 1-12。
    ProductModel product = ProductModel::Model88; ///< 当前配置所属产品型号。
    QString partNumber;              ///< 该工位用于回查 Excel 的主品号。
    int boxQuantity = 0;             ///< 每箱数量；0 表示仓库中缺少已确认值。
    int spreadsheetUsage = 0;        ///< 表格策略单件用量；0 表示仓库中缺少已确认值。
    int safetyStock = 0;             ///< 安全库存阈值；0 表示仓库中缺少已确认值。
};

namespace shortageconfig_detail {

// 当前仓库未包含 `安全库存.xlsx` 的 36 组最终数值；先用 0 明确表示“待补表”，
// 防止系统在未确认阈值时误发现场缺料任务。
inline constexpr int kPendingSpreadsheetValue = 0;

inline const MaterialConfig kMaterialConfigs[] = {
    {1, ProductModel::Model88, QStringLiteral("18117-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {1, ProductModel::Model88R, QStringLiteral("18117-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {1, ProductModel::Model92, QStringLiteral("18117-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {2, ProductModel::Model88, QStringLiteral("18167-RM700"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {2, ProductModel::Model88R, QStringLiteral("18167-RM700"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {2, ProductModel::Model92, QStringLiteral("18167-RM700"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    // 工位 3 当前按设计文档临时复制工位 4 的 Excel 参数，但累计和事件保持独立。
    {3, ProductModel::Model88, QStringLiteral("18116-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {3, ProductModel::Model88R, QStringLiteral("18116-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {3, ProductModel::Model92, QStringLiteral("18116-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {4, ProductModel::Model88, QStringLiteral("18116-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {4, ProductModel::Model88R, QStringLiteral("18116-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {4, ProductModel::Model92, QStringLiteral("18116-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {5, ProductModel::Model88, QStringLiteral("18214-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {5, ProductModel::Model88R, QStringLiteral("18214-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {5, ProductModel::Model92, QStringLiteral("18214-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {6, ProductModel::Model88, QStringLiteral("18114-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {6, ProductModel::Model88R, QStringLiteral("18114-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {6, ProductModel::Model92, QStringLiteral("18114-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {7, ProductModel::Model88, QStringLiteral("18225-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {7, ProductModel::Model88R, QStringLiteral("18225-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {7, ProductModel::Model92, QStringLiteral("18225-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {8, ProductModel::Model88, QStringLiteral("18215-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {8, ProductModel::Model88R, QStringLiteral("18215-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {8, ProductModel::Model92, QStringLiteral("18215-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {9, ProductModel::Model88, QStringLiteral("18125-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {9, ProductModel::Model88R, QStringLiteral("18125-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {9, ProductModel::Model92, QStringLiteral("18125-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {10, ProductModel::Model88, QStringLiteral("18115-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {10, ProductModel::Model88R, QStringLiteral("18115-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {10, ProductModel::Model92, QStringLiteral("18115-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {11, ProductModel::Model88, QStringLiteral("18112-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {11, ProductModel::Model88R, QStringLiteral("18112-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {11, ProductModel::Model92, QStringLiteral("18112-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {12, ProductModel::Model88, QStringLiteral("18118-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {12, ProductModel::Model88R, QStringLiteral("18118-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
    {12, ProductModel::Model92, QStringLiteral("18118-RM8S0"), kPendingSpreadsheetValue, kPendingSpreadsheetValue, kPendingSpreadsheetValue},
};

} // namespace shortageconfig_detail

inline const MaterialConfig *materialConfig(int stationId, ProductModel product)
{
    for (const MaterialConfig &config : shortageconfig_detail::kMaterialConfigs) {
        if (config.stationId == stationId && config.product == product) {
            return &config;
        }
    }
    return nullptr;
}

inline QList<MaterialConfig> defaultMaterialConfigs()
{
    QList<MaterialConfig> configs;
    configs.reserve(static_cast<int>(sizeof(shortageconfig_detail::kMaterialConfigs)
                                     / sizeof(MaterialConfig)));
    for (const MaterialConfig &config : shortageconfig_detail::kMaterialConfigs) {
        configs.append(config);
    }
    return configs;
}

inline bool materialConfigHasConfirmedThresholds(const MaterialConfig &config)
{
    return config.boxQuantity > 0
        && config.spreadsheetUsage > 0
        && config.safetyStock > 0;
}

inline int usagePerProduct(const MaterialConfig &config, ProductionMode mode)
{
    const UsageStrategy strategy = SHORTAGE_USAGE_STRATEGY;
    if (strategy == UsageStrategy::Spreadsheet) {
        return config.spreadsheetUsage;
    }

    switch (mode) {
    case ProductionMode::L68:
    case ProductionMode::L69:
        return 2;
    case ProductionMode::L1998:
        return 1;
    }
    return 0;
}

inline QString productModelText(ProductModel product)
{
    switch (product) {
    case ProductModel::Model88:
        return QStringLiteral("88");
    case ProductModel::Model88R:
        return QStringLiteral("88R");
    case ProductModel::Model92:
        return QStringLiteral("92");
    }
    return QStringLiteral("-");
}

inline QString productionModeText(ProductionMode mode)
{
    switch (mode) {
    case ProductionMode::L68:
        return QStringLiteral("L68");
    case ProductionMode::L69:
        return QStringLiteral("L69");
    case ProductionMode::L1998:
        return QStringLiteral("L1998");
    }
    return QStringLiteral("-");
}

#endif // SHORTAGECONFIG_H
