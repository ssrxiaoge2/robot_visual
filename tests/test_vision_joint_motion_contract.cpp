#include <QFile>

#include <cstdlib>
#include <initializer_list>
#include <iostream>

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

void requireContainsInOrder(const QString &source,
                            std::initializer_list<QString> needles,
                            const char *message)
{
    qsizetype from = 0;
    for (const QString &needle : needles) {
        const qsizetype index = source.indexOf(needle, from);
        requireTrue(index >= 0, message);
        from = index + needle.size();
    }
}

QString requireFunctionBody(const QString &source,
                            const QString &signature,
                            const char *message)
{
    const qsizetype signatureIndex = source.indexOf(signature);
    requireTrue(signatureIndex >= 0, message);
    const qsizetype openBraceIndex =
        source.indexOf(QLatin1Char('{'), signatureIndex + signature.size());
    requireTrue(openBraceIndex >= 0, message);

    int depth = 0;
    for (qsizetype index = openBraceIndex; index < source.size(); ++index) {
        if (source.at(index) == QLatin1Char('{')) {
            ++depth;
        } else if (source.at(index) == QLatin1Char('}')) {
            --depth;
            if (depth == 0)
                return source.mid(openBraceIndex, index - openBraceIndex + 1);
        }
    }

    requireTrue(false, message);
    return QString();
}

QString requireSegmentBetween(const QString &source,
                              const QString &begin,
                              const QString &end,
                              const char *message)
{
    const qsizetype beginIndex = source.indexOf(begin);
    requireTrue(beginIndex >= 0, message);
    const qsizetype endIndex =
        source.indexOf(end, beginIndex + begin.size());
    requireTrue(endIndex > beginIndex, message);
    return source.mid(beginIndex, endIndex - beginIndex);
}

QString normalizeCppCode(const QString &source)
{
    enum class ScanState {
        Code,
        LineComment,
        BlockComment,
        StringLiteral,
        CharacterLiteral
    };

    QString withoutComments;
    withoutComments.reserve(source.size());
    ScanState state = ScanState::Code;
    bool escaped = false;

    // 先按 C++ 词法状态剥离行注释和块注释，避免注释里的 SDK 名称、参数说明
    // 被误认为真实调用；字符串和字符字面量保持原样，转义符用于正确识别结束引号。
    for (qsizetype index = 0; index < source.size(); ++index) {
        const QChar current = source.at(index);
        const QChar next =
            index + 1 < source.size() ? source.at(index + 1) : QChar();

        if (state == ScanState::LineComment) {
            if (current == QLatin1Char('\n')) {
                withoutComments.append(current);
                state = ScanState::Code;
            }
            continue;
        }
        if (state == ScanState::BlockComment) {
            if (current == QLatin1Char('*') && next == QLatin1Char('/')) {
                ++index;
                withoutComments.append(QLatin1Char(' '));
                state = ScanState::Code;
            }
            continue;
        }
        if (state == ScanState::StringLiteral
            || state == ScanState::CharacterLiteral) {
            withoutComments.append(current);
            if (escaped) {
                escaped = false;
            } else if (current == QLatin1Char('\\')) {
                escaped = true;
            } else if ((state == ScanState::StringLiteral
                        && current == QLatin1Char('"'))
                       || (state == ScanState::CharacterLiteral
                           && current == QLatin1Char('\''))) {
                state = ScanState::Code;
            }
            continue;
        }

        if (current == QLatin1Char('/') && next == QLatin1Char('/')) {
            ++index;
            state = ScanState::LineComment;
        } else if (current == QLatin1Char('/') && next == QLatin1Char('*')) {
            ++index;
            state = ScanState::BlockComment;
        } else {
            withoutComments.append(current);
            if (current == QLatin1Char('"')) {
                state = ScanState::StringLiteral;
            } else if (current == QLatin1Char('\'')) {
                state = ScanState::CharacterLiteral;
            }
        }
    }

    QString normalized;
    normalized.reserve(withoutComments.size());
    for (const QChar character : withoutComments) {
        if (!character.isSpace())
            normalized.append(character);
    }
    return normalized;
}

QString requireSingleNormalizedCall(const QString &functionBody,
                                    const QString &callName,
                                    const char *message)
{
    const QString normalized = normalizeCppCode(functionBody);
    const QString callAnchor = callName + QLatin1Char('(');
    const qsizetype callIndex = normalized.indexOf(callAnchor);
    requireTrue(callIndex >= 0, message);
    requireTrue(normalized.indexOf(callAnchor, callIndex + callAnchor.size()) < 0,
                message);

    const qsizetype openParenthesis =
        callIndex + callAnchor.size() - 1;
    int depth = 0;
    for (qsizetype index = openParenthesis; index < normalized.size(); ++index) {
        if (normalized.at(index) == QLatin1Char('(')) {
            ++depth;
        } else if (normalized.at(index) == QLatin1Char(')')) {
            --depth;
            if (depth == 0)
                return normalized.mid(callIndex, index - callIndex + 1);
        }
    }

    requireTrue(false, message);
    return QString();
}

QString requireNormalizedIfConditionContaining(
    const QString &source,
    const QString &requiredFragment,
    const char *message)
{
    const QString normalized = normalizeCppCode(source);
    requireTrue(normalized.count(requiredFragment) == 1, message);

    const qsizetype fragmentIndex = normalized.indexOf(requiredFragment);
    const qsizetype ifIndex =
        normalized.lastIndexOf(QStringLiteral("if("), fragmentIndex);
    requireTrue(ifIndex >= 0, message);

    const qsizetype openParenthesis = ifIndex + 2;
    int parenthesisDepth = 0;
    // 从包含目标片段的最近 if 左括号开始做括号配平，只提取条件本身。
    // 这样断言不依赖换行和缩进，同时不会把分支体中的后续调用混入条件。
    for (qsizetype index = openParenthesis;
         index < normalized.size();
         ++index) {
        if (normalized.at(index) == QLatin1Char('(')) {
            ++parenthesisDepth;
        } else if (normalized.at(index) == QLatin1Char(')')) {
            --parenthesisDepth;
            if (parenthesisDepth == 0) {
                requireTrue(fragmentIndex < index, message);
                return normalized.mid(
                    openParenthesis + 1,
                    index - openParenthesis - 1);
            }
        }
    }

    requireTrue(false, message);
    return QString();
}

} // namespace

int main()
{
    const QString header =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/huayanScheduler.h"));
    const QString source =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/huayanScheduler.cpp"));
    const QString runtimeSettingsHeader =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/runtimesettings.h"));

    requireContainsInOrder(
        header,
        {QStringLiteral("MoveToPregrasp,"),
         QStringLiteral("ValidateVisionAlignment,"),
         QStringLiteral("FineCorrectAlignment,")},
        "阶段一必须声明初始联合对准、运动后验证和联合精修三个独立状态");
    requireTrue(!header.contains(QStringLiteral("ValidateStableZ")),
                "统一联合窗口接入后不得继续保留旧 ValidateStableZ 状态");
    requireContainsInOrder(
        header,
        {QStringLiteral("bool m_initialVisionMoveCompleted = false;"),
         QStringLiteral("int m_completedFineCorrectionCount = 0;"),
         QStringLiteral(
             "VisionAlignment::ToolCorrection m_pendingAlignmentCorrection;")},
        "阶段一必须保存首次 MoveJ 完成标志、已完成精修次数和待确认联合修正");
    requireTrue(
        header.contains(QStringLiteral(
            "bool queueVisionFineCorrection("))
            && header.contains(QStringLiteral(
                "void recordCompletedVisionAlignmentMove();"))
            && header.contains(QStringLiteral(
                "void enterVisionAlignmentValidation();")),
        "调度器必须声明联合精修排队、完成后记录和进入验证的状态迁移入口");
    requireTrue(!header.contains(QStringLiteral("m_grabMoves")),
                "阶段一不得继续持有逐轴抓取动作队列");
    requireTrue(!header.contains(QStringLiteral("m_grabMoveIdx")),
                "阶段一不得继续持有逐轴抓取动作索引");
    requireTrue(!header.contains(QStringLiteral("m_grabIterations")),
                "新闭环不得继续使用旧15轮计数");
    requireTrue(!source.contains(QStringLiteral("executeNextGrabMove()")),
                "生产路径不得继续执行逐轴抓取循环");
    requireTrue(!source.contains(QStringLiteral(
                    "m_runtimeSettings.vision.maxGrabIterations")),
                "新生产路径不得读取旧最大迭代参数");
    requireTrue(!runtimeSettingsHeader.contains(
                    QStringLiteral("maxGrabIterations")),
                "运行设置不得继续暴露旧15轮逐轴循环字段");

    const QString startStageOneBody = requireFunctionBody(
        source,
        QStringLiteral("void HuayanScheduler::startStageOne()"),
        "必须能定位阶段一启动状态重置");
    requireContainsInOrder(
        startStageOneBody,
        {QStringLiteral("m_initialVisionMoveCompleted = false;"),
         QStringLiteral("m_completedFineCorrectionCount = 0;"),
         QStringLiteral("m_pendingAlignmentCorrection = {};"),
         QStringLiteral("resetStableZValidation();"),
         QStringLiteral("resetVisionAnchorTracking();")},
        "每次启动阶段一必须清空有限联合闭环状态、稳定窗口和锚点累计");

    const QString setGrabOffsetBody = requireFunctionBody(
        source,
        QStringLiteral(
            "void HuayanScheduler::setGrabOffset("
            "double x, double y, double z, double rz)"),
        "必须能定位阶段一四参数视觉结果处理入口");
    const QString normalizedSetGrabOffsetBody =
        normalizeCppCode(setGrabOffsetBody);
    requireTrue(
        normalizedSetGrabOffsetBody.count(
            QStringLiteral("queueInitialVisionPregrasp(motionSample)")) == 1,
        "首次可信目标必须且只能从生产入口排队一次初始联合 MoveJ");
    requireContainsInOrder(
        normalizedSetGrabOffsetBody,
        {QStringLiteral(
             "constVisionAlignment::SamplemeasuredSample{x,y,z,measuredRz,true};"),
         QStringLiteral(
             "constVisionAlignment::SamplemotionSample{x,y,z,motionRz,true};"),
         QStringLiteral("if(!m_initialVisionMoveCompleted){"),
         QStringLiteral("if(!queueInitialVisionPregrasp(motionSample))"),
         QStringLiteral("return;"),
         QStringLiteral("m_stageStep=StageStep::MoveToPregrasp;"),
         QStringLiteral("return;"),
         QStringLiteral("VisionAlignment::decideWindow(input,policy)")},
        "首次样本即使已在容差内也必须先排队联合 MoveJ，后续只能进入统一窗口判定");
    requireContainsInOrder(
        normalizedSetGrabOffsetBody,
        {QStringLiteral("VisionAlignment::decideWindow(input,policy)"),
         QStringLiteral("caseVisionAlignment::WindowAction::FineCorrect:"),
         QStringLiteral("queueVisionFineCorrection(decision.correction)")},
        "首次 MoveJ 后是否允许精修必须完全由统一窗口判定并排队一条联合 MoveL");

    const QString enterValidationBody = requireFunctionBody(
        source,
        QStringLiteral(
            "void HuayanScheduler::enterVisionAlignmentValidation()"),
        "必须实现联合运动后的统一视觉验证入口");
    requireContainsInOrder(
        enterValidationBody,
        {QStringLiteral("resetStableZValidation();"),
         QStringLiteral(
             "m_stableZLastFrameId = m_visionClient->lastInferenceFrameId();"),
         QStringLiteral(
             "m_stableZLastTimestampMs = m_visionClient->lastInferenceTimestampMs();"),
         QStringLiteral("m_stableZElapsedTimer.start();"),
         QStringLiteral(
             "m_stageStep = StageStep::ValidateVisionAlignment;"),
         QStringLiteral("requestNextStableZFrame();")},
        "联合运动确认完成后必须立即以当前帧为基线开启新的统一窗口");
    requireTrue(!enterValidationBody.contains(QStringLiteral(
                    "m_runtimeSettings.vision.settleMs"))
                    && !enterValidationBody.contains(QStringLiteral(
                        "m_completedFineCorrectionCount = 0")),
                "新窗口不得增加settle延迟，也不得清空已完成精修次数");

    requireContainsInOrder(
        header,
        {QStringLiteral("MoveJ,"),
         QStringLiteral("VisionPregraspMoveJ,"),
         QStringLiteral("VisionFineCorrectionMoveL")},
        "待调度命令类型必须包含初始联合 MoveJ 和联合精修 MoveL");
    requireContainsInOrder(
        header,
        {QStringLiteral("Pose targetPose;"),
         QStringLiteral("std::array<double, 6> referenceJoints{};")},
        "联合运动命令载荷必须同时保存目标位姿和当前实际关节参考");
    requireTrue(
        header.contains(QStringLiteral(
            "bool dispatchVisionPregraspMoveJ(const PendingCommand &cmd);"))
            && header.contains(QStringLiteral(
                "bool dispatchVisionFineCorrectionMoveL(const PendingCommand &cmd);")),
        "调度器必须为两种视觉联合运动提供独立 SDK 下发入口");
    requireContainsInOrder(
        header,
        {QStringLiteral("struct RobotPoseAndJoints"),
         QStringLiteral("Pose actualTcp;"),
         QStringLiteral("std::array<double, 6> actualJoints{};"),
         QStringLiteral(
             "bool readActualPoseAndJoints(RobotPoseAndJoints *snapshot,"),
         QStringLiteral(
             "bool composeVisionPregraspPose("),
         QStringLiteral(
             "bool queueInitialVisionPregrasp(")},
        "调度器必须声明实际 TCP/关节快照、SDK 位姿组合和首次预抓取命令组装入口");

    const QString readSnapshotBody = requireFunctionBody(
        source,
        QStringLiteral(
            "bool HuayanScheduler::readActualPoseAndJoints("
            "RobotPoseAndJoints *snapshot, QString *error) const"),
        "必须实现同一调度时刻的实际 TCP 与 J1～J6 快照读取");
    const QString normalizedReadSnapshotBody =
        normalizeCppCode(readSnapshotBody);
    requireTrue(
        normalizedReadSnapshotBody.count(
            QStringLiteral("HRIF_ReadActTcpPos(")) == 1
            && normalizedReadSnapshotBody.count(
                QStringLiteral("HRIF_ReadActJointPos(")) == 1,
        "实际快照必须且只能各读取一次 TCP 与 J1～J6，禁止提前或重复读取");
    requireContainsInOrder(
        normalizedReadSnapshotBody,
        {QStringLiteral("HRIF_ReadActTcpPos("),
         QStringLiteral("if(tcpRet!=0){"),
         QStringLiteral("returnfalse;"),
         QStringLiteral("HRIF_ReadActJointPos("),
         QStringLiteral("if(jointsRet!=0){"),
         QStringLiteral("returnfalse;")},
        "实际快照必须严格按 TCP读取、TCP失败返回、关节读取、关节失败返回的全局顺序执行");
    const QString tcpFailureBranch = requireSegmentBetween(
        normalizedReadSnapshotBody,
        QStringLiteral("if(tcpRet!=0){"),
        QStringLiteral("constintjointsRet=HRIF_ReadActJointPos("),
        "必须能定位 TCP 失败分支与后续关节读取边界");
    requireContainsInOrder(
        tcpFailureBranch,
        {QStringLiteral("describeError(m_boxID,tcpRet)"),
         QStringLiteral("returnfalse;")},
        "TCP 读取失败必须在读取关节前立即携带错误说明返回");
    const QString jointsFailureBranch = requireSegmentBetween(
        normalizedReadSnapshotBody,
        QStringLiteral("if(jointsRet!=0){"),
        QStringLiteral("if(error)error->clear();"),
        "必须能定位关节读取失败分支与成功出口边界");
    requireContainsInOrder(
        jointsFailureBranch,
        {QStringLiteral("describeError(m_boxID,jointsRet)"),
         QStringLiteral("returnfalse;")},
        "关节读取失败必须在关节 SDK 调用后携带错误说明返回");
    requireContainsInOrder(
        normalizedReadSnapshotBody,
        {QStringLiteral("HRIF_ReadActTcpPos("),
         QStringLiteral("snapshot->actualTcp.x"),
         QStringLiteral("snapshot->actualTcp.rz"),
         QStringLiteral("HRIF_ReadActJointPos("),
         QStringLiteral("snapshot->actualJoints[0]"),
         QStringLiteral("snapshot->actualJoints[5]")},
        "实际快照必须先读完整 TCP，再读完整 J1～J6");
    requireTrue(
        normalizedReadSnapshotBody.count(
            QStringLiteral("describeError(m_boxID,")) == 2
            && normalizedReadSnapshotBody.contains(
                QStringLiteral("describeError(m_boxID,tcpRet)"))
            && normalizedReadSnapshotBody.contains(
                QStringLiteral("读取实际TCP失败：ret=%1"))
            && normalizedReadSnapshotBody.contains(
                QStringLiteral("describeError(m_boxID,jointsRet)"))
            && normalizedReadSnapshotBody.contains(
                QStringLiteral("读取实际J1～J6失败：ret=%1"))
            && normalizedReadSnapshotBody.count(QStringLiteral("returnfalse;")) >= 3,
        "TCP 与关节读取失败必须分别携带各自 SDK 返回码和错误说明，并 fail-closed");

    const QString composeBody = requireFunctionBody(
        source,
        QStringLiteral(
            "bool HuayanScheduler::composeVisionPregraspPose("),
        "必须实现绝对预抓取位姿组合入口");
    requireContainsInOrder(
        composeBody,
        {QStringLiteral("HRIF_PoseTrans("),
         QStringLiteral("capturePose.x"),
         QStringLiteral("correction.xMm"),
         QStringLiteral("correction.yMm"),
         QStringLiteral("correction.rzDeg"),
         QStringLiteral("pregraspPose->x")},
        "绝对预抓取位必须由拍照位实际 TCP 右乘工具系联合修正得到");
    const QString composeCall = requireSingleNormalizedCall(
        composeBody,
        QStringLiteral("HRIF_PoseTrans"),
        "绝对预抓取位必须且只能调用一次 HRIF_PoseTrans");
    requireTrue(
        composeCall
            == QStringLiteral(
                "HRIF_PoseTrans("
                "m_boxID,m_rbtID,"
                "capturePose.x,capturePose.y,capturePose.z,"
                "capturePose.rx,capturePose.ry,capturePose.rz,"
                "correction.xMm,correction.yMm,0,"
                "0,0,correction.rzDeg,"
                "pregraspPose->x,pregraspPose->y,pregraspPose->z,"
                "pregraspPose->rx,pregraspPose->ry,pregraspPose->rz)"),
        "PoseTrans 相对位姿必须固定 Z/Rx/Ry 为 0，并完整输出六维绝对位姿");
    const QString normalizedComposeBody = normalizeCppCode(composeBody);
    requireTrue(
        normalizedComposeBody.contains(QStringLiteral("describeError("))
            && normalizedComposeBody.contains(QStringLiteral("if(ret!=0){"))
            && normalizedComposeBody.contains(QStringLiteral("returnfalse;")),
        "PoseTrans 失败必须携带 SDK 返回码和错误说明并 fail-closed");

    const QString queueBody = requireFunctionBody(
        source,
        QStringLiteral(
            "bool HuayanScheduler::queueInitialVisionPregrasp("),
        "必须实现首次视觉预抓取命令组装入口");
    requireContainsInOrder(
        queueBody,
        {QStringLiteral("readActualPoseAndJoints"),
         QStringLiteral("VisionAlignment::toToolCorrection"),
         QStringLiteral("composeVisionPregraspPose"),
         QStringLiteral("PendingCommandKind::VisionPregraspMoveJ"),
         QStringLiteral("beginCommandWhenReady")},
        "首次视觉结果必须先读取实位姿与关节，再组合目标并下发");
    const QString normalizedQueueBody = normalizeCppCode(queueBody);
    requireTrue(
        normalizedQueueBody.count(
            QStringLiteral("beginCommandWhenReady(cmd)")) == 1
            && normalizedQueueBody.contains(
                QStringLiteral(
                    "constboolqueued=beginCommandWhenReady(cmd);")),
        "queued 必须且只能直接绑定一次 beginCommandWhenReady(cmd) 的返回值");
    requireTrue(
        normalizedQueueBody.count(
            QStringLiteral(
                "m_pendingAlignmentCorrection=correction;")) == 1,
        "待确认修正量在首次命令组装中必须且只能赋值一次");
    const QString acceptedCommandSegment = requireSegmentBetween(
        normalizedQueueBody,
        QStringLiteral(
            "constboolqueued=beginCommandWhenReady(cmd);"),
        QStringLiteral(
            "m_pendingAlignmentCorrection=correction;"),
        "必须能定位门控返回值与待确认修正赋值边界");
    requireContainsInOrder(
        acceptedCommandSegment,
        {QStringLiteral("if(!queued){"),
         QStringLiteral("returnfalse;")},
        "门控失败必须先返回，不能覆盖已有命令对应的待确认修正量");
    requireContainsInOrder(
        normalizedQueueBody,
        {QStringLiteral("cmd.targetPose=pregraspPose;"),
         QStringLiteral("cmd.referenceJoints=snapshot.actualJoints;"),
         QStringLiteral("beginCommandWhenReady(cmd);"),
         QStringLiteral("if(!queued){"),
         QStringLiteral("returnfalse;"),
         QStringLiteral("m_pendingAlignmentCorrection=correction;"),
         QStringLiteral("returntrue;")},
        "首次命令必须携带组合后的绝对位姿与当前关节参考，并仅在门控接受后记录待确认修正量");
    requireTrue(
        normalizedQueueBody.count(QStringLiteral("emitOperationError(")) >= 2
            && normalizedQueueBody.count(QStringLiteral("returnfalse;")) >= 2,
        "读取或位姿组合失败必须经统一错误出口停止调度");
    requireContainsInOrder(
        queueBody,
        {QStringLiteral("composeVisionPregraspPose"),
         QStringLiteral("[阶段一][位姿组合]"),
         QStringLiteral("captureBase=(X=%1mm,Y=%2mm,Z=%3mm"),
         QStringLiteral("toolDelta=(X=%7mm,Y=%8mm,Z=0mm"),
         QStringLiteral("pregraspBase=(X=%10mm,Y=%11mm,Z=%12mm"),
         QStringLiteral("beginCommandWhenReady")},
        "绝对位姿组合成功后、运动排队前必须记录带mm和角度单位的拍照位、工具修正与预抓取位");

    const QString fineQueueBody = requireFunctionBody(
        source,
        QStringLiteral(
            "bool HuayanScheduler::queueVisionFineCorrection("),
        "必须实现联合精修命令组装入口");
    const QString normalizedFineQueueBody = normalizeCppCode(fineQueueBody);
    const QString pendingCorrectionAssignment =
        QStringLiteral("m_pendingAlignmentCorrection=correction;");
    requireTrue(
        normalizedFineQueueBody.contains(
            QStringLiteral(
                "qAbs(correction.xMm)>"
                "m_runtimeSettings.safety.maxSingleXyAdjustMm"))
            && normalizedFineQueueBody.contains(
                QStringLiteral(
                    "qAbs(correction.yMm)>"
                    "m_runtimeSettings.safety.maxSingleXyAdjustMm")),
        "联合精修必须分别校验工具 X 和 Y，任一轴超过单次安全上限都应拒绝");
    requireTrue(
        normalizedFineQueueBody.count(pendingCorrectionAssignment) == 1,
        "联合精修函数内必须且只能登记一次待确认修正量");
    requireTrue(
        normalizedQueueBody.contains(QStringLiteral(
            "m_pendingLargeRzExecution=qAbs(correction.rzDeg)>="
            "m_runtimeSettings.vision.largeRzJumpThresholdDeg;"))
            && normalizedFineQueueBody.contains(QStringLiteral(
                "m_pendingLargeRzExecution=qAbs(cmd.targetPose.rz)>="
                "m_runtimeSettings.vision.largeRzJumpThresholdDeg;")),
        "首次MoveJ与精修MoveL必须共用同一个待完成大角度Rz执行标记");
    const qsizetype fineQueueGateIndex = normalizedFineQueueBody.indexOf(
        QStringLiteral("constboolqueued=beginCommandWhenReady(cmd);"));
    requireTrue(
        fineQueueGateIndex >= 0
            && !normalizedFineQueueBody.left(fineQueueGateIndex)
                    .contains(pendingCorrectionAssignment),
        "联合精修在 beginCommandWhenReady(cmd) 返回前不得提前登记待确认修正量");
    requireContainsInOrder(
        normalizedFineQueueBody,
        {QStringLiteral("PendingCommandKind::VisionFineCorrectionMoveL"),
         QStringLiteral("cmd.targetPose.x=correction.xMm;"),
         QStringLiteral("cmd.targetPose.y=correction.yMm;"),
         QStringLiteral("cmd.targetPose.z=0;"),
         QStringLiteral("cmd.targetPose.rx=0;"),
         QStringLiteral("cmd.targetPose.ry=0;"),
         QStringLiteral("cmd.targetPose.rz=correction.rzDeg;"),
         QStringLiteral("constboolqueued=beginCommandWhenReady(cmd);"),
         QStringLiteral("if(!queued){"),
         QStringLiteral("returnfalse;"),
         QStringLiteral("m_pendingAlignmentCorrection=correction;"),
         QStringLiteral("m_stageStep=StageStep::FineCorrectAlignment;"),
         QStringLiteral("returntrue;")},
        "联合精修必须用单条 Tool MoveL，并仅在统一门控接受后登记待确认修正和状态");

    const QString recordCompletedBody = requireFunctionBody(
        source,
        QStringLiteral(
            "void HuayanScheduler::recordCompletedVisionAlignmentMove()"),
        "必须实现联合运动确认完成后的状态记录入口");
    const QString normalizedRecordCompletedBody =
        normalizeCppCode(recordCompletedBody);
    const QString initialMoveCompletedAssignment =
        QStringLiteral("m_initialVisionMoveCompleted=true;");
    const QString fineCorrectionCompletedIncrement =
        QStringLiteral("++m_completedFineCorrectionCount;");
    requireTrue(
        normalizedRecordCompletedBody.count(initialMoveCompletedAssignment) == 1,
        "联合运动完成函数内必须且只能设置一次首次 MoveJ 完成标志");
    requireTrue(
        normalizedRecordCompletedBody.count(
            fineCorrectionCompletedIncrement) == 1,
        "联合运动完成函数内必须且只能递增一次联合精修完成次数");
    requireContainsInOrder(
        normalizedRecordCompletedBody,
        {QStringLiteral(
             "m_anchorAccumulatedToolX+=m_pendingAlignmentCorrection.xMm;"),
         QStringLiteral(
             "m_anchorAccumulatedToolY+=m_pendingAlignmentCorrection.yMm;"),
         QStringLiteral("m_initialVisionMoveCompleted=true;"),
         QStringLiteral("++m_completedFineCorrectionCount;"),
         QStringLiteral("m_pendingAlignmentCorrection={};")},
        "联合运动只允许在确认完成后累计工具 XY，并分别记录首次 MoveJ 或精修完成次数");
    requireTrue(
        !normalizedRecordCompletedBody.contains(QStringLiteral(
            "m_anchorAccumulatedToolX+=m_pendingAlignmentCorrection.rzDeg"))
            && !normalizedRecordCompletedBody.contains(QStringLiteral(
                "m_anchorAccumulatedToolY+=m_pendingAlignmentCorrection.rzDeg")),
        "Rz 修正不得加入拍照锚点 XY 累计量");
    requireContainsInOrder(
        recordCompletedBody,
        {QStringLiteral("m_anchorAccumulatedToolX +="),
         QStringLiteral("m_anchorAccumulatedToolY +="),
         QStringLiteral("[阶段一][联合运动完成]"),
         QStringLiteral("kind=%1"),
         QStringLiteral("accumulatedToolXY=(X=%2mm,Y=%3mm)"),
         QStringLiteral("completedFine=%4/%5"),
         QStringLiteral("m_pendingAlignmentCorrection = {};")},
        "联合运动到位后必须在清空待修正前记录类型、累计工具XY和已完成精修次数");
    requireTrue(
        header.contains(QStringLiteral(
            "bool m_pendingLargeRzExecution = false;"))
            && normalizedRecordCompletedBody.contains(
                QStringLiteral("if(m_pendingLargeRzExecution){"))
            && normalizedRecordCompletedBody.contains(
                QStringLiteral("++m_stageOneLargeRzExecutionCount;")),
        "Rz大角度执行次数只能在联合运动确认到位后递增");
    requireTrue(
        normalizedSetGrabOffsetBody.count(
            QStringLiteral("++m_stageOneLargeRzExecutionCount;")) == 0,
        "视觉帧确认大角度方向时不得提前消耗实际执行次数");
    requireTrue(
        normalizedSetGrabOffsetBody.contains(
            QStringLiteral("evaluateLargeRzConfirmation("))
            && normalizedSetGrabOffsetBody.contains(QStringLiteral(
                "m_runtimeSettings.vision.largeRzJumpThresholdDeg"))
            && normalizedSetGrabOffsetBody.contains(QStringLiteral(
                "m_runtimeSettings.vision.largeRzDeltaToleranceDeg"))
            && normalizedSetGrabOffsetBody.contains(QStringLiteral(
                "m_stageOneLargeRzExecutionCount"))
            && normalizedSetGrabOffsetBody.contains(QStringLiteral(
                "m_runtimeSettings.vision.maxLargeRzExecutions"))
            && header.contains(QStringLiteral(
                "frameId <= state.frameId || timestampMs <= state.timestampMs"))
            && header.contains(QStringLiteral(
                "result.motionRzDeg = 0.0;")),
        "大角度Rz必须由阈值触发、两个严格新帧确认、保留测量残差并受实际执行次数限制");
    requireTrue(
        !normalizeCppCode(enterValidationBody).contains(QStringLiteral(
            "m_stageOneLargeRzExecutionCount=0;")),
        "每次联合运动后重开观察窗口不得清零初始MoveJ与精修MoveL共用的大角度执行计数");

    const QString initialCompletionBranch = requireSegmentBetween(
        normalizedRecordCompletedBody,
        QStringLiteral(
            "if(m_stageStep==StageStep::MoveToPregrasp){"),
        QStringLiteral(
            "}elseif(m_stageStep==StageStep::FineCorrectAlignment){"),
        "必须能单独定位首次联合 MoveJ 的完成分支");
    requireTrue(
        initialCompletionBranch.count(
            initialMoveCompletedAssignment) == 1
            && !initialCompletionBranch.contains(
                QStringLiteral("m_completedFineCorrectionCount")),
        "首次联合 MoveJ 完成分支只能设置首次完成标志，不能递增联合精修次数");

    const QString fineCompletionBranch = requireSegmentBetween(
        normalizedRecordCompletedBody,
        QStringLiteral(
            "}elseif(m_stageStep==StageStep::FineCorrectAlignment){"),
        QStringLiteral("m_pendingAlignmentCorrection={};"),
        "必须能单独定位联合精修 MoveL 的完成分支");
    requireTrue(
        fineCompletionBranch.count(
            fineCorrectionCompletedIncrement) == 1
            && !fineCompletionBranch.contains(
                QStringLiteral("m_initialVisionMoveCompleted")),
        "联合精修 MoveL 完成分支只能递增精修次数，不能改写首次 MoveJ 完成标志");

    const QString pollBody = requireFunctionBody(
        source,
        QStringLiteral("void HuayanScheduler::onPollTick()"),
        "必须能定位运动到位轮询");
    const QString pregraspCompletedKindComparison = QStringLiteral(
        "completedCommandKind==PendingCommandKind::VisionPregraspMoveJ");
    const QString completionCondition =
        requireNormalizedIfConditionContaining(
            pollBody,
            pregraspCompletedKindComparison,
            "必须能结构化提取视觉联合运动完成 if 的完整条件");
    const QString redundantStageMembershipGuard = QStringLiteral(
        "&&(m_stageStep==StageStep::MoveToPregrasp"
        "||m_stageStep==StageStep::FineCorrectAlignment)");
    QString canonicalCompletionCondition = completionCondition;
    const qsizetype redundantGuardCount =
        canonicalCompletionCondition.count(redundantStageMembershipGuard);
    requireTrue(
        redundantGuardCount <= 1,
        "视觉联合运动完成条件不得重复叠加状态集合前置约束");
    if (redundantGuardCount == 1) {
        // 现有生产代码的状态集合前置约束已被后续两个精确配对分支蕴含。
        // 仅移除这一段完整且固定的等价冗余，其余条件标记全部保留参与等值比较。
        canonicalCompletionCondition.remove(redundantStageMembershipGuard);
    }
    const QString expectedCompletionCondition = QStringLiteral(
        "m_stage==Stage::StageOne"
        "&&((m_stageStep==StageStep::MoveToPregrasp"
        "&&completedCommandKind==PendingCommandKind::VisionPregraspMoveJ)"
        "||(m_stageStep==StageStep::FineCorrectAlignment"
        "&&completedCommandKind"
        "==PendingCommandKind::VisionFineCorrectionMoveL))");
    requireTrue(
        canonicalCompletionCondition == expectedCompletionCondition,
        "视觉联合运动完成条件必须精确匹配两个状态及其唯一命令类型，不得增加命令 OR 旁路");

    const QString normalizedPollBody = normalizeCppCode(pollBody);
    const QString normalizedCompletionBranch = requireSegmentBetween(
        normalizedPollBody,
        QStringLiteral("if(") + completionCondition + QStringLiteral("){"),
        QStringLiteral("if(m_action==Action::PalletPlace"),
        "必须能定位初始 MoveJ 和联合精修 MoveL 的统一完成分支");
    requireContainsInOrder(
        normalizedCompletionBranch,
        {QStringLiteral("recordCompletedVisionAlignmentMove()"),
         QStringLiteral("enterVisionAlignmentValidation()")},
        "精确匹配联合运动完成条件后，必须先累计锚点再开启视觉窗口");

    const QString dispatchBody = normalizeCppCode(requireFunctionBody(
        source,
        QStringLiteral(
            "bool HuayanScheduler::dispatchReadyCommand(const PendingCommand &cmd)"),
        "必须能定位统一 SDK 下发入口"));
    const QString pregraspRoute = requireSegmentBetween(
        dispatchBody,
        QStringLiteral("casePendingCommandKind::VisionPregraspMoveJ:"),
        QStringLiteral("casePendingCommandKind::VisionFineCorrectionMoveL:"),
        "必须能定位初始联合 MoveJ 的 dispatch 分支");
    requireTrue(
        pregraspRoute
            == QStringLiteral(
                "casePendingCommandKind::VisionPregraspMoveJ:"
                "returndispatchVisionPregraspMoveJ(cmd);"),
        "初始联合 MoveJ 分支必须直接调用并返回对应 helper，不能依赖后续源文件顺序");
    const QString fineCorrectionRoute = requireSegmentBetween(
        dispatchBody,
        QStringLiteral("casePendingCommandKind::VisionFineCorrectionMoveL:"),
        QStringLiteral("casePendingCommandKind::None:"),
        "必须能定位联合精修 MoveL 的 dispatch 分支");
    requireTrue(
        fineCorrectionRoute
            == QStringLiteral(
                "casePendingCommandKind::VisionFineCorrectionMoveL:"
                "returndispatchVisionFineCorrectionMoveL(cmd);"),
        "联合精修 MoveL 分支必须直接调用并返回对应 helper，不能依赖后续源文件顺序");

    const QString pregraspBody = requireFunctionBody(
        source,
        QStringLiteral(
            "bool HuayanScheduler::dispatchVisionPregraspMoveJ(const PendingCommand &cmd)"),
        "必须实现初始联合 MoveJ 下发入口");
    const QString pregraspCall = requireSingleNormalizedCall(
        pregraspBody, QStringLiteral("HRIF_WayPoint"),
        "初始联合 MoveJ 必须且只能调用一次 HRIF_WayPoint");
    requireTrue(
        pregraspCall
            == QStringLiteral(
                "HRIF_WayPoint("
                "m_boxID,m_rbtID,"
                "0,"
                "cmd.targetPose.x,cmd.targetPose.y,cmd.targetPose.z,"
                "cmd.targetPose.rx,cmd.targetPose.ry,cmd.targetPose.rz,"
                "cmd.referenceJoints[0],cmd.referenceJoints[1],"
                "cmd.referenceJoints[2],cmd.referenceJoints[3],"
                "cmd.referenceJoints[4],cmd.referenceJoints[5],"
                "kTcpName.toStdString(),cmd.ucsName.toStdString(),"
                "m_runtimeSettings.motion.velocity,"
                "m_runtimeSettings.motion.acceleration,"
                "m_runtimeSettings.motion.radius,"
                "0,0,0,0,"
                "cmd.cmdId.toStdString())"),
        "初始联合 MoveJ 必须完整锁定笛卡尔目标、六轴逆解参考、坐标系、运动参数和四个控制参数");
    const QString normalizedPregraspBody = normalizeCppCode(pregraspBody);
    requireTrue(
        pregraspBody.contains(QStringLiteral("[阶段一][初始联合MoveJ]"))
            && pregraspBody.contains(QStringLiteral("command=%1"))
            && pregraspBody.contains(QStringLiteral(
                "referenceJoints=(J1=%8°,J2=%9°,J3=%10°,J4=%11°,J5=%12°,J6=%13°)")),
        "初始联合MoveJ下发前必须记录命令号和带角度单位的六轴逆解参考");
    requireContainsInOrder(
        normalizedPregraspBody,
        {QStringLiteral("if(nRet!=0){"),
         QStringLiteral("returnfalse;"),
         QStringLiteral("m_activeCommandKind=cmd.kind;"),
         QStringLiteral("m_activeCommandLabel=cmd.label;"),
         QStringLiteral("startWaitForIdle(cmd.timeoutMs);"),
         QStringLiteral("returntrue;")},
        "初始联合 MoveJ 成功后必须登记活动命令并按命令超时启动到位轮询");

    const QString fineCorrectionBody = requireFunctionBody(
        source,
        QStringLiteral(
            "bool HuayanScheduler::dispatchVisionFineCorrectionMoveL(const PendingCommand &cmd)"),
        "必须实现联合精修 MoveL 下发入口");
    const QString fineCorrectionCall = requireSingleNormalizedCall(
        fineCorrectionBody, QStringLiteral("HRIF_WayPointRel"),
        "X/Y/Rz 联合精修必须且只能调用一次 HRIF_WayPointRel");
    requireTrue(
        fineCorrectionCall
            == QStringLiteral(
                "HRIF_WayPointRel("
                "m_boxID,m_rbtID,"
                "1,0,"
                "0,0,0,0,0,0,"
                "0,0,0,0,0,0,"
                "2,"
                "1,1,0,0,0,1,"
                "cmd.targetPose.x,cmd.targetPose.y,0,0,0,cmd.targetPose.rz,"
                "kTcpName.toStdString(),cmd.ucsName.toStdString(),"
                "m_runtimeSettings.motion.velocity,"
                "m_runtimeSettings.motion.acceleration,"
                "m_runtimeSettings.motion.radius,"
                "0,0,0,0,"
                "cmd.cmdId.toStdString())"),
        "联合精修 MoveL 必须完整锁定线性 Tool 模式、点位零值、掩码、六轴增量、坐标系、运动参数和四个控制参数");
    requireTrue(
        !normalizeCppCode(fineCorrectionBody)
             .contains(QStringLiteral("HRIF_MoveRelL(")),
        "联合精修不得退化为逐轴 HRIF_MoveRelL");
    const QString normalizedFineCorrectionBody =
        normalizeCppCode(fineCorrectionBody);
    requireContainsInOrder(
        normalizedFineCorrectionBody,
        {QStringLiteral("if(nRet!=0){"),
         QStringLiteral("returnfalse;"),
         QStringLiteral("m_activeCommandKind=cmd.kind;"),
         QStringLiteral("m_activeCommandLabel=cmd.label;"),
         QStringLiteral("startWaitForIdle(cmd.timeoutMs);"),
         QStringLiteral("returntrue;")},
        "联合精修 MoveL 成功后必须登记活动命令并按命令超时启动到位轮询");

    return 0;
}
