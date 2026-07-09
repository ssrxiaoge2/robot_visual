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
                qPrintable(QStringLiteral("必须能读取 %1").arg(path)));
    return QString::fromUtf8(file.readAll());
}

} // namespace

int main()
{
    const QString visionHeader =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/visionclient.h"));
    const QString visionSource =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/visionclient.cpp"));
    const QString palletHeader =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/palletscheduler.h"));
    const QString palletSource =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/palletscheduler.cpp"));
    const QString taskExecutorHeader =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/taskexecutor.h"));
    const QString taskExecutorSource =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/taskexecutor.cpp"));
    const QString dialogSource =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/palletparamdialog.cpp"));

    requireTrue(visionHeader.contains(QStringLiteral("void fetchInference();")),
                "VisionHttpClient 必须保留正常取料推理接口");
    requireTrue(visionHeader.contains(QStringLiteral("void coordinatesReady(QList<quint16> values);")),
                "VisionHttpClient 必须保留正常取料坐标信号");
    requireTrue(visionHeader.contains(QStringLiteral("void rawCoordinatesReady(double x, double y, double z, double rz);")),
                "VisionHttpClient 必须保留正常取料原始坐标信号");
    requireTrue(visionSource.contains(QStringLiteral("emit rawCoordinatesReady(raw.x, raw.y, raw.z, raw.rz);")),
                "visionclient.cpp 必须继续发出正常取料原始坐标信号");
    requireTrue(!visionHeader.contains(QStringLiteral("fetchPalletOccupancy")),
                "visionclient.h 不应再声明码垛占用接口");
    requireTrue(!visionHeader.contains(QStringLiteral("palletOccupancyReady")),
                "visionclient.h 不应再声明码垛占用成功信号");
    requireTrue(!visionHeader.contains(QStringLiteral("palletOccupancyError")),
                "visionclient.h 不应再声明码垛占用失败信号");
    requireTrue(!visionSource.contains(QStringLiteral("fetchPalletOccupancy")),
                "visionclient.cpp 不应再实现码垛占用请求");
    requireTrue(!visionSource.contains(QStringLiteral("palletOccupancyReady")),
                "visionclient.cpp 不应再发出码垛占用成功信号");
    requireTrue(!visionSource.contains(QStringLiteral("palletOccupancyError")),
                "visionclient.cpp 不应再发出码垛占用失败信号");

    requireTrue(!palletHeader.contains(QStringLiteral("setStackingActive")),
                "palletscheduler.h 不应再暴露 stackingActive 接口");
    requireTrue(!palletHeader.contains(QStringLiteral("markAreaObservedEmpty")),
                "palletscheduler.h 不应再暴露空区观察接口");
    requireTrue(!palletHeader.contains(QStringLiteral("markAreaObservedOccupied")),
                "palletscheduler.h 不应再暴露占用观察接口");
    requireTrue(!palletHeader.contains(QStringLiteral("emptyObserveCount")),
                "palletscheduler.h 不应再保留连续空计数字段或接口");
    requireTrue(!palletHeader.contains(QStringLiteral("lastAutoResetTime")),
                "palletscheduler.h 不应再保留自动清零时间字段或接口");
    requireTrue(!palletHeader.contains(QStringLiteral("areaAutoReset")),
                "palletscheduler.h 不应再暴露视觉自动清零信号");
    requireTrue(!palletSource.contains(QStringLiteral("setStackingActive")),
                "palletscheduler.cpp 不应再实现 stackingActive 逻辑");
    requireTrue(!palletSource.contains(QStringLiteral("markAreaObservedEmpty")),
                "palletscheduler.cpp 不应再实现空区观察逻辑");
    requireTrue(!palletSource.contains(QStringLiteral("markAreaObservedOccupied")),
                "palletscheduler.cpp 不应再实现占用观察逻辑");
    requireTrue(!palletSource.contains(QStringLiteral("emptyObserveCount")),
                "palletscheduler.cpp 不应再读写连续空计数");
    requireTrue(!palletSource.contains(QStringLiteral("lastAutoResetTime")),
                "palletscheduler.cpp 不应再读写自动清零时间");
    requireTrue(!palletSource.contains(QStringLiteral("/emptyObserveCount")),
                "palletscheduler.cpp 不应再读写 emptyObserveCount 设置项");
    requireTrue(!palletSource.contains(QStringLiteral("/lastAutoResetTime")),
                "palletscheduler.cpp 不应再读写 lastAutoResetTime 设置项");
    requireTrue(!taskExecutorHeader.contains(QStringLiteral("clearPalletStackingFlag")),
                "taskexecutor.h 不应再声明 clearPalletStackingFlag");
    requireTrue(!taskExecutorSource.contains(QStringLiteral("clearPalletStackingFlag")),
                "taskexecutor.cpp 不应再调用或实现 clearPalletStackingFlag");
    requireTrue(!taskExecutorSource.contains(QStringLiteral("setStackingActive")),
                "taskexecutor.cpp 不应再操作 stackingActive");
    requireTrue(!dialogSource.contains(QStringLiteral("setStackingActive")),
                "palletparamdialog.cpp 不应再操作 stackingActive");
    requireTrue(!dialogSource.contains(QStringLiteral("lastAutoResetTime")),
                "palletparamdialog.cpp 不应再读取自动清零时间");
    requireTrue(!dialogSource.contains(QStringLiteral("areaAutoReset")),
                "palletparamdialog.cpp 不应再连接视觉自动清零信号");

    return 0;
}
