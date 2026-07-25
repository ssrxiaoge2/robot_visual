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
