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

#include <QObject>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>

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

    bool    isConfigured() const { return !m_ip.isEmpty(); }
    QString ip()           const { return m_ip; }
    int     port()         const { return m_port; }

public slots:
    /**
     * @brief 发起一次推理结果查询（GET /inference）
     *
     * 成功且有目标：解析第一个目标，应用手眼变换后
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
    /// 网络或 JSON 解析错误
    void errorOccurred(QString msg);

private:
    /// 解析 /inference 响应 JSON
    void parseInferenceReply(QNetworkReply *reply);

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
};

#endif // VISIONCLIENT_H
