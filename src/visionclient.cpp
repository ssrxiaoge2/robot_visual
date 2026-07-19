/**
 * @file visionclient.cpp
 * @brief VisionHttpClient 视觉 HTTP 客户端实现
 *
 * 所有网络请求均异步：sendRequest() 返回后立即退出，
 * 结果通过 QNetworkReply::finished 信号回调，再 emit 对应信号。
 *
 * ── Rz 角度规范化说明 ─────────────────────────────────────────
 *   夹爪关于 180° 对称（angle=0° 和 angle=180° 物理等价）。
 *   规范化目标：将任意角映射到 (-90°, 90°]，选择最小旋转量：
 *
 *     angle   → normalized → 含义
 *     0°      →  0°        → 无需旋转
 *     45°     → +45°       → 顺时针 45°
 *     90°     → -90°       → 等价（选最小旋转）
 *     135°    → -45°       → 等价于顺时针 -45°
 *     180°    →  0°        → 等价于 0°，无需旋转 ✓
 *     -45°    → -45°       → 逆时针 45°
 *
 *   ⚠ Rz 正方向与实际机器人旋转方向是否一致，需联机调试确认。
 *     若方向相反，在 transformToRegisters() 中将 normAngle 取反即可。
 */

#include "visionclient.h"

#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>
#include <QtMath>

#include <cmath>

// ── 默认手眼变换矩阵（T_tool_cam，行主序）──────────────────────
//
// 来源：hand-eye/calibration_output/handeye_matrix.txt
// 含义：将相机坐标系中的点变换到末端工具坐标系，单位 mm
//
// ⚠ 当前矩阵误差较大，重新标定后调用 setHandEyeMatrix() 更新
//    或修改下方常量直接重新编译
//
// 矩阵结构（行主序）：
//   [ R(3×3) | t(3×1) ]
//   [ 0  0  0 |   1   ]
// 平移 t = (-28.63, 5.07, 156.09) mm（相机光心在工具坐标系中的位置）
static const float kDefaultHandEye[16] = {
    0.99970807, -0.01952258, -0.01423497, -6.07400241,
    0.01923080, 0.99960787, -0.02035380, 12.84364319,
    0.01462674, 0.02007411, 0.99969150, 147.64502743,
    0.00000000, 0.00000000, 0.00000000, 1.00000000,
};

// ── 构造 ─────────────────────────────────────────────────────

VisionHttpClient::VisionHttpClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    setHandEyeMatrix(kDefaultHandEye);
}

// ── 配置接口 ─────────────────────────────────────────────────

void VisionHttpClient::setServerUrl(const QString &ip, int port)
{
    m_ip   = ip;
    m_port = port;
}

void VisionHttpClient::setHandEyeMatrix(const float m[16])
{
    // 将平铺的 16 个浮点数填入 4×4 数组（行主序）。矩阵错误会使后续机械臂
    // 相对运动方向/距离整体错误，因此它属于现场标定的安全关键输入。
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            m_T[r][c] = m[r * 4 + c];
}

void VisionHttpClient::setBaseRz(qint32 baseRzReg) { m_baseRzReg = baseRzReg; }
void VisionHttpClient::setBaseRx(qint32 baseRxReg) { m_baseRxReg = baseRxReg; }
void VisionHttpClient::setBaseRy(qint32 baseRyReg) { m_baseRyReg = baseRyReg; }

void VisionHttpClient::setTargetSelectionContext(const TargetSelectionContext &context)
{
    m_targetSelectionContext = context;
}

// ── 公开请求接口 ─────────────────────────────────────────────

/**
 * @brief GET /inference — 获取最新推理结果
 *
 * 成功且有目标：解析第一个目标 → 应用手眼变换 → emit coordinatesReady
 * 目标数 == 0：emit noObjectDetected
 * 网络/解析错误：emit errorOccurred
 */
void VisionHttpClient::fetchInference()
{
    if (!isConfigured()) {
        emit errorOccurred(QStringLiteral("[视觉] 服务器地址未配置"));
        return;
    }

    QNetworkRequest req(QUrl(
        QString("http://%1:%2/inference").arg(m_ip).arg(m_port)));
    req.setTransferTimeout(5000); // 5s 超时，防止阻塞工作流

    QNetworkReply *reply = m_nam->get(req);
    reply->setParent(this);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        parseInferenceReply(reply);
        reply->deleteLater();
    });
}
/**
 * @brief GET /frame/annotated — 获取带标注的 JPEG 图像
 *
 * 成功：将 JPEG 解码为 QImage，emit frameReady
 * 失败（网络错误/解码失败）：静默丢弃（不影响工作流）
 */
void VisionHttpClient::fetchAnnotatedFrame()
{
    if (!isConfigured()) return;

    QNetworkRequest req(QUrl(
        QString("http://%1:%2/frame/annotated").arg(m_ip).arg(m_port)));
    req.setTransferTimeout(3000); // 3s 超时（帧获取允许更短超时）

    QNetworkReply *reply = m_nam->get(req);
    reply->setParent(this);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (reply->error() == QNetworkReply::NoError) {
            QImage img;
            if (img.loadFromData(reply->readAll()))
                emit frameReady(img);
        }
        reply->deleteLater();
    });
}

/**
 * @brief GET /status — 检测视觉服务连通性
 *
 * 解析响应中的 camera_running 字段判断相机是否就绪。
 * 结果通过 statusChanged(bool, QString) 信号异步通知。
 */
void VisionHttpClient::checkStatus()
{
    if (!isConfigured()) {
        emit statusChanged(false, QStringLiteral("未配置 IP"));
        return;
    }

    QNetworkRequest req(QUrl(
        QString("http://%1:%2/status").arg(m_ip).arg(m_port)));
    req.setTransferTimeout(5000);

    QNetworkReply *reply = m_nam->get(req);
    reply->setParent(this);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (reply->error() == QNetworkReply::NoError) {
            const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            if (doc.isObject()) {
                const bool camRunning = doc.object().value("camera_running").toBool();
                const double fps      = doc.object().value("fps").toDouble();
                emit statusChanged(camRunning,
                    camRunning
                        ? QString("在线 %1 (%2 fps)").arg(m_ip).arg(fps, 0, 'f', 1)
                        : QStringLiteral("相机未就绪"));
            } else {
                emit statusChanged(false, QStringLiteral("响应解析失败"));
            }
        } else {
            emit statusChanged(false,
                QString("连接失败: %1").arg(reply->errorString()));
        }
        reply->deleteLater();
    });
}

// ── 推理结果解析 ─────────────────────────────────────────────

namespace {

bool readFiniteNumber(const QJsonValue &value, double *result)
{
    if (!result || !value.isDouble())
        return false;
    const double number = value.toDouble();
    if (!qIsFinite(number))
        return false;
    *result = number;
    return true;
}

double centerDistSq(const VisionHttpClient::TargetCandidate &candidate)
{
    return candidate.x * candidate.x + candidate.y * candidate.y;
}

double distanceSq(double x, double y)
{
    return x * x + y * y;
}

double lockSideValue(const VisionHttpClient::TargetCandidate &candidate,
                     const VisionHttpClient::TargetSelectionContext &context)
{
    return context.lockFixedSideAxisY ? candidate.y : candidate.x;
}

bool isBetterFixedSideCandidate(const VisionHttpClient::TargetCandidate &candidate,
                                const VisionHttpClient::TargetCandidate &best,
                                const VisionHttpClient::TargetSelectionContext &context)
{
    const double candidateValue = lockSideValue(candidate, context);
    const double bestValue = lockSideValue(best, context);
    if (context.lockFixedSidePickMin) {
        if (candidateValue < bestValue)
            return true;
        if (candidateValue > bestValue)
            return false;
    } else {
        if (candidateValue > bestValue)
            return true;
        if (candidateValue < bestValue)
            return false;
    }
    return candidate.sourceIndex < best.sourceIndex;
}

QList<int> sameLayerIndexesFor(const QList<VisionHttpClient::TargetCandidate> &candidates,
                               const QList<int> &candidateIndexes,
                               double sameLayerTol)
{
    QList<int> sameLayerIndexes;
    if (candidateIndexes.isEmpty())
        return sameLayerIndexes;

    double highestDepth = candidates.at(candidateIndexes.first()).depth;
    for (int candidateIndex : candidateIndexes)
        highestDepth = qMin(highestDepth, candidates.at(candidateIndex).depth);

    for (int candidateIndex : candidateIndexes) {
        if (qAbs(candidates.at(candidateIndex).depth - highestDepth) <= sameLayerTol)
            sameLayerIndexes.append(candidateIndex);
    }
    return sameLayerIndexes;
}

int chooseFixedSideCandidate(const QList<VisionHttpClient::TargetCandidate> &candidates,
                             const QList<int> &candidateIndexes,
                             const VisionHttpClient::TargetSelectionContext &context)
{
    int bestIndex = candidateIndexes.first();
    for (int candidateIndex : candidateIndexes) {
        if (isBetterFixedSideCandidate(candidates.at(candidateIndex),
                                       candidates.at(bestIndex),
                                       context)) {
            bestIndex = candidateIndex;
        }
    }
    return bestIndex;
}

struct ToolCoords {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double rz = 0.0;
};

ToolCoords transformWithMatrix(const float matrix[4][4],
                               double cx,
                               double cy,
                               double cz,
                               double angleDeg)
{
    const float in[4] = {
        static_cast<float>(cx),
        static_cast<float>(cy),
        static_cast<float>(cz),
        1.0f
    };
    float out[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c)
            out[r] += matrix[r][c] * in[c];
    }

    float normAngle = std::fmod(static_cast<float>(angleDeg), 180.0f);
    if (normAngle < 0.0f)
        normAngle += 180.0f;
    if (normAngle > 90.0f)
        normAngle -= 180.0f;

    return {
        static_cast<double>(out[0]),
        static_cast<double>(out[1]),
        static_cast<double>(out[2]),
        static_cast<double>(normAngle)
    };
}

QString selectionReasonText(VisionHttpClient::TargetSelectionReason reason)
{
    using Reason = VisionHttpClient::TargetSelectionReason;
    switch (reason) {
    case Reason::HighestLayer:
        return QStringLiteral("最高层");
    case Reason::SameLayerNearestCenter:
        return QStringLiteral("同层最近中心");
    case Reason::StableSourceIndex:
        return QStringLiteral("原始下标稳定兜底");
    case Reason::AnchorHighestLayer:
        return QStringLiteral("锚点最高层");
    case Reason::AnchorSameLayerNearest:
        return QStringLiteral("锚点同层最近");
    case Reason::AnchorDistanceTooFar:
        return QStringLiteral("最高目标离拍照锚点过远，目标不可信");
    case Reason::AnchorTargetJumpTooFar:
        return QStringLiteral("目标跳变过大，目标不可信");
    case Reason::LockInitialHighestLayer:
        return QStringLiteral("锁定初始最高层");
    case Reason::LockInitialFixedSide:
        return QStringLiteral("锁定初始同层固定侧");
    case Reason::LockTrackingTarget:
        return QStringLiteral("锁定目标连续");
    case Reason::LockTrackingFixedSide:
        return QStringLiteral("锁定同层固定侧");
    case Reason::LockTargetMissing:
        return QStringLiteral("锁定目标暂时丢失，拒绝切换旁站目标");
    case Reason::LockTargetLost:
        return QStringLiteral("锁定目标连续丢失达到上限，拒绝切换旁站目标");
    case Reason::None:
        return QStringLiteral("无目标");
    }
    return QStringLiteral("无目标");
}

bool isAnchorTrustRejection(VisionHttpClient::TargetSelectionReason reason)
{
    using Reason = VisionHttpClient::TargetSelectionReason;
    return reason == Reason::AnchorDistanceTooFar
        || reason == Reason::AnchorTargetJumpTooFar
        || reason == Reason::LockTargetMissing
        || reason == Reason::LockTargetLost;
}

} // namespace

VisionHttpClient::TargetSelection VisionHttpClient::selectTarget(const QJsonArray &objects)
{
    TargetSelection selection;
    QList<int> insideIndexes;

    for (int sourceIndex = 0; sourceIndex < objects.size(); ++sourceIndex) {
        TargetCandidate candidate;
        candidate.sourceIndex = sourceIndex;
        const QJsonValue entry = objects.at(sourceIndex);
        if (!entry.isObject()) {
            candidate.rejectionReason = QStringLiteral("目标不是JSON对象");
            selection.candidates.append(candidate);
            continue;
        }

        const QJsonObject object = entry.toObject();
        const QJsonValue offsetValue = object.value(QStringLiteral("offset_mm"));
        if (!offsetValue.isObject()) {
            candidate.rejectionReason = QStringLiteral("offset_mm不是对象");
            selection.candidates.append(candidate);
            continue;
        }

        const QJsonObject offset = offsetValue.toObject();
        if (!readFiniteNumber(offset.value(QStringLiteral("x")), &candidate.x)
            || !readFiniteNumber(offset.value(QStringLiteral("y")), &candidate.y)
            || !readFiniteNumber(object.value(QStringLiteral("depth_compensated")), &candidate.depth)
            || !readFiniteNumber(object.value(QStringLiteral("angle")), &candidate.angle)
            || !readFiniteNumber(object.value(QStringLiteral("confidence")), &candidate.confidence)) {
            candidate.rejectionReason = QStringLiteral("字段缺失或不是有限数值");
            selection.candidates.append(candidate);
            continue;
        }

        candidate.valid = true;
        candidate.insideStationRoi = qAbs(candidate.x) <= VISION_STATION_ROI_HALF_X_MM
            && qAbs(candidate.y) <= VISION_STATION_ROI_HALF_Y_MM;
        selection.candidates.append(candidate);
        if (candidate.insideStationRoi)
            insideIndexes.append(selection.candidates.size() - 1);
    }

    if (insideIndexes.isEmpty())
        return selection;

    double highestDepth = selection.candidates.at(insideIndexes.first()).depth;
    for (int candidateIndex : insideIndexes)
        highestDepth = qMin(highestDepth, selection.candidates.at(candidateIndex).depth);

    QList<int> sameLayerIndexes;
    for (int candidateIndex : insideIndexes) {
        if (qAbs(selection.candidates.at(candidateIndex).depth - highestDepth)
            <= kSameLayerTolMm) {
            sameLayerIndexes.append(candidateIndex);
        }
    }

    int bestIndex = sameLayerIndexes.first();
    bool usedStableTieBreak = false;
    for (int candidateIndex : sameLayerIndexes) {
        const double candidateDistance = centerDistSq(selection.candidates.at(candidateIndex));
        const double bestDistance = centerDistSq(selection.candidates.at(bestIndex));
        if (candidateDistance < bestDistance) {
            bestIndex = candidateIndex;
            usedStableTieBreak = false;
        } else if (qFuzzyCompare(candidateDistance + 1.0, bestDistance + 1.0)
                   && selection.candidates.at(candidateIndex).sourceIndex
                       < selection.candidates.at(bestIndex).sourceIndex) {
            bestIndex = candidateIndex;
            usedStableTieBreak = true;
        } else if (candidateIndex != bestIndex
                   && qFuzzyCompare(candidateDistance + 1.0, bestDistance + 1.0)) {
            usedStableTieBreak = true;
        }
    }

    selection.selectedCandidateIndex = bestIndex;
    if (usedStableTieBreak) {
        selection.reason = TargetSelectionReason::StableSourceIndex;
    } else if (sameLayerIndexes.size() > 1) {
        selection.reason = TargetSelectionReason::SameLayerNearestCenter;
    } else {
        selection.reason = TargetSelectionReason::HighestLayer;
    }
    return selection;
}

VisionHttpClient::TargetSelection VisionHttpClient::selectTarget(
    const QJsonArray &objects,
    const TargetSelectionContext &context,
    const float handEyeMatrix[4][4])
{
    if (!context.anchorEnabled)
        return selectTarget(objects);

    TargetSelection selection;
    selection.lockContextActive = context.lockEnabled;
    selection.hasLockTarget = context.lockEnabled && context.hasPreviousAnchorTarget;
    selection.lockAnchorX = context.previousAnchorX;
    selection.lockAnchorY = context.previousAnchorY;
    selection.lockMissingFrames = context.lockMissingFrames;
    selection.maxLockMissingFrames = context.maxLockMissingFrames;
    QList<int> validIndexes;

    for (int sourceIndex = 0; sourceIndex < objects.size(); ++sourceIndex) {
        TargetCandidate candidate;
        candidate.sourceIndex = sourceIndex;
        const QJsonValue entry = objects.at(sourceIndex);
        if (!entry.isObject()) {
            candidate.rejectionReason = QStringLiteral("目标不是JSON对象");
            selection.candidates.append(candidate);
            continue;
        }

        const QJsonObject object = entry.toObject();
        const QJsonValue offsetValue = object.value(QStringLiteral("offset_mm"));
        if (!offsetValue.isObject()) {
            candidate.rejectionReason = QStringLiteral("offset_mm不是对象");
            selection.candidates.append(candidate);
            continue;
        }

        const QJsonObject offset = offsetValue.toObject();
        if (!readFiniteNumber(offset.value(QStringLiteral("x")), &candidate.x)
            || !readFiniteNumber(offset.value(QStringLiteral("y")), &candidate.y)
            || !readFiniteNumber(object.value(QStringLiteral("depth_compensated")), &candidate.depth)
            || !readFiniteNumber(object.value(QStringLiteral("angle")), &candidate.angle)
            || !readFiniteNumber(object.value(QStringLiteral("confidence")), &candidate.confidence)) {
            candidate.rejectionReason = QStringLiteral("字段缺失或不是有限数值");
            selection.candidates.append(candidate);
            continue;
        }

        const ToolCoords raw = transformWithMatrix(handEyeMatrix,
                                                   candidate.x,
                                                   candidate.y,
                                                   candidate.depth,
                                                   candidate.angle);
        candidate.valid = true;
        candidate.toolX = raw.x;
        candidate.toolY = raw.y;
        candidate.toolZ = raw.z;
        candidate.toolRz = raw.rz;
        candidate.alignmentX = candidate.toolX;
        candidate.alignmentY = -candidate.toolY;
        candidate.anchorX = context.accumulatedToolX + candidate.alignmentX;
        candidate.anchorY = context.accumulatedToolY + candidate.alignmentY;
        candidate.anchorDistance = std::sqrt(distanceSq(candidate.anchorX, candidate.anchorY));
        candidate.trusted = candidate.anchorDistance <= context.maxTrustDistance;
        if (context.lockEnabled && context.hasPreviousAnchorTarget) {
            candidate.lockDistance = std::sqrt(distanceSq(candidate.anchorX - context.previousAnchorX,
                                                          candidate.anchorY - context.previousAnchorY));
            if (!selection.hasNearestLockDistance
                || candidate.lockDistance < selection.nearestLockDistance) {
                selection.nearestLockDistance = candidate.lockDistance;
                selection.hasNearestLockDistance = true;
            }
        }
        candidate.insideStationRoi = true;
        selection.candidates.append(candidate);
        validIndexes.append(selection.candidates.size() - 1);
    }

    if (validIndexes.isEmpty()) {
        if (context.lockEnabled && context.hasPreviousAnchorTarget) {
            const int nextMissingFrames = context.lockMissingFrames + 1;
            selection.lockMissingFrames = nextMissingFrames;
            selection.reason = nextMissingFrames >= context.maxLockMissingFrames
                ? TargetSelectionReason::LockTargetLost
                : TargetSelectionReason::LockTargetMissing;
        }
        return selection;
    }

    if (context.lockEnabled) {
        if (context.hasPreviousAnchorTarget) {
            QList<int> lockedIndexes;
            for (int candidateIndex : validIndexes) {
                const TargetCandidate &candidate = selection.candidates.at(candidateIndex);
                if (candidate.trusted && candidate.lockDistance <= context.lockTrackRadius)
                    lockedIndexes.append(candidateIndex);
                else
                    selection.candidates[candidateIndex].trusted = false;
            }

            if (lockedIndexes.isEmpty()) {
                const int nextMissingFrames = context.lockMissingFrames + 1;
                selection.lockMissingFrames = nextMissingFrames;
                selection.reason = nextMissingFrames >= context.maxLockMissingFrames
                    ? TargetSelectionReason::LockTargetLost
                    : TargetSelectionReason::LockTargetMissing;
                return selection;
            }

            int bestIndex = lockedIndexes.first();
            for (int candidateIndex : lockedIndexes) {
                const TargetCandidate &candidate = selection.candidates.at(candidateIndex);
                const TargetCandidate &best = selection.candidates.at(bestIndex);
                if (candidate.lockDistance < best.lockDistance) {
                    bestIndex = candidateIndex;
                } else if (qFuzzyCompare(candidate.lockDistance + 1.0, best.lockDistance + 1.0)
                           && candidate.sourceIndex < best.sourceIndex) {
                    // 极少数距离完全相等时按原始下标稳定兜底，避免同一输入在不同平台上选择不稳定。
                    bestIndex = candidateIndex;
                }
            }
            selection.selectedCandidateIndex = bestIndex;
            selection.lockMissingFrames = 0;
            selection.lockAnchorX = selection.candidates.at(bestIndex).anchorX;
            selection.lockAnchorY = selection.candidates.at(bestIndex).anchorY;
            selection.reason = TargetSelectionReason::LockTrackingTarget;
            return selection;
        }

        QList<int> trustedIndexes;
        for (int candidateIndex : validIndexes) {
            if (selection.candidates.at(candidateIndex).trusted)
                trustedIndexes.append(candidateIndex);
        }

        if (trustedIndexes.isEmpty()) {
            selection.reason = TargetSelectionReason::AnchorDistanceTooFar;
            return selection;
        }

        const QList<int> sameLayerIndexes =
            sameLayerIndexesFor(selection.candidates, trustedIndexes, context.lockSameLayerZTol);
        const int bestIndex = chooseFixedSideCandidate(selection.candidates, sameLayerIndexes, context);
        selection.selectedCandidateIndex = bestIndex;
        selection.lockMissingFrames = 0;
        selection.lockAnchorX = selection.candidates.at(bestIndex).anchorX;
        selection.lockAnchorY = selection.candidates.at(bestIndex).anchorY;
        selection.reason = sameLayerIndexes.size() > 1
            ? TargetSelectionReason::LockInitialFixedSide
            : TargetSelectionReason::LockInitialHighestLayer;
        return selection;
    }

    double highestDepth = selection.candidates.at(validIndexes.first()).depth;
    for (int candidateIndex : validIndexes)
        highestDepth = qMin(highestDepth, selection.candidates.at(candidateIndex).depth);

    QList<int> sameLayerIndexes;
    for (int candidateIndex : validIndexes) {
        if (qAbs(selection.candidates.at(candidateIndex).depth - highestDepth)
            <= context.sameLayerZTol) {
            sameLayerIndexes.append(candidateIndex);
        }
    }

    int bestIndex = sameLayerIndexes.first();
    for (int candidateIndex : sameLayerIndexes) {
        const TargetCandidate &candidate = selection.candidates.at(candidateIndex);
        const TargetCandidate &best = selection.candidates.at(bestIndex);
        if (candidate.anchorDistance < best.anchorDistance) {
            bestIndex = candidateIndex;
        } else if (qFuzzyCompare(candidate.anchorDistance + 1.0, best.anchorDistance + 1.0)
                   && candidate.sourceIndex < best.sourceIndex) {
            bestIndex = candidateIndex;
        }
    }

    TargetCandidate &best = selection.candidates[bestIndex];
    if (best.anchorDistance > context.maxTrustDistance) {
        best.trusted = false;
        selection.reason = TargetSelectionReason::AnchorDistanceTooFar;
        return selection;
    }

    if (context.hasPreviousAnchorTarget) {
        const double jump = std::sqrt(distanceSq(best.anchorX - context.previousAnchorX,
                                                 best.anchorY - context.previousAnchorY));
        if (jump > context.maxSwitchDistance) {
            best.trusted = false;
            selection.reason = TargetSelectionReason::AnchorTargetJumpTooFar;
            return selection;
        }
    }

    selection.selectedCandidateIndex = bestIndex;
    selection.reason = sameLayerIndexes.size() > 1
        ? TargetSelectionReason::AnchorSameLayerNearest
        : TargetSelectionReason::AnchorHighestLayer;
    return selection;
}

QString VisionHttpClient::formatTargetSelectionLog(const TargetSelection &selection)
{
    QStringList candidateParts;
    int insideCount = 0;
    using Reason = VisionHttpClient::TargetSelectionReason;
    const bool includeAnchorText = selection.reason == Reason::AnchorHighestLayer
        || selection.reason == Reason::AnchorSameLayerNearest
        || selection.reason == Reason::AnchorDistanceTooFar
        || selection.reason == Reason::AnchorTargetJumpTooFar
        || selection.reason == Reason::LockInitialHighestLayer
        || selection.reason == Reason::LockInitialFixedSide
        || selection.reason == Reason::LockTrackingTarget
        || selection.reason == Reason::LockTrackingFixedSide
        || selection.reason == Reason::LockTargetMissing
        || selection.reason == Reason::LockTargetLost;
    for (const TargetCandidate &candidate : selection.candidates) {
        if (!candidate.valid) {
            candidateParts.append(QStringLiteral("#%1 非法(%2)")
                                      .arg(candidate.sourceIndex)
                                      .arg(candidate.rejectionReason));
            continue;
        }
        if (candidate.insideStationRoi)
            ++insideCount;
        QString anchorText;
        if (includeAnchorText) {
            anchorText = selection.lockContextActive
                ? QStringLiteral(" tool=(%1,%2,%3) anchor=(%4,%5) dist=%6 lockdist=%7 %8")
                      .arg(candidate.toolX, 0, 'f', 1)
                      .arg(candidate.toolY, 0, 'f', 1)
                      .arg(candidate.toolZ, 0, 'f', 1)
                      .arg(candidate.anchorX, 0, 'f', 1)
                      .arg(candidate.anchorY, 0, 'f', 1)
                      .arg(candidate.anchorDistance, 0, 'f', 1)
                      .arg(candidate.lockDistance, 0, 'f', 1)
                      .arg(candidate.trusted ? QStringLiteral("可信") : QStringLiteral("不可信"))
                : QStringLiteral(" tool=(%1,%2,%3) anchor=(%4,%5) dist=%6 %7")
                      .arg(candidate.toolX, 0, 'f', 1)
                      .arg(candidate.toolY, 0, 'f', 1)
                      .arg(candidate.toolZ, 0, 'f', 1)
                      .arg(candidate.anchorX, 0, 'f', 1)
                      .arg(candidate.anchorY, 0, 'f', 1)
                      .arg(candidate.anchorDistance, 0, 'f', 1)
                      .arg(candidate.trusted ? QStringLiteral("可信") : QStringLiteral("不可信"));
        }
        candidateParts.append(QStringLiteral("#%1 x=%2 y=%3 z=%4 %5%6")
                                  .arg(candidate.sourceIndex)
                                  .arg(candidate.x, 0, 'f', 1)
                                  .arg(candidate.y, 0, 'f', 1)
                                  .arg(candidate.depth, 0, 'f', 1)
                                  .arg(candidate.insideStationRoi
                                           ? QStringLiteral("范围内")
                                           : QStringLiteral("范围外"))
                                  .arg(anchorText));
    }

    const QString selectedText = selection.hasTarget()
        ? QStringLiteral("#%1")
              .arg(selection.candidates.at(selection.selectedCandidateIndex).sourceIndex)
        : QStringLiteral("无");
    QString lockText;
    if (selection.lockContextActive) {
        const QString nearestText = selection.hasNearestLockDistance
            ? QString::number(selection.nearestLockDistance, 'f', 1)
            : QStringLiteral("无候选");
        lockText = QStringLiteral(" 锁定=(%1,%2) 最近锁定距离=%3 丢失=%4/%5")
                       .arg(selection.lockAnchorX, 0, 'f', 1)
                       .arg(selection.lockAnchorY, 0, 'f', 1)
                       .arg(nearestText)
                       .arg(selection.lockMissingFrames)
                       .arg(selection.maxLockMissingFrames);
    }
    return QStringLiteral("[视觉选择] 候选数=%1 范围内=%2%3 [%4] 选中=%5 原因=%6")
        .arg(selection.candidates.size())
        .arg(insideCount)
        .arg(lockText)
        .arg(candidateParts.join(QStringLiteral("; ")))
        .arg(selectedText)
        .arg(selectionReasonText(selection.reason));
}

/**
 * @brief 解析 GET /inference 的 JSON 响应
 *
 * JSON 结构（关键字段）：
 * {
 *   "object_count": 1,
 *   "objects": [{
 *     "offset_mm": {"x": 10.5, "y": -5.2},  // 相对图像中心偏移（mm）
 *     "depth_compensated": 389.0,             // 补偿后深度（mm）
 *     "angle": 45.2,                          // 物体旋转角（度）
 *     "confidence": 0.95                      // 置信度
 *   }]
 * }
 *
 * 多目标抓取优先级：先过滤当前工位 ROI，再按最高层、同层最近中心、
 * 原始下标稳定兜底的确定性规则选择。
 */
void VisionHttpClient::parseInferenceReply(QNetworkReply *reply)
{
    if (reply->error() != QNetworkReply::NoError) {
        emit errorOccurred(QString("[视觉] 网络错误: %1").arg(reply->errorString()));
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (doc.isNull() || !doc.isObject()) {
        emit errorOccurred(QStringLiteral("[视觉] JSON 解析失败（响应非 JSON）"));
        return;
    }

    const QJsonArray objects = doc.object().value(QStringLiteral("objects")).toArray();
    const TargetSelection selection = selectTarget(objects, m_targetSelectionContext, m_T);
    emit selectionLogMessage(formatTargetSelectionLog(selection));
    if (!selection.hasTarget()) {
        if (isAnchorTrustRejection(selection.reason)) {
            // 检测到目标但被锚点可信规则拒绝，不能复用普通无目标信号，否则调度器会搜索下移。
            emit targetRejectedByTrustRule(selection.reason,
                QStringLiteral("[视觉] 目标被拍照锚点可信规则拒绝：%1")
                    .arg(selectionReasonText(selection.reason)));
            return;
        }
        emit noObjectDetected();
        return;
    }

    const TargetCandidate &candidate =
        selection.candidates.at(selection.selectedCandidateIndex);
    const float cx = static_cast<float>(candidate.x);          // 原始视觉 X 偏移，单位 mm。
    const float cy = static_cast<float>(candidate.y);          // 原始视觉 Y 偏移，单位 mm。
    const float cz = static_cast<float>(candidate.depth);      // 补偿深度，单位 mm。
    const float angle = static_cast<float>(candidate.angle);   // 箱体角度，单位 deg。
    const float conf = static_cast<float>(candidate.confidence); // 仅记录，不新增置信度阈值。
    Q_UNUSED(conf)

    RawCoords raw;
    if (m_targetSelectionContext.anchorEnabled) {
        raw = {candidate.toolX, candidate.toolY, candidate.toolZ, candidate.toolRz};
    } else {
        raw = transformToMm(cx, cy, cz, angle);
    }
    emit rawCoordinatesReady(raw.x, raw.y, raw.z, raw.rz);
    if (m_targetSelectionContext.anchorEnabled) {
        // 锚点坐标为相对阶段一初始拍照位的工具系 XY(mm)，供调度器闭环累计上一帧目标。
        emit rawCoordinatesReady(raw.x, raw.y, raw.z, raw.rz,
                                 candidate.anchorX, candidate.anchorY);
    }

    const qint32 regX  = qRound(raw.x * kCoordScale);
    const qint32 regY  = qRound(raw.y * kCoordScale);
    const qint32 regZ  = qRound(raw.z * kCoordScale);
    const qint32 regRz = m_baseRzReg + qRound(raw.rz * kRzScale);
    emit coordinatesReady({
        static_cast<quint16>(regX),   static_cast<quint16>(regY),
        static_cast<quint16>(regZ),   static_cast<quint16>(m_baseRxReg),
        static_cast<quint16>(m_baseRyReg), static_cast<quint16>(regRz)
    });
}

// ── 坐标变换 ─────────────────────────────────────────────────

VisionHttpClient::RawCoords VisionHttpClient::transformToMm(
    float cx, float cy, float cz, float angleDeg)
{
    const float in[4]  = { cx, cy, cz, 1.0f };
    float       out[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r] += m_T[r][c] * in[c];

    float normAngle = std::fmod(angleDeg, 180.0f);
    if (normAngle < 0.0f)  normAngle += 180.0f;
    if (normAngle > 90.0f) normAngle -= 180.0f;

    return { static_cast<double>(out[0]),
             static_cast<double>(out[1]),
             static_cast<double>(out[2]),
             static_cast<double>(normAngle) };
}

/**
 * @brief 将相机坐标转换为机器人 Holding 寄存器值（6 个 quint16）
 *
 * 步骤：
 *   1. 应用手眼矩阵 T_tool_cam（相机系 → 工具系，mm）
 *   2. 乘以 kCoordScale（默认 1000）→ 0.001mm 单位
 *   3. 强制转 quint16（负值通过补码表示，机器人读时解释为 int16_t）
 *   4. Rz = baseRzReg + normalize(angle) × kRzScale
 *
 * ⚠ 若机器人坐标精度不是 0.001mm/unit，修改 kCoordScale 常量。
 * ⚠ 若 Rz 旋转方向与实际相反，将 normAngle 取反。
 */
QList<quint16> VisionHttpClient::transformToRegisters(
    float cx, float cy, float cz, float angleDeg)
{
    const RawCoords raw = transformToMm(cx, cy, cz, angleDeg);

    // ── 单位转换：mm → 寄存器值（kCoordScale = 1000 → 0.001mm/unit）─
    const qint32 regX  = qRound(raw.x * kCoordScale);
    const qint32 regY  = qRound(raw.y * kCoordScale);
    const qint32 regZ  = qRound(raw.z * kCoordScale);
    const qint32 regRx = m_baseRxReg;
    const qint32 regRy = m_baseRyReg;
    // ⚠ 若 Rz 旋转方向与实际相反，改为 m_baseRzReg - qRound(raw.rz * kRzScale)
    const qint32 regRz = m_baseRzReg + qRound(raw.rz * kRzScale);

    // 打包为 quint16（保留补码，机器人按 int16_t 解释负值）
    return {
        static_cast<quint16>(regX), static_cast<quint16>(regY),
        static_cast<quint16>(regZ), static_cast<quint16>(regRx),
        static_cast<quint16>(regRy), static_cast<quint16>(regRz),
    };
}
