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

    requireTrue(header.contains(QStringLiteral("ValidateStableZ")),
                "阶段一必须定义独立的 ValidateStableZ 状态");
    requireTrue(header.contains(QStringLiteral("m_stableZSamples"))
                    && header.contains(QStringLiteral("m_stableZLastFrameId"))
                    && header.contains(QStringLiteral("m_stableZLastTimestampMs"))
                    && header.contains(QStringLiteral("m_stableZElapsedTimer"))
                    && header.contains(QStringLiteral("m_stableZUniqueFrames")),
                "HuayanScheduler 必须维护 Z 样本、最近真实帧元数据、本地计时器和唯一帧计数");
    requireTrue(header.contains(QStringLiteral("resetStableZValidation()"))
                    && header.contains(QStringLiteral("requestNextStableZFrame()")),
                "HuayanScheduler 必须提供 Z 稳定验证清理和下一帧请求接口");

    requireTrue(source.contains(QStringLiteral("kStableZWindowFrames = 3"))
                    && source.contains(QStringLiteral("kStableZMinElapsedMs = 4000"))
                    && source.contains(QStringLiteral("kStableZMaxElapsedMs = 8000"))
                    && source.contains(QStringLiteral("kStableZMaxRangeMm = 5.0"))
                    && source.contains(QStringLiteral("kStableZPollIntervalMs = 100")),
                "Z 稳定验证必须按真实 4 秒、最长 8 秒、最近 3 帧、5mm 极差和 100ms 缓存轮询执行");

    const QString setGrabOffsetBody = requireBracedScopeAfter(
        source,
        QStringLiteral("void HuayanScheduler::setGrabOffset(double x, double y, double z, double rz)"),
        "必须能定位四参数 setGrabOffset() 函数体");
    requireTrue(setGrabOffsetBody.contains(QStringLiteral("const bool validatingStableZ"))
                    && setGrabOffsetBody.contains(QStringLiteral("m_stageStep = StageStep::ValidateStableZ")),
                "setGrabOffset() 必须区分普通对准与 Z 稳定验证结果");
    requireTrue(setGrabOffsetBody.contains(QStringLiteral("resetStableZValidation();"))
                    && setGrabOffsetBody.contains(QStringLiteral("requestNextStableZFrame();")),
                "首次对准后必须清空旧样本并请求新的 Z 验证帧");
    requireTrue(setGrabOffsetBody.contains(QStringLiteral("frameId <= m_stableZLastFrameId"))
                    && setGrabOffsetBody.contains(QStringLiteral("重复缓存帧"))
                    && setGrabOffsetBody.contains(QStringLiteral("m_stableZSamples.append(z)"))
                    && setGrabOffsetBody.contains(QStringLiteral("++m_stableZUniqueFrames")),
                "只有 frame_id 递增的真实新帧才能进入 Z 稳定窗口");
    requireTrue(setGrabOffsetBody.contains(QStringLiteral("windowMax - windowMin"))
                    && setGrabOffsetBody.contains(QStringLiteral("<= kStableZMaxRangeMm"))
                    && setGrabOffsetBody.contains(QStringLiteral("std::sort")),
                "最近 3 帧必须通过极差判断并使用排序后的中位数");
    requireTrue(setGrabOffsetBody.contains(QStringLiteral("elapsedMs >= kStableZMinElapsedMs"))
                    && setGrabOffsetBody.contains(QStringLiteral("timestampMs <= m_stableZLastTimestampMs")),
                "Z 稳定验证必须等待真实 4 秒，并要求算法时间戳随真实帧递增");
    requireTrue(setGrabOffsetBody.contains(QStringLiteral("m_grabOffset.z = sortedSamples.at(1)"))
                    && setGrabOffsetBody.contains(QStringLiteral("m_stageStep = StageStep::DescendZ")),
                "稳定验证通过后必须把三帧中位数作为最终 Z 再进入下探");
    requireTrue(setGrabOffsetBody.contains(QStringLiteral("elapsedMs >= kStableZMaxElapsedMs"))
                    && setGrabOffsetBody.contains(QStringLiteral("emitOperationError")),
                "等待 8 秒仍不稳定时必须使用统一错误出口停止阶段");

    const QString noObjectBody = requireBracedScopeAfter(
        source,
        QStringLiteral("void HuayanScheduler::onVisionNoObject()"),
        "必须能定位 onVisionNoObject() 函数体");
    requireTrue(noObjectBody.contains(QStringLiteral("StageStep::ValidateStableZ"))
                    && noObjectBody.contains(QStringLiteral("拒绝搜索下移")),
                "Z 稳定验证期间无目标必须失败并明确拒绝搜索下移");

    const QString requestBody = requireBracedScopeAfter(
        source,
        QStringLiteral("void HuayanScheduler::requestNextStableZFrame()"),
        "必须能定位 requestNextStableZFrame() 函数体");
    requireTrue(requestBody.contains(QStringLiteral("emit surveyReady();"))
                    && requestBody.contains(QStringLiteral("m_timeoutTimer->start(10000);")),
                "每一帧 Z 验证请求都必须重新启动独立的 10 秒等待超时");
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
