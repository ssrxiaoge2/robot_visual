#include "shortageledger.h"

#include <limits>

#include <QSet>

namespace {

LedgerApplyResult failResult(const QString &messageZh, bool criticalLock = false)
{
    LedgerApplyResult result;
    result.ok = false;
    result.changed = false;
    result.criticalLock = criticalLock;
    result.messageZh = messageZh;
    return result;
}

LedgerApplyResult okResult(bool changed, const QString &messageZh)
{
    LedgerApplyResult result;
    result.ok = true;
    result.changed = changed;
    result.messageZh = messageZh;
    return result;
}

bool checkedAdd(qint64 left, qint64 right, qint64 *result)
{
    if (right > 0 && left > std::numeric_limits<qint64>::max() - right)
        return false;
    if (right < 0 && left < std::numeric_limits<qint64>::min() - right)
        return false;
    *result = left + right;
    return true;
}

/// 64 位乘法预检；任一工位失败时调用方不得替换正式状态。
bool checkedMultiply(qint64 delta, qint64 usage, qint64 *result)
{
    if (delta < 0 || usage < 0)
        return false;
    if (delta != 0 && usage > std::numeric_limits<qint64>::max() / delta)
        return false;
    *result = delta * usage;
    return true;
}

/// 64 位扣减预检；允许库存变负，但不允许 qint64 算术溢出。
bool checkedSubtract(qint64 left, qint64 right, qint64 *result)
{
    if (right < 0)
        return false;
    if (left < std::numeric_limits<qint64>::min() + right)
        return false;
    *result = left - right;
    return true;
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
    return 0;
}

const ShortageStationConfig *findConfig(const ShortageConfiguration &configuration,
                                        ProductModel product,
                                        int stationId)
{
    for (const ShortageStationConfig &station : configuration.stations) {
        if (station.product == product && station.stationId == stationId)
            return &station;
    }
    return nullptr;
}

ShortageStationRuntime *findStation(ShortageRuntimeState *state, int stationId)
{
    for (ShortageStationRuntime &station : state->stations) {
        if (station.stationId == stationId)
            return &station;
    }
    return nullptr;
}

bool applyContext(ShortageRuntimeState *work, const ShortageSample &sample)
{
    const bool changed = !work->hasStableContext || work->product != sample.product
        || work->mode != sample.mode;
    work->hasStableContext = true;
    work->product = sample.product;
    work->mode = sample.mode;
    return changed;
}

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
    return QStringLiteral("unknown");
}

QString modeName(ProductionMode mode)
{
    switch (mode) {
    case ProductionMode::LeftRight:
        return QStringLiteral("L/R");
    case ProductionMode::LeftOnly:
        return QStringLiteral("L/L");
    case ProductionMode::RightOnly:
        return QStringLiteral("R/H");
    }
    return QStringLiteral("unknown");
}

LedgerApplyResult deductProduction(ShortageRuntimeState *work,
                                   const ShortageConfiguration &configuration,
                                   ProductModel product,
                                   ProductionMode mode,
                                   qint64 oldBaseline,
                                   qint64 current,
                                   qint64 delta)
{
    for (ShortageStationRuntime &station : work->stations) {
        const ShortageStationConfig *config =
            findConfig(configuration, product, station.stationId);
        if (config == nullptr) {
            return failResult(QStringLiteral("缺少产品工位配置：stationId=%1，处理动作=严重锁定")
                                  .arg(station.stationId),
                              true);
        }
        if (!config->enabled)
            continue;

        qint64 consumed = 0;
        qint64 newStock = 0;
        const qint64 usage = usageForMode(*config, mode);
        if (!checkedMultiply(delta, usage, &consumed)
            || !checkedSubtract(station.stock, consumed, &newStock)) {
            return failResult(QStringLiteral(
                                  "产量扣减64位溢出：stationId=%1，oldBaseline=%2，actualQty=%3，delta=%4，usage=%5，oldStock=%6，处理动作=严重锁定且不修改账本")
                                  .arg(station.stationId)
                                  .arg(oldBaseline)
                                  .arg(current)
                                  .arg(delta)
                                  .arg(usage)
                                  .arg(station.stock),
                              true);
        }
        station.stock = newStock;
    }

    work->actualQty.hasBaseline = true;
    work->actualQty.baseline = current;
    work->actualQty.hasResetCandidate = false;
    work->actualQty.resetCandidate = 0;

    LedgerApplyResult result =
        okResult(true,
                 QStringLiteral("产量扣减成功：oldBaseline=%1，actualQty=%2，delta=%3，处理动作=按当前产品和模式扣减")
                     .arg(oldBaseline)
                     .arg(current)
                     .arg(delta));
    result.hasProductionDelta = delta > 0;
    result.productionDelta = delta;
    return result;
}

} // namespace

ShortageLedger::ShortageLedger(ShortageConfiguration configuration)
    : m_configuration(std::move(configuration))
{
}

LedgerApplyResult ShortageLedger::initializeZero(ShortageRuntimeState *state,
                                                 bool siteIsConfirmedEmpty,
                                                 const QDateTime &nowUtc)
{
    Q_UNUSED(nowUtc)

    if (state == nullptr)
        return failResult(QStringLiteral("0建账失败：state为空，处理动作=不修改账本"));
    if (!siteIsConfirmedEmpty) {
        return failResult(QStringLiteral(
            "0建账失败：现场未确认12工位为空，处理动作=不修改库存、基线和候选"));
    }

    ShortageRuntimeState work = *state;
    if (work.stations.isEmpty()) {
        work.stations.reserve(12);
        for (int stationId = 1; stationId <= 12; ++stationId) {
            ShortageStationRuntime station;
            station.stationId = stationId;
            work.stations.append(station);
        }
    }
    if (work.stations.size() != 12) {
        return failResult(QStringLiteral("0建账失败：工位数量=%1，期望=12，处理动作=不修改账本")
                              .arg(work.stations.size()));
    }

    QSet<int> seen;
    for (ShortageStationRuntime &station : work.stations) {
        if (station.stationId < 1 || station.stationId > 12 || seen.contains(station.stationId)) {
            return failResult(QStringLiteral(
                "0建账失败：stationId=%1 非法或重复，处理动作=不修改账本")
                                  .arg(station.stationId));
        }
        seen.insert(station.stationId);
        station.stock = 0;
    }

    work.configurationRevision = m_configuration.revision;
    work.initialized = true;
    work.actualQty = ActualQtyRuntime{};
    *state = work;
    return okResult(true, QStringLiteral("0建账成功：已确认现场空料，12工位库存设为0"));
}

LedgerApplyResult ShortageLedger::applyStableSample(ShortageRuntimeState *state,
                                                    const ShortageSample &sample)
{
    if (state == nullptr)
        return failResult(QStringLiteral("采样入账失败：state为空，处理动作=不修改账本"));
    if (sample.actualQty < 0) {
        return failResult(QStringLiteral("采样入账失败：actualQty=%1 为负，处理动作=不修改账本")
                              .arg(sample.actualQty));
    }

    ShortageRuntimeState work = *state;
    const ProductModel oldProduct = work.product;
    const ProductionMode oldMode = work.mode;
    const bool oldHasStableContext = work.hasStableContext;
    const bool contextChanged = applyContext(&work, sample);
    ActualQtyRuntime &actual = work.actualQty;
    const qint64 current = sample.actualQty;

    if (!actual.hasBaseline) {
        actual.hasBaseline = true;
        actual.baseline = current;
        actual.hasResetCandidate = false;
        actual.resetCandidate = 0;
        work.actualQty.interrupted = false;
        *state = work;
        return okResult(true,
                        QStringLiteral("首次有效样本只建立基线：actualQty=%1，处理动作=不扣库存")
                            .arg(current));
    }

    const qint64 oldBaseline = actual.baseline;
    if ((actual.interrupted || sample.recoveredAfterInterruption) && oldHasStableContext
        && (sample.product != oldProduct || sample.mode != oldMode)) {
        return failResult(QStringLiteral(
                              "断线恢复上下文变化需要维护确认：oldProduct=%1，oldMode=%2，newProduct=%3，newMode=%4，oldBaseline=%5，actualQty=%6，处理动作=不扣库存且不切换上下文")
                              .arg(productName(oldProduct), modeName(oldMode),
                                   productName(sample.product), modeName(sample.mode))
                              .arg(oldBaseline)
                              .arg(current),
                          true);
    }
    if ((actual.interrupted || sample.recoveredAfterInterruption) && current < oldBaseline) {
        return failResult(QStringLiteral(
                              "断线恢复回退需要维护确认：oldBaseline=%1，actualQty=%2，处理动作=不自动清零")
                              .arg(oldBaseline)
                              .arg(current),
                          true);
    }

    // 当前值低于旧基线时先进入清零候选分支，不扣库存也不移动旧基线。
    if (current < oldBaseline) {
        if (!actual.hasResetCandidate) {
            actual.hasResetCandidate = true;
            actual.resetCandidate = current;
            *state = work;
            return okResult(true,
                            QStringLiteral("保存清零候选：oldBaseline=%1，candidate=%2，处理动作=等待下一轮确认")
                                .arg(oldBaseline)
                                .arg(current));
        }
        if (current < actual.resetCandidate) {
            actual.resetCandidate = current;
            *state = work;
            return okResult(true,
                            QStringLiteral("下移清零候选：oldBaseline=%1，candidate=%2，处理动作=库存和旧基线不变")
                                .arg(oldBaseline)
                                .arg(current));
        }
        if (current == actual.resetCandidate) {
            return okResult(false,
                            QStringLiteral("清零候选重复：oldBaseline=%1，candidate=%2，处理动作=继续等待")
                                .arg(oldBaseline)
                                .arg(current));
        }

        // 候选之后回升但仍低于旧基线，确认进入新周期，按 current 从 0 累计扣减。
        const qint64 confirmedCandidate = actual.resetCandidate;
        LedgerApplyResult result =
            deductProduction(&work, m_configuration, sample.product, sample.mode,
                             qint64{0}, current, current);
        if (!result.ok)
            return result;
        *state = work;
        result.messageZh = QStringLiteral(
                               "确认actualQty清零新周期：oldBaseline=%1，candidate=%2，actualQty=%3，处理动作=按0到当前值扣减")
                               .arg(oldBaseline)
                               .arg(confirmedCandidate)
                               .arg(current);
        return result;
    }

    if (actual.hasResetCandidate && current >= oldBaseline) {
        // 候选后回到旧基线以上，候选为毛刺；只按旧基线到当前值扣减。
        const qint64 delta = current - oldBaseline;
        if (delta == 0) {
            actual.hasResetCandidate = false;
            actual.resetCandidate = 0;
            *state = work;
            return okResult(true,
                            QStringLiteral("清零候选判定为毛刺：oldBaseline=%1，actualQty=%2，处理动作=清除候选不扣库存")
                                .arg(oldBaseline)
                                .arg(current));
        }
        LedgerApplyResult result =
            deductProduction(&work, m_configuration, sample.product, sample.mode,
                             oldBaseline, current, delta);
        if (!result.ok)
            return result;
        *state = work;
        result.messageZh = QStringLiteral(
                               "清零候选判定为毛刺：oldBaseline=%1，actualQty=%2，delta=%3，处理动作=按旧基线扣减")
                               .arg(oldBaseline)
                               .arg(current)
                               .arg(delta);
        return result;
    }

    if (current == oldBaseline) {
        if (contextChanged)
            *state = work;
        return okResult(contextChanged,
                        QStringLiteral("actualQty未变化：baseline=%1，处理动作=不写库存")
                            .arg(oldBaseline));
    }

    const qint64 delta = current - oldBaseline;
    LedgerApplyResult result =
        deductProduction(&work, m_configuration, sample.product, sample.mode,
                         oldBaseline, current, delta);
    if (!result.ok)
        return result;
    *state = work;
    return result;
}

LedgerApplyResult ShortageLedger::recordUnloadedBox(ShortageRuntimeState *state,
                                                    int stationId,
                                                    ReplenishmentOrigin origin,
                                                    const QDateTime &nowUtc)
{
    Q_UNUSED(nowUtc)

    if (state == nullptr)
        return failResult(QStringLiteral("倒料入账失败：state为空，处理动作=不修改账本"));

    const ShortageStationConfig *config =
        findConfig(m_configuration, state->product, stationId);
    if (config == nullptr) {
        return failResult(QStringLiteral("倒料入账失败：stationId=%1 缺少配置，处理动作=严重锁定")
                              .arg(stationId),
                          true);
    }

    ShortageRuntimeState work = *state;
    ShortageStationRuntime *station = findStation(&work, stationId);
    if (station == nullptr) {
        return failResult(QStringLiteral("倒料入账失败：stationId=%1 不在12工位状态内，处理动作=严重锁定")
                              .arg(stationId),
                          true);
    }

    qint64 newStock = 0;
    if (!checkedAdd(station->stock, config->boxQuantity, &newStock)) {
        return failResult(QStringLiteral(
                              "倒料入账64位溢出：stationId=%1，oldStock=%2，boxQuantity=%3，处理动作=严重锁定且不修改账本")
                              .arg(stationId)
                              .arg(station->stock)
                              .arg(config->boxQuantity),
                          true);
    }
    station->stock = newStock;

    *state = work;
    return okResult(true,
                    QStringLiteral("倒料一箱入账成功：stationId=%1，origin=%2，boxQuantity=%3，处理动作=只增加一箱")
                        .arg(stationId)
                        .arg(origin == ReplenishmentOrigin::Automatic ? QStringLiteral("自动")
                                                                      : QStringLiteral("人工"))
                        .arg(config->boxQuantity));
}

ShortageOperationResult ShortageLedger::validateRestoredState(
    const ShortageRuntimeState &state) const
{
    QStringList errors;
    if (state.configurationRevision != m_configuration.revision) {
        errors.append(QStringLiteral("配置修订号不一致：state=%1，configuration=%2")
                          .arg(state.configurationRevision)
                          .arg(m_configuration.revision));
    }
    if (state.stations.size() != 12) {
        errors.append(QStringLiteral("工位数量必须为12，当前=%1").arg(state.stations.size()));
    }

    QSet<int> seen;
    for (const ShortageStationRuntime &station : state.stations) {
        if (station.stationId < 1 || station.stationId > 12) {
            errors.append(QStringLiteral("stationId=%1 超出1～12").arg(station.stationId));
        } else if (seen.contains(station.stationId)) {
            errors.append(QStringLiteral("stationId=%1 重复").arg(station.stationId));
        }
        seen.insert(station.stationId);
    }
    if (state.actualQty.hasBaseline && state.actualQty.baseline < 0) {
        errors.append(QStringLiteral("actualQty baseline=%1 为负").arg(state.actualQty.baseline));
    }
    if (state.actualQty.hasResetCandidate && state.actualQty.resetCandidate < 0) {
        errors.append(QStringLiteral("actualQty resetCandidate=%1 为负")
                          .arg(state.actualQty.resetCandidate));
    }

    if (!errors.isEmpty())
        return {false, QStringLiteral("恢复状态校验失败：%1，处理动作=进入维护")
                           .arg(errors.join(QStringLiteral("；")))};
    return {true, QStringLiteral("恢复状态校验通过：12工位、基线和配置修订号一致")};
}
