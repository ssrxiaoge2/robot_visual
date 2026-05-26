/**
 * @file camerawindow.cpp
 * @brief CameraWindow 奥比相机实时画面（HTTP 轮询版）
 *
 * 帧获取流程：
 *   QTimer(150ms) → onTimerTick() → GET /frame/annotated
 *                                         ↓
 *   QNetworkReply::finished → onFrameReply() → QImage::loadFromData() → setPixmap()
 *
 * 防止请求堆积：
 *   若上一帧的 HTTP 请求尚未完成，本次跳过（m_pending != nullptr 时 return）。
 *   这样即使网络慢（>150ms/帧），也不会无限积压请求。
 *
 * 与旧版（Python 子进程）的区别：
 *   旧版：启动 camera_stream.py 子进程，通过 stdout 接收自定义帧协议（字节数\nJPEG）
 *   新版：直接 HTTP GET，更简单、更稳定、利用视觉服务已有的 Flask 路由
 *         返回的图像已含 YOLO 检测框、质心和旋转角度标注
 */

#include "camerawindow.h"

#include <QVBoxLayout>
#include <QStatusBar>
#include <QNetworkRequest>

// ── 构造 / 析构 ───────────────────────────────────────────────

CameraWindow::CameraWindow(const QString &ip, int port, QWidget *parent)
    : QDialog(parent)
    , m_url(QString("http://%1:%2/frame/annotated").arg(ip).arg(port))
{
    setWindowTitle(QString("视觉标注画面 — %1:%2").arg(ip).arg(port));
    resize(960, 720);
    setMinimumSize(640, 480);

    // ── 图像显示区（铺满对话框，保持宽高比，深色背景）───────
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_label = new QLabel(this);
    m_label->setAlignment(Qt::AlignCenter);
    m_label->setScaledContents(false); // 使用 Qt::KeepAspectRatio 缩放
    m_label->setStyleSheet("background:#1a1a1a; color:#888;");
    m_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_label->setText(QString("正在连接视觉服务 %1:%2 ...").arg(ip).arg(port));
    layout->addWidget(m_label);

    // ── HTTP 客户端 ──────────────────────────────────────────
    m_nam = new QNetworkAccessManager(this);
    // 统一在 QNAM 的 finished 信号中处理响应（不在各 reply 上单独 connect）
    connect(m_nam, &QNetworkAccessManager::finished,
            this, &CameraWindow::onFrameReply);

    // ── 轮询定时器（150ms = 约 7fps）─────────────────────────
    // 视觉服务运行在约 15fps，Qt 侧取 7fps 足够流畅，同时降低带宽压力
    m_timer = new QTimer(this);
    m_timer->setInterval(150);
    connect(m_timer, &QTimer::timeout, this, &CameraWindow::onTimerTick);
    m_timer->start();

    // 构造后立即发一帧（不等第一个 timer 周期）
    onTimerTick();
}

CameraWindow::~CameraWindow()
{
    m_timer->stop();

    // 若有正在进行的请求，中止并释放
    if (m_pending) {
        m_pending->abort();
        m_pending->deleteLater();
        m_pending = nullptr;
    }
}

// ── 帧请求与响应 ─────────────────────────────────────────────

/**
 * @brief 定时器触发：发起一次 GET /frame/annotated 请求
 *
 * 防堆积机制：若上一帧请求尚未完成（m_pending != nullptr），
 * 本次跳过，等待下一个定时器周期。这避免了慢网络下请求无限积压。
 */
void CameraWindow::onTimerTick()
{
    if (m_pending) return; // 上一帧还未返回，跳过本次

    QNetworkRequest req(m_url);
    req.setTransferTimeout(3000); // 3s 超时，防止单帧卡住所有后续帧
    m_pending = m_nam->get(req);
}

/**
 * @brief HTTP 响应到达：解码 JPEG 并更新显示
 * @param reply 已完成的 QNetworkReply（由 QNAM::finished 信号传入）
 *
 * 无论成功还是失败，都必须释放 reply 并清除 m_pending，
 * 确保下次 onTimerTick() 能发起新请求。
 */
void CameraWindow::onFrameReply(QNetworkReply *reply)
{
    // 仅处理当前期待的那个 reply（防止窗口关闭后残留 reply 触发此槽）
    if (reply != m_pending) {
        reply->deleteLater();
        return;
    }
    m_pending = nullptr; // 清除标记，允许下次轮询

    if (reply->error() == QNetworkReply::NoError) {
        QImage img;
        if (img.loadFromData(reply->readAll())) {
            ++m_frameCount;
            // 按窗口大小等比例缩放，不超出 label 边界
            const QPixmap pm = QPixmap::fromImage(img).scaled(
                m_label->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            m_label->setPixmap(pm);
            setWindowTitle(QString("视觉标注画面 — 帧 #%1").arg(m_frameCount));
        }
        // JPEG 解码失败（损坏帧）：静默丢弃，下次重新获取
    } else {
        // 网络错误：更新提示文字（不弹框，避免打断用户）
        if (reply->error() != QNetworkReply::OperationCanceledError) {
            // OperationCanceledError 是析构时主动 abort() 触发，不提示
            m_label->setText(QString("连接失败: %1\n将在下次定时器周期重试…")
                                 .arg(reply->errorString()));
        }
    }

    reply->deleteLater();
}
