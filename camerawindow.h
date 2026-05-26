/**
 * @file camerawindow.h
 * @brief 奥比中光相机实时画面窗口
 *
 * 通过子进程运行 camera_stream.py（调用 pyorbbecsdk），
 * 解析 Python 脚本通过 stdout 输出的自定义帧协议，
 * 将解码后的图像帧渲染到对话框中央。
 *
 * 帧传输协议（文本头 + 二进制体）：
 *   <帧字节数>\n
 *   <JPEG 二进制数据>
 * Python 端循环发送，Qt 端通过 QByteArray 缓冲区逐帧解析。
 */

#ifndef CAMERAWINDOW_H
#define CAMERAWINDOW_H

#include <QDialog>
#include <QLabel>
#include <QProcess>

/**
 * @brief 奥比相机实时画面对话框
 *
 * 生命周期：打开时启动 Python 子进程，关闭时终止子进程。
 * 若 Python 脚本 3s 内未退出则强制 kill。
 *
 * 使用示例（从 MainWindow 打开）：
 * @code
 *   auto *cam = new CameraWindow("192.168.1.10", this);
 *   cam->setAttribute(Qt::WA_DeleteOnClose);
 *   cam->show();
 * @endcode
 */
class CameraWindow : public QDialog
{
    Q_OBJECT
public:
    /**
     * @brief 构造函数，同时启动相机 Python 子进程
     * @param ip     奥比相机网络 IP（透传给 camera_stream.py）
     * @param parent 父控件
     */
    explicit CameraWindow(const QString &ip, QWidget *parent = nullptr);

    /// 析构时自动停止 Python 子进程
    ~CameraWindow() override;

private slots:
    /// 子进程有数据写入 stdout 时触发，解析帧头 + 图像体
    void onReadyRead();

    /// 子进程启动失败（例如 Python 未安装）时触发
    void onProcessError(QProcess::ProcessError err);

private:
    /// 启动 Python 子进程，注册 stdout/error 信号
    void startStream();

    /// 停止 Python 子进程（先 terminate，超时后 kill）
    void stopStream();

    QLabel    *m_label        = nullptr; ///< 图像显示区域（全窗口填充）
    QProcess  *m_proc         = nullptr; ///< Python 子进程句柄
    QString    m_ip;                     ///< 相机 IP（传给 Python 脚本的命令行参数）
    QByteArray m_buffer;                 ///< stdout 缓冲区（跨 readyRead 累积数据）
    int        m_expectedSize = 0;       ///< 当前帧的预期字节数（0=等待帧头）
};

#endif // CAMERAWINDOW_H
