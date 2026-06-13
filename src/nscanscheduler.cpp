/**
 * @file nscanscheduler.cpp
 * @brief Synchronous N-ScanHub network scanner wrapper.
 */

#include "nscanscheduler.h"

#ifdef NSCAN_SDK_AVAILABLE
#include "N-ScanHub.h"
#endif

#include <QMutex>
#include <QMutexLocker>
#include <QStringDecoder>

#include <array>
#include <chrono>
#include <thread>

namespace {
constexpr int kReceiveBufferSize = 4096;
// The vendor documentation does not guarantee concurrent API access.
QMutex &sdkMutex()
{
    static QMutex mutex;
    return mutex;
}

#ifdef NSCAN_SDK_AVAILABLE
QString sdkErrorMessage()
{
    const char *message = nl_GetLastError();
    if (!message || message[0] == '\0')
        return QString();
    return QString::fromLocal8Bit(message);
}

QString operationError(const QString &operation, int code)
{
    const QString detail = sdkErrorMessage();
    if (detail.isEmpty())
        return QStringLiteral("%1 failed (SDK code %2)").arg(operation).arg(code);
    return QStringLiteral("%1 failed (SDK code %2): %3")
        .arg(operation).arg(code).arg(detail);
}

bool isTimeoutError(const QString &detail)
{
    return detail.contains(QStringLiteral("timeout"), Qt::CaseInsensitive)
        || detail.contains(QStringLiteral("time out"), Qt::CaseInsensitive);
}

#endif
}

NScanScheduler::~NScanScheduler()
{
    shutdown();
}

void NScanScheduler::shutdown()
{
#ifdef NSCAN_SDK_AVAILABLE
    QMutexLocker locker(&sdkMutex());
    closeConnection();
#endif
}

void NScanScheduler::closeConnection() const
{
#ifdef NSCAN_SDK_AVAILABLE
    if (m_socket >= 0)
        nl_CloseClientSocket(&m_socket);
#endif
    m_socket = -1;
    m_connectedIp.clear();
    m_connectedPort = 0;
}

NScanScheduler::ScanResult NScanScheduler::scan(const ScanOptions &options) const
{
    const ScanResult validation = validateOptions(options);
    if (validation.status == ScanResult::Status::InvalidConfig)
        return validation;

#ifndef NSCAN_SDK_AVAILABLE
    ScanResult result;
    result.status = ScanResult::Status::SdkUnavailable;
    result.errorMessage = QStringLiteral("N-ScanHub SDK is unavailable on this platform");
    return result;
#else
    QMutexLocker locker(&sdkMutex());
    ScanResult lastResult;
    const QString targetIp = options.ip.trimmed();

    for (int attempt = 1; attempt <= options.maxAttempts; ++attempt) {
        ScanResult result;
        result.attempts = attempt;

        // Reuse a healthy connection as recommended by the vendor demo.
        if (m_socket >= 0
            && (m_connectedIp != targetIp || m_connectedPort != options.port)) {
            closeConnection();
        }

        if (m_socket < 0) {
            QByteArray ipBytes = targetIp.toLatin1();
            const int connectRet = nl_connectToService(
                ipBytes.data(), static_cast<int>(options.port), &m_socket);
            if (connectRet != 0 || m_socket < 0) {
                result.status = ScanResult::Status::ConnectError;
                result.errorMessage = operationError(
                    QStringLiteral("Scanner connection to %1:%2 (socket %3)")
                        .arg(targetIp).arg(options.port).arg(m_socket),
                    connectRet);
                closeConnection();
                lastResult = result;
                if (attempt < options.maxAttempts && options.retryIntervalMs > 0)
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(options.retryIntervalMs));
                continue;
            }

            m_connectedIp = targetIp;
            m_connectedPort = options.port;
        }

        QByteArray trigger = options.triggerCommand;
        const int sendRet = nl_sendDataToSocket(
            m_socket, trigger.data(), static_cast<int>(trigger.size()));
        if (sendRet != 0) {
            result.status = ScanResult::Status::SendError;
            result.errorMessage = operationError(QStringLiteral("Scan trigger"), sendRet);
            closeConnection();
            lastResult = result;
        } else {
            std::array<char, kReceiveBufferSize> buffer{};
            int receivedLength = 0;
            const int readRet = nl_readFromSocket(
                m_socket, options.timeoutMs, buffer.data(), &receivedLength);

            if (readRet != 0) {
                const QString detail = sdkErrorMessage();
                result.status = isTimeoutError(detail)
                    ? ScanResult::Status::Timeout
                    : ScanResult::Status::ReadError;
                result.errorMessage = detail.isEmpty()
                    ? QStringLiteral("Scan read failed (SDK code %1)").arg(readRet)
                    : QStringLiteral("Scan read failed (SDK code %1): %2")
                          .arg(readRet).arg(detail);
                closeConnection();
                lastResult = result;
            } else if (receivedLength <= 0) {
                result.status = ScanResult::Status::Timeout;
                result.errorMessage = QStringLiteral("Timed out waiting for barcode data");
                closeConnection();
                lastResult = result;
            } else if (receivedLength > static_cast<int>(buffer.size())) {
                result.status = ScanResult::Status::ReadError;
                result.errorMessage = QStringLiteral("Barcode data exceeds receive buffer");
                closeConnection();
                lastResult = result;
            } else {
                result.rawData = QByteArray(buffer.data(), receivedLength);
                result.code = decodeBarcode(result.rawData);
                if (result.code.isEmpty()) {
                    result.status = ScanResult::Status::ReadError;
                    result.errorMessage = QStringLiteral("Barcode data is empty");
                    closeConnection();
                    lastResult = result;
                } else {
                    result.status = ScanResult::Status::Success;
                    return result;
                }
            }
        }

        if (attempt < options.maxAttempts && options.retryIntervalMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(options.retryIntervalMs));
    }

    return lastResult;
#endif
}

QString NScanScheduler::statusText(ScanResult::Status status)
{
    switch (status) {
    case ScanResult::Status::Success:        return QStringLiteral("Success");
    case ScanResult::Status::Timeout:        return QStringLiteral("Timeout");
    case ScanResult::Status::ConnectError:   return QStringLiteral("Connect error");
    case ScanResult::Status::SendError:      return QStringLiteral("Send error");
    case ScanResult::Status::ReadError:      return QStringLiteral("Read error");
    case ScanResult::Status::InvalidConfig:  return QStringLiteral("Invalid configuration");
    case ScanResult::Status::SdkUnavailable: return QStringLiteral("SDK unavailable");
    }
    return QStringLiteral("Unknown status");
}

NScanScheduler::ScanResult NScanScheduler::validateOptions(const ScanOptions &options)
{
    ScanResult result;
    result.status = ScanResult::Status::Success;

    if (options.ip.trimmed().isEmpty())
        result.errorMessage = QStringLiteral("Scanner IP must not be empty");
    else if (options.port == 0)
        result.errorMessage = QStringLiteral("Scanner port must be greater than zero");
    else if (options.timeoutMs <= 0)
        result.errorMessage = QStringLiteral("Scan timeout must be greater than zero");
    else if (options.maxAttempts < 1)
        result.errorMessage = QStringLiteral("Maximum attempts must be at least one");
    else if (options.retryIntervalMs < 0)
        result.errorMessage = QStringLiteral("Retry interval must not be negative");
    else if (options.triggerCommand.isEmpty())
        result.errorMessage = QStringLiteral("Scan trigger command must not be empty");

    if (!result.errorMessage.isEmpty())
        result.status = ScanResult::Status::InvalidConfig;
    return result;
}

QString NScanScheduler::decodeBarcode(const QByteArray &rawData)
{
    QByteArray normalized = rawData;
    while (!normalized.isEmpty()) {
        const char tail = normalized.back();
        if (tail != '\r' && tail != '\n' && tail != '\0')
            break;
        normalized.chop(1);
    }

    QStringDecoder utf8Decoder(QStringDecoder::Utf8);
    const QString utf8 = utf8Decoder.decode(normalized);
    return utf8Decoder.hasError() ? QString::fromLatin1(normalized) : utf8;
}
