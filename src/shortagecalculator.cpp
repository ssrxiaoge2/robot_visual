#include "shortagecalculator.h"

#include <limits>

namespace {

constexpr int kStationCount = 12;

bool isMultiplicationOverflow(qint64 left, int right)
{
    if (left < 0 || right < 0) {
        return true;
    }
    if (left == 0 || right == 0) {
        return false;
    }
    return left > (std::numeric_limits<qint64>::max() / right);
}

bool isAdditionOverflow(qint64 left, qint64 right)
{
    return right > 0 && left > (std::numeric_limits<qint64>::max() - right);
}

bool isSubtractionOverflow(qint64 left, qint64 right)
{
    return right > 0 && left < (std::numeric_limits<qint64>::min() + right);
}

} // namespace

ShortageCalculator::ShortageCalculator()
    : ShortageCalculator(defaultMaterialConfigs())
{
}

ShortageCalculator::ShortageCalculator(const QList<MaterialConfig> &configs)
    : m_configs(configs)
{
}

void ShortageCalculator::initializeForProduct(ProductModel product)
{
    m_currentProduct = product;
    m_initialized = true;
    m_hasActualBaseline = false;
    m_rebaselineAfterCommunication = false;
    m_lastActualQty = 0;

    // 测试面板库存起点严格按“安全库存 + 每箱数量”计算；
    // 每个工位分别初始化，工位 3/4 即使共享配置值，也保留独立 runtime/state。
    for (int stationId = 1; stationId <= kStationCount; ++stationId) {
        initializeStationRuntime(stationId, product);
    }
}

IngestResult ShortageCalculator::ingest(const ShortageSample &sample)
{
    IngestResult result;
    if (sample.actualQty < 0) {
        result.errorMessage = QStringLiteral("actualQty 不能为负数");
        return result;
    }

    if (!m_initialized || sample.product != m_currentProduct) {
        initializeForProduct(sample.product);
    }

    if (!m_hasActualBaseline || m_rebaselineAfterCommunication || sample.actualQty < m_lastActualQty) {
        // 基线只用于计算 productionDelta；重建基线时不追补扣减，避免把断线期间产量重复算入。
        m_hasActualBaseline = true;
        m_rebaselineAfterCommunication = false;
        m_lastActualQty = sample.actualQty;
        result.ok = true;
        result.pendingStations = collectPendingStations(sample.product);
        return result;
    }

    const qint64 productionDelta = sample.actualQty - m_lastActualQty;
    for (int stationId = 1; stationId <= kStationCount; ++stationId) {
        StationRuntime &runtime = m_stationStates[stationId - 1];
        if (!runtime.configured) {
            runtime.awaitingAcceptance = false;
            continue;
        }

        const MaterialConfig *config = configFor(stationId, sample.product);
        if (!config) {
            runtime.configured = false;
            runtime.reason = QStringLiteral("配置缺失");
            runtime.awaitingAcceptance = false;
            continue;
        }

        const int usage = usagePerProduct(*config, sample.mode);
        if (usage <= 0) {
            runtime.configured = false;
            runtime.reason = QStringLiteral("配置缺失");
            runtime.awaitingAcceptance = false;
            continue;
        }
        if (isMultiplicationOverflow(productionDelta, usage)) {
            result.errorMessage = QStringLiteral("工位%1 的产量增量乘以用量后溢出").arg(stationId);
            return result;
        }

        const qint64 deltaConsumption = productionDelta * usage;
        if (isAdditionOverflow(runtime.consumed, deltaConsumption)) {
            result.errorMessage = QStringLiteral("工位%1 的累计消耗溢出").arg(stationId);
            return result;
        }
        if (isSubtractionOverflow(runtime.estimatedAvailable, deltaConsumption)) {
            result.errorMessage = QStringLiteral("工位%1 的预计可用物料扣减后溢出").arg(stationId);
            return result;
        }

        runtime.consumed += deltaConsumption;
        runtime.estimatedAvailable -= deltaConsumption;
        runtime.reason.clear();
        runtime.awaitingAcceptance = (runtime.estimatedAvailable <= config->safetyStock);
    }

    m_lastActualQty = sample.actualQty;
    result.ok = true;
    result.pendingStations = collectPendingStations(sample.product);
    return result;
}

bool ShortageCalculator::confirmAccepted(int stationId)
{
    if (stationId < 1 || stationId > kStationCount) {
        return false;
    }

    // Task 2 只实现测试面板的纯计算模型，不在这里做任何 FIFO/派单/加箱动作。
    m_stationStates[stationId - 1].awaitingAcceptance = false;
    return true;
}

bool ShortageCalculator::markRejected(int stationId)
{
    if (stationId < 1 || stationId > kStationCount) {
        return false;
    }

    return m_stationStates[stationId - 1].awaitingAcceptance;
}

void ShortageCalculator::markCommunicationInterrupted()
{
    if (m_initialized) {
        m_rebaselineAfterCommunication = true;
    }
}

QList<StationConsumption> ShortageCalculator::snapshot() const
{
    QList<StationConsumption> states;
    states.reserve(kStationCount);
    const ProductModel product = m_initialized ? m_currentProduct : ProductModel::Model88;

    for (int stationId = 1; stationId <= kStationCount; ++stationId) {
        StationConsumption item;
        item.stationId = stationId;

        const StationRuntime &runtime = m_stationStates[stationId - 1];
        item.accumulated = runtime.consumed;
        item.estimatedAvailable = runtime.estimatedAvailable;
        item.awaitingAcceptance = runtime.awaitingAcceptance;
        item.configured = runtime.configured;
        item.reason = runtime.reason;

        if (const MaterialConfig *config = configFor(stationId, product)) {
            item.safetyStock = config->safetyStock;
            item.boxQuantity = config->boxQuantity;
            item.shortage = runtime.configured && runtime.estimatedAvailable <= config->safetyStock;
        } else {
            item.reason = QStringLiteral("配置缺失");
        }
        states.append(item);
    }

    return states;
}

void ShortageCalculator::reset()
{
    clearRuntimeStates();
    m_initialized = false;
    m_hasActualBaseline = false;
    m_rebaselineAfterCommunication = false;
    m_currentProduct = ProductModel::Model88;
    m_lastActualQty = 0;
}

const MaterialConfig *ShortageCalculator::configFor(int stationId, ProductModel product) const
{
    for (const MaterialConfig &config : m_configs) {
        if (config.stationId == stationId && config.product == product) {
            return &config;
        }
    }
    return nullptr;
}

QList<int> ShortageCalculator::collectPendingStations(ProductModel product) const
{
    QList<int> stations;
    for (int stationId = 1; stationId <= kStationCount; ++stationId) {
        const MaterialConfig *config = configFor(stationId, product);
        const StationRuntime &runtime = m_stationStates[stationId - 1];
        if (!runtime.configured || !config) {
            continue;
        }
        if (runtime.estimatedAvailable <= config->safetyStock) {
            stations.append(stationId);
        }
    }
    return stations;
}

void ShortageCalculator::initializeStationRuntime(int stationId, ProductModel product)
{
    StationRuntime &runtime = m_stationStates[stationId - 1];
    runtime = StationRuntime{};

    const MaterialConfig *config = configFor(stationId, product);
    if (!config || !materialConfigHasConfirmedThresholds(*config)) {
        runtime.reason = QStringLiteral("配置缺失");
        return;
    }

    if (isAdditionOverflow(config->safetyStock, config->boxQuantity)) {
        runtime.reason = QStringLiteral("初始化库存溢出");
        return;
    }

    runtime.configured = true;
    runtime.estimatedAvailable = qint64(config->safetyStock) + config->boxQuantity;
}

void ShortageCalculator::clearRuntimeStates()
{
    for (StationRuntime &runtime : m_stationStates) {
        runtime = StationRuntime{};
    }
}
