#include "chargesettingstransaction.h"

#include "autochargecoordinator.h"
#include "chargepilecontroller.h"

bool applyChargeSettingsTransaction(
    const ChargeSettingsTransactionTargets &targets,
    const ChargeSettings &candidate,
    QString *error)
{
    if (error)
        error->clear();
    if (targets.settingsPath.trimmed().isEmpty()
        || !targets.controller
        || !targets.coordinator
        || !targets.deviceManagerSnapshot) {
        if (error)
            *error = QStringLiteral("充电参数事务目标不完整");
        return false;
    }

    QString rejection;
    if (!targets.controller->canApplySettings(candidate, &rejection)) {
        if (error)
            *error = rejection;
        return false;
    }

    const ChargeSettings previous = *targets.deviceManagerSnapshot;
    if (!saveChargeSettings(targets.settingsPath, candidate, error))
        return false;

    // canApplySettings 与 applySettings 在同一 UI 线程连续执行，正常情况下状态
    // 不会在两者之间变化；仍保留显式失败处理，避免未来引入重入后产生部分提交。
    QString applyError;
    if (!targets.controller->applySettings(candidate, &applyError)) {
        QString rollbackError;
        const bool rollbackOk =
            saveChargeSettings(targets.settingsPath, previous, &rollbackError);
        if (error) {
            *error = rollbackOk
                         ? QStringLiteral("控制器意外拒绝候选参数，磁盘已回滚：%1")
                               .arg(applyError)
                         : QStringLiteral("控制器拒绝候选参数且磁盘回滚失败：%1；%2")
                               .arg(applyError, rollbackError);
        }
        return false;
    }

    targets.coordinator->applySettings(candidate);
    *targets.deviceManagerSnapshot = candidate;
    return true;
}
