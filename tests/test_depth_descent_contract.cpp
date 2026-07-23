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
    const qsizetype rzLogic = source.indexOf(QStringLiteral("auto sameDirection"), callback);
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
