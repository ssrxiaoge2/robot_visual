#include "replenishmentplanner.h"

#include <algorithm>

namespace {

PlannerApplyResult plannerOk(bool changed, const QString &messageZh)
{
    PlannerApplyResult result;
    result.ok = true;
    result.changed = changed;
    result.messageZh = messageZh;
    return result;
}

PlannerApplyResult plannerFail(const QString &messageZh, bool criticalLock = false)
{
    PlannerApplyResult result;
    result.ok = false;
    result.changed = false;
    result.criticalLock = criticalLock;
    result.messageZh = messageZh;
    return result;
}

const ShortageStationConfig *configFor(const ShortageConfiguration &configuration,
                                       ProductModel product,
                                       int stationId)
{
    for (const ShortageStationConfig &station : configuration.stations) {
        if (station.product == product && station.stationId == stationId)
            return &station;
    }
    return nullptr;
}

ShortageStationRuntime *runtimeStation(ShortageRuntimeState *state, int stationId)
{
    for (ShortageStationRuntime &station : state->stations) {
        if (station.stationId == stationId)
            return &station;
    }
    return nullptr;
}

const ShortageStationRuntime *runtimeStation(const ShortageRuntimeState &state, int stationId)
{
    for (const ShortageStationRuntime &station : state.stations) {
        if (station.stationId == stationId)
            return &station;
    }
    return nullptr;
}

ReplenishmentOrder *orderByNo(ShortageRuntimeState *state, quint64 orderNo)
{
    for (ReplenishmentOrder &order : state->orders) {
        if (order.orderNo == orderNo)
            return &order;
    }
    return nullptr;
}

const ReplenishmentOrder *orderByNo(const ShortageRuntimeState &state, quint64 orderNo)
{
    for (const ReplenishmentOrder &order : state.orders) {
        if (order.orderNo == orderNo)
            return &order;
    }
    return nullptr;
}

bool isNotUnloaded(const ReplenishmentOrder &order)
{
    return order.state == ReplenishmentOrderState::AwaitingDispatch
        || order.state == ReplenishmentOrderState::Queued
        || order.state == ReplenishmentOrderState::Running;
}

bool hasNotUnloadedOrder(const ShortageRuntimeState &state, int stationId)
{
    for (const ReplenishmentOrder &order : state.orders) {
        if (order.stationId == stationId && isNotUnloaded(order))
            return true;
    }
    return false;
}

bool isTerminal(ReplenishmentOrderState state)
{
    return state == ReplenishmentOrderState::Succeeded
        || state == ReplenishmentOrderState::FailedBeforeUnload
        || state == ReplenishmentOrderState::FailedAfterUnload
        || state == ReplenishmentOrderState::Canceled;
}

void removeWaiting(ShortageRuntimeState *state, int stationId)
{
    state->waitingStationIds.erase(
        std::remove(state->waitingStationIds.begin(), state->waitingStationIds.end(), stationId),
        state->waitingStationIds.end());
}

void cancelAwaitingOrders(ShortageRuntimeState *state, int stationId, const QString &reasonZh)
{
    for (ReplenishmentOrder &order : state->orders) {
        if (order.stationId == stationId
            && order.state == ReplenishmentOrderState::AwaitingDispatch) {
            // 尚未进入主 FIFO 的补料单可安全取消；Queued/Running 不在本任务重排或删除。
            order.state = ReplenishmentOrderState::Canceled;
            order.lastReasonZh = reasonZh;
        }
    }
}

bool factMatchesOrder(const ReplenishmentOrder &order, const TaskFact &fact)
{
    return order.orderNo == fact.replenishmentOrderNo
        && order.taskId == fact.taskId
        && order.taskId != 0
        && order.stationId == fact.stationId
        && order.origin == fact.origin;
}

bool sameOrders(const QList<ReplenishmentOrder> &left, const QList<ReplenishmentOrder> &right)
{
    if (left.size() != right.size())
        return false;
    for (qsizetype i = 0; i < left.size(); ++i) {
        const ReplenishmentOrder &a = left[i];
        const ReplenishmentOrder &b = right[i];
        if (a.orderNo != b.orderNo
            || a.stationId != b.stationId
            || a.origin != b.origin
            || a.state != b.state
            || a.taskId != b.taskId
            || a.unloadAccounted != b.unloadAccounted
            || a.createdAtUtc != b.createdAtUtc
            || a.lastReasonZh != b.lastReasonZh) {
            return false;
        }
    }
    return true;
}

bool sameStations(const QList<ShortageStationRuntime> &left,
                  const QList<ShortageStationRuntime> &right)
{
    if (left.size() != right.size())
        return false;
    for (qsizetype i = 0; i < left.size(); ++i) {
        const ShortageStationRuntime &a = left[i];
        const ShortageStationRuntime &b = right[i];
        if (a.stationId != b.stationId
            || a.stock != b.stock
            || a.firstLowAtUtc != b.firstLowAtUtc
            || a.consecutivePreUnloadFailures != b.consecutivePreUnloadFailures
            || a.automaticPaused != b.automaticPaused
            || a.pauseReasonZh != b.pauseReasonZh) {
            return false;
        }
    }
    return true;
}

} // namespace

ReplenishmentPlanner::ReplenishmentPlanner(ShortageConfiguration configuration)
    : m_configuration(std::move(configuration))
{
}

PlannerApplyResult ReplenishmentPlanner::reevaluate(ShortageRuntimeState *state,
                                                    const QDateTime &nowUtc)
{
    if (state == nullptr)
        return plannerFail(QStringLiteral("补料重评估失败：state为空，处理动作=不发布"));

    ShortageRuntimeState work = *state;

    for (ShortageStationRuntime &station : work.stations) {
        const ShortageStationConfig *config =
            configFor(m_configuration, work.product, station.stationId);
        if (config == nullptr)
            return plannerFail(QStringLiteral("补料重评估失败：stationId=%1 缺少配置")
                                   .arg(station.stationId),
                               true);

        const bool low = config->enabled && !station.automaticPaused
            && station.stock < config->minimumStock;
        if (low) {
            if (!work.waitingStationIds.contains(station.stationId))
                work.waitingStationIds.append(station.stationId);
            if (!station.firstLowAtUtc.isValid())
                station.firstLowAtUtc = nowUtc.toUTC();
        } else if (work.activeStationId != station.stationId) {
            removeWaiting(&work, station.stationId);
            station.firstLowAtUtc = QDateTime();
        }

        // 活动工位只在达到高位、被暂停或系统严重锁定时释放，避免被后续低库存工位抢占。
        if (work.activeStationId == station.stationId
            && (work.criticalLock || station.automaticPaused
                || station.stock >= config->maximumStock)) {
            if (station.stock >= config->maximumStock) {
                cancelAwaitingOrders(&work, station.stationId,
                                     QStringLiteral("库存达到最高位，取消尚未派发的补料单"));
            }
            work.activeStationId = 0;
            removeWaiting(&work, station.stationId);
            station.firstLowAtUtc = QDateTime();
        }
    }

    std::sort(work.waitingStationIds.begin(), work.waitingStationIds.end(),
              [&work](int left, int right) {
                  const ShortageStationRuntime *leftStation = runtimeStation(work, left);
                  const ShortageStationRuntime *rightStation = runtimeStation(work, right);
                  if (leftStation == nullptr || rightStation == nullptr)
                      return left < right;
                  if (leftStation->firstLowAtUtc != rightStation->firstLowAtUtc)
                      return leftStation->firstLowAtUtc < rightStation->firstLowAtUtc;
                  return left < right;
              });
    work.waitingStationIds.erase(std::unique(work.waitingStationIds.begin(),
                                             work.waitingStationIds.end()),
                                 work.waitingStationIds.end());

    if (work.activeStationId == 0 && !work.criticalLock && !work.waitingStationIds.isEmpty())
        work.activeStationId = work.waitingStationIds.first();

    if (work.activeStationId != 0 && !hasNotUnloadedOrder(work, work.activeStationId)) {
        const ShortageStationConfig *config =
            configFor(m_configuration, work.product, work.activeStationId);
        const ShortageStationRuntime *station = runtimeStation(work, work.activeStationId);
        if (config != nullptr && station != nullptr && station->stock < config->maximumStock) {
            ReplenishmentOrder order;
            order.orderNo = work.nextReplenishmentOrderNo++;
            order.stationId = work.activeStationId;
            order.origin = ReplenishmentOrigin::Automatic;
            order.state = ReplenishmentOrderState::AwaitingDispatch;
            order.createdAtUtc = nowUtc.toUTC();
            work.orders.append(order);
        }
    }

    const bool changed = work.waitingStationIds != state->waitingStationIds
        || work.activeStationId != state->activeStationId
        || !sameStations(work.stations, state->stations)
        || !sameOrders(work.orders, state->orders)
        || work.nextReplenishmentOrderNo != state->nextReplenishmentOrderNo;
    *state = work;
    return plannerOk(changed, QStringLiteral("补料重评估完成：稳定等待顺序并保持非抢占"));
}

PlannerApplyResult ReplenishmentPlanner::requestManualBox(ShortageRuntimeState *state,
                                                          int stationId,
                                                          bool highStockRiskConfirmed,
                                                          const QDateTime &nowUtc)
{
    if (state == nullptr)
        return plannerFail(QStringLiteral("人工补料失败：state为空"));
    const ShortageStationConfig *config = configFor(m_configuration, state->product, stationId);
    const ShortageStationRuntime *runtime = runtimeStation(*state, stationId);
    if (config == nullptr || runtime == nullptr)
        return plannerFail(QStringLiteral("人工补料失败：stationId=%1 非法").arg(stationId), true);
    if (runtime->stock >= config->minimumStock && !highStockRiskConfirmed) {
        return plannerFail(QStringLiteral(
            "人工补料失败：stationId=%1 当前库存=%2 不低于最低位，需要高库存风险确认")
                               .arg(stationId)
                               .arg(runtime->stock));
    }
    if (hasNotUnloadedOrder(*state, stationId))
        return plannerFail(QStringLiteral("人工补料失败：stationId=%1 已有未倒料补料单").arg(stationId));

    ReplenishmentOrder order;
    order.orderNo = state->nextReplenishmentOrderNo++;
    order.stationId = stationId;
    order.origin = ReplenishmentOrigin::Manual;
    order.state = ReplenishmentOrderState::AwaitingDispatch;
    order.createdAtUtc = nowUtc.toUTC();
    state->orders.append(order);
    return plannerOk(true, QStringLiteral("人工补料已建单：orderNo=%1，stationId=%2")
                               .arg(order.orderNo)
                               .arg(stationId));
}

std::optional<ShortageDispatchRequest> ReplenishmentPlanner::nextDispatchRequest(
    const ShortageRuntimeState &state) const
{
    if (!state.initialized || !state.operatorConfirmedRestore)
        return std::nullopt;

    for (const ReplenishmentOrder &order : state.orders) {
        if (order.state != ReplenishmentOrderState::AwaitingDispatch)
            continue;
        if (state.criticalLock && order.origin != ReplenishmentOrigin::Manual)
            continue;
        for (const ReplenishmentOrder &other : state.orders) {
            if (other.orderNo != order.orderNo
                && other.stationId == order.stationId
                && isNotUnloaded(other)) {
                return std::nullopt;
            }
        }
        return ShortageDispatchRequest{order.orderNo, order.stationId, order.origin};
    }
    return std::nullopt;
}

PlannerApplyResult ReplenishmentPlanner::markDispatchResult(ShortageRuntimeState *state,
                                                            quint64 replenishmentOrderNo,
                                                            bool accepted,
                                                            quint64 taskId,
                                                            const QString &reasonZh)
{
    if (state == nullptr)
        return plannerFail(QStringLiteral("派单结果失败：state为空"));
    ReplenishmentOrder *order = orderByNo(state, replenishmentOrderNo);
    if (order == nullptr || order->state != ReplenishmentOrderState::AwaitingDispatch)
        return plannerFail(QStringLiteral("派单结果失败：orderNo=%1 未知或状态不等待")
                               .arg(replenishmentOrderNo),
                           true);
    if (!accepted) {
        order->lastReasonZh = reasonZh;
        return plannerOk(true, QStringLiteral("派单被拒收：orderNo=%1，保留原单号等待重试")
                                   .arg(replenishmentOrderNo));
    }
    if (taskId == 0)
        return plannerFail(QStringLiteral("派单接受失败：orderNo=%1 taskId=0").arg(replenishmentOrderNo),
                           true);
    order->taskId = taskId;
    order->state = ReplenishmentOrderState::Queued;
    order->lastReasonZh.clear();
    return plannerOk(true, QStringLiteral("派单已绑定：orderNo=%1，taskId=%2")
                               .arg(replenishmentOrderNo)
                               .arg(taskId));
}

PlannerApplyResult ReplenishmentPlanner::markTaskStarted(ShortageRuntimeState *state,
                                                         const TaskFact &fact)
{
    if (state == nullptr)
        return plannerFail(QStringLiteral("任务开始失败：state为空"));
    ReplenishmentOrder *order = orderByNo(state, fact.replenishmentOrderNo);
    if (order == nullptr || order->state != ReplenishmentOrderState::Queued
        || !factMatchesOrder(*order, fact)) {
        return plannerFail(QStringLiteral(
                               "任务开始校验失败：orderNo=%1，taskId=%2，stationId=%3，处理动作=严重锁定且不修改补料单")
                               .arg(fact.replenishmentOrderNo)
                               .arg(fact.taskId)
                               .arg(fact.stationId),
                           true);
    }
    order->state = ReplenishmentOrderState::Running;
    return plannerOk(true, QStringLiteral("任务开始：orderNo=%1，taskId=%2")
                               .arg(fact.replenishmentOrderNo)
                               .arg(fact.taskId));
}

PlannerApplyResult ReplenishmentPlanner::validateUnloadFact(const ShortageRuntimeState &state,
                                                            const TaskFact &fact) const
{
    const ReplenishmentOrder *order = orderByNo(state, fact.replenishmentOrderNo);
    if (order == nullptr || !factMatchesOrder(*order, fact)
        || order->state != ReplenishmentOrderState::Running
        || order->unloadAccounted) {
        return plannerFail(QStringLiteral(
                               "倒料事实校验失败：orderNo=%1，taskId=%2，stationId=%3，处理动作=严重锁定且不加库存")
                               .arg(fact.replenishmentOrderNo)
                               .arg(fact.taskId)
                               .arg(fact.stationId),
                           true);
    }
    return plannerOk(false, QStringLiteral("倒料事实校验通过：orderNo=%1，taskId=%2")
                                .arg(fact.replenishmentOrderNo)
                                .arg(fact.taskId));
}

PlannerApplyResult ReplenishmentPlanner::markUnloaded(ShortageRuntimeState *state,
                                                      const TaskFact &fact)
{
    if (state == nullptr)
        return plannerFail(QStringLiteral("倒料标记失败：state为空"));
    ReplenishmentOrder *order = orderByNo(state, fact.replenishmentOrderNo);
    if (order == nullptr || !factMatchesOrder(*order, fact)
        || order->state != ReplenishmentOrderState::Running
        || order->unloadAccounted) {
        return plannerFail(QStringLiteral("倒料标记失败：orderNo=%1 重复或不匹配")
                               .arg(fact.replenishmentOrderNo),
                           true);
    }
    order->unloadAccounted = true;
    order->state = ReplenishmentOrderState::Unloaded;
    order->lastReasonZh = fact.reasonZh;
    if (ShortageStationRuntime *station = runtimeStation(state, order->stationId))
        station->consecutivePreUnloadFailures = 0;
    return plannerOk(true, QStringLiteral("倒料已标记入账：orderNo=%1，stationId=%2")
                               .arg(order->orderNo)
                               .arg(order->stationId));
}

PlannerApplyResult ReplenishmentPlanner::markTerminal(ShortageRuntimeState *state,
                                                      const TaskFact &fact,
                                                      int failureLimit)
{
    if (state == nullptr)
        return plannerFail(QStringLiteral("任务终态失败：state为空"));
    ReplenishmentOrder *order = orderByNo(state, fact.replenishmentOrderNo);
    if (order == nullptr || !factMatchesOrder(*order, fact) || isTerminal(order->state)) {
        return plannerFail(QStringLiteral("任务终态校验失败：orderNo=%1 不匹配")
                               .arg(fact.replenishmentOrderNo),
                           true);
    }

    ShortageStationRuntime *runtime = runtimeStation(state, order->stationId);
    if (runtime == nullptr)
        return plannerFail(QStringLiteral("任务终态失败：stationId=%1 缺失").arg(order->stationId),
                           true);

    // 倒料后的失败只记录准确终态，不回滚已经入账的一箱。
    if (order->unloadAccounted || order->state == ReplenishmentOrderState::Unloaded) {
        order->state = fact.kind == TaskFactKind::Succeeded
            ? ReplenishmentOrderState::Succeeded
            : ReplenishmentOrderState::FailedAfterUnload;
        runtime->consecutivePreUnloadFailures = 0;
    } else if (fact.kind == TaskFactKind::Canceled || fact.kind == TaskFactKind::SystemError) {
        // 倒料前取消/系统错误释放本工位占用，允许后续重新评估建单。
        order->state = ReplenishmentOrderState::Canceled;
        ++runtime->consecutivePreUnloadFailures;
    } else if (fact.kind == TaskFactKind::Succeeded) {
        return plannerFail(QStringLiteral("任务成功终态缺少倒料事实：orderNo=%1").arg(order->orderNo),
                           true);
    } else {
        order->state = ReplenishmentOrderState::FailedBeforeUnload;
        ++runtime->consecutivePreUnloadFailures;
    }

    order->lastReasonZh = fact.reasonZh;
    if (runtime->consecutivePreUnloadFailures >= failureLimit && failureLimit > 0) {
        runtime->automaticPaused = true;
        runtime->pauseReasonZh = QStringLiteral("连续%1次倒料前失败，已暂停本工位自动补料")
                                     .arg(runtime->consecutivePreUnloadFailures);
        removeWaiting(state, runtime->stationId);
    }
    if (state->activeStationId == order->stationId && !order->unloadAccounted)
        state->activeStationId = 0;
    return plannerOk(true, QStringLiteral("任务终态已记录：orderNo=%1，stationId=%2")
                               .arg(order->orderNo)
                               .arg(order->stationId));
}
