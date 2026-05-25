#ifndef CAMERAWINDOW_H
#define CAMERAWINDOW_H

#include <QDialog>
#include <QLabel>
#include <QProcess>

class CameraWindow : public QDialog
{
    Q_OBJECT
public:
    explicit CameraWindow(const QString &ip, QWidget *parent = nullptr);
    ~CameraWindow() override;

private slots:
    void onReadyRead();
    void onProcessError(QProcess::ProcessError err);

private:
    void startStream();
    void stopStream();

    QLabel    *m_label = nullptr;
    QProcess  *m_proc  = nullptr;
    QString    m_ip;
    QByteArray m_buffer;
    int        m_expectedSize = 0;
};

#endif
