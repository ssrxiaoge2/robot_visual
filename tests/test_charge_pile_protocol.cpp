#include "chargepileprotocol.h"

#include <QtTest>

using namespace ChargePileProtocol;

class ChargePileProtocolTest : public QObject
{
    Q_OBJECT

private slots:
    void statusReadFrameMatchesVerifiedPythonBytes()
    {
        // 若大端地址、数量或 CRC 字节序任一实现错误，本断言都会失败。
        QCOMPARE(buildReadRequest(1, 0x04, 0x0000, 2).toHex(),
                 QByteArray("01040000000271cb"));
    }

    void startStopRetractResetUseApprovedAddresses()
    {
        // 若将线圈地址误用为文档中的相邻地址，将无法控制对应充电桩动作。
        QCOMPARE(buildWriteCoilRequest(1, kCoilStart, true).toHex(),
                 QByteArray("01050003ff007c3a"));
        QCOMPARE(buildWriteCoilRequest(1, kCoilStop, true).toHex(),
                 QByteArray("01050006ff006c3b"));
        QCOMPARE(buildWriteCoilRequest(1, kCoilRetract, true).toHex(),
                 QByteArray("01050001ff00ddfa"));
        QCOMPARE(buildWriteCoilRequest(1, kCoilReset, true).toHex(),
                 QByteArray("01050007ff003dfb"));
    }

    void registerWriteUsesFunctionSixAndLowByteFirstCrc()
    {
        // 若误把写单寄存器编码为写线圈或 CRC 高字节在前，设备会拒绝该帧。
        QCOMPARE(buildWriteRegisterRequest(1, kRegSetVoltage, 0x1234).toHex(),
                 QByteArray("01060017123434b9"));
    }

    void unsupportedFunctionReturnsEmptyRequest()
    {
        // 若非法功能码仍被发送，将制造无法由调用层识别的无效 RTU 请求。
        QVERIFY(buildReadRequest(1, 0x10, 0, 1).isEmpty());
    }

    void extractorWaitsForSplitFrameAndPreservesStickyTail()
    {
        // 若提前消费半帧或连带消费下一帧，串口粘包场景会永久丢失报文。
        QByteArray buffer = QByteArray::fromHex("010404024800");
        const auto first = takeResponseFrame(&buffer, 1, 0x04);
        QCOMPARE(first.status, FrameExtractStatus::Incomplete);
        QCOMPARE(buffer.toHex(), QByteArray("010404024800"));

        buffer += QByteArray::fromHex("7b3a0901050003ff007c3a");
        const auto second = takeResponseFrame(&buffer, 1, 0x04);
        QCOMPARE(second.status, FrameExtractStatus::Complete);
        QCOMPARE(second.frame.toHex(), QByteArray("0104040248007b3a09"));
        QCOMPARE(buffer.toHex(), QByteArray("01050003ff007c3a"));
    }

    void extractorUsesReadByteCountForDynamicFrameLength()
    {
        // 若固定读取长度，会截断多寄存器读响应或把后续帧误并入当前帧。
        QByteArray buffer = QByteArray::fromHex("0104080001000200030004bcce");
        const auto result = takeResponseFrame(&buffer, 1, 0x04);
        QCOMPARE(result.status, FrameExtractStatus::Complete);
        QCOMPARE(result.frame.toHex(), QByteArray("0104080001000200030004bcce"));
        QVERIFY(buffer.isEmpty());
    }

    void extractorAcceptsFixedLengthWriteEcho()
    {
        // 若写回显被当作读响应，第二个字节会被错当字节数量而无法拆帧。
        QByteArray buffer = QByteArray::fromHex("01050003ff007c3a");
        const auto result = takeResponseFrame(&buffer, 1, 0x05);
        QCOMPARE(result.status, FrameExtractStatus::Complete);
        QCOMPARE(result.frame.toHex(), QByteArray("01050003ff007c3a"));
        QVERIFY(buffer.isEmpty());
    }

    void extractorReturnsChineseReasonForBadCrcWithoutConsuming()
    {
        // 若 CRC 损坏帧被当作有效帧消费，控制器会基于不可信数据推进状态机。
        QByteArray buffer = QByteArray::fromHex("0104040248007b0000");
        const auto result = takeResponseFrame(&buffer, 1, 0x04);
        QCOMPARE(result.status, FrameExtractStatus::Invalid);
        QCOMPARE(result.reason, QStringLiteral("CRC校验失败"));
        QCOMPARE(buffer.toHex(), QByteArray("0104040248007b0000"));
    }

    void extractorRejectsUnexpectedSlaveWithoutConsuming()
    {
        // 若接收了别的站号的帧，多设备总线会串扰充电状态。
        QByteArray buffer = QByteArray::fromHex("0204040248007b0909");
        const auto result = takeResponseFrame(&buffer, 1, 0x04);
        QCOMPARE(result.status, FrameExtractStatus::Invalid);
        QCOMPARE(result.reason, QStringLiteral("站号不匹配"));
        QCOMPARE(buffer.toHex(), QByteArray("0204040248007b0909"));
    }

    void extractorReportsModbusException()
    {
        // 若忽略异常响应，调用方会把设备拒绝操作误判成超时。
        QByteArray buffer = QByteArray::fromHex("018402c2c1");
        const auto result = takeResponseFrame(&buffer, 1, 0x04);
        QCOMPARE(result.status, FrameExtractStatus::Complete);
        QCOMPARE(result.exceptionCode, quint8(0x02));
        QCOMPARE(result.reason, QStringLiteral("Modbus异常响应：非法数据地址"));
        QVERIFY(buffer.isEmpty());
    }

    void extractorRejectsUnexpectedFunctionWithoutConsuming()
    {
        // 若错误功能码可通过校验，读写响应会在调用层被错误解释。
        QByteArray buffer = QByteArray::fromHex("010304000100022a32");
        const auto result = takeResponseFrame(&buffer, 1, 0x04);
        QCOMPARE(result.status, FrameExtractStatus::Invalid);
        QCOMPARE(result.reason, QStringLiteral("功能码不匹配"));
        QCOMPARE(buffer.toHex(), QByteArray("010304000100022a32"));
    }

    void signalBitHelpersReadRequestedBit()
    {
        // 若位移方向或掩码错误，控制器会反转输入、输出、事件或故障判断。
        constexpr quint16 flags = 0x8009;
        QVERIFY(inputSignalBit(flags, 0));
        QVERIFY(!outputSignalBit(flags, 1));
        QVERIFY(eventBit(flags, 3));
        QVERIFY(faultBit(flags, 15));
        QVERIFY(!faultBit(flags, 16));
    }

    void namedInputSignalBitsMatchVerifiedPythonMapping()
    {
        // 若具名输入位与 Python 现场映射错位，控制器会误判充电机构与无线状态。
        constexpr quint16 inputSignals = 0x008F;
        QVERIFY(inputSignalBit(inputSignals, kInputDryContactBit));
        QVERIFY(inputSignalBit(inputSignals, kInputInfraredReceiverBit));
        QVERIFY(inputSignalBit(inputSignals, kInputExtendedBit));
        QVERIFY(inputSignalBit(inputSignals, kInputRetractedBit));
        QVERIFY(inputSignalBit(inputSignals, kInputWirelessConnectedBit));
    }

    void namedOutputSignalBitsMatchVerifiedPythonMapping()
    {
        // 若具名输出位与 Python 现场映射错位，控制器会把设备运行状态解释为错误状态。
        constexpr quint16 outputSignals = 0x023F;
        QVERIFY(outputSignalBit(outputSignals, kOutputInfraredTransmitterBit));
        QVERIFY(outputSignalBit(outputSignals, kOutputWaitingChargeBit));
        QVERIFY(outputSignalBit(outputSignals, kOutputFaultBit));
        QVERIFY(outputSignalBit(outputSignals, kOutputFullyChargedBit));
        QVERIFY(outputSignalBit(outputSignals, kOutputWorkingBit));
        QVERIFY(outputSignalBit(outputSignals, kOutputFanBit));
        QVERIFY(outputSignalBit(outputSignals, kOutputRelayBit));
    }
};

QTEST_APPLESS_MAIN(ChargePileProtocolTest)
#include "test_charge_pile_protocol.moc"
