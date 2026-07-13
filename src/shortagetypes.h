#ifndef SHORTAGETYPES_H
#define SHORTAGETYPES_H

#include <QDateTime>
#include <QList>
#include <QString>

/// 客户三种产品；枚举值只表达业务结果，不直接保存 PLC 地址。
enum class ProductModel {
    Model88,  ///< PLC L71 单独为 true 时的 88 产品。
    Model88R, ///< PLC L72 单独为 true 时的 88R 产品。
    Model92   ///< PLC L73 单独为 true 时的 92 产品。
};

/// 客户三种生产方式；与 L68/L69/L1998 的映射由采样层唯一维护。
enum class ProductionMode {
    LeftRight, ///< L68：L/R，左右生产。
    LeftOnly,  ///< L69：L/L，目前只有左。
    RightOnly  ///< L1998：R/H，只有右。
};

/// 正式补料单来源；模拟任务没有正式补料单，因此不在此枚举中。
enum class ReplenishmentOrigin {
    Automatic, ///< 正式账本自动计划生成。
    Manual     ///< 人工二次确认生成，仍进入同一账本和审计。
};

/// 主调度缺料来源开关；默认 Mock，任一时刻只能激活一个值。
enum class ShortageInputSource {
    Mock, ///< 保持现有模拟缺料按钮行为，正式采样停止。
    Live  ///< 启动正式采样和自动计划，模拟按钮改为人工补料确认。
};

/// 采样通信状态只表达可用性，不直接修改库存。
enum class ShortageCommunicationState {
    Stopped,             ///< 尚未启动或已人工停止。
    Sampling,            ///< 正在等待当前 roundId 的四组响应。
    Interrupted,         ///< 当前轮失败但尚未达到红色报警时间。
    Alarm,               ///< 连续失败已经达到配置报警时间。
    RecoveryNeedsReview  ///< 恢复值小于旧基线，必须联系维护人员。
};

/// 正式一箱补料单生命周期；Unloaded 后不得因后续失败回滚库存。
enum class ReplenishmentOrderState {
    AwaitingDispatch,  ///< 等待 LineManager 接受，拒收重试仍使用同一补料单号。
    Queued,            ///< 已绑定当前程序 taskId 并追加 FIFO。
    Running,           ///< 原有主流程已经开始执行。
    Unloaded,          ///< 唯一倒料事实已经入账。
    Succeeded,         ///< 倒料和全部设备收尾成功。
    FailedBeforeUnload,///< 倒料前失败，本箱未入账。
    FailedAfterUnload, ///< 倒料后失败，已入账的一箱保留。
    Canceled           ///< Stop 或系统错误在倒料前取消。
};

/// 一个产品下一个代码工位的完整 Sheet3 配置；三列用量同处一行。
struct ShortageStationConfig {
    ProductModel product = ProductModel::Model88; ///< 所属产品，决定 36 条中的分组。
    int stationId = 0;                            ///< 上位机代码工位，合法范围 1～12。
    QString temporaryNo;                          ///< Sheet3 临时 NO，可为数字文本或 T。
    QString sitePosition;                         ///< 现场位置文本，允许保存“暂未确定”。
    QString partNumber;                           ///< 现场物料品号；启用时不能为空。
    bool enabled = true;                          ///< false 时该产品下不扣料、不自动补料。
    qint64 boxQuantity = 0;                       ///< 有效倒料一次增加的 ea 数量。
    qint64 minimumStock = 0;                      ///< 库存严格小于此值才触发。
    qint64 maximumStock = 0;                      ///< 库存达到或超过此值即停止。
    qint64 usageLeftRight = 0;                    ///< L/R 每个 actualQty 的消耗。
    qint64 usageLeftOnly = 0;                     ///< L/L 每个 actualQty 的消耗。
    qint64 usageRightOnly = 0;                    ///< R/H 每个 actualQty 的消耗。
};

/// 真实通信、定时采样和工位保护参数，单位全部写入字段名或注释。
struct ShortageParameters {
    QString liveMesDayEndpoint = QStringLiteral(
        "http://192.168.115.228:5084/api/MesData/day"); ///< 真实缺料唯一 MES 地址，不读取已删除的 .229 旧配置。
    int sampleIntervalSeconds = 15;      ///< 两轮启动间隔，合法 5～300 秒。
    int roundTimeoutSeconds = 5;         ///< 单轮等待上限，合法 1～30 秒且小于间隔。
    int communicationAlarmMinutes = 10;  ///< 连续异常达到该分钟数转红色报警。
    int preUnloadFailureLimit = 3;       ///< 同工位连续倒料前失败暂停阈值。
};

/// 可保存的完整配置；ShortageConfigStore::save() 正式保存成功时持久化 revision + 1。
struct ShortageConfiguration {
    QList<ShortageStationConfig> stations; ///< 36 条独立产品/工位记录。
    ShortageParameters parameters;         ///< 全产品共享的通信和保护参数。
    quint64 revision = 1;                  ///< 当前内存修订号；保存接口不修改入参。
};

/// 所有无异常抛出的业务接口统一返回中文结果，失败时不得部分修改状态。
struct ShortageOperationResult {
    bool ok = false;       ///< true 表示整个操作已经完成。
    QString messageZh;     ///< 成功摘要或包含字段/原值/原因/处理动作的失败说明。
};

/// 配置保存的五项运行门禁；UI 和业务层都调用同一个判定函数。
struct ShortageEditConditions {
    bool standaloneTestStopped = true; ///< 独立测试已停止。
    bool liveSamplingStopped = true;   ///< 正式采样已停止。
    bool lineStopped = true;           ///< LineManager 不在 Running/ReturningHome。
    bool currentTaskEmpty = true;      ///< 当前执行任务为空。
    bool fifoEmpty = true;             ///< Pending FIFO 为空。
};

/// 异常库存修正是领域命令而不是 Dialog 私有类型，生产协调器会再次校验全部门禁。
struct ShortageMaintenanceCorrection {
    int stationId = 0;          ///< 本次唯一目标工位。
    qint64 oldStock = 0;        ///< 打开窗口时的账本值，用于并发校验。
    qint64 newStock = 0;        ///< 维护人员确认的新库存。
    QString reason;             ///< 非空原因，写入审计流水。
    QString typedStationId;     ///< 必须与 stationId 的十进制文本完全一致。
};

/// 一轮已经完成三选一和稳定确认的采样，才能交给账本。
struct ShortageSample {
    quint64 roundId = 0;                         ///< 采样层单调轮次号。
    ProductModel product = ProductModel::Model88;///< 本轮稳定产品。
    ProductionMode mode = ProductionMode::LeftRight; ///< 本轮稳定模式。
    qint64 actualQty = 0;                        ///< MES 64 位累计产量，必须非负。
    QDateTime capturedAtUtc;                     ///< 完整轮次完成 UTC 时间。
    bool recoveredAfterInterruption = false;     ///< true 时禁止把回退值自动当作日清零。
};

/// 跨程序重启的正式补料单；程序 taskId 只是一段运行期绑定。
struct ReplenishmentOrder {
    quint64 orderNo = 0;                         ///< 正式命名空间内单调编号。
    int stationId = 0;                           ///< 目标代码工位。
    ReplenishmentOrigin origin = ReplenishmentOrigin::Automatic; ///< 自动或人工。
    ReplenishmentOrderState state = ReplenishmentOrderState::AwaitingDispatch;
    quint64 taskId = 0;                          ///< 当前运行期绑定，未入队为 0。
    bool unloadAccounted = false;                ///< true 后重复倒料不能再次加箱。
    QDateTime createdAtUtc;                      ///< 建单 UTC 时间。
    QString lastReasonZh;                        ///< 最近拒收/失败/终态原因。
};

#endif // SHORTAGETYPES_H
