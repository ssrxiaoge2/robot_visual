#include <QFile>

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <type_traits>

#include "huayanScheduler.h"

namespace {

void requireTrue(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << std::endl;
        std::exit(1);
    }
}

QString readUtf8File(const QString &path)
{
    QFile file(path);
    requireTrue(file.open(QIODevice::ReadOnly | QIODevice::Text),
                qPrintable(QStringLiteral("必须能读取 %1").arg(path)));
    return QString::fromUtf8(file.readAll());
}

qsizetype requireIndexOf(const QString &source, const QString &needle, const char *message,
                         qsizetype from = 0)
{
    const qsizetype idx = source.indexOf(needle, from);
    requireTrue(idx >= 0, message);
    return idx;
}

void requireContainsInOrder(const QString &source, std::initializer_list<QString> needles,
                            const char *message)
{
    qsizetype from = 0;
    for (const QString &needle : needles) {
        const qsizetype idx = source.indexOf(needle, from);
        requireTrue(idx >= 0, message);
        from = idx + needle.size();
    }
}

QString requireBracedScopeAfter(const QString &source, const QString &anchor, const char *message)
{
    const qsizetype anchorIdx = requireIndexOf(source, anchor, message);
    const qsizetype openBraceIdx = requireIndexOf(source, QStringLiteral("{"), message, anchorIdx);
    int depth = 0;
    for (qsizetype i = openBraceIdx; i < source.size(); ++i) {
        if (source.at(i) == QLatin1Char('{')) {
            ++depth;
        } else if (source.at(i) == QLatin1Char('}')) {
            --depth;
            if (depth == 0)
                return source.mid(openBraceIdx, i - openBraceIdx + 1);
        }
    }

    requireTrue(false, message);
    return QString();
}

QString requireSegmentBetween(const QString &source, const QString &begin, const QString &end,
                              const char *message)
{
    const qsizetype beginIdx = requireIndexOf(source, begin, message);
    const qsizetype endIdx = requireIndexOf(source, end, message, beginIdx + begin.size());
    return source.mid(beginIdx, endIdx - beginIdx);
}

void requireStageOneLargeRzGuard(const QString &source)
{
    requireTrue(source.contains(QStringLiteral("HUAYAN_STAGE_ONE_MAX_LARGE_RZ_EXECUTIONS")),
                "必须定义阶段一 Rz 大角度最大执行次数宏，避免 90 度重复旋转");
    requireTrue(source.contains(QStringLiteral("m_stageOneLargeRzExecutionCount")),
                "必须记录同一阶段一目标锁定周期内已执行的大角度 Rz 次数");
    requireTrue(source.contains(QStringLiteral("已执行过 Rz 大角度修正")),
                "重复出现 Rz 大角度时必须输出现场可读日志");
    requireTrue(source.contains(QStringLiteral("疑似视觉旧帧或角度歧义")),
                "重复 Rz 大角度日志必须说明可能是视觉旧帧或角度歧义");
}

void requireStageOneZDescendTimeout(const QString &source)
{
    requireTrue(source.contains(QStringLiteral("HUAYAN_STAGE_ONE_Z_DESCEND_TIMEOUT_MS")),
                "必须定义阶段一 Z 下探专用到位等待超时");
    requireTrue(source.contains(QStringLiteral("cmd.timeoutMs = HUAYAN_STAGE_ONE_Z_DESCEND_TIMEOUT_MS")),
                "Z 下探 PendingCommand 必须使用专用超时，不能继续使用 30000ms 默认值");
}

} // namespace

static_assert(std::is_same_v<decltype(&HuayanScheduler::startPalletPlace),
                             void (HuayanScheduler::*)(const PalletPose &, double, double)>,
              "HuayanScheduler::startPalletPlace must accept targetOffset, releaseZOffsetMm and robotBaseHeightFromGroundMm");
static_assert(std::is_same_v<decltype(&HuayanScheduler::startPalletPlaceFromClampedSafety),
                             void (HuayanScheduler::*)(const PalletPose &, double, double)>,
              "HuayanScheduler::startPalletPlaceFromClampedSafety must reuse pallet place arguments");
static_assert(std::is_same_v<decltype(&HuayanScheduler::applyRuntimeSettings),
                             void (HuayanScheduler::*)(const RuntimeSettings &)>,
              "HuayanScheduler must accept a complete runtime settings snapshot");
static_assert(std::is_same_v<decltype(&HuayanScheduler::runtimeSettings),
                             const RuntimeSettings &(HuayanScheduler::*)() const>,
              "HuayanScheduler must expose its current runtime settings snapshot");

int main()
{
    const QString header =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/huayanScheduler.h"));
    const QString source =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/huayanScheduler.cpp"));
    const QString palletSequenceHeader =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/palletplacesequence.h"));
    const QString palletSequenceSource =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/palletplacesequence.cpp"));
    const QString deviceManagerSource =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/devicemanager.cpp"));

    requireStageOneLargeRzGuard(source);
    requireStageOneZDescendTimeout(source);
    requireTrue(source.contains(QStringLiteral("m_runtimeSettings.vision.xyToleranceMm")),
                "抓取收敛阈值必须来自运行设置快照");
    requireTrue(source.contains(QStringLiteral("m_runtimeSettings.search.descendStepMm")),
                "无目标搜索步长必须来自运行设置快照");
    requireTrue(source.contains(QStringLiteral("m_runtimeSettings.safety.maxSingleXyAdjustMm")),
                "单次 XY 安全上限必须来自运行设置快照");
    requireTrue(source.contains(
                    QStringLiteral("m_runtimeSettings.pickup.grabXCompensationMm")),
                "抓取 X 补偿必须来自运行时设置快照");
    requireTrue(source.contains(
                    QStringLiteral("m_runtimeSettings.pickup.grabYCompensationMm")),
                "抓取 Y 补偿必须来自运行时设置快照");
    requireTrue(!source.contains(QStringLiteral("kGrabXCompensation")),
                "抓取 X 补偿不得继续使用编译期常量");
    requireTrue(!source.contains(QStringLiteral("kGrabYCompensation")),
                "抓取 Y 补偿不得继续使用编译期常量");

    requireTrue(header.contains(QStringLiteral("void schedulerStopped();")),
                "HuayanScheduler 必须声明专用的 schedulerStopped 信号");
    requireTrue(source.contains(QStringLiteral("emit schedulerStopped();")),
                "HuayanScheduler::stop 必须发出 schedulerStopped 信号");
    requireTrue(source.contains(
                    QStringLiteral("if (emitStoppedLog) {\n        emit logMessage(QStringLiteral(\"调度已停止\"));\n        emit schedulerStopped();\n    }")),
                "HuayanScheduler::stop 必须在停止日志路径上同步发出 schedulerStopped");
    requireTrue(source.contains(QStringLiteral("HRIF_ReadActTcpPos")),
                "码垛基准点函数到位后必须读取当前 TCP Z，用于计算真实下降量");
    requireTrue(palletSequenceHeader.contains(QStringLiteral("#define PALLET_GRIPPER_RELEASE_Z_OFFSET_MM 420.0")),
                "夹爪释放点相对 TCP 高度必须用宏定义固定为 420mm");
    requireTrue(source.contains(QStringLiteral("PalletScheduler::releaseTcpZ"))
                    && source.contains(QStringLiteral("targetTcpZ - basePose.z"))
                    && source.contains(QStringLiteral("目标TCP Z")),
                "码垛 Z 下降量必须按目标 TCP Z 和当前基准点 TCP Z 计算，并记录调试日志");
    requireTrue(palletSequenceSource.contains(QStringLiteral("targetTcpZ > palletBaseTcpZMm")),
                "目标 TCP Z 高于码垛初始点位时必须拒绝执行，避免高层接近奇异点");
    requireTrue(source.contains(QStringLiteral("startPalletPlaceFromClampedSafety")),
                "HuayanScheduler 必须提供已夹紧安全位入口给主流程复用");
    requireTrue(header.contains(QStringLiteral("struct MotionDiagnosticSnapshot")),
                "HuayanScheduler 必须定义 MoveRelL 失败诊断快照");
    requireTrue(header.contains(QStringLiteral("quint64 diagnosticCommandId")),
                "待下发命令必须携带现场诊断命令序号");
    requireTrue(source.contains(QStringLiteral("HRIF_ReadCmdTcpPos"))
                    && source.contains(QStringLiteral("HRIF_ReadActJointPos"))
                    && source.contains(QStringLiteral("HRIF_ReadAxisErrorCode")),
                "失败快照必须读取指令 TCP、实际关节角和轴错误码");
    requireTrue(source.contains(QStringLiteral("[华沿][MoveRelL诊断][命令=%1]")),
                "MoveRelL 诊断日志必须带本地命令序号");

    const qsizetype moveCall = source.indexOf(QStringLiteral("const int nRet = HRIF_MoveRelL"));
    const qsizetype failureBranch = source.indexOf(QStringLiteral("if (nRet != 0)"), moveCall);
    const qsizetype diagnostics = source.indexOf(
        QStringLiteral("emitMoveRelFailureDiagnostics"), failureBranch);
    const qsizetype successState = source.indexOf(
        QStringLiteral("m_activeCommandKind = cmd.kind"), failureBranch);
    requireTrue(moveCall >= 0 && failureBranch > moveCall
                    && diagnostics > failureBranch && diagnostics < successState,
                "诊断输出必须只位于 MoveRelL SDK 非零返回分支，成功路径不得打印快照");
    requireTrue(source.contains(QStringLiteral("readMotionDiagnosticSnapshot(true, false)"))
                    && source.contains(QStringLiteral("readMotionDiagnosticSnapshot(false, true)")),
                "SDK 调用前必须保存位姿/关节，失败后必须读取控制器/轴错误状态");

    requireTrue(header.contains(QStringLiteral("resetVisionAnchorTracking()"))
                    && header.contains(QStringLiteral("makeVisionTargetSelectionContext() const"))
                    && header.contains(QStringLiteral("recordCompletedGrabMove(const RelMove &move)")),
                "HuayanScheduler 必须声明锚点清零、上下文生成和已完成微调累计接口");
    requireTrue(header.contains(QStringLiteral("m_anchorMissingFrames")),
                "HuayanScheduler 必须维护锁定目标连续丢失帧数，避免视觉丢帧时切换旁站目标");

    const QString startStageOneBody = requireBracedScopeAfter(
        source, QStringLiteral("void HuayanScheduler::startStageOne()"),
        "必须能定位 HuayanScheduler::startStageOne() 函数体");
    requireTrue(startStageOneBody.contains(QStringLiteral("resetVisionAnchorTracking();")),
                "阶段一启动时必须清零本轮拍照锚点累计状态");

    const QString executeCurrentStepBody = requireBracedScopeAfter(
        source, QStringLiteral("void HuayanScheduler::executeCurrentStep()"),
        "必须能定位 HuayanScheduler::executeCurrentStep() 函数体");
    const QString waitForVisionBranch = requireSegmentBetween(
        executeCurrentStepBody,
        QStringLiteral("case StageStep::WaitForVision:"),
        QStringLiteral("case StageStep::SearchDescend:"),
        "必须能定位 executeCurrentStep() 中阶段一 WaitForVision 分支");
    requireContainsInOrder(
        waitForVisionBranch,
        {QStringLiteral("m_visionClient->setTargetSelectionContext(makeVisionTargetSelectionContext())"),
         QStringLiteral("emit surveyReady();")},
        "每次进入 WaitForVision 发起推理前必须在 surveyReady 前注入固定拍照锚点上下文");
    requireTrue(source.contains(QStringLiteral("context.lockEnabled = true"))
                    && source.contains(QStringLiteral("context.lockMissingFrames = m_anchorMissingFrames")),
                "阶段一视觉上下文必须启用目标锁定，并把连续丢失帧数传给 VisionHttpClient");

    const QString onPollTickBody = requireBracedScopeAfter(
        source, QStringLiteral("void HuayanScheduler::onPollTick()"),
        "必须能定位 HuayanScheduler::onPollTick() 函数体");
    const QString moveToGrabCompletionBranch = requireBracedScopeAfter(
        onPollTickBody,
        QStringLiteral("if (m_stage == Stage::StageOne && m_stageStep == StageStep::MoveToGrab)"),
        "必须能定位 MoveToGrab 单轴完成分支");
    requireContainsInOrder(
        moveToGrabCompletionBranch,
        {QStringLiteral("recordCompletedGrabMove(m_grabMoves.at(m_grabMoveIdx))"),
         QStringLiteral("m_grabMoveIdx++;")},
        "MoveToGrab 单轴完成后必须在 m_grabMoveIdx++ 之前累计已完成工具系 XY 位移");

    const QString recordCompletedGrabMoveBody = requireBracedScopeAfter(
        source, QStringLiteral("void HuayanScheduler::recordCompletedGrabMove(const RelMove &move)"),
        "必须能定位 HuayanScheduler::recordCompletedGrabMove() 函数体");
    requireContainsInOrder(
        recordCompletedGrabMoveBody,
        {QStringLiteral("const double signedDistance = move.direction ? move.distance : -move.distance;"),
         QStringLiteral("if (move.poseId == 0)"),
         QStringLiteral("m_anchorAccumulatedToolX += signedDistance;"),
         QStringLiteral("else if (move.poseId == 1)"),
         QStringLiteral("m_anchorAccumulatedToolY += signedDistance;")},
        "recordCompletedGrabMove() 必须按方向符号累计工具系 X/Y 已完成位移");
    requireTrue(recordCompletedGrabMoveBody.count(QStringLiteral("move.poseId ==")) == 2,
                "recordCompletedGrabMove() 只能累计 poseId == 0 和 poseId == 1");

    requireTrue(source.contains(QStringLiteral("HUAYAN_MAX_SINGLE_XY_ADJUST_MM"))
                    && source.contains(QStringLiteral("HUAYAN_MAX_Z_DESCEND_MM")),
                "阶段一必须有独立的 XY 单步和 Z 下探硬保护常量");

    requireTrue(source.contains(QStringLiteral("validateStageOneRelMoveBeforeDispatch"))
                    && source.contains(QStringLiteral("目标不可信：计划"))
                    && source.contains(QStringLiteral("拒绝下发 MoveRelL")),
                "阶段一相对运动下发前必须执行 fail-closed 保护并记录原因");

    const QString executeNextGrabMoveBody = requireBracedScopeAfter(
        source, QStringLiteral("bool HuayanScheduler::executeNextGrabMove()"),
        "必须能定位 HuayanScheduler::executeNextGrabMove() 函数体");
    requireContainsInOrder(
        executeNextGrabMoveBody,
        {QStringLiteral("const RelMove &mv = m_grabMoves.at(m_grabMoveIdx);"),
         QStringLiteral("validateStageOneRelMoveBeforeDispatch(mv)"),
         QStringLiteral("PendingCommand cmd;")},
        "executeNextGrabMove() 必须在创建 PendingCommand 前校验阶段一相对运动");
    requireContainsInOrder(
        executeNextGrabMoveBody,
        {QStringLiteral("if (!validateStageOneRelMoveBeforeDispatch(mv))"),
         QStringLiteral("return false;"),
         QStringLiteral("beginCommandWhenReady(cmd)")},
        "executeNextGrabMove() 必须在 beginCommandWhenReady 前 fail-closed 拒绝保护失败的相对运动");

    const QString validateStageOneRelMoveBody = requireBracedScopeAfter(
        source, QStringLiteral("bool HuayanScheduler::validateStageOneRelMoveBeforeDispatch(const RelMove &move)"),
        "必须能定位 validateStageOneRelMoveBeforeDispatch() 函数体");
    requireTrue(validateStageOneRelMoveBody.contains(QStringLiteral("move.poseId == 0 || move.poseId == 1"))
                    && !validateStageOneRelMoveBody.contains(QStringLiteral("move.poseId == 5")),
                "validateStageOneRelMoveBeforeDispatch() 只能限制 poseId == 0 或 poseId == 1，不能限制 Rz");
    requireTrue(validateStageOneRelMoveBody.contains(
                    QStringLiteral("move.distance > m_runtimeSettings.safety.maxSingleXyAdjustMm")),
                "validateStageOneRelMoveBeforeDispatch() 必须用 move.distance 与运行时 XY 安全上限比较");

    requireTrue(source.contains(QStringLiteral("qMin(kMaxDescend, HUAYAN_MAX_Z_DESCEND_MM)")),
                "Z 下探计算必须同时受原有 kMaxDescend 和新的硬保护上限约束");

    const QString descendZBranch = requireSegmentBetween(
        executeCurrentStepBody,
        QStringLiteral("case StageStep::DescendZ: {"),
        QStringLiteral("case StageStep::WaitPreGripScan:"),
        "必须能定位 executeCurrentStep() 中阶段一 DescendZ 分支");
    requireContainsInOrder(
        descendZBranch,
        {QStringLiteral("const double plannedDescend = m_grabOffset.z - m_grabZClearance;"),
         QStringLiteral("if (plannedDescend > HUAYAN_MAX_Z_DESCEND_MM)"),
         QStringLiteral("emitOperationError"),
         QStringLiteral("return;"),
         QStringLiteral("const double descend = calculateGrabDescend"),
         QStringLiteral("qMin(kMaxDescend, HUAYAN_MAX_Z_DESCEND_MM)"),
         QStringLiteral("PendingCommand cmd;"),
         QStringLiteral("beginCommandWhenReady(cmd)")},
        "Z 下探必须先用未截断 plannedDescend(mm) 做硬上限 fail-closed，再计算截断下发值；拒绝分支必须 return 且不能下发 MoveRelL");

    requireTrue(source.contains(QStringLiteral("m_anchorHasPreviousTarget = true"))
                    && source.contains(QStringLiteral("m_anchorPreviousTargetX = contextSelectedAnchorX")),
                "收到可信视觉结果后必须记录上一帧锚点目标用于跳变保护");
    requireTrue(source.contains(QStringLiteral("m_anchorMissingFrames = 0"))
                    && source.contains(QStringLiteral("锁定目标暂时丢失"))
                    && source.contains(QStringLiteral("拒绝切换旁边工位目标")),
                "锁定目标选中后必须清零丢失帧数；锁定目标丢失时必须等待下一帧或失败，不能切换旁站目标");
    const QString visionHeader =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/visionclient.h"));
    const QString visionSource =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/visionclient.cpp"));
    requireTrue(visionHeader.contains(QStringLiteral("targetRejectedByTrustRule"))
                    && visionHeader.contains(QStringLiteral("TargetSelectionReason reason")),
                "VisionHttpClient 必须提供携带拒绝原因的锚点可信规则拒绝信号");
    const QString parseInferenceReplyBody = requireBracedScopeAfter(
        visionSource, QStringLiteral("void VisionHttpClient::parseInferenceReply(QNetworkReply *reply)"),
        "必须能定位 VisionHttpClient::parseInferenceReply() 函数体");
    const QString noTargetBranch = requireBracedScopeAfter(
        parseInferenceReplyBody,
        QStringLiteral("if (!selection.hasTarget())"),
        "必须能定位 parseInferenceReply() 无目标分支");
    requireContainsInOrder(
        noTargetBranch,
        {QStringLiteral("isAnchorTrustRejection(selection.reason)"),
         QStringLiteral("emit targetRejectedByTrustRule(selection.reason"),
         QStringLiteral("return;"),
         QStringLiteral("emit noObjectDetected();")},
        "AnchorDistanceTooFar/AnchorTargetJumpTooFar 必须发锚点拒绝信号并 return，不能继续走普通 noObjectDetected 搜索下移");
    requireTrue(visionSource.contains(QStringLiteral("reason == Reason::AnchorDistanceTooFar"))
                    && visionSource.contains(QStringLiteral("reason == Reason::AnchorTargetJumpTooFar")),
                "VisionHttpClient 必须只把 AnchorDistanceTooFar/AnchorTargetJumpTooFar 归类为锚点可信规则拒绝");
    requireTrue(header.contains(QStringLiteral("onVisionTargetRejectedForPickup(VisionHttpClient::TargetSelectionReason reason"))
                    && source.contains(QStringLiteral("void HuayanScheduler::onVisionTargetRejectedForPickup(VisionHttpClient::TargetSelectionReason reason")),
                "HuayanScheduler 必须声明并实现视觉锚点可信拒绝槽");
    const QString rejectedSlotBody = requireBracedScopeAfter(
        source, QStringLiteral("void HuayanScheduler::onVisionTargetRejectedForPickup(VisionHttpClient::TargetSelectionReason reason"),
        "必须能定位 HuayanScheduler::onVisionTargetRejectedForPickup() 函数体");
    requireTrue(!rejectedSlotBody.contains(QStringLiteral("SearchDescend"))
                    && !rejectedSlotBody.contains(QStringLiteral("onVisionNoObject"))
                    && !rejectedSlotBody.contains(QStringLiteral("executeSearchDrop")),
                "HuayanScheduler 收到锚点可信拒绝时不得进入普通未检测到目标搜索下移路径");
    requireContainsInOrder(
        rejectedSlotBody,
        {QStringLiteral("stopVisionWaitTimeout();"),
         QStringLiteral("emitOperationError")},
        "HuayanScheduler 收到锚点可信拒绝后必须停止视觉等待并直接阶段失败");
    requireContainsInOrder(
        deviceManagerSource,
        {QStringLiteral("connect(m_visionClient,"),
         QStringLiteral("qOverload<double, double, double, double, double, double>(&VisionHttpClient::rawCoordinatesReady)"),
         QStringLiteral("m_huayanScheduler,"),
         QStringLiteral("qOverload<double, double, double, double, double, double>(&HuayanScheduler::setGrabOffset)")},
        "DeviceManager 必须用 6 参数 rawCoordinatesReady 连接到 6 参数 HuayanScheduler::setGrabOffset");
    requireContainsInOrder(
        deviceManagerSource,
        {QStringLiteral("connect(m_visionClient, &VisionHttpClient::targetRejectedByTrustRule"),
         QStringLiteral("m_huayanScheduler, &HuayanScheduler::onVisionTargetRejectedForPickup")},
        "DeviceManager 必须把视觉锚点可信规则拒绝信号连接到 HuayanScheduler 阶段失败槽");

    return 0;
}
