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

} // namespace

int main()
{
    const QString header =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/huayanScheduler.h"));
    const QString source =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/huayanScheduler.cpp"));

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
        source,
        {QStringLiteral("case PendingCommandKind::VisionPregraspMoveJ"),
         QStringLiteral("HRIF_WayPoint("),
         QStringLiteral("cmd.referenceJoints[0]"),
         QStringLiteral("cmd.referenceJoints[5]")},
        "初始联合对准必须通过 HRIF_WayPoint 使用当前关节参考");
    requireContainsInOrder(
        source,
        {QStringLiteral("case PendingCommandKind::VisionFineCorrectionMoveL"),
         QStringLiteral("HRIF_WayPointRel("),
         QStringLiteral("cmd.targetPose.x"),
         QStringLiteral("cmd.targetPose.y"),
         QStringLiteral("cmd.targetPose.rz")},
        "精修正必须通过一次 HRIF_WayPointRel 联合下发 X/Y/Rz");

    const QString pregraspBody = requireFunctionBody(
        source,
        QStringLiteral(
            "bool HuayanScheduler::dispatchVisionPregraspMoveJ(const PendingCommand &cmd)"),
        "必须实现初始联合 MoveJ 下发入口");
    requireContainsInOrder(
        pregraspBody,
        {QStringLiteral("HRIF_WayPoint("),
         QStringLiteral("m_boxID, m_rbtID,"),
         QStringLiteral("0,"),
         QStringLiteral("cmd.targetPose.x"),
         QStringLiteral("cmd.targetPose.rz"),
         QStringLiteral("cmd.referenceJoints[0]"),
         QStringLiteral("cmd.referenceJoints[5]"),
         QStringLiteral("kTcpName.toStdString()"),
         QStringLiteral("cmd.ucsName.toStdString()"),
         QStringLiteral("m_runtimeSettings.motion.velocity"),
         QStringLiteral("m_runtimeSettings.motion.acceleration"),
         QStringLiteral("m_runtimeSettings.motion.radius"),
         QStringLiteral("cmd.cmdId.toStdString()")},
        "初始联合 MoveJ 必须使用笛卡尔目标、实际关节逆解参考、当前坐标系和运行时运动参数");

    const QString fineCorrectionBody = requireFunctionBody(
        source,
        QStringLiteral(
            "bool HuayanScheduler::dispatchVisionFineCorrectionMoveL(const PendingCommand &cmd)"),
        "必须实现联合精修 MoveL 下发入口");
    requireContainsInOrder(
        fineCorrectionBody,
        {QStringLiteral("HRIF_WayPointRel("),
         QStringLiteral("m_boxID, m_rbtID,"),
         QStringLiteral("1,"),
         QStringLiteral("0,"),
         QStringLiteral("0, 0, 0, 0, 0, 0,"),
         QStringLiteral("0, 0, 0, 0, 0, 0,"),
         QStringLiteral("2,"),
         QStringLiteral("1, 1, 0, 0, 0, 1,"),
         QStringLiteral("cmd.targetPose.x"),
         QStringLiteral("cmd.targetPose.y"),
         QStringLiteral("0,"),
         QStringLiteral("0,"),
         QStringLiteral("0,"),
         QStringLiteral("cmd.targetPose.rz"),
         QStringLiteral("kTcpName.toStdString()"),
         QStringLiteral("cmd.ucsName.toStdString()"),
         QStringLiteral("m_runtimeSettings.motion.velocity"),
         QStringLiteral("m_runtimeSettings.motion.acceleration"),
         QStringLiteral("m_runtimeSettings.motion.radius"),
         QStringLiteral("cmd.cmdId.toStdString()")},
        "联合精修 MoveL 必须使用线性类型、非点表、Tool 模式 2、X/Y/Rz 掩码及运行时运动参数");
    requireTrue(
        fineCorrectionBody.count(QStringLiteral("HRIF_WayPointRel(")) == 1,
        "X/Y/Rz 联合精修必须由单条 HRIF_WayPointRel 完成");
    requireTrue(
        !fineCorrectionBody.contains(QStringLiteral("HRIF_MoveRelL(")),
        "联合精修不得退化为逐轴 HRIF_MoveRelL");

    return 0;
}
