#ifndef SHORTAGETYPES_H
#define SHORTAGETYPES_H

#include <QDateTime>
#include <QJsonObject>
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

/// 正式和独立测试使用不同目录前缀，禁止调用方自由拼接文件名。
enum class ShortageStateNamespace {
    Production,    ///< 正式账本、正式任务和正式审计。
    StandaloneTest ///< 独立测试状态，不得影响 Production。
};

/// 一条聚合审计事件；details 保存确定字段，不保存不可解析的整段日志文本。
struct ShortageAuditEvent {
    quint64 sequence = 0;      ///< 状态命名空间内单调递增流水号。
    QDateTime occurredAtUtc;   ///< UTC 事件时间，用于排序和现场追踪。
    QString eventType;         ///< 稳定英文键，例如 sample_applied、box_unloaded。
    QString messageZh;         ///< 面向维护人员的完整中文说明。
    QJsonObject details;       ///< 工位、原值、新值、taskId、补料单等结构化字段。
};

/// 单工位的正式账本状态；库存属于事实，等待/暂停属于计划恢复所需状态。
struct ShortageStationRuntime {
    int stationId = 0;               ///< 代码工位 1～12。
    qint64 stock = 0;                ///< 当前正式库存，允许为负数。
    QDateTime firstLowAtUtc;         ///< 首次跌破最低位时间；不在等待表时为空。
    int consecutivePreUnloadFailures = 0; ///< 连续倒料前失败次数。
    bool automaticPaused = false;    ///< true 时只阻止该工位自动补料。
    QString pauseReasonZh;           ///< 暂停原因和维护处理提示。
};

/// actualQty 基线及两轮清零候选；恢复回退时不能复用普通清零分支。
struct ActualQtyRuntime {
    bool hasBaseline = false;        ///< false 时首个有效样本只建立基线。
    qint64 baseline = 0;             ///< 最近一次已经入账扣减的累计产量。
    bool hasResetCandidate = false;  ///< true 表示观察到一次小于 baseline 的值。
    qint64 resetCandidate = 0;       ///< 等待下一有效轮确认的较小值。
    bool interrupted = false;        ///< 通信中断后恢复值回退必须联系维护。
};

inline constexpr int kShortageStateFormatVersion = 1; ///< JSON 格式版本，不兼容版本拒绝加载。

/// 可从快照和流水完整恢复的全部正式/测试运行状态。
struct ShortageRuntimeState {
    int formatVersion = kShortageStateFormatVersion; ///< 序列化版本。
    quint64 configurationRevision = 0;               ///< 建账/保存时配置修订号。
    bool initialized = false;                        ///< 是否已经安全恢复或从现场清空建账。
    bool operatorConfirmedRestore = false;           ///< 启动后人工确认前不得自动派单。
    bool hasStableContext = false;                   ///< 当前产品/模式是否已两轮稳定。
    ProductModel product = ProductModel::Model88;    ///< 当前稳定产品。
    ProductionMode mode = ProductionMode::LeftRight;///< 当前稳定模式。
    bool hasPendingContext = false;                  ///< 换型已确认但旧任务尚未排空。
    ProductModel pendingProduct = ProductModel::Model88; ///< 待切换产品。
    ProductionMode pendingMode = ProductionMode::LeftRight; ///< 待切换模式。
    bool hasPendingActualQty = false;                ///< 换型等待期间是否保存了最新产量。
    qint64 pendingActualQty = 0;                     ///< 旧任务排空后按新用量一次补扣到该值。
    ActualQtyRuntime actualQty;                       ///< 产量基线和清零候选。
    QList<ShortageStationRuntime> stations;           ///< 恰好 12 个代码工位状态。
    QList<int> waitingStationIds;                     ///< 按首次时间/工位号排好的等待表。
    int activeStationId = 0;                          ///< 0 表示无活动计划。
    QList<ReplenishmentOrder> orders;                 ///< 恢复和幂等所需未完成/近期补料单。
    quint64 nextReplenishmentOrderNo = 1;             ///< 下一个正式补料单号。
    quint64 nextAuditSequence = 1;                    ///< 下一条流水号。
    bool criticalLock = false;                        ///< true 时停止所有新自动派单。
    QString criticalReasonZh;                         ///< 严重锁定原因和处理动作。
    QDateTime lastSavedAtUtc;                         ///< 最近完整快照时间。
};

/// 恢复来源用于 UI 摘要和审计，不能只返回一个 bool。
enum class ShortageRestoreSource {
    None,    ///< 没有任何状态文件，可在现场清空确认后新建。
    Main,    ///< 主快照校验通过。
    Backup,  ///< 主快照失败，使用备份。
    Journal  ///< 在有效快照后重放流水得到最终状态。
};

struct ShortageStateLoadResult {
    bool ok = false;                         ///< false 时 state 不得进入自动模式。
    bool stateFound = false;                 ///< None 且无文件时为 false。
    bool requiresMaintenance = false;        ///< 三份损坏或语义不确定时为 true。
    ShortageRestoreSource source = ShortageRestoreSource::None; ///< 实际恢复来源。
    ShortageRuntimeState state;              ///< 仅 ok=true 时可使用。
    QString messageZh;                       ///< 安全恢复摘要或联系维护人员原因。
};

/// Planner 只请求追加一箱，不暴露 FIFO 插队、重排或删除能力。
struct ShortageDispatchRequest {
    quint64 replenishmentOrderNo = 0; ///< 跨重启幂等编号；拒收重试不得改变。
    int stationId = 0;                ///< 目标代码工位 1～12。
    ReplenishmentOrigin origin = ReplenishmentOrigin::Automatic; ///< 自动或人工。
};

/// LineManager 任务信号翻译后的领域事实；Engine 不依赖 UI Task 结构。
enum class TaskFactKind {
    Started,          ///< 已从 FIFO 取出并开始执行。
    MaterialUnloaded, ///< 唯一倒料动作已经完成。
    Succeeded,        ///< 全流程成功终态。
    Failed,           ///< 任务失败；是否倒料由补料单状态判断。
    Canceled,         ///< Stop/清队列取消。
    SystemError       ///< 设备级错误进入 Error。
};

struct TaskFact {
    TaskFactKind kind = TaskFactKind::Started; ///< 本次事实类型。
    quint64 replenishmentOrderNo = 0;          ///< 跨重启幂等键。
    quint64 taskId = 0;                        ///< 当前程序任务号。
    int stationId = 0;                         ///< 事实声称的代码工位。
    ReplenishmentOrigin origin = ReplenishmentOrigin::Automatic; ///< 正式来源。
    QDateTime occurredAtUtc;                   ///< 事实发生 UTC 时间。
    QString reasonZh;                          ///< 失败/取消/错误原因。
};

/// Engine 的统一结果；changed=false 时 UI 可以只追加日志而不重绘整表。
struct ShortageEngineResult {
    bool ok = false;          ///< 事务是否完整成功。
    bool changed = false;     ///< 正式运行状态是否改变。
    bool criticalLock = false;///< 是否已停止新的自动意图。
    bool hasProductionDelta = false; ///< 账本确认正增量时透传给 UI 和结构化日志。
    qint64 productionDelta = 0;      ///< 本轮实际用于扣减的产量增量，不做未定义上限判断。
    QString messageZh;        ///< 完整中文结果。
};

/// UI 只读快照；不暴露可修改的 Ledger/Planner 引用。
struct ShortageUiSnapshot {
    ShortageInputSource inputSource = ShortageInputSource::Mock; ///< 当前二选一来源。
    ShortageCommunicationState communication = ShortageCommunicationState::Stopped;
    ShortageRuntimeState runtime;              ///< 复制后的账本/计划摘要。
    bool hasLastProductionDelta = false;        ///< 最近一次已确认正增量是否可显示。
    qint64 lastProductionDelta = 0;             ///< 最近一次已入账扣减增量；每次有效增量均更新日志和该字段。
    QString summaryLine1Zh;                    ///< 产品/模式/actualQty/通信。
    QString summaryLine2Zh;                    ///< 活动工位/等待数/账本报警。
};

/// Planner 操作结果；ok=false 时传入状态副本不得再发布为正式状态。
struct PlannerApplyResult {
    bool ok = false;          ///< 计划状态是否完整更新。
    bool changed = false;     ///< 等待表、活动工位、补料单或失败计数是否变化。
    bool criticalLock = false;///< 未知/重复/错工位事实是否触发严重锁定。
    QString messageZh;        ///< 包含补料单、taskId、工位和处理动作。
};

#endif // SHORTAGETYPES_H
