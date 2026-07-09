#include <QFile>

#include <cstdlib>
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

} // namespace

static_assert(std::is_same_v<decltype(&HuayanScheduler::startPalletPlace),
                             void (HuayanScheduler::*)(const PalletPose &, double)>,
              "HuayanScheduler::startPalletPlace must accept targetOffset and releaseZOffsetMm");

int main()
{
    const QString header =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/huayanScheduler.h"));
    const QString source =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/huayanScheduler.cpp"));

    requireTrue(header.contains(QStringLiteral("void schedulerStopped();")),
                "HuayanScheduler 必须声明专用的 schedulerStopped 信号");
    requireTrue(source.contains(QStringLiteral("emit schedulerStopped();")),
                "HuayanScheduler::stop 必须发出 schedulerStopped 信号");
    requireTrue(source.contains(
                    QStringLiteral("if (emitStoppedLog) {\n        emit logMessage(QStringLiteral(\"调度已停止\"));\n        emit schedulerStopped();\n    }")),
                "HuayanScheduler::stop 必须在停止日志路径上同步发出 schedulerStopped");

    return 0;
}
