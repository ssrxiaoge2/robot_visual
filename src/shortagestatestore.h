#ifndef SHORTAGESTATESTORE_H
#define SHORTAGESTATESTORE_H

#include "shortagetypes.h"

#include <QString>

/// 缺料运行状态仓库拥有快照、备份和流水文件名；调用方只选择正式/测试命名空间。
class ShortageStateStore final
{
public:
    /// baseDirectory 通常为可执行文件目录下 shortage-state；单元测试传入 QTemporaryDir。
    explicit ShortageStateStore(QString baseDirectory,
                                ShortageStateNamespace stateNamespace);

    /// 先尝试主快照+连续流水，再尝试备份快照+连续流水；任一流水缺口都会阻止自动模式。
    ShortageStateLoadResult load() const;
    /// 关键事实先追加流水，再用 QSaveFile 写快照；失败时只返回中文原因，不回滚调用方内存事实。
    ShortageOperationResult saveCritical(const ShortageRuntimeState &state,
                                          const ShortageAuditEvent &event);
    /// 非关键变化只追加聚合流水，并且距上次快照至少 60 秒才刷新快照。
    ShortageOperationResult savePeriodic(const ShortageRuntimeState &state,
                                          const ShortageAuditEvent &event,
                                          const QDateTime &nowUtc);
    /// 异常修正前复制一份带时间戳的维护备份；备份归 Store 管理生命周期。
    ShortageOperationResult backupBeforeMaintenance(const ShortageRuntimeState &state);

private:
    QString baseDirectory_;                 ///< 仓库根目录；构造后不再改变。
    ShortageStateNamespace stateNamespace_; ///< 固定命名空间，防止正式/测试文件串用。
};

#endif // SHORTAGESTATESTORE_H
