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
#include <QtMath>

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
     0.99979039f, -0.01978854f,  0.00525241f, -28.62873261f,  // 第0行
     0.01977454f,  0.99980081f,  0.00270399f,   5.07318523f,  // 第1行
    -0.00530488f, -0.00259956f,  0.99998255f, 156.09358373f,  // 第2行
     0.00000000f,  0.00000000f,  0.00000000f,   1.00000000f,  // 第3行（齐次）
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
    // 将平铺的 16 个浮点数填入 4×4 数组（行主序）
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            m_T[r][c] = m[r * 4 + c];
}

void VisionHttpClient::setBaseRz(qint32 baseRzReg) { m_baseRzReg = baseRzReg; }
void VisionHttpClient::setBaseRx(qint32 baseRxReg) { m_baseRxReg = baseRxReg; }
void VisionHttpClient::setBaseRy(qint32 baseRyReg) { m_baseRyReg = baseRyReg; }

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
 * @brief GET /inference — 码垛区占用检测
 *
 * 只解析 object_count/objects/confidence，供码垛状态校验使用。
 * 不调用 parseInferenceReply()，因此不会 emit rawCoordinatesReady / coordinatesReady，
 * 也不会触发现有机械臂抓取偏移闭环。
 */
void VisionHttpClient::fetchPalletOccupancy(const QString &requestId)
{
    if (!isConfigured()) {
        emit palletOccupancyError(requestId, QStringLiteral("[视觉] 服务器地址未配置"));
        return;
    }

    QNetworkRequest req(QUrl(
        QString("http://%1:%2/inference").arg(m_ip).arg(m_port)));
    req.setTransferTimeout(5000);

    QNetworkReply *reply = m_nam->get(req);
    reply->setParent(this);
    connect(reply, &QNetworkReply::finished, this, [this, reply, requestId]() {
        if (reply->error() != QNetworkReply::NoError) {
            emit palletOccupancyError(
                requestId,
                QStringLiteral("[视觉] 码垛占用检测网络错误: %1")
                    .arg(reply->errorString()));
            reply->deleteLater();
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (doc.isNull() || !doc.isObject()) {
            emit palletOccupancyError(
                requestId,
                QStringLiteral("[视觉] 码垛占用检测 JSON 解析失败"));
            reply->deleteLater();
            return;
        }

        const QJsonObject root = doc.object();
        const int objCount = root.value("object_count").toInt(0);
        const QJsonArray objects = root.value("objects").toArray();
        double bestConfidence = 0.0;
        for (const QJsonValue &v : objects) {
            if (v.isObject())
                bestConfidence = qMax(bestConfidence,
                                      v.toObject().value("confidence").toDouble(0.0));
        }

        const bool occupied = objCount > 0 && !objects.isEmpty();
        const QString summary = QStringLiteral("object_count=%1, objects=%2, best_confidence=%3")
            .arg(objCount)
            .arg(objects.size())
            .arg(bestConfidence, 0, 'f', 3);
        emit palletOccupancyReady(requestId, occupied, objCount, bestConfidence, summary);
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

// 目标到图像中心的平面距离平方（mm²），用于同层时择近
static double centerDistSq(const QJsonObject &obj)
{
    const QJsonObject off = obj.value("offset_mm").toObject();
    const double x = off.value("x").toDouble();
    const double y = off.value("y").toDouble();
    return x * x + y * y;
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
 * 多目标抓取优先级：深度最小（离相机最近/堆叠最上层）优先；深度差在
 * kSameLayerTolMm 内视为并排平放同层，改取离图像中心最近者。
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

    const QJsonObject root = doc.object();
    const int objCount = root.value("object_count").toInt(0);

    if (objCount == 0) {
        emit noObjectDetected(); // 未检测到目标
        return;
    }

    const QJsonArray objects = root.value("objects").toArray();
    if (objects.isEmpty()) {
        emit noObjectDetected();
        return;
    }

    // 线性挑选最优抓取目标：深度小的优先，同层（深度差 < 容差）时离中心近的优先
    QJsonObject obj = objects.first().toObject();
    for (int i = 1; i < objects.size(); ++i) {
        const QJsonObject cand = objects.at(i).toObject();
        const double dz = cand.value("depth_compensated").toDouble()
                        - obj.value("depth_compensated").toDouble();
        const bool prefer = (qAbs(dz) >= kSameLayerTolMm)
                          ? dz < 0.0                                  // 不同层：候选更浅则优先
                          : centerDistSq(cand) < centerDistSq(obj);   // 同层：候选离中心更近则优先
        if (prefer)
            obj = cand;
    }

    const QJsonObject offsetMm = obj.value("offset_mm").toObject();

    const float cx    = static_cast<float>(offsetMm.value("x").toDouble());  // X 偏移（mm）
    const float cy    = static_cast<float>(offsetMm.value("y").toDouble());  // Y 偏移（mm）
    const float cz    = static_cast<float>(obj.value("depth_compensated").toDouble()); // Z 深度（mm）
    const float angle = static_cast<float>(obj.value("angle").toDouble());   // 旋转角（度）
    const float conf  = static_cast<float>(obj.value("confidence").toDouble());

    // 发出日志（供上层转发到 UI）
    // 注意：此信号没有 log，由调用方记录
    Q_UNUSED(conf) // 当前仅记录 confidence，不过滤

    const RawCoords raw = transformToMm(cx, cy, cz, angle);
    emit rawCoordinatesReady(raw.x, raw.y, raw.z, raw.rz);

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
