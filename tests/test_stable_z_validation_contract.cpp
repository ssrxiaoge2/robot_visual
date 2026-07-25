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

QString readUtf8File(const QString &path)
{
    QFile file(path);
    requireTrue(file.open(QIODevice::ReadOnly | QIODevice::Text),
                "必须能够读取 HuayanScheduler 源文件");
    return QString::fromUtf8(file.readAll());
}

QString requireBracedScopeAfter(const QString &source,
                                const QString &needle,
                                const char *message)
{
    const qsizetype start = source.indexOf(needle);
    requireTrue(start >= 0, message);
    const qsizetype openingBrace = source.indexOf(QLatin1Char('{'), start);
    requireTrue(openingBrace >= 0, message);

    int depth = 0;
    for (qsizetype i = openingBrace; i < source.size(); ++i) {
        if (source.at(i) == QLatin1Char('{'))
            ++depth;
        else if (source.at(i) == QLatin1Char('}'))
            --depth;

        if (depth == 0)
            return source.mid(openingBrace + 1, i - openingBrace - 1);
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
    const QString visionHeader =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/visionclient.h"));
    const QString visionSource =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/visionclient.cpp"));

    requireTrue(visionHeader.contains(QStringLiteral("lastInferenceFrameId() const"))
                    && visionHeader.contains(QStringLiteral("lastInferenceTimestampMs() const"))
                    && visionHeader.contains(QStringLiteral("m_lastInferenceFrameId"))
                    && visionHeader.contains(QStringLiteral("m_lastInferenceTimestampMs")),
                "VisionHttpClient 必须公开最近一次算法真实帧编号和时间戳");
    requireTrue(visionSource.contains(QStringLiteral("\"frame_id\""))
                    && visionSource.contains(QStringLiteral("\"timestamp\""))
                    && visionSource.contains(QStringLiteral("m_lastInferenceFrameId"))
                    && visionSource.contains(QStringLiteral("m_lastInferenceTimestampMs")),
                "解析 /inference 响应时必须保存根字段 frame_id 和 timestamp");

    requireTrue(header.contains(QStringLiteral("ValidateVisionAlignment")),
                "阶段一必须定义统一的 ValidateVisionAlignment 状态");
    requireTrue(!header.contains(QStringLiteral("ValidateStableZ")),
                "统一窗口接入后不得保留旧 ValidateStableZ 生产状态");
    requireTrue(header.contains(QStringLiteral("m_stableZSamples"))
                    && header.contains(QStringLiteral("m_stableZLastFrameId"))
                    && header.contains(QStringLiteral("m_stableZLastTimestampMs"))
                    && header.contains(QStringLiteral("m_stableZElapsedTimer"))
                    && header.contains(QStringLiteral("m_stableZUniqueFrames")),
                "HuayanScheduler 必须维护联合窗口的 Z 样本、最近真实帧元数据、本地计时器和唯一帧计数");
    requireTrue(header.contains(QStringLiteral("resetStableZValidation()"))
                    && header.contains(QStringLiteral("requestNextStableZFrame()")),
                "HuayanScheduler 必须提供联合窗口清理和下一帧请求接口");

    requireTrue(source.contains(QStringLiteral("kStableZWindowFrames = 3")),
                "统一窗口必须保留最近三个真实新帧");
    requireTrue(source.contains(QStringLiteral("kStableZMinElapsedMs = 4000")),
                "统一观察窗口最短时间必须保持4秒");
    requireTrue(source.contains(QStringLiteral("kStableZMaxElapsedMs = 8000")),
                "统一观察窗口硬上限必须保持8秒");
    requireTrue(source.contains(QStringLiteral("kStableZMaxRangeMm = 5.0")),
                "统一窗口的三帧 Z 极差必须不大于5mm");
    requireTrue(source.contains(QStringLiteral("kStableZPollIntervalMs = 100")),
                "统一窗口必须保留100ms缓存轮询");
    requireTrue(source.contains(QStringLiteral(
                    "m_runtimeSettings.vision.xyToleranceMm")),
                "统一窗口必须使用运行时XY阈值");
    requireTrue(source.contains(QStringLiteral(
                    "m_runtimeSettings.vision.rzToleranceDeg")),
                "统一窗口必须使用运行时Rz阈值");
    requireTrue(source.contains(QStringLiteral(
                    "m_runtimeSettings.vision.maxFineCorrectionCount")),
                "统一窗口必须使用运行时联合精修次数");
    requireTrue(!source.contains(QStringLiteral("kRzTolerance")),
                "调度器不得继续使用硬编码Rz阈值");

    const QString setGrabOffsetBody = requireBracedScopeAfter(
        source,
        QStringLiteral("void HuayanScheduler::setGrabOffset(double x, double y, double z, double rz)"),
        "必须能定位四参数 setGrabOffset() 函数体");
    requireTrue(setGrabOffsetBody.contains(QStringLiteral(
                    "m_stageStep == StageStep::ValidateVisionAlignment")),
                "setGrabOffset() 必须在统一联合验证状态内处理观察结果");
    requireTrue(setGrabOffsetBody.contains(QStringLiteral("frameId <= m_stableZLastFrameId"))
                    && setGrabOffsetBody.contains(QStringLiteral("重复缓存帧"))
                    && setGrabOffsetBody.contains(QStringLiteral("m_stableZSamples.append(z)"))
                    && setGrabOffsetBody.contains(QStringLiteral("++m_stableZUniqueFrames")),
                "只有 frame_id 递增的真实新帧才能进入联合窗口");
    requireTrue(setGrabOffsetBody.contains(QStringLiteral("windowMax - windowMin"))
                    && setGrabOffsetBody.contains(QStringLiteral("<= kStableZMaxRangeMm"))
                    && setGrabOffsetBody.contains(QStringLiteral("const bool zStable")),
                "最近3帧必须通过不大于5mm的极差得到zStable");
    requireTrue(setGrabOffsetBody.contains(QStringLiteral(
                    "timestampMs <= m_stableZLastTimestampMs")),
                "联合窗口必须要求算法时间戳随真实帧递增");
    requireTrue(setGrabOffsetBody.contains(QStringLiteral(
                    "VisionAlignment::WindowPolicy policy"))
                    && setGrabOffsetBody.contains(QStringLiteral(
                        "policy.xyToleranceMm = m_runtimeSettings.vision.xyToleranceMm"))
                    && setGrabOffsetBody.contains(QStringLiteral(
                        "policy.rzToleranceDeg = m_runtimeSettings.vision.rzToleranceDeg"))
                    && setGrabOffsetBody.contains(QStringLiteral(
                        "policy.maxFineCorrectionCount = m_runtimeSettings.vision.maxFineCorrectionCount"))
                    && setGrabOffsetBody.contains(QStringLiteral(
                        "policy.minElapsedMs = kStableZMinElapsedMs"))
                    && setGrabOffsetBody.contains(QStringLiteral(
                        "policy.maxElapsedMs = kStableZMaxElapsedMs")),
                "联合窗口必须从运行时设置和固定4至8秒边界构造WindowPolicy");
    requireTrue(setGrabOffsetBody.contains(QStringLiteral(
                    "VisionAlignment::WindowInput input"))
                    && setGrabOffsetBody.contains(QStringLiteral(
                        "input.completedFineCorrectionCount = m_completedFineCorrectionCount"))
                    && setGrabOffsetBody.contains(QStringLiteral(
                        "VisionAlignment::decideWindow(input, policy)")),
                "每个真实新帧必须同时把XY/Rz、Z稳定性、耗时和已完成精修次数交给统一判定");

    const qsizetype continueAction = setGrabOffsetBody.indexOf(
        QStringLiteral("case VisionAlignment::WindowAction::ContinueObserving:"));
    const qsizetype continueRequest = setGrabOffsetBody.indexOf(
        QStringLiteral("requestNextStableZFrame();"), continueAction);
    const qsizetype descendAction = setGrabOffsetBody.indexOf(
        QStringLiteral("case VisionAlignment::WindowAction::Descend:"), continueRequest);
    const qsizetype descendStep = setGrabOffsetBody.indexOf(
        QStringLiteral("m_stageStep = StageStep::DescendZ;"), descendAction);
    const qsizetype fineCorrectAction = setGrabOffsetBody.indexOf(
        QStringLiteral("case VisionAlignment::WindowAction::FineCorrect:"), descendStep);
    const qsizetype fineCorrectQueue = setGrabOffsetBody.indexOf(
        QStringLiteral("queueVisionFineCorrection(decision.correction)"), fineCorrectAction);
    const qsizetype stopAction = setGrabOffsetBody.indexOf(
        QStringLiteral("case VisionAlignment::WindowAction::Stop:"), fineCorrectQueue);
    const qsizetype stopError = setGrabOffsetBody.indexOf(
        QStringLiteral("emitOperationError"), stopAction);
    requireTrue(continueAction >= 0 && continueRequest > continueAction
                    && descendAction > continueRequest && descendStep > descendAction
                    && fineCorrectAction > descendStep && fineCorrectQueue > fineCorrectAction
                    && stopAction > fineCorrectQueue && stopError > stopAction,
                "四种动作必须显式按继续观察、下探、联合精修、停止的顺序分派");

    const QString enterValidationBody = requireBracedScopeAfter(
        source,
        QStringLiteral("void HuayanScheduler::enterVisionAlignmentValidation()"),
        "必须能定位 enterVisionAlignmentValidation() 函数体");
    const qsizetype resetIndex =
        enterValidationBody.indexOf(QStringLiteral("resetStableZValidation();"));
    const qsizetype frameBaselineIndex =
        enterValidationBody.indexOf(QStringLiteral(
            "m_stableZLastFrameId = m_visionClient->lastInferenceFrameId();"));
    const qsizetype timestampBaselineIndex =
        enterValidationBody.indexOf(QStringLiteral(
            "m_stableZLastTimestampMs = m_visionClient->lastInferenceTimestampMs();"));
    const qsizetype timerStartIndex =
        enterValidationBody.indexOf(QStringLiteral("m_stableZElapsedTimer.start();"));
    const qsizetype stateIndex =
        enterValidationBody.indexOf(QStringLiteral(
            "m_stageStep = StageStep::ValidateVisionAlignment;"));
    const qsizetype requestIndex =
        enterValidationBody.indexOf(QStringLiteral("requestNextStableZFrame();"));
    requireTrue(resetIndex >= 0
                    && frameBaselineIndex > resetIndex
                    && timestampBaselineIndex > frameBaselineIndex
                    && timerStartIndex > timestampBaselineIndex
                    && stateIndex > timerStartIndex
                    && requestIndex > stateIndex,
                "每次联合运动完成后必须立即清空旧窗口、记录帧基线并启动新的4至8秒窗口");
    requireTrue(!enterValidationBody.contains(QStringLiteral("settleMs"))
                    && !enterValidationBody.contains(QStringLiteral(
                        "QTimer::singleShot")),
                "联合运动到位后不得附加稳定等待或再等一帧");
    requireTrue(!enterValidationBody.contains(QStringLiteral(
                    "m_completedFineCorrectionCount = 0")),
                "重启观察窗口不得清空已完成联合精修次数");

    const QString noObjectBody = requireBracedScopeAfter(
        source,
        QStringLiteral("void HuayanScheduler::onVisionNoObject()"),
        "必须能定位 onVisionNoObject() 函数体");
    requireTrue(noObjectBody.contains(QStringLiteral(
                    "StageStep::ValidateVisionAlignment"))
                    && noObjectBody.contains(QStringLiteral("拒绝搜索下移")),
                "联合验证期间无目标必须失败并明确拒绝搜索下移");

    const QString requestBody = requireBracedScopeAfter(
        source,
        QStringLiteral("void HuayanScheduler::requestNextStableZFrame()"),
        "必须能定位 requestNextStableZFrame() 函数体");
    requireTrue(requestBody.contains(QStringLiteral(
                    "m_stageStep != StageStep::ValidateVisionAlignment"))
                    && requestBody.contains(QStringLiteral(
                        "m_stableZElapsedTimer.elapsed() >= kStableZMaxElapsedMs"))
                    && requestBody.contains(QStringLiteral("emitOperationError"))
                    && requestBody.contains(QStringLiteral("emit surveyReady();")),
                "统一窗口轮询必须限定联合验证状态，并在8秒硬上限停止而不是继续等待");
    requireTrue(visionSource.contains(QStringLiteral("req.setTransferTimeout(5000)")),
                "视觉 HTTP 单次推理必须保留 5 秒网络超时，不能让某一帧无限阻塞");

    const QString stopBody = requireBracedScopeAfter(
        source,
        QStringLiteral("void HuayanScheduler::stop(bool emitStoppedLog)"),
        "必须能定位 stop() 函数体");
    requireTrue(stopBody.contains(QStringLiteral("resetStableZValidation();")),
                "停止阶段时必须清空 Z 稳定验证状态");

    return 0;
}
