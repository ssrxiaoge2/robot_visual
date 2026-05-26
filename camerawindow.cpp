/**
 * @file camerawindow.cpp
 * @brief CameraWindow 奥比相机实时画面对话框实现
 *
 * 帧解析协议说明：
 *   Python 端（camera_stream.py）每帧发送：
 *     1. 文本行：帧的 JPEG 字节数 + '\n'（例："45123\n"）
 *     2. 二进制体：JPEG 数据，长度即为上行声明的字节数
 *   Qt 端通过 m_buffer 累积 stdout 数据，逐帧解析：
 *     - m_expectedSize == 0：在缓冲区中查找 '\n'，解析帧头，记录期望字节数
 *     - m_expectedSize > 0：等待缓冲区积累到足够字节后，截取并解码 JPEG
 */

#include "camerawindow.h"

#include <QVBoxLayout>
#include <QImage>
#include <QFile>

// ── Python 路径检测 ───────────────────────────────────────────

/**
 * @brief 运行时检测 Python 可执行文件路径
 * @return Python 解释器的路径或命令名
 *
 * 优先级：
 *   Linux: /home/agv/miniconda3/bin/python3 （AGV 专用 conda 环境）
 *          → 找不到则退回系统 python3
 *   Windows: PATH 中的 python（开发环境）
 */
static QString findPython()
{
#ifdef Q_OS_WIN
    return QStringLiteral("python");
#else
    // 优先使用 AGV 设备上的 conda 环境（已安装 pyorbbecsdk）
    if (QFile::exists(QStringLiteral("/home/agv/miniconda3/bin/python3")))
        return QStringLiteral("/home/agv/miniconda3/bin/python3");
    return QStringLiteral("python3"); // 回退到系统 Python
#endif
}

/// camera_stream.py 的绝对路径（由 CMake 在编译期注入 PROJECT_SOURCE_DIR 宏）
static const QString kCameraScript = QStringLiteral(PROJECT_SOURCE_DIR "/camera_stream.py");

// ── 构造 / 析构 ───────────────────────────────────────────────

CameraWindow::CameraWindow(const QString &ip, QWidget *parent)
    : QDialog(parent), m_ip(ip)
{
    setWindowTitle(QStringLiteral("奥比相机 - %1").arg(ip));
    resize(960, 720);
    setMinimumSize(640, 480);

    // 图像显示区：铺满对话框，内容缩放，深色背景
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_label = new QLabel(this);
    m_label->setAlignment(Qt::AlignCenter);
    m_label->setScaledContents(true);
    m_label->setStyleSheet("background:#1a1a1a;");
    m_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_label->setText(QStringLiteral("正在连接相机 %1 ...").arg(ip));
    layout->addWidget(m_label);

    startStream(); // 构造时立即启动相机子进程
}

CameraWindow::~CameraWindow()
{
    stopStream(); // 确保子进程在窗口销毁前退出
}

// ── 子进程管理 ────────────────────────────────────────────────

void CameraWindow::startStream()
{
    m_proc = new QProcess(this);
    // 分离 stdout（图像数据）与 stderr（日志/报错），避免混淆
    m_proc->setProcessChannelMode(QProcess::SeparateChannels);

    // stdout 有新数据 → 解析帧
    connect(m_proc, &QProcess::readyReadStandardOutput,
            this, &CameraWindow::onReadyRead);
    // 子进程启动失败（Python 未找到等）
    connect(m_proc, &QProcess::errorOccurred,
            this, &CameraWindow::onProcessError);
    // 子进程正常退出（相机断连或脚本结束）
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int /*exitCode*/, QProcess::ExitStatus /*status*/) {
        m_label->setText(QStringLiteral("相机流已断开"));
    });

    // 启动：python camera_stream.py <ip>
    m_proc->start(findPython(), QStringList() << kCameraScript << m_ip);
}

void CameraWindow::stopStream()
{
    if (!m_proc) return;
    m_proc->terminate();                       // 发送 SIGTERM（Windows 上等同于 kill）
    if (!m_proc->waitForFinished(3000))        // 等待最多 3s
        m_proc->kill();                        // 超时仍未退出则强制终止
}

// ── 帧数据解析（stdout 回调）─────────────────────────────────

void CameraWindow::onReadyRead()
{
    // 将新到达的数据追加到缓冲区（数据可能跨多次 readyRead 到达）
    m_buffer.append(m_proc->readAllStandardOutput());

    // 循环处理缓冲区中已积累的完整帧
    while (true) {

        // ── 阶段 1：读帧头（等待换行符，解析帧字节数）────────
        if (m_expectedSize == 0) {
            const int nl = m_buffer.indexOf('\n');
            if (nl < 0) break; // 换行符未到，等待更多数据

            bool ok   = false;
            int  size = m_buffer.left(nl).toInt(&ok);
            m_buffer.remove(0, nl + 1); // 移除帧头（含换行符）

            // 合法性检查：必须是正整数，且不超过 10MB（防止恶意/损坏数据）
            if (ok && size > 0 && size < 10 * 1024 * 1024)
                m_expectedSize = size;
            // 非法帧头直接丢弃，继续尝试下一行
        }

        // ── 阶段 2：读帧体（等待完整 JPEG 字节到达）─────────
        if (m_expectedSize > 0 && m_buffer.size() >= m_expectedSize) {
            const QByteArray frameData = m_buffer.left(m_expectedSize);
            m_buffer.remove(0, m_expectedSize); // 从缓冲区移除已消费的帧
            m_expectedSize = 0;                 // 重置，准备接收下一帧帧头

            // 解码 JPEG 并更新显示
            QImage img;
            if (img.loadFromData(frameData))
                m_label->setPixmap(QPixmap::fromImage(img));
            // 解码失败（损坏帧）则静默丢弃，不影响后续帧
        } else {
            break; // 帧体数据不足，等待更多 stdout 数据
        }
    }
}

// ── 错误处理 ─────────────────────────────────────────────────

void CameraWindow::onProcessError(QProcess::ProcessError err)
{
    if (err == QProcess::FailedToStart) {
        // 最常见的错误：Python 不在 PATH 中，或脚本文件不存在
        m_label->setText(QStringLiteral("无法启动相机脚本（请检查 Python 环境）"));
    }
}
