#pragma once

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QtGlobal>

// Qt 5.15/Qt 6 由 QNetworkRequest 提供“连续无数据传输”超时。
// Qt 5.12 没有该接口，在回复上附加可重置计时器实现同等语义。
inline void setNetworkTransferTimeout(QNetworkRequest &request, int timeoutMs)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(timeoutMs);
#else
    Q_UNUSED(request)
    Q_UNUSED(timeoutMs)
#endif
}

inline void attachNetworkTransferTimeout(QNetworkReply *reply, int timeoutMs)
{
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
    QTimer *timer = new QTimer(reply);
    timer->setSingleShot(true);
    timer->setInterval(timeoutMs);

    const auto restartTimer = [timer]() {
        timer->start();
    };
    QObject::connect(reply, &QIODevice::readyRead, timer, restartTimer);
    QObject::connect(reply, &QNetworkReply::downloadProgress, timer,
                     [restartTimer](qint64, qint64) { restartTimer(); });
    QObject::connect(reply, &QNetworkReply::uploadProgress, timer,
                     [restartTimer](qint64, qint64) { restartTimer(); });
    QObject::connect(reply, &QNetworkReply::finished,
                     timer, &QTimer::stop);
    QObject::connect(timer, &QTimer::timeout, reply, [reply]() {
        if (reply->isRunning())
            reply->abort();
    });
    timer->start();
#else
    Q_UNUSED(reply)
    Q_UNUSED(timeoutMs)
#endif
}
