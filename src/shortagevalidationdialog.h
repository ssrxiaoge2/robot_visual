#ifndef SHORTAGEVALIDATIONDIALOG_H
#define SHORTAGEVALIDATIONDIALOG_H

#include "shortagetestcontroller.h"

#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>

class QLabel;
class QListWidget;
class QPushButton;
class QTextEdit;
class QPlainTextEdit;

/// 单项验证的进度和结论；自动结果与人工现场确认严格分开。
enum class ShortageValidationStatus {
    NotStarted,             ///< 尚未执行任何步骤。
    InProgress,             ///< 已开始且仍有步骤待执行。
    PassedAutomatically,    ///< 只读快照足以证明结果通过。
    FailedAutomatically,    ///< 只读快照与通过条件不一致。
    WaitingManualEvidence,  ///< 涉及真实通信、窗口或硬件隔离，等待人工核对。
    PassedByOperator        ///< 操作员已经根据现场证据确认通过。
};

/// 向导允许调用的测试控制器公开动作；不包含正式账本、FIFO 或硬件命令。
enum class ShortageValidationAction {
    ShowInstruction,     ///< 只显示人工操作说明，不调用业务接口。
    ClearTestState,      ///< 清空 StandaloneTest 文件并重建测试逻辑对象。
    InitializeZero,      ///< 明确确认测试现场从 0 建账。
    SelectManualSource,  ///< 切换手工源，不访问真实系统。
    SelectFieldSource,   ///< 切换现场源，但不自动启动采样。
    ApplyManualSample,   ///< 使用步骤携带的产品、模式和 actualQty 提交样本。
    StartFieldSampling,  ///< 经正式 Live 停止门禁后启动测试现场采样。
    StopFieldSampling,   ///< 停止测试现场采样并屏蔽迟到样本。
    DispatchRejected,    ///< 模拟主调度拒收当前补料单。
    DispatchAccepted,    ///< 模拟接受并生成测试专用 taskId。
    FailureBeforeUnload, ///< 模拟倒料前失败，不增加库存。
    MaterialUnloaded,    ///< 模拟唯一倒料事实并增加一箱。
    FailureAfterUnload,  ///< 模拟倒料后失败，保留已入账库存。
    TaskSucceeded,       ///< 模拟任务成功终态。
    ResendUnload,        ///< 重发最近倒料事实，验证幂等和严重锁定。
    SaveState,           ///< 记录测试状态保存证据。
    ReloadState,         ///< 从 StandaloneTest 文件加载并走恢复校验。
    SimulateRestart      ///< 重建测试运行对象并恢复，不接触 production-* 文件。
};

/// 一个可执行验证步骤；输入只在 ApplyManualSample 时生效。
struct ShortageValidationStep {
    QString instructionZh; ///< 面向现场人员的当前操作。
    QString expectedZh;    ///< 执行后应观察到的状态。
    ShortageValidationAction action = ShortageValidationAction::ShowInstruction;
    ProductModel product = ProductModel::Model88;       ///< 手工样本产品。
    ProductionMode mode = ProductionMode::LeftRight;   ///< 手工样本模式。
    qint64 actualQty = 0;                               ///< 非负累计产量。
};

/// 固定验证项定义；步骤和通过条件来自已确认的中文设计文档。
struct ShortageValidationCase {
    QString id;                         ///< VT-01～VT-15 稳定编号。
    QString nameZh;                     ///< 列表显示名称。
    QList<ShortageValidationStep> steps;///< 按顺序执行的全部步骤。
    QString passCriteriaZh;             ///< 完整通过条件，不使用省略描述。
    bool requiresManualEvidence = false;///< true 时自动执行结束后仍等待人工确认。
};

/// 独立缺料验证控制台；只编排 StandaloneTest 公开动作，不持有正式账本/FIFO/硬件引用。
class ShortageValidationDialog final : public QDialog
{
    Q_OBJECT
public:
    /// testController 为非拥有指针，DeviceManager 保证其生命周期覆盖所有缺料窗口。
    explicit ShortageValidationDialog(ShortageTestController *testController,
                                      QWidget *parent = nullptr);
    /// 返回固定 VT-01～VT-15 定义的只读副本，供 UI 和 Qt Test 核对完整性。
    QList<ShortageValidationCase> validationCases() const { return m_cases; }

private:
    void buildUi();                              ///< 创建三栏和底部日志，不修改测试状态。
    void selectCase(int row);                    ///< 重置当前步骤显示，不自动执行业务动作。
    void executeNextStep();                      ///< 只分发 ShortageValidationAction 到控制器公开接口。
    void evaluateCurrentCase(const ShortageUiSnapshot &snapshot); ///< 依据只读证据判定可自动项目。
    bool evaluateManualLocalCriteria(const QString &caseId,
                                     QString *evidenceZh) const; ///< 人工项先核对本地可证明门禁。
    void confirmManualEvidence();                ///< 只允许 WaitingManualEvidence 转人工通过。
    void appendValidationLog(const QString &messageZh); ///< 追加带时间、编号和步骤的中文证据。
    /// 创建固定 VT-01～VT-15 定义；只生成编排数据，不读取或修改测试状态。
    static QList<ShortageValidationCase> createValidationCases();

    void refreshCaseDetails();                   ///< 刷新当前验证项、步骤和通过标准文本。
    void refreshStatusLabel();                   ///< 按当前状态刷新中文结论和人工按钮门禁。
    void refreshSnapshotEvidence(const ShortageUiSnapshot &snapshot); ///< 刷新右侧只读实际证据。
    QString currentCaseId() const;               ///< 返回当前 VT 编号，未选择时返回占位文本。
    static QString productText(ProductModel product); ///< 产品枚举转中文显示。
    static QString modeText(ProductionMode mode);      ///< 模式枚举转中文显示。
    static QString orderStateText(ReplenishmentOrderState state); ///< 补料单状态转中文显示。
    static QString actionText(ShortageValidationAction action);   ///< 验证动作转中文显示。

    ShortageTestController *m_testController = nullptr; ///< 非拥有；为空时只允许浏览步骤。
    QList<ShortageValidationCase> m_cases;              ///< Dialog 拥有的固定验证定义。
    ShortageUiSnapshot m_latestSnapshot;                ///< 最近一次控制器信号快照，保留增量证据。
    QList<ShortageUiSnapshot> m_currentCaseSnapshots;   ///< 当前项逐步快照，用于自动判定前后对比。
    QStringList m_currentCaseControllerLogs;            ///< 当前项控制器中文日志，用于本地证据核对。
    QStringList m_currentCaseRejections;                ///< 当前项控制器拒绝原因，用于误操作门禁核对。
    int m_currentCaseIndex = -1;                        ///< 当前验证项下标，-1 表示未选择。
    int m_currentStepIndex = 0;                         ///< 下一条待执行步骤下标。
    ShortageValidationStatus m_status = ShortageValidationStatus::NotStarted; ///< 当前项结论。

    QListWidget *m_caseList = nullptr;        ///< Qt 父子对象持有；左侧固定验证项列表。
    QTextEdit *m_stepText = nullptr;          ///< Qt 父子对象持有；中间显示步骤和通过标准。
    QLabel *m_statusLabel = nullptr;          ///< Qt 父子对象持有；显示当前中文验证状态。
    QLabel *m_snapshotEvidenceLabel = nullptr;///< Qt 父子对象持有；显示实际快照证据。
    QPushButton *m_nextStepButton = nullptr;  ///< Qt 父子对象持有；执行下一步动作。
    QPushButton *m_manualConfirmButton = nullptr; ///< Qt 父子对象持有；人工现场证据确认。
    QPlainTextEdit *m_logEdit = nullptr;      ///< Qt 父子对象持有；追加只读中文证据日志。
};

#endif // SHORTAGEVALIDATIONDIALOG_H
