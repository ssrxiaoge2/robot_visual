#include "shortagetestcontroller.h"

#include <QDir>
#include <QFile>

ShortageTestController::ShortageTestController(
    ShortageConfiguration configuration,
    QString testStateDirectory,
    ShortageSampleCoordinator *sampleCoordinator,
    std::function<ShortageOperationResult()> fieldSamplingStartGuard,
    QObject *parent)
    : QObject(parent),
      m_configuration(std::move(configuration)),
      m_testStateDirectory(std::move(testStateDirectory)),
      m_sampleCoordinator(sampleCoordinator),
      m_fieldSamplingStartGuard(std::move(fieldSamplingStartGuard))
{
    rebuildEngine();
    connectCoordinator();
}

const ShortageEngine &ShortageTestController::engineForTest() const
{
    return *m_engine;
}

ShortageStateNamespace ShortageTestController::stateNamespaceForTest() const
{
    return ShortageStateNamespace::StandaloneTest;
}

ShortageUiSnapshot ShortageTestController::currentSnapshot() const
{
    return buildSnapshot();
}

ShortageTestActionAvailability ShortageTestController::actionAvailability() const
{
    ShortageTestActionAvailability availability;
    availability.canSubmitManualSample =
        m_inputSource == ShortageTestInputSource::Manual && !m_fieldSamplingActive;
    availability.canStartFieldSampling =
        m_inputSource == ShortageTestInputSource::Field && !m_fieldSamplingActive;
    availability.canStopFieldSampling = m_fieldSamplingActive;
    availability.canResendUnload = m_lastUnloadFact.has_value();

    // 动作能力修改前由按钮流程隐式决定；现场问题是两个测试窗口会出现状态分歧；
    // 修改后只遍历 Engine 权威补料单状态，且不影响 Engine 自身的二次业务校验。
    const ShortageRuntimeState &state = m_engine->state();
    for (const ReplenishmentOrder &order : state.orders) {
        if (order.state == ReplenishmentOrderState::AwaitingDispatch) {
            availability.canAcceptDispatch = true;
            availability.canRejectDispatch = true;
        }

        const bool isCurrentTask =
            order.orderNo == m_currentOrderNo && order.taskId == m_currentTaskId
            && m_currentOrderNo != 0 && m_currentTaskId != 0;
        if (!isCurrentTask)
            continue;

        if (order.state == ReplenishmentOrderState::Running) {
            availability.canFailBeforeUnload = true;
            availability.canRecordUnload = true;
            availability.canSucceed = true;
        } else if (order.state == ReplenishmentOrderState::Unloaded) {
            availability.canFailAfterUnload = true;
            availability.canSucceed = true;
        }
    }
    return availability;
}

void ShortageTestController::initializeZeroAfterConfirmation()
{
    applyEngineResult(m_engine->initializeZero(true, QDateTime::currentDateTimeUtc()));
}

void ShortageTestController::applyManualSample(ProductModel product,
                                               ProductionMode mode,
                                               qint64 actualQty)
{
    if (m_inputSource != ShortageTestInputSource::Manual || m_fieldSamplingActive) {
        // 修改前手工样本可在现场采样运行时直接进 Engine；现场问题是手工 actualQty
        // 与真实 MES/PLC 稳定样本会混入同一测试账本；修改后拒绝且不调用 Engine，
        // 不影响 Manual 源且采样停止时的既有手工测试链路。
        rejectOperation(QStringLiteral("手工样本已拒绝：当前不是手工源或现场采样仍在运行"));
        return;
    }
    ShortageSample sample;
    sample.roundId = m_nextManualRoundId++;
    sample.product = product;
    sample.mode = mode;
    sample.actualQty = actualQty;
    sample.capturedAtUtc = QDateTime::currentDateTimeUtc();
    m_lastProduct = product;
    m_lastMode = mode;
    m_lastActualQty = actualQty;
    applyEngineResult(m_engine->applyStableSample(sample));
}

void ShortageTestController::startFieldSampling()
{
    if (m_inputSource != ShortageTestInputSource::Field) {
        // 修改前手工源也能启动现场协调器；现场问题是打开独立测试可能误访问真实
        // MES/PLC；修改后仅 Field 源允许启动，不影响 Field 源下原有 guard 校验。
        rejectOperation(QStringLiteral("测试采样启动失败：请先切换到现场源"));
        return;
    }
    const ShortageOperationResult allowed =
        m_fieldSamplingStartGuard ? m_fieldSamplingStartGuard()
                                  : ShortageOperationResult{true, QStringLiteral("允许测试采样")};
    if (!allowed.ok) {
        rejectOperation(QStringLiteral("测试采样启动失败：%1").arg(allowed.messageZh));
        return;
    }
    m_fieldSamplingActive = true;
    if (m_sampleCoordinator != nullptr)
        m_sampleCoordinator->start();
    emit eventLogged(QStringLiteral("测试采样已启动：仅 fieldSamplingActive=true 时接收样本"));
    emitSnapshot();
}

void ShortageTestController::selectInputSource(ShortageTestInputSource source)
{
    if (source == m_inputSource)
        return;

    // 修改前现场采样可能继续运行；切换到手工源必须先停采样，避免手工与真实样本混入同一测试账本。
    if (source == ShortageTestInputSource::Manual && m_fieldSamplingActive)
        stop();
    m_inputSource = source;
    emit eventLogged(source == ShortageTestInputSource::Manual
                         ? QStringLiteral("测试输入源已切换为手工源：不会访问真实 MES/PLC")
                         : QStringLiteral("测试输入源已切换为现场源：尚未启动采样"));
    emitSnapshot();
}

void ShortageTestController::simulateRestart()
{
    // 修改前测试只能由外部重新构造对象验证恢复；现场问题是窗口内重启验证容易误碰
    // production-* 文件；修改后仅重建 StandaloneTest Engine 并走原恢复校验，不删除文件。
    stop();
    m_currentOrderNo = 0;
    m_currentTaskId = 0;
    m_currentStationId = 0;
    m_lastUnloadFact.reset();
    rebuildEngine();
    reloadTestState();
}

void ShortageTestController::stop()
{
    // 先关闭接收标志再停止协调器，迟到 stableSampleReady 不会进入测试或正式 Engine。
    m_fieldSamplingActive = false;
    if (m_sampleCoordinator != nullptr)
        m_sampleCoordinator->stop();
    emit eventLogged(QStringLiteral("测试采样已停止：迟到样本已屏蔽"));
    emitSnapshot();
}

void ShortageTestController::simulateDispatchAccepted()
{
    const auto request = currentRequestOrReject(QStringLiteral("模拟派单接受"));
    if (!request.has_value())
        return;

    const quint64 taskId = m_nextStandaloneTaskId++;
    applyEngineResult(m_engine->recordDispatchResult(request->replenishmentOrderNo, true, taskId,
                                                     QStringLiteral("测试控制器模拟接受")));
    if (m_engine->state().criticalLock)
        return;

    m_currentOrderNo = request->replenishmentOrderNo;
    m_currentTaskId = taskId;
    m_currentStationId = request->stationId;
    applyEngineResult(m_engine->recordTaskStarted(
        currentTaskFact(TaskFactKind::Started, QStringLiteral("测试任务开始"))));
}

void ShortageTestController::simulateDispatchRejected()
{
    const auto request = currentRequestOrReject(QStringLiteral("模拟派单拒收"));
    if (!request.has_value())
        return;
    applyEngineResult(m_engine->recordDispatchResult(request->replenishmentOrderNo, false, 0,
                                                     QStringLiteral("测试控制器模拟拒收")));
}

void ShortageTestController::simulateFailureBeforeUnload()
{
    recordTerminal(TaskFactKind::Failed, QStringLiteral("测试倒料前失败"));
    // 倒料前失败释放占用后，测试控制器显式用最后样本重评估，驱动拒收/重试闭环。
    if (!m_engine->state().criticalLock)
        applyManualSample(m_lastProduct, m_lastMode, m_lastActualQty);
}

void ShortageTestController::simulateMaterialUnloaded()
{
    if (m_currentOrderNo == 0 || m_currentTaskId == 0) {
        rejectOperation(QStringLiteral("模拟倒料失败：没有已接受的测试任务"));
        return;
    }
    TaskFact fact = currentTaskFact(TaskFactKind::MaterialUnloaded, QStringLiteral("测试倒料完成"));
    m_lastUnloadFact = fact;
    applyEngineResult(m_engine->recordMaterialUnloaded(fact));
}

void ShortageTestController::simulateFailureAfterUnload()
{
    recordTerminal(TaskFactKind::Failed, QStringLiteral("测试倒料后失败"));
}

void ShortageTestController::simulateTaskSucceeded()
{
    recordTerminal(TaskFactKind::Succeeded, QStringLiteral("测试任务成功"));
}

void ShortageTestController::resendLastUnloadFact()
{
    if (!m_lastUnloadFact.has_value()) {
        rejectOperation(QStringLiteral("重复倒料失败：没有可重发的倒料事实"));
        return;
    }
    // 重复倒料事实必须交给同一个 Engine 校验并严重锁定，不能在控制器吞掉。
    applyEngineResult(m_engine->recordMaterialUnloaded(*m_lastUnloadFact));
}

void ShortageTestController::saveTestState()
{
    // Engine 已在关键/周期事务中持久化；此公开事件只发布确认日志和最新快照。
    emit eventLogged(QStringLiteral("测试状态保存完成：使用 StandaloneTest 命名空间"));
    emitSnapshot();
}

void ShortageTestController::reloadTestState()
{
    ShortageStateLoadResult loaded = m_store->load();
    const ShortageEngineResult result =
        m_engine->installRestoredState(loaded, QDateTime::currentDateTimeUtc());
    if (!result.ok && loaded.ok) {
        // 已加载但因 Queued/Running/Unloaded 等不安全任务被 Engine 拒绝安装时，
        // 独立测试仍发布只读快照，便于验证重启后锁定原因和库存事实。
        emit operationRejected(result.messageZh);
        ShortageUiSnapshot snapshot;
        snapshot.inputSource = ShortageInputSource::Mock;
        snapshot.communication = ShortageCommunicationState::Stopped;
        snapshot.runtime = loaded.state;
        snapshot.summaryLine1Zh = QStringLiteral("测试态恢复待维护确认：%1")
                                      .arg(result.messageZh);
        snapshot.summaryLine2Zh = QStringLiteral("活动工位=%1，等待数=%2，严重锁定=%3")
                                      .arg(loaded.state.activeStationId)
                                      .arg(loaded.state.waitingStationIds.size())
                                      .arg(loaded.state.criticalReasonZh);
        emit snapshotChanged(snapshot);
        return;
    }
    applyEngineResult(result);
}

void ShortageTestController::clearTestStateAfterConfirmation()
{
    stop();
    const QDir directory(m_testStateDirectory);
    QFile::remove(directory.filePath(QStringLiteral("test-state.json")));
    QFile::remove(directory.filePath(QStringLiteral("test-state.backup.json")));
    QFile::remove(directory.filePath(QStringLiteral("test-events.jsonl")));
    rebuildEngine();
    m_currentOrderNo = 0;
    m_currentTaskId = 0;
    m_currentStationId = 0;
    m_lastUnloadFact.reset();
    emit eventLogged(QStringLiteral("测试状态已清除：正式 production-* 文件未触碰"));
    emitSnapshot();
}

void ShortageTestController::rebuildEngine()
{
    m_store = std::make_unique<ShortageStateStore>(m_testStateDirectory,
                                                   ShortageStateNamespace::StandaloneTest);
    m_engine = std::make_unique<ShortageEngine>(m_configuration, m_store.get());
}

void ShortageTestController::connectCoordinator()
{
    if (m_sampleCoordinator == nullptr)
        return;
    // stableSampleReady 是现场样本进入独立测试 Engine 的唯一连接点；构造期连接一次。
    connect(m_sampleCoordinator, &ShortageSampleCoordinator::stableSampleReady, this,
            [this](ShortageSample sample) {
                if (!m_fieldSamplingActive) {
                    emit eventLogged(QStringLiteral("测试采样样本已忽略：fieldSamplingActive=false"));
                    return;
                }
                m_lastProduct = sample.product;
                m_lastMode = sample.mode;
                m_lastActualQty = sample.actualQty;
                applyEngineResult(m_engine->applyStableSample(sample));
            });
    // contextChangeConfirmed 是现场换型确认进入独立测试 Engine 的唯一连接点；不连接正式账本。
    connect(m_sampleCoordinator, &ShortageSampleCoordinator::contextChangeConfirmed, this,
            [this](ProductModel, ProductionMode) {
                if (!m_fieldSamplingActive) {
                    emit eventLogged(QStringLiteral("测试采样上下文确认已忽略：fieldSamplingActive=false"));
                    return;
                }
                applyEngineResult(m_engine->activatePendingContextIfDrained(
                    false, QDateTime::currentDateTimeUtc()));
            });
}

void ShortageTestController::applyEngineResult(const ShortageEngineResult &result)
{
    if (!result.ok && !result.messageZh.isEmpty()) {
        emit operationRejected(result.messageZh);
    } else if (!result.messageZh.isEmpty()) {
        emit eventLogged(result.messageZh);
    }
    emitSnapshot(result);
}

void ShortageTestController::rejectOperation(const QString &reasonZh)
{
    emit operationRejected(reasonZh);
    emitSnapshot();
}

void ShortageTestController::emitSnapshot(const ShortageEngineResult &result)
{
    const ShortageUiSnapshot snapshot = buildSnapshot(result);
    emit snapshotChanged(snapshot);
    emit actionAvailabilityChanged(actionAvailability());
}

ShortageUiSnapshot ShortageTestController::buildSnapshot(const ShortageEngineResult &result) const
{
    ShortageUiSnapshot snapshot;
    snapshot.inputSource = m_fieldSamplingActive ? ShortageInputSource::Live
                                                 : ShortageInputSource::Mock;
    snapshot.communication = m_fieldSamplingActive ? ShortageCommunicationState::Sampling
                                                   : ShortageCommunicationState::Stopped;
    snapshot.runtime = m_engine->state();
    snapshot.hasLastProductionDelta = result.hasProductionDelta;
    snapshot.lastProductionDelta = result.productionDelta;
    snapshot.summaryLine1Zh = summaryLine1();
    snapshot.summaryLine2Zh = summaryLine2();
    return snapshot;
}

void ShortageTestController::recordTerminal(TaskFactKind kind, const QString &reasonZh)
{
    if (m_currentOrderNo == 0 || m_currentTaskId == 0) {
        rejectOperation(QStringLiteral("模拟终态失败：没有已接受的测试任务"));
        return;
    }
    applyEngineResult(m_engine->recordTaskTerminal(currentTaskFact(kind, reasonZh)));
    m_currentOrderNo = 0;
    m_currentTaskId = 0;
    m_currentStationId = 0;
}

TaskFact ShortageTestController::currentTaskFact(TaskFactKind kind, const QString &reasonZh) const
{
    TaskFact fact;
    fact.kind = kind;
    fact.replenishmentOrderNo = m_currentOrderNo;
    fact.taskId = m_currentTaskId;
    fact.stationId = m_currentStationId;
    fact.origin = ReplenishmentOrigin::Automatic;
    fact.occurredAtUtc = QDateTime::currentDateTimeUtc();
    fact.reasonZh = reasonZh;
    return fact;
}

std::optional<ShortageDispatchRequest> ShortageTestController::currentRequestOrReject(
    const QString &actionZh)
{
    std::optional<ShortageDispatchRequest> request = m_engine->nextDispatchRequest();
    if (!request.has_value()) {
        rejectOperation(QStringLiteral("%1失败：当前没有待派测试补料单").arg(actionZh));
        return std::nullopt;
    }
    return request;
}

QString ShortageTestController::summaryLine1() const
{
    const ShortageRuntimeState &state = m_engine->state();
    return QStringLiteral("测试态：产品=%1，模式=%2，actualQty=%3，采样=%4")
        .arg(productText(state.product), modeText(state.mode))
        .arg(m_lastActualQty)
        .arg(m_fieldSamplingActive ? QStringLiteral("运行") : QStringLiteral("停止"));
}

QString ShortageTestController::summaryLine2() const
{
    const ShortageRuntimeState &state = m_engine->state();
    return QStringLiteral("活动工位=%1，等待数=%2，严重锁定=%3")
        .arg(state.activeStationId)
        .arg(state.waitingStationIds.size())
        .arg(state.criticalLock ? state.criticalReasonZh : QStringLiteral("否"));
}

QString ShortageTestController::productText(ProductModel product)
{
    switch (product) {
    case ProductModel::Model88:
        return QStringLiteral("88");
    case ProductModel::Model88R:
        return QStringLiteral("88R");
    case ProductModel::Model92:
        return QStringLiteral("92");
    }
    return QStringLiteral("未知产品");
}

QString ShortageTestController::modeText(ProductionMode mode)
{
    switch (mode) {
    case ProductionMode::LeftRight:
        return QStringLiteral("L/R");
    case ProductionMode::LeftOnly:
        return QStringLiteral("L/L");
    case ProductionMode::RightOnly:
        return QStringLiteral("R/H");
    }
    return QStringLiteral("未知模式");
}
