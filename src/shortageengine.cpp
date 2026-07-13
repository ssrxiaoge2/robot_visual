#include "shortageengine.h"

#include <algorithm>

#include <QJsonObject>
#include <QSet>

namespace {

ShortageEngineResult engineOk(bool changed, const QString &messageZh)
{
    ShortageEngineResult result;
    result.ok = true;
    result.changed = changed;
    result.messageZh = messageZh;
    return result;
}

ShortageEngineResult engineFail(const QString &messageZh, bool criticalLock = false)
{
    ShortageEngineResult result;
    result.ok = false;
    result.changed = false;
    result.criticalLock = criticalLock;
    result.messageZh = messageZh;
    return result;
}

ShortageEngineResult fromPlanner(const PlannerApplyResult &planner)
{
    ShortageEngineResult result;
    result.ok = planner.ok;
    result.changed = planner.changed;
    result.criticalLock = planner.criticalLock;
    result.messageZh = planner.messageZh;
    return result;
}

ShortageEngineResult fromLedger(const LedgerApplyResult &ledger)
{
    ShortageEngineResult result;
    result.ok = ledger.ok;
    result.changed = ledger.changed;
    result.criticalLock = ledger.criticalLock;
    result.hasProductionDelta = ledger.hasProductionDelta;
    result.productionDelta = ledger.productionDelta;
    result.messageZh = ledger.messageZh;
    return result;
}

bool hasUnsafeOrderForRestore(const ShortageRuntimeState &state)
{
    for (const ReplenishmentOrder &order : state.orders) {
        if (order.state == ReplenishmentOrderState::Queued
            || order.state == ReplenishmentOrderState::Running
            || order.state == ReplenishmentOrderState::Unloaded) {
            return true;
        }
    }
    return false;
}

bool hasInFlightOrder(const ShortageRuntimeState &state)
{
    for (const ReplenishmentOrder &order : state.orders) {
        // AwaitingDispatch 尚未进入主 FIFO；Queued/Running/Unloaded 重启时需要协调器确认。
        if (order.state == ReplenishmentOrderState::Queued
            || order.state == ReplenishmentOrderState::Running
            || order.state == ReplenishmentOrderState::Unloaded) {
            return true;
        }
    }
    return false;
}

bool isOutstandingNotUnloadedOrder(ReplenishmentOrderState state)
{
    return state == ReplenishmentOrderState::AwaitingDispatch
        || state == ReplenishmentOrderState::Queued
        || state == ReplenishmentOrderState::Running;
}

bool sampleChangesContext(const ShortageRuntimeState &state, const ShortageSample &sample)
{
    return state.hasStableContext
        && (state.product != sample.product || state.mode != sample.mode);
}

ShortageOperationResult validatePlannerRestoreState(const ShortageConfiguration &configuration,
                                                    const ShortageRuntimeState &state)
{
    QSet<int> stationIds;
    for (const ShortageStationRuntime &station : state.stations)
        stationIds.insert(station.stationId);

    const ShortageStationRuntime *activeStation = nullptr;
    if (state.activeStationId != 0) {
        for (const ShortageStationRuntime &station : state.stations) {
            if (station.stationId == state.activeStationId) {
                activeStation = &station;
                break;
            }
        }
        if (activeStation == nullptr) {
            return {false, QStringLiteral("恢复状态校验失败：活动工位%1不在12工位状态内")
                               .arg(state.activeStationId)};
        }
    }

    QSet<quint64> orderNos;
    QSet<int> stationsWithOutstandingNotUnloadedOrder;
    quint64 maxOrderNo = 0;
    for (const ReplenishmentOrder &order : state.orders) {
        if (order.orderNo == 0)
            return {false, QStringLiteral("恢复状态校验失败：补料单号不能为0")};
        if (orderNos.contains(order.orderNo))
            return {false, QStringLiteral("恢复状态校验失败：补料单号%1重复").arg(order.orderNo)};
        orderNos.insert(order.orderNo);
        maxOrderNo = std::max(maxOrderNo, order.orderNo);
        if (!stationIds.contains(order.stationId)) {
            return {false, QStringLiteral("恢复状态校验失败：补料单%1 工位%2不在12工位状态内")
                               .arg(order.orderNo)
                               .arg(order.stationId)};
        }
        if (order.state == ReplenishmentOrderState::AwaitingDispatch && order.taskId != 0) {
            return {false, QStringLiteral("恢复状态校验失败：等待派发补料单%1 已绑定taskId")
                               .arg(order.orderNo)};
        }
        if ((order.state == ReplenishmentOrderState::Queued
             || order.state == ReplenishmentOrderState::Running
             || order.state == ReplenishmentOrderState::Unloaded)
            && order.taskId == 0) {
            return {false, QStringLiteral("恢复状态校验失败：补料单%1 状态需要非0 taskId")
                               .arg(order.orderNo)};
        }
        if (order.unloadAccounted
            && order.state != ReplenishmentOrderState::Unloaded
            && order.state != ReplenishmentOrderState::Succeeded
            && order.state != ReplenishmentOrderState::FailedAfterUnload) {
            return {false, QStringLiteral("恢复状态校验失败：补料单%1 已倒料但终态不一致")
                               .arg(order.orderNo)};
        }
        if (isOutstandingNotUnloadedOrder(order.state)) {
            if (stationsWithOutstandingNotUnloadedOrder.contains(order.stationId)) {
                return {false, QStringLiteral("恢复状态校验失败：工位%1存在多个未倒料补料单")
                                   .arg(order.stationId)};
            }
            stationsWithOutstandingNotUnloadedOrder.insert(order.stationId);
        }
    }
    if (state.nextReplenishmentOrderNo <= maxOrderNo) {
        return {false, QStringLiteral("恢复状态校验失败：nextReplenishmentOrderNo=%1 不大于最大补料单号%2")
                           .arg(state.nextReplenishmentOrderNo)
                           .arg(maxOrderNo)};
    }

    QSet<int> waitingSeen;
    QDateTime previousFirstLow;
    int previousStationId = 0;
    for (int stationId : state.waitingStationIds) {
        if (waitingSeen.contains(stationId))
            return {false, QStringLiteral("恢复状态校验失败：等待表工位%1重复").arg(stationId)};
        waitingSeen.insert(stationId);
        const ShortageStationRuntime *station = nullptr;
        for (const ShortageStationRuntime &candidate : state.stations) {
            if (candidate.stationId == stationId) {
                station = &candidate;
                break;
            }
        }
        if (station == nullptr || !station->firstLowAtUtc.isValid()) {
            return {false, QStringLiteral("恢复状态校验失败：等待表工位%1缺少首次低位时间")
                               .arg(stationId)};
        }
        const ShortageStationConfig *stationConfig = nullptr;
        for (const ShortageStationConfig &candidate : configuration.stations) {
            if (candidate.product == state.product && candidate.stationId == stationId) {
                stationConfig = &candidate;
                break;
            }
        }
        if (stationConfig == nullptr || !stationConfig->enabled || station->automaticPaused) {
            return {false, QStringLiteral("恢复状态校验失败：等待表工位%1不是可自动补料工位")
                               .arg(stationId)};
        }
        const bool activeContinuation = stationId == state.activeStationId
            && station->stock < stationConfig->maximumStock;
        const bool currentlyLow = station->stock < stationConfig->minimumStock;
        if (!currentlyLow && !activeContinuation) {
            return {false, QStringLiteral("恢复状态校验失败：等待表工位%1既非低位也非有效活动工位")
                               .arg(stationId)};
        }
        if (previousFirstLow.isValid()
            && (station->firstLowAtUtc < previousFirstLow
                || (station->firstLowAtUtc == previousFirstLow
                    && stationId < previousStationId))) {
            return {false, QStringLiteral("恢复状态校验失败：等待表未按首次低位时间和工位号排序")};
        }
        previousFirstLow = station->firstLowAtUtc;
        previousStationId = stationId;
    }

    if (activeStation != nullptr) {
        const ShortageStationConfig *stationConfig = nullptr;
        for (const ShortageStationConfig &candidate : configuration.stations) {
            if (candidate.product == state.product && candidate.stationId == state.activeStationId) {
                stationConfig = &candidate;
                break;
            }
        }
        if (state.criticalLock
            || stationConfig == nullptr
            || !stationConfig->enabled
            || activeStation->automaticPaused
            || activeStation->stock >= stationConfig->maximumStock
            || !waitingSeen.contains(state.activeStationId)
            || !activeStation->firstLowAtUtc.isValid()) {
            return {false, QStringLiteral("恢复状态校验失败：活动工位%1与等待/低位/暂停/最高位语义不一致")
                               .arg(state.activeStationId)};
        }
    } else if (!state.criticalLock && !state.waitingStationIds.isEmpty()) {
        return {false, QStringLiteral("恢复状态校验失败：存在等待工位但活动工位为0")};
    }

    for (const ShortageStationRuntime &station : state.stations) {
        const ShortageStationConfig *stationConfig = nullptr;
        for (const ShortageStationConfig &candidate : configuration.stations) {
            if (candidate.product == state.product && candidate.stationId == station.stationId) {
                stationConfig = &candidate;
                break;
            }
        }
        if (stationConfig == nullptr)
            return {false, QStringLiteral("恢复状态校验失败：stationId=%1 缺少当前产品配置").arg(station.stationId)};
        const bool requiresReplenishment = stationConfig->enabled
            && !station.automaticPaused
            && station.stock < stationConfig->minimumStock;
        if (requiresReplenishment && !waitingSeen.contains(station.stationId)) {
            return {false, QStringLiteral("恢复状态校验失败：低位工位%1缺少等待表记录")
                               .arg(station.stationId)};
        }
        if (!waitingSeen.contains(station.stationId) && station.firstLowAtUtc.isValid()) {
            return {false, QStringLiteral("恢复状态校验失败：非等待工位%1保留了首次低位时间")
                               .arg(station.stationId)};
        }
    }

    return {true, QStringLiteral("恢复计划状态校验通过：补料单和等待顺序一致")};
}

ShortageAuditEvent audit(ShortageRuntimeState *state,
                         const QString &eventType,
                         const QString &messageZh,
                         const QDateTime &nowUtc)
{
    ShortageAuditEvent event;
    event.sequence = state->nextAuditSequence++;
    event.occurredAtUtc = nowUtc.toUTC();
    event.eventType = eventType;
    event.messageZh = messageZh;
    return event;
}

void installCriticalLock(ShortageRuntimeState *state, const QString &reasonZh)
{
    state->criticalLock = true;
    state->criticalReasonZh = reasonZh;
}

ShortageEngineResult persistCritical(ShortageStateStore *store,
                                     ShortageRuntimeState *state,
                                     const QString &eventType,
                                     const QString &messageZh,
                                     const QDateTime &nowUtc)
{
    if (store == nullptr)
        return engineOk(true, messageZh);
    ShortageAuditEvent event = audit(state, eventType, messageZh, nowUtc);
    const ShortageOperationResult saved = store->saveCritical(*state, event);
    if (!saved.ok) {
        installCriticalLock(state, QStringLiteral("%1；关键持久化失败：%2")
                                       .arg(messageZh, saved.messageZh));
        ShortageEngineResult result = engineFail(state->criticalReasonZh, true);
        result.changed = true;
        return result;
    }
    return engineOk(true, messageZh);
}

ShortageEngineResult persistCriticalPlannerFailure(ShortageStateStore *store,
                                                   ShortageRuntimeState *state,
                                                   const QString &eventType,
                                                   const PlannerApplyResult &planner,
                                                   const QDateTime &nowUtc)
{
    installCriticalLock(state, planner.messageZh);
    const ShortageEngineResult persisted =
        persistCritical(store, state, eventType, planner.messageZh, nowUtc);
    if (!persisted.ok)
        return persisted;
    ShortageEngineResult result = engineFail(planner.messageZh, true);
    result.changed = true;
    return result;
}

ShortageEngineResult persistPeriodic(ShortageStateStore *store,
                                     ShortageRuntimeState *state,
                                     const QString &eventType,
                                     const QString &messageZh,
                                     const QDateTime &nowUtc)
{
    if (store == nullptr)
        return engineOk(true, messageZh);
    ShortageAuditEvent event = audit(state, eventType, messageZh, nowUtc);
    const ShortageOperationResult saved = store->savePeriodic(*state, event, nowUtc);
    if (!saved.ok) {
        installCriticalLock(state, QStringLiteral("%1；周期持久化失败：%2")
                                       .arg(messageZh, saved.messageZh));
        ShortageEngineResult result = engineFail(state->criticalReasonZh, true);
        result.changed = true;
        return result;
    }
    return engineOk(true, messageZh);
}

ShortageStationRuntime *stationById(ShortageRuntimeState *state, int stationId)
{
    for (ShortageStationRuntime &station : state->stations) {
        if (station.stationId == stationId)
            return &station;
    }
    return nullptr;
}

} // namespace

ShortageEngine::ShortageEngine(ShortageConfiguration configuration,
                               ShortageStateStore *stateStore)
    : m_configuration(std::move(configuration))
    , m_stateStore(stateStore)
    , m_ledger(m_configuration)
    , m_planner(m_configuration)
{
}

ShortageEngineResult ShortageEngine::installRestoredState(
    const ShortageStateLoadResult &loadResult,
    const QDateTime &nowUtc)
{
    Q_UNUSED(nowUtc)

    if (!loadResult.ok || loadResult.requiresMaintenance) {
        m_restoreLocked = true;
        installCriticalLock(&m_state, loadResult.messageZh);
        return engineFail(QStringLiteral("恢复安装失败：%1，处理动作=进入维护锁定")
                              .arg(loadResult.messageZh),
                          true);
    }
    if (loadResult.source == ShortageRestoreSource::None) {
        m_restoreLocked = true;
        installCriticalLock(&m_state, QStringLiteral("恢复安装失败：ok恢复结果缺少明确来源"));
        return engineFail(QStringLiteral("%1，处理动作=进入维护锁定").arg(m_state.criticalReasonZh),
                          true);
    }

    const ShortageOperationResult ledgerValidation =
        m_ledger.validateRestoredState(loadResult.state);
    if (!ledgerValidation.ok) {
        m_restoreLocked = true;
        installCriticalLock(&m_state, ledgerValidation.messageZh);
        return engineFail(ledgerValidation.messageZh, true);
    }
    const ShortageOperationResult plannerValidation =
        validatePlannerRestoreState(m_configuration, loadResult.state);
    if (!plannerValidation.ok) {
        m_restoreLocked = true;
        installCriticalLock(&m_state, plannerValidation.messageZh);
        return engineFail(plannerValidation.messageZh, true);
    }
    if (hasUnsafeOrderForRestore(loadResult.state)) {
        m_restoreLocked = true;
        installCriticalLock(&m_state,
                            QStringLiteral("恢复状态包含已入队/运行/倒料补料单，必须维护确认主FIFO和硬件现状"));
        return engineFail(m_state.criticalReasonZh, true);
    }

    m_state = loadResult.state;
    m_state.initialized = true;
    m_state.operatorConfirmedRestore = false;
    m_restoreInstalled = true;
    m_restoreLocked = false;
    return engineOk(true, QStringLiteral("恢复状态已安全安装：等待操作员确认后才允许自动派单"));
}

ShortageEngineResult ShortageEngine::initializeZero(bool siteIsConfirmedEmpty,
                                                    const QDateTime &nowUtc)
{
    ShortageRuntimeState work = m_state;
    const LedgerApplyResult ledger = m_ledger.initializeZero(&work, siteIsConfirmedEmpty, nowUtc);
    if (!ledger.ok)
        return fromLedger(ledger);
    work.operatorConfirmedRestore = true;
    m_state = work;
    m_restoreInstalled = true;
    m_restoreLocked = false;
    return persistCritical(m_stateStore, &m_state, QStringLiteral("zero_initialized"),
                           ledger.messageZh, nowUtc);
}

ShortageEngineResult ShortageEngine::confirmRestoredState(bool accepted,
                                                          const QDateTime &nowUtc)
{
    Q_UNUSED(nowUtc)
    if (m_restoreLocked)
        return engineFail(QStringLiteral("恢复确认失败：当前处于维护锁定"), true);
    if (!m_restoreInstalled)
        return engineFail(QStringLiteral("恢复确认失败：尚未安装可确认状态"));
    if (!accepted)
        return engineFail(QStringLiteral("恢复确认已拒绝：自动派单门禁保持关闭"));
    m_state.operatorConfirmedRestore = true;
    return engineOk(true, QStringLiteral("恢复摘要已确认：允许后续自动派单"));
}

ShortageEngineResult ShortageEngine::applyStableSample(const ShortageSample &sample)
{
    if (sampleChangesContext(m_state, sample) && hasInFlightOrder(m_state)) {
        ShortageRuntimeState work = m_state;
        work.hasPendingContext = true;
        work.pendingProduct = sample.product;
        work.pendingMode = sample.mode;
        work.hasPendingActualQty = true;
        work.pendingActualQty = sample.actualQty;
        m_state = work;
        const QString message = QStringLiteral(
            "采样上下文变化但旧任务未排空：保存pendingActualQty=%1，处理动作=不提前创建新产品任务")
                                    .arg(sample.actualQty);
        return persistPeriodic(m_stateStore, &m_state, QStringLiteral("pending_context_saved"),
                               message, sample.capturedAtUtc);
    }

    ShortageRuntimeState work = m_state;
    const LedgerApplyResult ledger = m_ledger.applyStableSample(&work, sample);
    if (!ledger.ok) {
        if (ledger.criticalLock)
            installCriticalLock(&m_state, ledger.messageZh);
        return fromLedger(ledger);
    }
    const PlannerApplyResult planner = m_planner.reevaluate(&work, sample.capturedAtUtc);
    if (!planner.ok) {
        if (planner.criticalLock)
            installCriticalLock(&m_state, planner.messageZh);
        return fromPlanner(planner);
    }
    m_state = work;
    ShortageEngineResult result = fromLedger(ledger);
    result.changed = ledger.changed || planner.changed;
    result.messageZh = QStringLiteral("%1；%2").arg(ledger.messageZh, planner.messageZh);
    if (result.changed) {
        const ShortageEngineResult persisted =
            persistPeriodic(m_stateStore, &m_state, QStringLiteral("sample_applied"),
                            result.messageZh, sample.capturedAtUtc);
        if (!persisted.ok)
            return persisted;
    }
    return result;
}

ShortageEngineResult ShortageEngine::activatePendingContextIfDrained(bool oldTasksDrained,
                                                                     const QDateTime &nowUtc)
{
    Q_UNUSED(nowUtc)
    if (!oldTasksDrained)
        return engineOk(false, QStringLiteral("旧任务未排空：继续等待换型激活"));
    if (!m_state.hasPendingContext)
        return engineOk(false, QStringLiteral("没有待激活上下文"));

    ShortageSample sample;
    sample.roundId = 0;
    sample.product = m_state.pendingProduct;
    sample.mode = m_state.pendingMode;
    sample.actualQty = m_state.hasPendingActualQty ? m_state.pendingActualQty
                                                   : m_state.actualQty.baseline;
    sample.capturedAtUtc = nowUtc;

    ShortageRuntimeState work = m_state;
    const LedgerApplyResult ledger = m_ledger.applyStableSample(&work, sample);
    if (!ledger.ok) {
        if (ledger.criticalLock)
            installCriticalLock(&m_state, ledger.messageZh);
        return fromLedger(ledger);
    }
    work.hasPendingContext = false;
    work.hasPendingActualQty = false;
    work.pendingActualQty = 0;
    const PlannerApplyResult planner = m_planner.reevaluate(&work, nowUtc);
    if (!planner.ok) {
        if (planner.criticalLock)
            installCriticalLock(&m_state, planner.messageZh);
        return fromPlanner(planner);
    }
    m_state = work;
    ShortageEngineResult result = fromLedger(ledger);
    result.changed = true;
    result.messageZh = QStringLiteral("%1；%2；待切换上下文已激活")
                           .arg(ledger.messageZh, planner.messageZh);
    const ShortageEngineResult persisted =
        persistPeriodic(m_stateStore, &m_state, QStringLiteral("pending_context_activated"),
                        result.messageZh, nowUtc);
    if (!persisted.ok)
        return persisted;
    return result;
}

ShortageEngineResult ShortageEngine::requestManualBox(int stationId,
                                                      bool highStockRiskConfirmed,
                                                      const QDateTime &nowUtc)
{
    if (m_restoreLocked)
        return engineFail(QStringLiteral("人工补料失败：启动恢复处于维护锁定"), true);
    if (!m_state.initialized || !m_state.operatorConfirmedRestore)
        return engineFail(QStringLiteral("人工补料失败：账本尚未恢复确认"));
    ShortageRuntimeState work = m_state;
    const PlannerApplyResult planner =
        m_planner.requestManualBox(&work, stationId, highStockRiskConfirmed, nowUtc);
    if (!planner.ok)
        return fromPlanner(planner);
    m_state = work;
    return persistCritical(m_stateStore, &m_state, QStringLiteral("manual_box_requested"),
                           planner.messageZh, nowUtc);
}

std::optional<ShortageDispatchRequest> ShortageEngine::nextDispatchRequest()
{
    if (m_restoreLocked)
        return std::nullopt;
    return m_planner.nextDispatchRequest(m_state);
}

ShortageEngineResult ShortageEngine::recordDispatchResult(quint64 replenishmentOrderNo,
                                                          bool accepted,
                                                          quint64 taskId,
                                                          const QString &reason)
{
    ShortageRuntimeState work = m_state;
    const PlannerApplyResult planner =
        m_planner.markDispatchResult(&work, replenishmentOrderNo, accepted, taskId, reason);
    if (!planner.ok) {
        if (planner.criticalLock)
            installCriticalLock(&m_state, planner.messageZh);
        return fromPlanner(planner);
    }
    m_state = work;
    return persistCritical(m_stateStore, &m_state, QStringLiteral("dispatch_result_recorded"),
                           planner.messageZh, QDateTime::currentDateTimeUtc());
}

ShortageEngineResult ShortageEngine::recordTaskStarted(const TaskFact &fact)
{
    ShortageRuntimeState work = m_state;
    const PlannerApplyResult planner = m_planner.markTaskStarted(&work, fact);
    if (!planner.ok) {
        if (planner.criticalLock) {
            return persistCriticalPlannerFailure(m_stateStore, &m_state,
                                                 QStringLiteral("task_fact_rejected"),
                                                 planner, fact.occurredAtUtc);
        }
        return fromPlanner(planner);
    }
    m_state = work;
    return persistCritical(m_stateStore, &m_state, QStringLiteral("task_started"),
                           planner.messageZh, fact.occurredAtUtc);
}

ShortageEngineResult ShortageEngine::recordMaterialUnloaded(const TaskFact &fact)
{
    ShortageRuntimeState work = m_state;
    const PlannerApplyResult validated = m_planner.validateUnloadFact(work, fact);
    if (!validated.ok) {
        if (validated.criticalLock) {
            return persistCriticalPlannerFailure(m_stateStore, &m_state,
                                                 QStringLiteral("task_fact_rejected"),
                                                 validated, fact.occurredAtUtc);
        }
        return fromPlanner(validated);
    }
    const LedgerApplyResult ledger =
        m_ledger.recordUnloadedBox(&work, fact.stationId, fact.origin, fact.occurredAtUtc);
    if (!ledger.ok) {
        if (ledger.criticalLock)
            installCriticalLock(&m_state, ledger.messageZh);
        return fromLedger(ledger);
    }
    const PlannerApplyResult unloaded = m_planner.markUnloaded(&work, fact);
    if (!unloaded.ok) {
        if (unloaded.criticalLock)
            installCriticalLock(&m_state, unloaded.messageZh);
        return fromPlanner(unloaded);
    }
    m_state = work;
    const QString message = QStringLiteral("%1；%2").arg(ledger.messageZh, unloaded.messageZh);
    if (m_stateStore != nullptr) {
        ShortageAuditEvent event =
            audit(&m_state, QStringLiteral("box_unloaded"), message, fact.occurredAtUtc);
        event.details = {
            {QStringLiteral("stationId"), fact.stationId},
            {QStringLiteral("taskId"), QString::number(fact.taskId)},
            {QStringLiteral("orderNo"), QString::number(fact.replenishmentOrderNo)},
        };
        const ShortageOperationResult saved = m_stateStore->saveCritical(m_state, event);
        if (!saved.ok) {
            installCriticalLock(&m_state,
                                QStringLiteral("倒料已入账但关键持久化失败：%1").arg(saved.messageZh));
            ShortageEngineResult failed = engineFail(m_state.criticalReasonZh, true);
            failed.changed = true;
            return failed;
        }
    }
    ShortageEngineResult result = fromLedger(ledger);
    result.changed = true;
    result.messageZh = message;
    return result;
}

ShortageEngineResult ShortageEngine::recordTaskTerminal(const TaskFact &fact)
{
    ShortageRuntimeState work = m_state;
    const PlannerApplyResult planner =
        m_planner.markTerminal(&work, fact, m_configuration.parameters.preUnloadFailureLimit);
    if (!planner.ok) {
        if (planner.criticalLock) {
            return persistCriticalPlannerFailure(m_stateStore, &m_state,
                                                 QStringLiteral("task_fact_rejected"),
                                                 planner, fact.occurredAtUtc);
        }
        return fromPlanner(planner);
    }
    m_state = work;
    return persistCritical(m_stateStore, &m_state, QStringLiteral("task_terminal"),
                           planner.messageZh, fact.occurredAtUtc);
}

ShortageEngineResult ShortageEngine::applyMaintenanceCorrection(
    const ShortageMaintenanceCorrection &correction,
    const QDateTime &nowUtc)
{
    Q_UNUSED(nowUtc)
    if (correction.typedStationId != QString::number(correction.stationId))
        return engineFail(QStringLiteral("维护修正失败：手输工位号不一致"));
    ShortageRuntimeState work = m_state;
    ShortageStationRuntime *station = stationById(&work, correction.stationId);
    if (station == nullptr || station->stock != correction.oldStock)
        return engineFail(QStringLiteral("维护修正失败：工位不存在或库存已变化"), true);
    if (m_stateStore != nullptr) {
        const ShortageOperationResult backup = m_stateStore->backupBeforeMaintenance(m_state);
        if (!backup.ok)
            return engineFail(backup.messageZh, true);
    }
    station->stock = correction.newStock;
    work.criticalLock = false;
    work.criticalReasonZh.clear();
    m_state = work;
    return persistCritical(m_stateStore, &m_state, QStringLiteral("maintenance_correction"),
                           QStringLiteral("维护修正已应用：stationId=%1").arg(correction.stationId),
                           nowUtc);
}

const ShortageConfiguration &ShortageEngine::configuration() const
{
    return m_configuration;
}

const ShortageRuntimeState &ShortageEngine::state() const
{
    return m_state;
}
