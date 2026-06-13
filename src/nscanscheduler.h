#ifndef NSCAN_SCHEDULER_H
#define NSCAN_SCHEDULER_H

#include <QByteArray>
#include <QString>
#include <QtGlobal>

/**
 * @brief Synchronous wrapper for the N-ScanHub network scanner API.
 *
 * scan() performs lazy connect, trigger, bounded read, and retry in the calling
 * thread. Healthy connections are reused as recommended by the vendor demo;
 * failures and shutdown() release the socket. This class never creates a
 * thread. UI callers must invoke scan() from a worker thread.
 */
class NScanScheduler
{
public:
    NScanScheduler() = default;
    ~NScanScheduler();

    NScanScheduler(const NScanScheduler &) = delete;
    NScanScheduler &operator=(const NScanScheduler &) = delete;

    struct ScanOptions {
        QString ip;
        quint16 port = 30000;
        int timeoutMs = 2000;
        int maxAttempts = 3;
        int retryIntervalMs = 500;
        QByteArray triggerCommand = QByteArray(1, static_cast<char>(0x41));
    };

    struct ScanResult {
        enum class Status {
            Success,
            Timeout,
            ConnectError,
            SendError,
            ReadError,
            InvalidConfig,
            SdkUnavailable
        };

        Status status = Status::InvalidConfig;
        QString code;
        QByteArray rawData;
        QString errorMessage;
        int attempts = 0;

        bool isSuccess() const { return status == Status::Success; }
    };

    ScanResult scan(const ScanOptions &options) const;
    void shutdown();
    static QString statusText(ScanResult::Status status);

private:
    static ScanResult validateOptions(const ScanOptions &options);
    static QString decodeBarcode(const QByteArray &rawData);
    void closeConnection() const;

    // The SDK demo recommends reusing the TCP connection between scans.
    mutable int m_socket = -1;
    mutable QString m_connectedIp;
    mutable quint16 m_connectedPort = 0;
};

#endif // NSCAN_SCHEDULER_H
