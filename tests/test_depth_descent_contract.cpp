#include <QFile>

#include <cstdlib>
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
                "must read scheduler source");
    return QString::fromUtf8(file.readAll());
}

qsizetype requireIndex(const QString &source, const QString &needle, const char *message)
{
    const qsizetype index = source.indexOf(needle);
    requireTrue(index >= 0, message);
    return index;
}

QString requireBracedScopeAfter(const QString &source,
                                const QString &needle,
                                const char *message)
{
    const qsizetype start = requireIndex(source, needle, message);
    const qsizetype openingBrace = source.indexOf(QLatin1Char('{'), start);
    requireTrue(openingBrace >= 0, message);

    int depth = 0;
    for (qsizetype index = openingBrace; index < source.size(); ++index) {
        if (source.at(index) == QLatin1Char('{'))
            ++depth;
        else if (source.at(index) == QLatin1Char('}'))
            --depth;

        if (depth == 0)
            return source.mid(openingBrace + 1, index - openingBrace - 1);
    }

    requireTrue(false, message);
    return {};
}

} // namespace

int main()
{
    const QString header =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/huayanScheduler.h"));
    const QString source =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/huayanScheduler.cpp"));

    requireTrue(header.contains(QStringLiteral("StageStep::DepthDescent"))
                    || header.contains(QStringLiteral("DepthDescent,")),
                "depth descent must have a dedicated stage step");
    requireTrue(header.contains(QStringLiteral("m_depthDescentAccumulatedMm"))
                    && header.contains(QStringLiteral("m_pendingDepthDescentMm")),
                "depth descent must keep an accumulator independent from no-object search");
    requireTrue(header.contains(QStringLiteral("resetDepthDescentState()"))
                    && header.contains(QStringLiteral("handleExcessiveVisionDepth(double depthMm)"))
                    && header.contains(QStringLiteral("executeDepthDescent(double moveMm)")),
                "scheduler must expose private depth-descent state-machine helpers");

    const qsizetype callback = requireIndex(
        source, QStringLiteral("void HuayanScheduler::setGrabOffset(double x, double y, double z, double rz)"),
        "vision success callback must exist");
    const qsizetype stopTimeout = source.indexOf(QStringLiteral("stopVisionWaitTimeout();"), callback);
    const qsizetype depthGuard = source.indexOf(QStringLiteral("handleExcessiveVisionDepth(z)"), callback);
    const qsizetype rzLogic = source.indexOf(
        QStringLiteral("evaluateLargeRzConfirmation("), callback);
    requireTrue(stopTimeout >= 0 && depthGuard > stopTimeout && depthGuard < rzLogic,
                "excessive depth must be handled before XY/Rz alignment");

    requireTrue(source.contains(QStringLiteral("decideDepthDescent("))
                    && source.contains(QStringLiteral("m_runtimeSettings.depth")),
                "depth decision must use the typed runtime settings");
    requireTrue(source.contains(QStringLiteral("cmd.kind = PendingCommandKind::MoveRelTool"))
                    && source.contains(QStringLiteral("cmd.label = QStringLiteral(\"深度自动下探\")"))
                    && source.contains(QStringLiteral("cmd.poseId = 2")),
                "depth descent must dispatch through PendingCommand on tool Z");
    requireTrue(source.contains(QStringLiteral("m_runtimeSettings.safety.maxZDescendMm")),
                "depth descent must enforce the configured Z safety ceiling");
    requireTrue(source.contains(QStringLiteral("m_depthDescentAccumulatedMm += m_pendingDepthDescentMm")),
                "accumulator must update only after robot motion completes");
    requireTrue(source.contains(QStringLiteral("QTimer::singleShot(m_runtimeSettings.vision.settleMs"))
                    && source.contains(QStringLiteral("emit surveyReady();")),
                "completed descent must wait for settling and then request redetection");
    requireTrue(source.contains(QStringLiteral("[深度下探] depth=%1mm > threshold=%2mm")),
                "depth descent must emit an operator-readable audit log");

    const QString executeCurrentStepBody = requireBracedScopeAfter(
        source,
        QStringLiteral("void HuayanScheduler::executeCurrentStep()"),
        "must locate executeCurrentStep");
    const qsizetype plannedDescendIndex = executeCurrentStepBody.indexOf(
        QStringLiteral(
            "const double plannedDescend = m_grabOffset.z - m_grabZClearance;"));
    const qsizetype hardLimitIndex = executeCurrentStepBody.indexOf(
        QStringLiteral("plannedDescend > HUAYAN_MAX_Z_DESCEND_MM"),
        plannedDescendIndex);
    const qsizetype toolMoveIndex = executeCurrentStepBody.indexOf(
        QStringLiteral("cmd.kind = PendingCommandKind::MoveRelTool"),
        hardLimitIndex);
    const qsizetype toolZIndex = executeCurrentStepBody.indexOf(
        QStringLiteral("cmd.poseId = 2"), toolMoveIndex);
    const qsizetype scanIndex = executeCurrentStepBody.indexOf(
        QStringLiteral("case StageStep::WaitPreGripScan:"), toolZIndex);
    const qsizetype gripIndex = executeCurrentStepBody.indexOf(
        QStringLiteral("case StageStep::CloseGripper:"), scanIndex);
    requireTrue(plannedDescendIndex >= 0
                    && hardLimitIndex > plannedDescendIndex
                    && toolMoveIndex > hardLimitIndex
                    && toolZIndex > toolMoveIndex
                    && scanIndex > toolZIndex
                    && gripIndex > scanIndex,
                "统一窗口只能改变下探门槛；Z公式、硬上限、工具Z、扫码和夹爪顺序必须保持不变");

    const qsizetype unifiedDecisionIndex = source.indexOf(
        QStringLiteral("VisionAlignment::decideWindow"));
    const qsizetype unifiedDescendIndex = source.indexOf(
        QStringLiteral("case VisionAlignment::WindowAction::Descend:"),
        unifiedDecisionIndex);
    const qsizetype updateDepthIndex = source.indexOf(
        QStringLiteral("m_grabOffset.z = stableDepth.filteredZMm;"),
        unifiedDescendIndex);
    const qsizetype enterDescendIndex = source.indexOf(
        QStringLiteral("m_stageStep = StageStep::DescendZ;"),
        updateDepthIndex);
    requireTrue(unifiedDecisionIndex >= 0
                    && unifiedDescendIndex > unifiedDecisionIndex
                    && updateDepthIndex > unifiedDescendIndex
                    && enterDescendIndex > updateDepthIndex,
                "只有统一窗口返回Descend后才能用稳健Z中值进入既有下探流程");

    const qsizetype start = requireIndex(source, QStringLiteral("void HuayanScheduler::startStageOne()"),
                                         "startStageOne must exist");
    requireTrue(source.indexOf(QStringLiteral("resetDepthDescentState();"), start) >= 0,
                "stage start must reset depth descent state");
    const qsizetype stop = requireIndex(source, QStringLiteral("void HuayanScheduler::stop(bool emitStoppedLog)"),
                                        "stop must exist");
    requireTrue(source.indexOf(QStringLiteral("resetDepthDescentState();"), stop) >= 0,
                "terminal stop paths must reset depth descent state");

    return 0;
}
