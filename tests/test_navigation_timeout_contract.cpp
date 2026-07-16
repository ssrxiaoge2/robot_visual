#include <QFile>
#include <QString>

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

QString readSource(const QString &relativePath)
{
    QFile file(QStringLiteral(PROJECT_SOURCE_DIR "/") + relativePath);
    requireTrue(file.open(QIODevice::ReadOnly | QIODevice::Text),
                qPrintable(QStringLiteral("必须能读取 %1").arg(relativePath)));
    return QString::fromUtf8(file.readAll());
}

} // namespace

int main()
{
    const QString taskHeader = readSource(QStringLiteral("src/taskexecutor.h"));
    const QString taskSource = readSource(QStringLiteral("src/taskexecutor.cpp"));
    const QString lineHeader = readSource(QStringLiteral("src/linemanager.h"));
    const QString lineSource = readSource(QStringLiteral("src/linemanager.cpp"));
    const QString oldHeader = readSource(QStringLiteral("src/lineorchestrator.h"));
    const QString oldSource = readSource(QStringLiteral("src/lineorchestrator.cpp"));

    requireTrue(taskHeader.contains(QStringLiteral("kAgvTimeoutMs = 300000")),
                "任务导航上限必须是 300000ms");
    requireTrue(lineHeader.contains(QStringLiteral("kReturnHomeTimeoutMs = 300000")),
                "LM1 返航上限必须是 300000ms");
    requireTrue(oldHeader.contains(QStringLiteral("kAgvTimeoutMs = 300000")),
                "兼容整线上限必须是 300000ms");

    requireTrue(taskHeader.contains(QStringLiteral("QElapsedTimer m_agvNavigationElapsed"))
                    && lineHeader.contains(QStringLiteral("QElapsedTimer m_returnHomeElapsed"))
                    && oldHeader.contains(QStringLiteral("QElapsedTimer m_agvElapsed")),
                "三条导航链路必须记录实际等待时间");
    requireTrue(taskSource.contains(QStringLiteral("已等待 %2 ms（上限 %3 ms）"))
                    && lineSource.contains(QStringLiteral("已等待 %1 ms（上限 %2 ms）"))
                    && oldSource.contains(QStringLiteral("已等待 %2 ms（上限 %3 ms）")),
                "三处上位机超时日志必须包含实际等待和配置上限");

    requireTrue(taskSource.contains(QStringLiteral("NavStatus::Failed"))
                    && taskSource.contains(QStringLiteral("NavStatus::Timeout")),
                "任务导航必须继续立即处理明确失败/取消/超时状态");
    requireTrue(lineSource.contains(QStringLiteral("NavStatus::Failed"))
                    && lineSource.contains(QStringLiteral("NavStatus::Canceled"))
                    && lineSource.contains(QStringLiteral("NavStatus::Timeout")),
                "LM1 返航必须继续立即处理明确失败状态");
    const qsizetype oldFailureBranch = oldSource.indexOf(
        QStringLiteral("明确失败/取消/超时是 AGV 对当前导航的终态"));
    const qsizetype oldSeenMovingGate = oldSource.indexOf(
        QStringLiteral("if (!m_agvSeenMoving) return;"));
    requireTrue(oldFailureBranch >= 0
                    && oldSeenMovingGate > oldFailureBranch,
                "兼容整线必须在 seen-moving 门控前立即处理明确失败状态");

    return 0;
}
