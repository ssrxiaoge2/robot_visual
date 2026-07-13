#ifndef REPLENISHMENTPLANNER_H
#define REPLENISHMENTPLANNER_H

#include "shortagetypes.h"

#include <optional>

class ReplenishmentPlanner final
{
public:
    explicit ReplenishmentPlanner(ShortageConfiguration configuration);

    /// 根据同一事务副本中的库存重建等待顺序；已有 firstLowAtUtc 不得刷新。
    PlannerApplyResult reevaluate(ShortageRuntimeState *state,
                                  const QDateTime &nowUtc);
    /// 高位人工任务需要风险确认；成功后在 state->orders 中只新增一箱。
    PlannerApplyResult requestManualBox(ShortageRuntimeState *state,
                                        int stationId,
                                        bool highStockRiskConfirmed,
                                        const QDateTime &nowUtc);
    /// 返回 AwaitingDispatch 补料单副本；不存在可派单时返回空且不改变状态。
    std::optional<ShortageDispatchRequest> nextDispatchRequest(
        const ShortageRuntimeState &state) const;
    /// accepted=false 只记录拒收原因；true 时绑定非 0 taskId 并标记 Queued。
    PlannerApplyResult markDispatchResult(ShortageRuntimeState *state,
                                          quint64 replenishmentOrderNo,
                                          bool accepted,
                                          quint64 taskId,
                                          const QString &reasonZh);
    /// 校验并把 Queued 标为 Running。
    PlannerApplyResult markTaskStarted(ShortageRuntimeState *state,
                                       const TaskFact &fact);
    /// 只校验倒料事实，供 Engine 在加库存前保证补料单/taskId/工位完全匹配。
    PlannerApplyResult validateUnloadFact(const ShortageRuntimeState &state,
                                          const TaskFact &fact) const;
    /// Engine 完成库存加箱后才调用，设置 unloadAccounted 和 Unloaded。
    PlannerApplyResult markUnloaded(ShortageRuntimeState *state,
                                    const TaskFact &fact);
    /// 按是否已经 Unloaded 写准确终态，并执行连续倒料前失败保护。
    PlannerApplyResult markTerminal(ShortageRuntimeState *state,
                                    const TaskFact &fact,
                                    int failureLimit);

private:
    ShortageConfiguration m_configuration; ///< 值语义配置副本，只供阈值/箱量查询。
};

#endif // REPLENISHMENTPLANNER_H
