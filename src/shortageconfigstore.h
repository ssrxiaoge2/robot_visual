#ifndef SHORTAGECONFIGSTORE_H
#define SHORTAGECONFIGSTORE_H

#include "shortagetypes.h"

#include <QString>

/// 缺料配置仓库只负责 36 条 Sheet3 配置与共享参数的校验、加载和原子保存。
class ShortageConfigStore final
{
public:
    /// 返回 3 个产品各 12 行的独立副本；运行期修改不会污染编译期默认表。
    static ShortageConfiguration sheet3Defaults();
    /// 返回全部字段错误；ok=false 时 message 是可直接展示的中文汇总。
    static ShortageOperationResult validate(const ShortageConfiguration &configuration);
    /// 五项条件全部为 true 才允许生产配置保存；失败消息逐项列出阻止原因。
    static ShortageOperationResult canEditConfiguration(
        const ShortageEditConditions &conditions);
    /// 先加载主配置，主文件缺失/损坏/非法时再读同目录备份；两者都失败不覆盖传入内存值。
    static ShortageOperationResult load(const QString &filePath,
                                        ShortageConfiguration *configuration);
    /// 先校验；将当前有效主文件原子写入备份后，再用 QSaveFile 原子替换主文件。
    static ShortageOperationResult save(const QString &filePath,
                                        const ShortageConfiguration &configuration);
    /// 只导出副本，不改变正式配置路径和当前内存配置。
    static ShortageOperationResult exportCopy(const QString &targetPath,
                                              const ShortageConfiguration &configuration);
};

#endif // SHORTAGECONFIGSTORE_H
