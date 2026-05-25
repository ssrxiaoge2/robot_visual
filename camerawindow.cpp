#include "camerawindow.h"

#include <QVBoxLayout>
#include <QImage>
#include <QFile>

// 运行时检测 Python 可执行文件路径
// Linux 优先使用 AGV 专用 conda 环境，找不到则退回系统 python3
// Windows 使用 PATH 里的 python
static QString findPython()
{
#ifdef Q_OS_WIN
    return QStringLiteral("python");
#else
    if (QFile::exists(QStringLiteral("/home/agv/miniconda3/bin/python3")))
        return QStringLiteral("/home/agv/miniconda3/bin/python3");
    return QStringLiteral("python3");
#endif
}

static const QString kCameraScript = QStringLiteral(PROJECT_SOURCE_DIR "/camera_stream.py");

CameraWindow::CameraWindow(const QString &ip, QWidget *parent)
    : QDialog(parent), m_ip(ip)
{
    setWindowTitle(QStringLiteral("奥比相机 - %1").arg(ip));
    resize(960, 720);
    setMinimumSize(640, 480);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_label = new QLabel(this);
    m_label->setAlignment(Qt::AlignCenter);
    m_label->setScaledContents(true);
    m_label->setStyleSheet("background:#1a1a1a;");
    m_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_label->setText(QStringLiteral("正在连接相机 %1 ...").arg(ip));
    layout->addWidget(m_label);

    startStream();
}

CameraWindow::~CameraWindow()
{
    stopStream();
}

void CameraWindow::startStream()
{
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::SeparateChannels);

    connect(m_proc, &QProcess::readyReadStandardOutput, this, &CameraWindow::onReadyRead);
    connect(m_proc, &QProcess::errorOccurred,           this, &CameraWindow::onProcessError);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int, QProcess::ExitStatus) {
        m_label->setText(QStringLiteral("相机流已断开"));
    });

    m_proc->start(findPython(), QStringList() << kCameraScript << m_ip);
}

void CameraWindow::stopStream()
{
    if (!m_proc) return;
    m_proc->terminate();
    if (!m_proc->waitForFinished(3000))
        m_proc->kill();
}

void CameraWindow::onReadyRead()
{
    m_buffer.append(m_proc->readAllStandardOutput());

    while (true) {
        if (m_expectedSize == 0) {
            const int nl = m_buffer.indexOf('\n');
            if (nl < 0) break;

            bool ok    = false;
            int  size  = m_buffer.left(nl).toInt(&ok);
            m_buffer.remove(0, nl + 1);

            if (ok && size > 0 && size < 10 * 1024 * 1024)
                m_expectedSize = size;
        }

        if (m_expectedSize > 0 && m_buffer.size() >= m_expectedSize) {
            const QByteArray frameData = m_buffer.left(m_expectedSize);
            m_buffer.remove(0, m_expectedSize);
            m_expectedSize = 0;

            QImage img;
            if (img.loadFromData(frameData))
                m_label->setPixmap(QPixmap::fromImage(img));
        } else {
            break;
        }
    }
}

void CameraWindow::onProcessError(QProcess::ProcessError err)
{
    if (err == QProcess::FailedToStart)
        m_label->setText(QStringLiteral("无法启动相机脚本"));
}
