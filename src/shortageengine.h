#ifndef SHORTAGEENGINE_H
#define SHORTAGEENGINE_H

#include "replenishmentplanner.h"
#include "shortageledger.h"
#include "shortagestatestore.h"

class ShortageEngine final
{
public:
    /// stateStore 为非拥有指针，调用方必须保证其生命周期覆盖 Engine。
    ShortageEngine(ShortageConfiguration configuration,
                   ShortageStateStore *stateStore);

    /// 全量校验恢复状态；失败或需维护时不安装传入 state，并设置恢复锁定。
    ShortageEngineResult installRestoredState(const ShortageStateLoadResult &loadResult,
                                              const QDateTime &nowUtc);
    /// 新账本只有现场清空确认后才能建立；成功后仍等待操作员确认/LineManager Start。
    ShortageEngineResult initializeZero(bool siteIsConfirmedEmpty,
                                        const QDateTime &nowUtc);
    /// 启动恢复摘要确认；accepted=false 保持自动派单门禁关闭。
    ShortageEngineResult confirmRestoredState(bool accepted,
                                              const QDateTime &nowUtc);
    /// 采样事务：账本局部计算 -> Planner 重评估 -> 一条聚合流水 -> 发布新快照。
    ShortageEngineResult applyStableSample(const ShortageSample &sample);
    /// oldTasksDrained=true 时激活待切换上下文，并按新用量从旧基线补扣到最新待处理产量。
    ShortageEngineResult activatePendingContextIfDrained(bool oldTasksDrained,
                                                         const QDateTime &nowUtc);
    /// 人工补一箱也生成正式补料单；高位时必须 highStockRiskConfirmed=true。
    ShortageEngineResult requestManualBox(int stationId,
                                          bool highStockRiskConfirmed,
                                          const QDateTime &nowUtc);
    /// 返回当前可派的一箱；没有条件时返回 std::nullopt，不偷偷创建 taskId。
    std::optional<ShortageDispatchRequest> nextDispatchRequest();
    /// 主调度接受后绑定 taskId；accepted=false 保留同一补料单等待重试。
    ShortageEngineResult recordDispatchResult(quint64 replenishmentOrderNo,
                                               bool accepted,
                                               quint64 taskId,
                                               const QString &reason);
    /// 任务从 FIFO 取出时标记 Running；未知绑定立即严重锁定。
    ShortageEngineResult recordTaskStarted(const TaskFact &fact);
    /// 唯一倒料事务：先校验补料单/taskId/工位，再加箱、标记 Unloaded、立即持久化。
    ShortageEngineResult recordMaterialUnloaded(const TaskFact &fact);
    /// 终态区分倒料前后，更新失败次数/暂停，绝不回滚已经入账的一箱。
    ShortageEngineResult recordTaskTerminal(const TaskFact &fact);
    /// 只允许在生产协调器复核全部门禁并完成维护备份后调用。
    ShortageEngineResult applyMaintenanceCorrection(
        const ShortageMaintenanceCorrection &correction,
        const QDateTime &nowUtc);
    /// 只读接口供 UI 确认和状态刷新；调用方不得 const_cast 修改。
    const ShortageConfiguration &configuration() const;
    const ShortageRuntimeState &state() const;
    /// true 表示启动恢复失败或需要维护，生产 Live 采样和派单入口必须保持关闭。
    bool restoreLocked() const { return m_restoreLocked; }

private:
    ShortageConfiguration m_configuration; ///< Engine 使用的配置修订副本。
    ShortageStateStore *m_stateStore = nullptr; ///< 非拥有指针，生命周期由调用方保证。
    ShortageLedger m_ledger;                    ///< 只操作 m_state 事务副本的账本规则。
    ReplenishmentPlanner m_planner;             ///< 只操作 m_state 事务副本的计划规则。
    ShortageRuntimeState m_state;                ///< 本进程唯一权威运行状态。
    bool m_restoreInstalled = false;             ///< true 表示已安全安装恢复状态，但仍可能等待操作员确认。
    bool m_restoreLocked = false;                ///< true 表示恢复结果不可安全使用，只允许维护流程。
};

#endif // SHORTAGEENGINE_H
