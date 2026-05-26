/**
 * @file camerawindow.h
 * @brief 奥比相机实时画面窗口（HTTP 版）
 *
 * 向视觉服务（main_angle_depth_samseg_depth_http.py）发起 HTTP GET 请求，
 * 定时获取带标注的 JPEG 帧并显示在对话框中。
 *
 * 通信方式：HTTP GET /frame/annotated（约 7fps，每 150ms 请求一帧）
 *   无需 Python 子进程，直接使用 QNetworkAccessManager 发请求。
 *   视觉服务同时运行 YOLO 检测，返回的图像已带检测框、质心、角度标注。
 *
 * 使用示例（从 MainWindow 打开）：
 * @code
 *   auto *cam = new CameraWindow("192.168.1.10", 8080, this);
 *   cam->setAttribute(Qt::WA_DeleteOnClose);
 *   cam->show();
 * @endcode
 */

#ifndef CAMERAWINDOW_H
#define CAMERAWINDOW_H

#include <QDialog>
#include <QLabel>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>

/**
 * @brief 奥比相机实时标注画面对话框
 *
 * 生命周期：
 *   构造时启动轮询定时器，析构（关闭）时停止定时器。
 *   若网络请求正在进行中，reply->abort() 确保资源释放。
 */
class CameraWindow : public QDialog
{
    Q_OBJECT
public:
    /**
     * @brief 构造函数，立即开始轮询标注图像
     * @param ip     视觉服务 IP（视觉工控机地址）
     * @param port   HTTP 端口（默认 8080）
     * @param parent 父控件
     */
    explicit CameraWindow(const QString &ip, int port = 8080, QWidget *parent = nullptr);
    ~CameraWindow() override;

private slots:
    /// 定时器触发 → 发起 GET /frame/annotated 请求
    void onTimerTick();

    /// HTTP 响应到达 → 解码 JPEG → 更新显示
    void onFrameReply(QNetworkReply *reply);

private:
    QLabel               *m_label   = nullptr; ///< 图像显示区域（全窗口填充）
    QTimer               *m_timer   = nullptr; ///< 轮询定时器（150ms，约 7fps）
    QNetworkAccessManager *m_nam    = nullptr; ///< HTTP 客户端
    QNetworkReply        *m_pending = nullptr; ///< 当前正在进行的请求（防止请求堆积）
    QString               m_url;              ///< /frame/annotated 完整 URL
    int                   m_frameCount = 0;   ///< 已接收帧数（用于调试/状态栏显示）
};

#endif // CAMERAWINDOW_H
