#include "chargepileprotocol.h"

namespace ChargePileProtocol {
namespace {

constexpr quint8 kReadHoldingRegisters = 0x03;
constexpr quint8 kReadInputRegisters = 0x04;
constexpr quint8 kWriteSingleCoil = 0x05;
constexpr quint8 kWriteSingleRegister = 0x06;
constexpr quint8 kExceptionMask = 0x80;

void appendBigEndianWord(QByteArray &frame, quint16 value)
{
    // Modbus PDU 中的 16 位地址、数量和值均使用高字节在前。
    frame.append(static_cast<char>((value >> 8) & 0xFF));
    frame.append(static_cast<char>(value & 0xFF));
}

void appendCrc(QByteArray &frame)
{
    // Modbus RTU 的 CRC 与 PDU 字段不同，必须低字节先发送。
    const quint16 crc = crc16(QByteArrayView(frame));
    frame.append(static_cast<char>(crc & 0xFF));
    frame.append(static_cast<char>((crc >> 8) & 0xFF));
}

bool isReadFunction(quint8 function)
{
    return function == kReadHoldingRegisters || function == kReadInputRegisters;
}

bool isWriteFunction(quint8 function)
{
    return function == kWriteSingleCoil || function == kWriteSingleRegister;
}

bool readSignalBit(quint16 signalWord, quint8 bitIndex)
{
    // 防止 bitIndex >= 16 时发生未定义或平台相关的移位行为。
    return bitIndex < 16 && (signalWord & (quint16(1) << bitIndex)) != 0;
}

QString exceptionReason(quint8 code)
{
    switch (code) {
    case 0x01: return QStringLiteral("Modbus异常响应：非法功能码");
    case 0x02: return QStringLiteral("Modbus异常响应：非法数据地址");
    case 0x03: return QStringLiteral("Modbus异常响应：非法数据值");
    case 0x04: return QStringLiteral("Modbus异常响应：从站设备故障");
    default: return QStringLiteral("Modbus异常响应：未知异常码%1").arg(code);
    }
}

} // namespace

quint16 crc16(QByteArrayView bytes)
{
    // 初值、右移方向和多项式均按 Modbus RTU CRC-16/A001；返回数值在帧中仍需
    // 由 appendCrc() 按低字节在前写入，不能直接追加主机字节序。
    quint16 crc = 0xFFFF;
    for (const char byte : bytes) {
        crc ^= static_cast<quint8>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x0001) != 0 ? static_cast<quint16>((crc >> 1) ^ 0xA001)
                                       : static_cast<quint16>(crc >> 1);
        }
    }
    return crc;
}

QByteArray buildReadRequest(quint8 slaveId, quint8 function, quint16 address, quint16 count)
{
    if (!isReadFunction(function))
        return {};

    QByteArray frame;
    frame.reserve(8);
    frame.append(static_cast<char>(slaveId));
    frame.append(static_cast<char>(function));
    appendBigEndianWord(frame, address);
    appendBigEndianWord(frame, count);
    appendCrc(frame);
    return frame;
}

QByteArray buildWriteRegisterRequest(quint8 slaveId, quint16 address, quint16 value)
{
    QByteArray frame;
    frame.reserve(8);
    frame.append(static_cast<char>(slaveId));
    frame.append(static_cast<char>(kWriteSingleRegister));
    appendBigEndianWord(frame, address);
    appendBigEndianWord(frame, value);
    appendCrc(frame);
    return frame;
}

QByteArray buildWriteCoilRequest(quint8 slaveId, quint16 address, bool on)
{
    QByteArray frame;
    frame.reserve(8);
    frame.append(static_cast<char>(slaveId));
    frame.append(static_cast<char>(kWriteSingleCoil));
    appendBigEndianWord(frame, address);
    appendBigEndianWord(frame, on ? 0xFF00 : 0x0000);
    appendCrc(frame);
    return frame;
}

FrameExtractResult takeResponseFrame(QByteArray *buffer, quint8 expectedSlave, quint8 expectedFunction)
{
    // 解析器只查看缓存头部，不扫描后续字节寻找“看起来合法”的帧。头部出现
    // 站号/功能码/CRC 冲突意味着当前请求响应不可证明，交由控制器统一恢复。
    if (buffer == nullptr)
        return {FrameExtractStatus::Invalid, {}, 0, QStringLiteral("接收缓存为空")};
    if (buffer->size() < 2)
        return {};

    const quint8 function = static_cast<quint8>(buffer->at(1));
    int frameLength = 0;
    if (function == static_cast<quint8>(expectedFunction | kExceptionMask)) {
        frameLength = 5;
    } else if (isReadFunction(function)) {
        if (buffer->size() < 3)
            return {};
        frameLength = 5 + static_cast<quint8>(buffer->at(2));
    } else if (isWriteFunction(function)) {
        frameLength = 8;
    } else {
        return {FrameExtractStatus::Invalid, {}, 0, QStringLiteral("不支持的响应功能码")};
    }

    if (buffer->size() < frameLength)
        return {};

    const QByteArray frame = buffer->left(frameLength);
    const quint16 receivedCrc = static_cast<quint8>(frame.at(frameLength - 2))
                              | (quint16(static_cast<quint8>(frame.at(frameLength - 1))) << 8);
    if (crc16(QByteArrayView(frame.constData(), frameLength - 2)) != receivedCrc)
        return {FrameExtractStatus::Invalid, {}, 0, QStringLiteral("CRC校验失败")};
    if (static_cast<quint8>(frame.at(0)) != expectedSlave)
        return {FrameExtractStatus::Invalid, {}, 0, QStringLiteral("站号不匹配")};

    if (function == static_cast<quint8>(expectedFunction | kExceptionMask)) {
        const quint8 exceptionCode = static_cast<quint8>(frame.at(2));
        buffer->remove(0, frameLength);
        return {FrameExtractStatus::Complete, frame, exceptionCode, exceptionReason(exceptionCode)};
    }
    if (function != expectedFunction)
        return {FrameExtractStatus::Invalid, {}, 0, QStringLiteral("功能码不匹配")};

    buffer->remove(0, frameLength);
    return {FrameExtractStatus::Complete, frame, 0, {}};
}

bool inputSignalBit(quint16 signalWord, quint8 bitIndex)
{
    return readSignalBit(signalWord, bitIndex);
}

bool outputSignalBit(quint16 signalWord, quint8 bitIndex)
{
    return readSignalBit(signalWord, bitIndex);
}

bool eventBit(quint16 eventWord, quint8 bitIndex)
{
    return readSignalBit(eventWord, bitIndex);
}

bool faultBit(quint16 faultWord, quint8 bitIndex)
{
    return readSignalBit(faultWord, bitIndex);
}

} // namespace ChargePileProtocol
