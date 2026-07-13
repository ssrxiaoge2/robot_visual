#include "shortageconfigstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QUrl>

namespace {

/// Sheet3 的 12 行基础数据，三个产品复用同一现场表并保存为独立 36 行。
struct Sheet3Row {
    int stationId;
    const char *temporaryNo;
    const char *sitePosition;
    const char *partNumber;
    qint64 boxQuantity;
    qint64 minimumStock;
    qint64 maximumStock;
    qint64 usageLeftRight;
    qint64 usageLeftOnly;
    qint64 usageRightOnly;
};

/// 固化客户最新 Sheet3 数值，禁止运行期通过旧 .229 配置回退。
constexpr Sheet3Row kSheet3Rows[] = {
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

/// 产品枚举的稳定保存文本，便于人工检查 JSON。
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

/// 从保存文本恢复产品枚举，未知值交给校验层形成中文错误。
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

/// 备份文件名固定在主文件同目录下派生，避免 UI 传入不一致路径。
QString backupPathFor(const QString &filePath)
{
    const QFileInfo info(filePath);
    const QString suffix = info.completeSuffix();
    QString baseName = info.fileName();
    if (!suffix.isEmpty())
        baseName.chop(suffix.size() + 1);
    const QString backupName = suffix.isEmpty()
        ? baseName + QStringLiteral(".backup")
        : baseName + QStringLiteral(".backup.") + suffix;
    return info.dir().filePath(backupName);
}

/// 单行配置转 JSON；字段名使用业务名，避免和 UI 表格列序强绑定。
QJsonObject stationToJson(const ShortageStationConfig &station)
{
    return {
        {QStringLiteral("product"), productToString(station.product)},
        {QStringLiteral("stationId"), station.stationId},
        {QStringLiteral("temporaryNo"), station.temporaryNo},
        {QStringLiteral("sitePosition"), station.sitePosition},
        {QStringLiteral("partNumber"), station.partNumber},
        {QStringLiteral("enabled"), station.enabled},
        {QStringLiteral("boxQuantity"), double(station.boxQuantity)},
        {QStringLiteral("minimumStock"), double(station.minimumStock)},
        {QStringLiteral("maximumStock"), double(station.maximumStock)},
        {QStringLiteral("usageLeftRight"), double(station.usageLeftRight)},
        {QStringLiteral("usageLeftOnly"), double(station.usageLeftOnly)},
        {QStringLiteral("usageRightOnly"), double(station.usageRightOnly)},
    };
}

/// JSON 单行恢复为领域对象；类型错误保留默认值，由 validate 一次汇总。
ShortageStationConfig stationFromJson(const QJsonObject &object)
{
    ShortageStationConfig station;
    ProductModel product = ProductModel::Model88;
    if (productFromString(object.value(QStringLiteral("product")).toString(), &product))
        station.product = product;
    station.stationId = object.value(QStringLiteral("stationId")).toInt();
    station.temporaryNo = object.value(QStringLiteral("temporaryNo")).toString();
    station.sitePosition = object.value(QStringLiteral("sitePosition")).toString();
    station.partNumber = object.value(QStringLiteral("partNumber")).toString();
    station.enabled = object.value(QStringLiteral("enabled")).toBool(true);
    station.boxQuantity = qint64(object.value(QStringLiteral("boxQuantity")).toDouble());
    station.minimumStock = qint64(object.value(QStringLiteral("minimumStock")).toDouble());
    station.maximumStock = qint64(object.value(QStringLiteral("maximumStock")).toDouble());
    station.usageLeftRight = qint64(object.value(QStringLiteral("usageLeftRight")).toDouble());
    station.usageLeftOnly = qint64(object.value(QStringLiteral("usageLeftOnly")).toDouble());
    station.usageRightOnly = qint64(object.value(QStringLiteral("usageRightOnly")).toDouble());
    return station;
}

/// 完整配置转 JSON 文档；revision 作为字符串保存，避免 64 位精度丢失。
QJsonDocument configurationToJson(const ShortageConfiguration &configuration)
{
    QJsonArray stations;
    for (const ShortageStationConfig &station : configuration.stations)
        stations.append(stationToJson(station));

    QJsonObject parameters {
        {QStringLiteral("liveMesDayEndpoint"), configuration.parameters.liveMesDayEndpoint},
        {QStringLiteral("sampleIntervalSeconds"), configuration.parameters.sampleIntervalSeconds},
        {QStringLiteral("roundTimeoutSeconds"), configuration.parameters.roundTimeoutSeconds},
        {QStringLiteral("communicationAlarmMinutes"), configuration.parameters.communicationAlarmMinutes},
        {QStringLiteral("preUnloadFailureLimit"), configuration.parameters.preUnloadFailureLimit},
    };

    QJsonObject root {
        {QStringLiteral("revision"), QString::number(configuration.revision)},
        {QStringLiteral("parameters"), parameters},
        {QStringLiteral("stations"), stations},
    };
    return QJsonDocument(root);
}

/// 从 JSON 文档恢复完整配置；结构错误会返回 false 且不触碰调用方内存。
bool configurationFromJson(const QByteArray &bytes,
                           ShortageConfiguration *configuration,
                           QString *errorZh)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *errorZh = QStringLiteral("配置 JSON 损坏：%1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = document.object();
    ShortageConfiguration parsed;
    bool revisionOk = false;
    parsed.revision = root.value(QStringLiteral("revision")).toString().toULongLong(&revisionOk);
    if (!revisionOk)
        parsed.revision = quint64(root.value(QStringLiteral("revision")).toDouble(1));

    const QJsonObject parameters = root.value(QStringLiteral("parameters")).toObject();
    parsed.parameters.liveMesDayEndpoint =
        parameters.value(QStringLiteral("liveMesDayEndpoint")).toString();
    parsed.parameters.sampleIntervalSeconds =
        parameters.value(QStringLiteral("sampleIntervalSeconds")).toInt();
    parsed.parameters.roundTimeoutSeconds =
        parameters.value(QStringLiteral("roundTimeoutSeconds")).toInt();
    parsed.parameters.communicationAlarmMinutes =
        parameters.value(QStringLiteral("communicationAlarmMinutes")).toInt();
    parsed.parameters.preUnloadFailureLimit =
        parameters.value(QStringLiteral("preUnloadFailureLimit")).toInt();

    const QJsonArray stations = root.value(QStringLiteral("stations")).toArray();
    for (const QJsonValue &value : stations)
        parsed.stations.append(stationFromJson(value.toObject()));

    const ShortageOperationResult validation = ShortageConfigStore::validate(parsed);
    if (!validation.ok) {
        *errorZh = QStringLiteral("配置内容非法：%1").arg(validation.messageZh);
        return false;
    }

    *configuration = parsed;
    return true;
}

/// 使用 QSaveFile 写入目标路径；失败时目标文件保持旧内容。
ShortageOperationResult writeJsonAtomically(const QString &filePath,
                                            const ShortageConfiguration &configuration,
                                            const QString &successMessage)
{
    QDir().mkpath(QFileInfo(filePath).absolutePath());
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return {false, QStringLiteral("无法打开配置文件 %1：%2")
                           .arg(filePath, file.errorString())};
    }

    const QByteArray bytes = configurationToJson(configuration).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size()) {
        return {false, QStringLiteral("写入配置文件 %1 失败：%2")
                           .arg(filePath, file.errorString())};
    }
    if (!file.commit()) {
        return {false, QStringLiteral("原子替换配置文件 %1 失败：%2")
                           .arg(filePath, file.errorString())};
    }
    return {true, successMessage};
}

/// 读取并校验单个文件，调用方据此决定是否回退备份。
bool readConfigurationFile(const QString &filePath,
                           ShortageConfiguration *configuration,
                           QString *errorZh)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        *errorZh = QStringLiteral("无法读取 %1：%2").arg(filePath, file.errorString());
        return false;
    }
    return configurationFromJson(file.readAll(), configuration, errorZh);
}

/// 添加一条带行号、产品、工位、字段和值的中文校验错误。
void appendStationError(QStringList *errors,
                        int row,
                        const ShortageStationConfig &station,
                        const QString &field,
                        const QString &value,
                        const QString &reason)
{
    errors->append(QStringLiteral("行%1 产品%2 工位%3 字段%4 值%5：%6")
                       .arg(row)
                       .arg(productToString(station.product))
                       .arg(station.stationId)
                       .arg(field, value, reason));
}

} // namespace

ShortageConfiguration ShortageConfigStore::sheet3Defaults()
{
    ShortageConfiguration configuration;
    configuration.stations.reserve(36);

    const QList<ProductModel> products {
        ProductModel::Model88,
        ProductModel::Model88R,
        ProductModel::Model92,
    };

    for (ProductModel product : products) {
        for (const Sheet3Row &row : kSheet3Rows) {
            ShortageStationConfig station;
            station.product = product;
            station.stationId = row.stationId;
            station.temporaryNo = QString::fromUtf8(row.temporaryNo);
            station.sitePosition = QString::fromUtf8(row.sitePosition);
            station.partNumber = QString::fromUtf8(row.partNumber);
            station.enabled = true;
            station.boxQuantity = row.boxQuantity;
            station.minimumStock = row.minimumStock;
            station.maximumStock = row.maximumStock;
            station.usageLeftRight = row.usageLeftRight;
            station.usageLeftOnly = row.usageLeftOnly;
            station.usageRightOnly = row.usageRightOnly;
            configuration.stations.append(station);
        }
    }

    return configuration;
}

ShortageOperationResult ShortageConfigStore::validate(const ShortageConfiguration &configuration)
{
    QStringList errors;

    if (configuration.stations.size() != 36) {
        errors.append(QStringLiteral("stations 数量必须为 36，当前为 %1")
                          .arg(configuration.stations.size()));
    }

    QHash<ProductModel, QSet<int>> stationIdsByProduct;
    for (int i = 0; i < configuration.stations.size(); ++i) {
        const ShortageStationConfig &station = configuration.stations.at(i);
        const int row = i + 1;

        if (station.stationId < 1 || station.stationId > 12) {
            appendStationError(&errors, row, station, QStringLiteral("stationId"),
                               QString::number(station.stationId),
                               QStringLiteral("工位必须在 1～12"));
        }
        if (stationIdsByProduct[station.product].contains(station.stationId)) {
            appendStationError(&errors, row, station, QStringLiteral("stationId"),
                               QString::number(station.stationId),
                               QStringLiteral("同一产品内工位重复"));
        }
        stationIdsByProduct[station.product].insert(station.stationId);

        if (station.temporaryNo.trimmed().isEmpty()) {
            appendStationError(&errors, row, station, QStringLiteral("temporaryNo"),
                               station.temporaryNo, QStringLiteral("不能为空"));
        }
        if (station.sitePosition.trimmed().isEmpty()) {
            appendStationError(&errors, row, station, QStringLiteral("sitePosition"),
                               station.sitePosition, QStringLiteral("不能为空"));
        }
        if (station.enabled && station.partNumber.trimmed().isEmpty()) {
            appendStationError(&errors, row, station, QStringLiteral("partNumber"),
                               station.partNumber, QStringLiteral("启用时不能为空"));
        }
        if (station.boxQuantity <= 0) {
            appendStationError(&errors, row, station, QStringLiteral("boxQuantity"),
                               QString::number(station.boxQuantity),
                               QStringLiteral("每箱数量必须大于 0"));
        }
        if (station.minimumStock < 0) {
            appendStationError(&errors, row, station, QStringLiteral("minimumStock"),
                               QString::number(station.minimumStock),
                               QStringLiteral("最低安全位不能为负"));
        }
        if (station.maximumStock <= station.minimumStock) {
            appendStationError(&errors, row, station, QStringLiteral("maximumStock"),
                               QString::number(station.maximumStock),
                               QStringLiteral("最高安全位必须大于最低安全位"));
        }
        if (station.usageLeftRight < 0) {
            appendStationError(&errors, row, station, QStringLiteral("usageLeftRight"),
                               QString::number(station.usageLeftRight),
                               QStringLiteral("用量不能为负"));
        }
        if (station.usageLeftOnly < 0) {
            appendStationError(&errors, row, station, QStringLiteral("usageLeftOnly"),
                               QString::number(station.usageLeftOnly),
                               QStringLiteral("用量不能为负"));
        }
        if (station.usageRightOnly < 0) {
            appendStationError(&errors, row, station, QStringLiteral("usageRightOnly"),
                               QString::number(station.usageRightOnly),
                               QStringLiteral("用量不能为负"));
        }
    }

    const QList<ProductModel> products {
        ProductModel::Model88,
        ProductModel::Model88R,
        ProductModel::Model92,
    };
    for (ProductModel product : products) {
        if (stationIdsByProduct.value(product).size() != 12) {
            errors.append(QStringLiteral("产品%1 必须包含 12 个唯一工位，当前为 %2")
                              .arg(productToString(product))
                              .arg(stationIdsByProduct.value(product).size()));
        }
    }

    const QString endpoint = configuration.parameters.liveMesDayEndpoint.trimmed();
    if (endpoint.isEmpty()) {
        errors.append(QStringLiteral("MES liveMesDayEndpoint 不能为空"));
    } else {
        const QUrl url(endpoint);
        if (url.scheme().isEmpty())
            errors.append(QStringLiteral("MES liveMesDayEndpoint 缺少 scheme"));
        if (url.host().isEmpty())
            errors.append(QStringLiteral("MES liveMesDayEndpoint 缺少 host"));
        if (!url.scheme().isEmpty()
            && url.scheme() != QStringLiteral("http")
            && url.scheme() != QStringLiteral("https")) {
            errors.append(QStringLiteral("MES liveMesDayEndpoint 只允许 HTTP/HTTPS"));
        }
        if (endpoint.contains(QStringLiteral(".229"))) {
            errors.append(QStringLiteral("MES liveMesDayEndpoint 禁止使用已删除的 .229 旧地址"));
        }
    }

    if (configuration.parameters.sampleIntervalSeconds < 5
        || configuration.parameters.sampleIntervalSeconds > 300) {
        errors.append(QStringLiteral("sampleIntervalSeconds 必须在 5～300 秒，当前为 %1")
                          .arg(configuration.parameters.sampleIntervalSeconds));
    }
    if (configuration.parameters.roundTimeoutSeconds < 1
        || configuration.parameters.roundTimeoutSeconds > 30) {
        errors.append(QStringLiteral("roundTimeoutSeconds 必须在 1～30 秒，当前为 %1")
                          .arg(configuration.parameters.roundTimeoutSeconds));
    }
    if (configuration.parameters.roundTimeoutSeconds
        >= configuration.parameters.sampleIntervalSeconds) {
        errors.append(QStringLiteral("roundTimeoutSeconds 必须满足 timeout < interval"));
    }
    if (configuration.parameters.communicationAlarmMinutes < 1
        || configuration.parameters.communicationAlarmMinutes > 60) {
        errors.append(QStringLiteral("communicationAlarmMinutes 必须在 1～60 分钟，当前为 %1")
                          .arg(configuration.parameters.communicationAlarmMinutes));
    }
    if (configuration.parameters.preUnloadFailureLimit < 1
        || configuration.parameters.preUnloadFailureLimit > 20) {
        errors.append(QStringLiteral("preUnloadFailureLimit 必须在 1～20 次，当前为 %1")
                          .arg(configuration.parameters.preUnloadFailureLimit));
    }

    if (!errors.isEmpty())
        return {false, errors.join(QStringLiteral("；"))};
    return {true, QStringLiteral("缺料配置校验通过")};
}

ShortageOperationResult ShortageConfigStore::canEditConfiguration(
    const ShortageEditConditions &conditions)
{
    QStringList blockers;
    if (!conditions.standaloneTestStopped)
        blockers.append(QStringLiteral("独立测试未停止"));
    if (!conditions.liveSamplingStopped)
        blockers.append(QStringLiteral("正式采样未停止"));
    if (!conditions.lineStopped)
        blockers.append(QStringLiteral("LineManager 正在 Running/ReturningHome"));
    if (!conditions.currentTaskEmpty)
        blockers.append(QStringLiteral("当前执行任务不为空"));
    if (!conditions.fifoEmpty)
        blockers.append(QStringLiteral("Pending FIFO 不为空"));

    if (!blockers.isEmpty())
        return {false, QStringLiteral("禁止保存缺料配置：%1").arg(blockers.join(QStringLiteral("；")))};
    return {true, QStringLiteral("允许保存缺料配置")};
}

ShortageOperationResult ShortageConfigStore::load(const QString &filePath,
                                                  ShortageConfiguration *configuration)
{
    if (configuration == nullptr)
        return {false, QStringLiteral("加载失败：configuration 指针为空")};

    ShortageConfiguration loaded;
    QString mainError;
    if (readConfigurationFile(filePath, &loaded, &mainError)) {
        *configuration = loaded;
        return {true, QStringLiteral("已从主配置加载")};
    }

    QString backupError;
    if (readConfigurationFile(backupPathFor(filePath), &loaded, &backupError)) {
        *configuration = loaded;
        return {true, QStringLiteral("主配置不可用，已来自备份加载：%1").arg(mainError)};
    }

    return {false, QStringLiteral("加载失败且未覆盖内存配置；主配置：%1；备份：%2")
                       .arg(mainError, backupError)};
}

ShortageOperationResult ShortageConfigStore::save(const QString &filePath,
                                                  const ShortageConfiguration &configuration)
{
    const ShortageOperationResult validation = validate(configuration);
    if (!validation.ok)
        return validation;

    const QString backupPath = backupPathFor(filePath);
    if (QFile::exists(filePath)) {
        ShortageConfiguration currentMain;
        QString readError;
        if (!readConfigurationFile(filePath, &currentMain, &readError)) {
            return {false, QStringLiteral("保存前主配置无效，拒绝覆盖；%1").arg(readError)};
        }
        const ShortageOperationResult backupResult =
            writeJsonAtomically(backupPath, currentMain, QStringLiteral("已原子写入备份配置"));
        if (!backupResult.ok)
            return backupResult;
    }

    return writeJsonAtomically(filePath, configuration, QStringLiteral("已原子保存缺料配置"));
}

ShortageOperationResult ShortageConfigStore::exportCopy(
    const QString &targetPath,
    const ShortageConfiguration &configuration)
{
    const ShortageOperationResult validation = validate(configuration);
    if (!validation.ok)
        return validation;
    return writeJsonAtomically(targetPath, configuration, QStringLiteral("已导出缺料配置副本"));
}
