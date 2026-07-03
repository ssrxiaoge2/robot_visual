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
 * L68/L69/L1998 对应不同生产方式；当前开发默认仍以 Excel 表格用量为准。
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
#define SHORTAGE_USAGE_STRATEGY UsageStrategy::Spreadsheet
#endif

/**
 * @brief 单工位、单产品的固定缺料配置。
 *
 * boxQuantity / spreadsheetUsage / safetyStock 固化自
 * `xiancahngxitong/安全库存.xlsx`：
 * - sheet2：88=左块 A-F row3-14，92=右块 K-P row3-14，88R=左块 A-F row19-30。
 * - sheet1：共同主品号基表 B/C/D/F row22-44，空值按去后缀后的共同主品号回退。
 * - 单位：每箱数量 / 用量 / 安全库存均按表格 ea 语义保存。
 *
 * 工位映射沿用现场既有 common partNumber 映射；工位 3 与工位 4 共用同一组 Excel 参数，
 * 但运行期 stationId 必须继续独立计数。
 */
struct MaterialConfig {
    int stationId = 0;               ///< 代码工位号，合法范围 1-12。
    ProductModel product = ProductModel::Model88; ///< 当前配置所属产品型号。
    QString partNumber;              ///< 共同主品号；sheet2 后缀 WS/DE/BR/HT 回退时使用该键。
    int boxQuantity = 0;             ///< 每箱数量；单位=ea。
    int spreadsheetUsage = 0;        ///< 表格单件用量；单位=ea/件。
    int safetyStock = 0;             ///< 安全库存阈值；单位=ea。
};

namespace shortageconfig_detail {

inline const MaterialConfig kMaterialConfigs[] = {
    // 工位 1，18117-RM8S0：88 对应 sheet2 A13:F13 空用量/安全库存，按 sheet1 row42 回退；
    // 88R 对应 sheet2 A29:F29；92 对应 sheet2 K13:P13。
    {1, ProductModel::Model88, QStringLiteral("18117-RM8S0"), 250, 1, 1000},
    {1, ProductModel::Model88R, QStringLiteral("18117-RM8S0"), 250, 1, 600},
    {1, ProductModel::Model92, QStringLiteral("18117-RM8S0"), 250, 1, 600},
    // 工位 2，18167-RM700：88 对应 sheet2 A12:F12；88R 对应 sheet2 A28:F28 空用量/安全库存，按 sheet1 row40 回退；
    // 92 对应 sheet2 K12:P12 空用量/安全库存，按 sheet1 row40 回退。
    {2, ProductModel::Model88, QStringLiteral("18167-RM700"), 250, 1, 600},
    {2, ProductModel::Model88R, QStringLiteral("18167-RM700"), 250, 1, 1000},
    {2, ProductModel::Model92, QStringLiteral("18167-RM700"), 250, 1, 1000},
    // 工位 3 沿用工位 4 的共同主品号 18116-RM8S0；来源为 sheet2 A11:F11 / A27:F27 / K11:P11。
    {3, ProductModel::Model88, QStringLiteral("18116-RM8S0"), 836, 1, 800},
    {3, ProductModel::Model88R, QStringLiteral("18116-RM8S0"), 836, 1, 800},
    {3, ProductModel::Model92, QStringLiteral("18116-RM8S0"), 836, 1, 800},
    // 工位 4，18116-RM8S0：sheet2 明确值优先。
    {4, ProductModel::Model88, QStringLiteral("18116-RM8S0"), 836, 1, 800},
    {4, ProductModel::Model88R, QStringLiteral("18116-RM8S0"), 836, 1, 800},
    {4, ProductModel::Model92, QStringLiteral("18116-RM8S0"), 836, 1, 800},
    // 工位 5，18214-RM8S0：88=A10:F10，88R=A26:F26，92=K10:P10。
    {5, ProductModel::Model88, QStringLiteral("18214-RM8S0"), 2000, 1, 2000},
    {5, ProductModel::Model88R, QStringLiteral("18214-RM8S0"), 2000, 1, 2000},
    {5, ProductModel::Model92, QStringLiteral("18214-RM8S0"), 2000, 1, 2000},
    // 工位 6，18114-RM8S0：88=A9:F9，88R=A25:F25，92=K9:P9。
    {6, ProductModel::Model88, QStringLiteral("18114-RM8S0"), 2000, 1, 2000},
    {6, ProductModel::Model88R, QStringLiteral("18114-RM8S0"), 2000, 1, 2000},
    {6, ProductModel::Model92, QStringLiteral("18114-RM8S0"), 2000, 1, 2000},
    // 工位 7，18225-RM8S0：88=A8:F8，88R=A24:F24，92=K8:P8。
    {7, ProductModel::Model88, QStringLiteral("18225-RM8S0"), 2000, 1, 2000},
    {7, ProductModel::Model88R, QStringLiteral("18225-RM8S0"), 2000, 1, 2000},
    {7, ProductModel::Model92, QStringLiteral("18225-RM8S0"), 2000, 1, 2000},
    // 工位 8，18215-RM8S0：88=A7:F7，88R=A23:F23，92=K7:P7。
    {8, ProductModel::Model88, QStringLiteral("18215-RM8S0"), 2000, 1, 2000},
    {8, ProductModel::Model88R, QStringLiteral("18215-RM8S0"), 2000, 1, 2000},
    {8, ProductModel::Model92, QStringLiteral("18215-RM8S0"), 2000, 1, 2000},
    // 工位 9，18125-RM8S0：88=A6:F6，88R=A22:F22，92=K6:P6。
    {9, ProductModel::Model88, QStringLiteral("18125-RM8S0"), 2000, 1, 2000},
    {9, ProductModel::Model88R, QStringLiteral("18125-RM8S0"), 2000, 1, 2000},
    {9, ProductModel::Model92, QStringLiteral("18125-RM8S0"), 2000, 1, 2000},
    // 工位 10，18115-RM8S0：88=A5:F5，88R=A21:F21，92=K5:P5。
    {10, ProductModel::Model88, QStringLiteral("18115-RM8S0"), 2000, 1, 2000},
    {10, ProductModel::Model88R, QStringLiteral("18115-RM8S0"), 2000, 1, 2000},
    {10, ProductModel::Model92, QStringLiteral("18115-RM8S0"), 2000, 1, 2000},
    // 工位 11，18112-RM8S0：88=A4:F4，88R=A20:F20，92=K4:P4。
    {11, ProductModel::Model88, QStringLiteral("18112-RM8S0"), 114, 1, 600},
    {11, ProductModel::Model88R, QStringLiteral("18112-RM8S0"), 114, 1, 600},
    {11, ProductModel::Model92, QStringLiteral("18112-RM8S0"), 114, 1, 600},
    // 工位 12，18118-RM8S0：88=A14:F14，88R=A30:F30，92=K14:P14。
    {12, ProductModel::Model88, QStringLiteral("18118-RM8S0"), 300, 1, 600},
    {12, ProductModel::Model88R, QStringLiteral("18118-RM8S0"), 300, 1, 600},
    {12, ProductModel::Model92, QStringLiteral("18118-RM8S0"), 300, 1, 600},
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

inline int usagePerMode(ProductionMode mode)
{
    switch (mode) {
    case ProductionMode::L68:
    case ProductionMode::L69:
        return 2;
    case ProductionMode::L1998:
        return 1;
    }
    return 0;
}

inline int usagePerProduct(const MaterialConfig &config, ProductionMode mode)
{
    const UsageStrategy strategy = SHORTAGE_USAGE_STRATEGY;
    if (strategy == UsageStrategy::Spreadsheet) {
        return config.spreadsheetUsage;
    }
    return usagePerMode(mode);
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
