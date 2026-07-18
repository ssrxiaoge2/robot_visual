/**
 * @file visionclient.h
 * @brief VisionHttpClient 视觉服务 HTTP 客户端
 *
 * 对接视觉服务（main_angle_depth_samseg_depth_http.py）的 Flask HTTP API：
 *   GET /inference      → 目标检测 JSON，含位置/深度/角度，转换为机器人寄存器值
 *   GET /frame/annotated → 带标注框的 JPEG 图像（供相机预览窗口实时显示）
 *   GET /status         → 服务运行状态（连通性检测）
 *
 * ── 坐标转换流程 ─────────────────────────────────────────────
 *   1. 视觉 API 返回相机坐标系偏移量（offset_mm.x/y）和深度（depth_compensated）
 *   2. 应用手眼变换矩阵 T_tool_cam → 末端工具坐标系（mm）
 *   3. 乘以 kCoordScale（默认 1000）→ 0.001mm 单位 → quint16 寄存器值
 *   4. Rz = baseRzReg + normalize(angle_deg) × kRzScale
 *      规范化：将 angle 映射到 [-90°, 90°]，利用夹爪 180° 对称性
 *   5. Rx、Ry 为固定配置值（不随目标变化）
 *
 * ── 手眼矩阵说明 ─────────────────────────────────────────────
 *   T_tool_cam（4×4，行主序）：相机坐标系 → 末端工具坐标系
 *   来源：hand-eye/calibration_output/handeye_matrix.txt
 *   ⚠ 当前矩阵误差较大，重新标定后调用 setHandEyeMatrix() 更新
 *
 * ── 坐标系约定 ───────────────────────────────────────────────
 *   视觉 API offset_mm：+X=右，+Y=上（物理空间，已从图像坐标转换）
 *   depth_compensated：已对 blue_box(-111mm) / purple_box(-190mm) 补偿
 */

#ifndef VISIONCLIENT_H
#define VISIONCLIENT_H

#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>

// 当前工位视觉候选矩形的 X 半宽，单位 mm；只过滤原始 offset_mm.x，不参与机械臂运动限位。
#define VISION_STATION_ROI_HALF_X_MM 500.0

// 当前工位视觉候选矩形的 Y 半宽，单位 mm；只过滤原始 offset_mm.y，不参与机械臂运动限位。
#define VISION_STATION_ROI_HALF_Y_MM 500.0

// 最高层候选离初始拍照锚点的最大可信距离，单位 mm；现场根据正常 anchor distance 日志微调。
#define VISION_ANCHOR_MAX_TRUST_XY_MM 450.0

// 锚点选择的同层 Z 容差，单位 mm；必须远小于料箱层高，避免低层被当作同层。
#define VISION_ANCHOR_SAME_LAYER_Z_TOL_MM 20.0

// 闭环过程中本帧目标相对上一帧锚点位置的最大可信跳变，单位 mm。
#define VISION_ANCHOR_SWITCH_MAX_XY_MM 220.0

class VisionHttpClient : public QObject
{
    Q_OBJECT
public:
    // ── 寄存器单位转换常量 ───────────────────────────────────
    /// 位置坐标倍率：mm → 寄存器值（0.001mm/unit → ×1000）
    static constexpr float kCoordScale = 1000.0f;
    /// 角度倍率：度 → 寄存器值（0.001°/unit → ×1000）
    /// ⚠ 若机器人角度寄存器单位不同（如 0.01°），修改此值
    static constexpr float kRzScale = 1000.0f;

    /// 多目标同层容差（mm）：深度差小于此值视为并排平放同层，改按离图像中心距离择近
    /// 须远小于料箱高度（110mm），仅覆盖深度噪声；少一层即差约 110mm，会判为不同层
    static constexpr float kSameLayerTolMm = 20.0f;

    /// 视觉目标最终入选原因；用于测试和现场候选摘要，不改变坐标转换语义。
    enum class TargetSelectionReason {
        None,                   ///< 没有合法且位于当前工位矩形内的目标。
        HighestLayer,           ///< 兼容旧逻辑：最高层分组中只有一个候选。
        SameLayerNearestCenter, ///< 兼容旧逻辑：最高层有多个候选，按当前中心择近。
        StableSourceIndex,      ///< 深度和 XY 距离完全相同时，按服务端原始下标稳定兜底。
        AnchorHighestLayer,     ///< 锚点逻辑：可信范围内只有一个最高层候选。
        AnchorSameLayerNearest, ///< 锚点逻辑：可信最高层有多个候选，按锚点 XY 最近选择。
        AnchorDistanceTooFar,   ///< 最高层候选离初始拍照锚点过远，目标不可信。
        AnchorTargetJumpTooFar  ///< 闭环目标相对上一帧锚点位置跳变过大，目标不可信。
    };

    /// 阶段一固定拍照锚点选择上下文；由 HuayanScheduler 在每次推理前注入。
    struct TargetSelectionContext {
        bool anchorEnabled = false; ///< false 时使用兼容旧选择逻辑。
        double accumulatedToolX = 0.0; ///< 初始拍照位到当前相机位置的已完成工具系 X 位移(mm)。
        double accumulatedToolY = 0.0; ///< 初始拍照位到当前相机位置的已完成工具系 Y 位移(mm)。
        bool hasPreviousAnchorTarget = false; ///< 是否有上一帧可信目标用于跳变保护。
        double previousAnchorX = 0.0; ///< 上一帧可信目标相对初始拍照锚点的 X(mm)。
        double previousAnchorY = 0.0; ///< 上一帧可信目标相对初始拍照锚点的 Y(mm)。
        double maxTrustDistance = VISION_ANCHOR_MAX_TRUST_XY_MM; ///< 目标可信最大锚点距离(mm)。
        double sameLayerZTol = VISION_ANCHOR_SAME_LAYER_Z_TOL_MM; ///< 同层 Z 容差(mm)。
        double maxSwitchDistance = VISION_ANCHOR_SWITCH_MAX_XY_MM; ///< 闭环目标最大跳变(mm)。
    };

    /// 单个视觉候选；坐标仍处于视觉 API 的原始 offset/depth 空间，单位均为 mm。
    struct TargetCandidate {
        int sourceIndex = -1;        ///< 候选在原始 objects 数组中的下标，用于日志和稳定兜底。
        double x = 0.0;              ///< 原始 offset_mm.x，单位 mm。
        double y = 0.0;              ///< 原始 offset_mm.y，单位 mm。
        double depth = 0.0;          ///< 原始 depth_compensated，值越小代表层越高。
        double angle = 0.0;          ///< 原始箱体角度，单位 deg。
        double confidence = 0.0;     ///< 视觉服务置信度，仅记录，不在本次新增阈值过滤。
        bool valid = false;          ///< 所有抓取所需 JSON 字段均存在、为数值且有限。
        bool insideStationRoi = false; ///< 合法目标是否位于共用当前工位 XY 矩形内。
        QString rejectionReason;     ///< 非法目标的首个拒绝原因；合法目标为空。
        double toolX = 0.0;           ///< 候选经手眼矩阵转换后的工具系 X 偏移(mm)。
        double toolY = 0.0;           ///< 候选经手眼矩阵转换后的工具系 Y 偏移(mm)。
        double toolZ = 0.0;           ///< 候选经手眼矩阵转换后的工具系 Z 深度(mm)。
        double toolRz = 0.0;          ///< 候选经角度规范化后的工具系 Rz(deg)。
        double alignmentX = 0.0;      ///< 为对准候选需要执行的工具系 X 位移(mm)。
        double alignmentY = 0.0;      ///< 为对准候选需要执行的工具系 Y 位移(mm)，按阶段一 Y 取反规则计算。
        double anchorX = 0.0;         ///< 候选相对初始拍照锚点的 X(mm)。
        double anchorY = 0.0;         ///< 候选相对初始拍照锚点的 Y(mm)。
        double anchorDistance = 0.0;  ///< 候选离初始拍照锚点的 XY 距离(mm)。
        bool trusted = true;          ///< 锚点逻辑下候选是否通过最终可信检查。
    };

    /// 一次推理响应的完整选择结果；selectedCandidateIndex 指向 candidates，而非原 JSON。
    struct TargetSelection {
        QList<TargetCandidate> candidates; ///< 保留所有原始候选的解析/ROI 状态，供单行日志使用。
        int selectedCandidateIndex = -1;   ///< 最终候选在 candidates 中的下标；-1 表示无目标。
        TargetSelectionReason reason = TargetSelectionReason::None; ///< 最终选择规则。

        bool hasTarget() const
        {
            return selectedCandidateIndex >= 0
                && selectedCandidateIndex < candidates.size();
        }
    };

    /// 纯逻辑：先过滤当前工位 XY，再找最高层，最后在同层中选择最近中心目标。
    static TargetSelection selectTarget(const QJsonArray &objects);
    /// 纯逻辑：按固定拍照锚点上下文选择目标；handEyeMatrix 为 4x4 行主序矩阵。
    static TargetSelection selectTarget(const QJsonArray &objects,
                                        const TargetSelectionContext &context,
                                        const float handEyeMatrix[4][4]);
    /// 把一次选择的全部候选压缩为单行现场日志；不得包含换行符。
    static QString formatTargetSelectionLog(const TargetSelection &selection);

    explicit VisionHttpClient(QObject *parent = nullptr);

    // ── 服务器配置 ───────────────────────────────────────────

    /// 设置视觉服务器地址（默认端口 8080）
    void setServerUrl(const QString &ip, int port = 8080);

    /**
     * @brief 设置手眼变换矩阵 T_tool_cam（4×4，行主序，16 个浮点数）
     *
     * 将相机坐标系中的点变换到末端工具坐标系。
     * 格式：m[0..3]=第0行, m[4..7]=第1行, m[8..11]=第2行, m[12..15]=第3行
     * 平移部分单位为 mm，旋转部分为归一化旋转矩阵。
     */
    void setHandEyeMatrix(const float m[16]);

    /**
     * @brief 设置 Rz 基准寄存器值（angle=0° 时写入 Holding906 的值）
     *
     * 单位与机器人协议一致（例：0.001°/unit 时，180° → 传入 180000）
     * ⚠ 必须在联机调试时通过手动测试确定正确值
     */
    void setBaseRz(qint32 baseRzReg);

    /// 设置 Rx 固定寄存器值（Holding 904，不随目标变化）
    void setBaseRx(qint32 baseRxReg);

    /// 设置 Ry 固定寄存器值（Holding 905，不随目标变化）
    void setBaseRy(qint32 baseRyReg);

    /// 设置下一次 /inference 使用的目标选择上下文；由机械臂阶段一在发起推理前注入。
    void setTargetSelectionContext(const TargetSelectionContext &context);

    bool    isConfigured() const { return !m_ip.isEmpty(); }
    QString ip()           const { return m_ip; }
    int     port()         const { return m_port; }

public slots:
    /**
     * @brief 发起一次推理结果查询（GET /inference）
     *
     * 成功且有目标：按抓取优先级选出一个目标，应用手眼变换后
     *   emit coordinatesReady(6 个寄存器值：X Y Z Rx Ry Rz)
     * 目标数 == 0：emit noObjectDetected()
     * 网络/解析错误：emit errorOccurred(msg)
     */
    void fetchInference();

    /**
     * @brief 获取一帧带标注的图像（GET /frame/annotated）
     *
     * 成功：emit frameReady(QImage)
     * 供 CameraWindow 以固定帧率（~7fps）刷新显示
     */
    void fetchAnnotatedFrame();

    /**
     * @brief 检测视觉服务连通性（GET /status）
     *
     * 成功：emit statusChanged(true, "在线 <ip>") + camera_running 字段
     * 失败：emit statusChanged(false, "连接失败")
     */
    void checkStatus();

signals:
    /// 坐标就绪，values 为 6 个寄存器值 [X,Y,Z,Rx,Ry,Rz]（对应 Holding 901-906）
    void coordinatesReady(QList<quint16> values);
    /// 标注图像帧就绪（供 CameraWindow 显示）
    void frameReady(QImage image);
    /// 视觉服务连通性变化
    void statusChanged(bool available, QString msg);
    /// 本次推理未检测到任何目标
    void noObjectDetected();
    /// 视觉检测到候选但被阶段一拍照锚点可信规则拒绝；reason 精确区分距离过远/跳变过大，不能当作普通无目标搜索下移。
    void targetRejectedByTrustRule(TargetSelectionReason reason, QString message);
    /// 网络或 JSON 解析错误
    void errorOccurred(QString msg);
    /// 工具坐标系原始 mm 值（手眼变换后，未乘寄存器倍率）
    void rawCoordinatesReady(double x, double y, double z, double rz);
    /// 工具坐标系原始 mm 值，并带回本次选中目标相对初始拍照锚点的 XY(mm)。
    void rawCoordinatesReady(double x, double y, double z, double rz,
                             double anchorX, double anchorY);
    /// 每次 /inference 响应最多发出一次候选选择摘要，由 DeviceManager 转发到现场日志。
    void selectionLogMessage(QString message);
private:
    /// 解析 /inference 响应 JSON
    void parseInferenceReply(QNetworkReply *reply);

    struct RawCoords { double x, y, z, rz; };
    RawCoords transformToMm(float cx, float cy, float cz, float angleDeg);

    /**
     * @brief 将相机坐标转换为机器人 Holding 寄存器值（6 个 quint16）
     * @param cx       相机坐标系 X 偏移（mm，+右）
     * @param cy       相机坐标系 Y 偏移（mm，+上）
     * @param cz       物体深度（mm，已补偿箱体高度）
     * @param angleDeg 物体旋转角（度）
     */
    QList<quint16> transformToRegisters(float cx, float cy, float cz, float angleDeg);

    QNetworkAccessManager *m_nam = nullptr;
    QString m_ip;
    int     m_port = 8080;

    // 手眼变换矩阵（行主序 4×4，T_tool_cam）
    // 默认值：来自 hand-eye/calibration_output/handeye_matrix.txt
    float m_T[4][4];

    // Rx/Ry/Rz 基准寄存器值
    qint32 m_baseRxReg = 0;
    qint32 m_baseRyReg = 0;
    qint32 m_baseRzReg = 0; ///< ⚠ 需联机调试后设置实际值

    TargetSelectionContext m_targetSelectionContext; ///< 最近一次推理使用的选择上下文，生命周期到下一次 set 覆盖。
};

#endif // VISIONCLIENT_H
