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

} // namespace

ShortageCalculator::ShortageCalculator()
    : ShortageCalculator(defaultMaterialConfigs())
{
}

ShortageCalculator::ShortageCalculator(const QList<MaterialConfig> &configs)
    : m_configs(configs)
{
}

IngestResult ShortageCalculator::ingest(const ShortageSample &sample)
{
    IngestResult result;
    if (sample.actualQty < 0) {
        result.errorMessage = QStringLiteral("actualQty 不能为负数");
        return result;
    }

    const bool resetForProductChange = m_hasBaseline
        && (sample.product != m_lastProduct || sample.mode != m_lastMode);
    const bool resetForShiftChange = m_hasBaseline
        && sample.actualQty < m_lastActualQty;
    if (!m_hasBaseline || m_rebaselineAfterCommunication
        || resetForProductChange || resetForShiftChange) {
        if (resetForProductChange || resetForShiftChange) {
            clearRuntimeStates();
        }
        resetAccumulation(sample.product, sample.mode, sample.actualQty);
        result.ok = true;
        return result;
    }

    const qint64 productionDelta = sample.actualQty - m_lastActualQty;
    for (int stationId = 1; stationId <= kStationCount; ++stationId) {
        const MaterialConfig *config = configFor(stationId, sample.product);
        if (!config || !materialConfigHasConfirmedThresholds(*config)) {
            continue;
        }

        const int usage = usagePerProduct(*config, sample.mode);
        if (usage <= 0) {
            continue;
        }
        if (isMultiplicationOverflow(productionDelta, usage)) {
            result.errorMessage = QStringLiteral("工位%1 的产量增量乘以用量后溢出").arg(stationId);
            return result;
        }

        const qint64 deltaConsumption = productionDelta * usage;
        StationRuntime &runtime = m_stationStates[stationId - 1];
        if (isAdditionOverflow(runtime.accumulated, deltaConsumption)) {
            result.errorMessage = QStringLiteral("工位%1 的累计消耗溢出").arg(stationId);
            return result;
        }
        runtime.accumulated += deltaConsumption;
    }

    m_lastActualQty = sample.actualQty;
    m_lastProduct = sample.product;
    m_lastMode = sample.mode;
    result.ok = true;
    result.pendingStations = collectPendingStations(sample.product, sample.mode);
    for (int stationId : result.pendingStations) {
        m_stationStates[stationId - 1].awaitingAcceptance = true;
    }
    return result;
}

bool ShortageCalculator::confirmAccepted(int stationId)
{
    if (stationId < 1 || stationId > kStationCount || !m_hasBaseline) {
        return false;
    }

    StationRuntime &runtime = m_stationStates[stationId - 1];
    if (!runtime.awaitingAcceptance) {
        return false;
    }

    const MaterialConfig *config = configFor(stationId, m_lastProduct);
    if (!config || config->safetyStock <= 0 || runtime.accumulated < config->safetyStock) {
        return false;
    }

    runtime.accumulated -= config->safetyStock;
    runtime.awaitingAcceptance = false;
    return true;
}

bool ShortageCalculator::markRejected(int stationId)
{
    if (stationId < 1 || stationId > kStationCount) {
        return false;
    }

    StationRuntime &runtime = m_stationStates[stationId - 1];
    if (!runtime.awaitingAcceptance) {
        return false;
    }

    runtime.awaitingAcceptance = false;
    return true;
}

void ShortageCalculator::markCommunicationInterrupted()
{
    if (!m_hasBaseline) {
        return;
    }
    m_rebaselineAfterCommunication = true;
}

QList<StationConsumption> ShortageCalculator::snapshot() const
{
    QList<StationConsumption> states;
    states.reserve(kStationCount);
    const ProductModel product = m_hasBaseline ? m_lastProduct : ProductModel::Model88;
    for (int stationId = 1; stationId <= kStationCount; ++stationId) {
        StationConsumption item;
        item.stationId = stationId;
        item.accumulated = m_stationStates[stationId - 1].accumulated;
        item.awaitingAcceptance = m_stationStates[stationId - 1].awaitingAcceptance;
        if (const MaterialConfig *config = configFor(stationId, product)) {
            item.safetyStock = config->safetyStock;
            item.boxQuantity = config->boxQuantity;
        }
        states.append(item);
    }
    return states;
}

void ShortageCalculator::reset()
{
    clearRuntimeStates();
    m_hasBaseline = false;
    m_rebaselineAfterCommunication = false;
    m_lastActualQty = 0;
    m_lastProduct = ProductModel::Model88;
    m_lastMode = ProductionMode::L68;
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

QList<int> ShortageCalculator::collectPendingStations(ProductModel product,
                                                      ProductionMode mode) const
{
    Q_UNUSED(mode);
    QList<int> stations;
    for (int stationId = 1; stationId <= kStationCount; ++stationId) {
        const MaterialConfig *config = configFor(stationId, product);
        if (!config || config->safetyStock <= 0) {
            continue;
        }

        const StationRuntime &runtime = m_stationStates[stationId - 1];
        if (!runtime.awaitingAcceptance && runtime.accumulated >= config->safetyStock) {
            stations.append(stationId);
        }
    }
    return stations;
}

void ShortageCalculator::resetAccumulation(ProductModel product,
                                           ProductionMode mode,
                                           qint64 actualQty)
{
    m_hasBaseline = true;
    m_rebaselineAfterCommunication = false;
    m_lastProduct = product;
    m_lastMode = mode;
    m_lastActualQty = actualQty;
}

void ShortageCalculator::clearRuntimeStates()
{
    for (StationRuntime &runtime : m_stationStates) {
        runtime.accumulated = 0;
        runtime.awaitingAcceptance = false;
    }
}
