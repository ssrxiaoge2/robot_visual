#ifndef SHORTAGELEDGER_H
#define SHORTAGELEDGER_H

#include "shortagetypes.h"

/// 账本操作先在副本计算；ok=false 时 changed 必须为 false，正式状态保持原值。
struct LedgerApplyResult {
    bool ok = false;                 ///< 整个账本事务是否成功。
    bool changed = false;            ///< 库存、基线、候选或上下文是否发生变化。
    bool criticalLock = false;       ///< 溢出、重复事实或不可自动判定回退是否要求严重锁定。
    bool hasProductionDelta = false; ///< true 表示本次事务确认了可展示的正产量增量。
    qint64 productionDelta = 0;      ///< 本轮已入账扣减的产量增量；不设置任意“巨大增量”黄色阈值。
    QString messageZh;               ///< 包含规则、原值、事件值和处理动作。
};

class ShortageLedger final
{
public:
    explicit ShortageLedger(ShortageConfiguration configuration);

    /// 只有 siteIsConfirmedEmpty=true 才把 12 工位设为 0；否则不修改任何状态。
    LedgerApplyResult initializeZero(ShortageRuntimeState *state,
                                     bool siteIsConfirmedEmpty,
                                     const QDateTime &nowUtc);
    /// 应用采样前先在局部副本完成全部 64 位溢出校验，通过后一次替换正式状态。
    LedgerApplyResult applyStableSample(ShortageRuntimeState *state,
                                        const ShortageSample &sample);
    /// 只执行一箱库存事实；补料单/taskId/重复事件已由 Engine 在调用前校验。
    LedgerApplyResult recordUnloadedBox(ShortageRuntimeState *state,
                                        int stationId,
                                        ReplenishmentOrigin origin,
                                        const QDateTime &nowUtc);
    /// 校验恢复状态的 12 工位、基线和配置修订号，不修改传入状态。
    ShortageOperationResult validateRestoredState(
        const ShortageRuntimeState &state) const;

private:
    ShortageConfiguration m_configuration; ///< 值语义配置副本，构造后只读。
};

#endif // SHORTAGELEDGER_H
