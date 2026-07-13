#ifndef SHORTAGETESTCONTROLLER_H
#define SHORTAGETESTCONTROLLER_H

#include "shortageengine.h"
#include "shortagesamplecoordinator.h"

#include <QObject>
#include <functional>
#include <memory>
#include <optional>

/// 独立缺料全逻辑测试控制器；只使用测试命名空间，不持有 FIFO 或硬件控制器。
class ShortageTestController final : public QObject
{
    Q_OBJECT
public:
    /// sampleCoordinator 和 guard 均为非拥有依赖；guard 失败时不启动测试采样。
    ShortageTestController(ShortageConfiguration configuration,
                           QString testStateDirectory,
                           ShortageSampleCoordinator *sampleCoordinator,
                           std::function<ShortageOperationResult()> fieldSamplingStartGuard,
                           QObject *parent = nullptr);

    /// 测试钩子：确认独立测试和正式逻辑使用同一个 Engine 类型。
    const ShortageEngine &engineForTest() const;
    /// 测试钩子：固定返回 StandaloneTest，防止误用正式状态文件。
    ShortageStateNamespace stateNamespaceForTest() const;

public slots:
    void initializeZeroAfterConfirmation();
    void applyManualSample(ProductModel product, ProductionMode mode, qint64 actualQty);
    void startFieldSampling();
    void stop();
    void simulateDispatchAccepted();
    void simulateDispatchRejected();
    void simulateFailureBeforeUnload();
    void simulateMaterialUnloaded();
    void simulateFailureAfterUnload();
    void simulateTaskSucceeded();
    void resendLastUnloadFact();
    void saveTestState();
    void reloadTestState();
    void clearTestStateAfterConfirmation();

signals:
    void snapshotChanged(ShortageUiSnapshot snapshot);
    void eventLogged(QString messageZh);
    void operationRejected(QString reasonZh);

private:
    void rebuildEngine();
    void connectCoordinator();
    void applyEngineResult(const ShortageEngineResult &result);
    void rejectOperation(const QString &reasonZh);
    void emitSnapshot(const ShortageEngineResult &result = {});
    void recordTerminal(TaskFactKind kind, const QString &reasonZh);
    TaskFact currentTaskFact(TaskFactKind kind, const QString &reasonZh) const;
    std::optional<ShortageDispatchRequest> currentRequestOrReject(const QString &actionZh);
    QString summaryLine1() const;
    QString summaryLine2() const;
    static QString productText(ProductModel product);
    static QString modeText(ProductionMode mode);

    ShortageConfiguration m_configuration; ///< 测试 Engine 使用的配置副本，不回写正式配置。
    QString m_testStateDirectory;          ///< 仓库根目录；Store 内部固定 test-* 文件名。
    std::unique_ptr<ShortageStateStore> m_store; ///< 仅 StandaloneTest 命名空间状态仓库。
    std::unique_ptr<ShortageEngine> m_engine;    ///< 与生产一致的账本/计划 Engine 类型。
    ShortageSampleCoordinator *m_sampleCoordinator = nullptr; ///< 非拥有采样协调器。
    std::function<ShortageOperationResult()> m_fieldSamplingStartGuard; ///< 正式 Live 停止门禁。
    bool m_fieldSamplingActive = false; ///< false 时迟到 stableSampleReady 一律忽略。
    quint64 m_nextManualRoundId = 1;    ///< 手工样本 roundId，便于测试日志追踪。
    quint64 m_nextStandaloneTaskId = 900000000000ULL; ///< 测试专用 taskId，永不来自主 FIFO。
    quint64 m_currentOrderNo = 0;       ///< 最近被测试任务绑定的补料单号。
    quint64 m_currentTaskId = 0;        ///< 最近生成的测试 taskId。
    int m_currentStationId = 0;         ///< 最近测试任务工位。
    std::optional<TaskFact> m_lastUnloadFact; ///< 重发倒料事实用于验证严重锁定语义。
    qint64 m_lastActualQty = 0;         ///< 最近手工或采样产量，失败重排时复用。
    ProductModel m_lastProduct = ProductModel::Model88; ///< 最近样本产品。
    ProductionMode m_lastMode = ProductionMode::LeftRight; ///< 最近样本模式。
};

#endif // SHORTAGETESTCONTROLLER_H
