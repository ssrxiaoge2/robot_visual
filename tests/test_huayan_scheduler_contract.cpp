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
                             void (HuayanScheduler::*)(const PalletPose &, double, double)>,
              "HuayanScheduler::startPalletPlace must accept targetOffset, releaseZOffsetMm and robotBaseHeightFromGroundMm");
static_assert(std::is_same_v<decltype(&HuayanScheduler::startPalletPlaceFromClampedSafety),
                             void (HuayanScheduler::*)(const PalletPose &, double, double)>,
              "HuayanScheduler::startPalletPlaceFromClampedSafety must reuse pallet place arguments");

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

    return 0;
}
