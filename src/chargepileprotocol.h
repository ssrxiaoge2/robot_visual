#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QtGlobal>

/**
 * @brief 充电桩 Modbus RTU 的无状态报文编解码工具。
 *
 * 本命名空间只负责字节级协议，不持有串口、定时器或 UI 对象；调用方将串口
 * 收到的字节持续追加到缓存，再以 takeResponseFrame() 逐帧取出即可。
 */
namespace ChargePileProtocol {

// 充电桩实时输出电压寄存器：读取值的物理单位及缩放由设备说明书与调用层约定。
inline constexpr quint16 kRegOutVoltage = 0x0000;
// 充电桩实时输出电流寄存器：用于判断实际是否已开始或停止充电。
inline constexpr quint16 kRegOutCurrent = 0x0001;
// 输入信号状态字寄存器：每一位反映一个外部输入状态，使用 inputSignalBit() 读取。
inline constexpr quint16 kRegInputSignals = 0x000C;
// 输出信号状态字寄存器：每一位反映一个设备输出状态，使用 outputSignalBit() 读取。
inline constexpr quint16 kRegOutputSignals = 0x000D;
// 事件状态字寄存器：每一位表示设备上报的事件，使用 eventBit() 读取。
inline constexpr quint16 kRegEvent = 0x0011;
// 故障状态字寄存器：每一位表示一个故障原因，使用 faultBit() 读取。
inline constexpr quint16 kRegError = 0x0013;
// 目标充电电压设定寄存器。
inline constexpr quint16 kRegSetVoltage = 0x0017;
// 目标充电电流设定寄存器。
inline constexpr quint16 kRegSetCurrent = 0x0018;
// 截止电流设定寄存器：达到该电流阈值可由设备结束充电。
inline constexpr quint16 kRegSetCutoffCurrent = 0x001A;
// 最大充电时长设定寄存器，单位与缩放由设备说明书及调用层约定。
inline constexpr quint16 kRegSetMaxSeconds = 0x001F;
// 启动充电线圈：写入 ON 触发充电流程。
inline constexpr quint16 kCoilStart = 0x0003;
// 停止充电线圈：写入 ON 请求停止输出。
inline constexpr quint16 kCoilStop = 0x0006;
// 收回充电机构线圈：写入 ON 请求机械机构回撤。
inline constexpr quint16 kCoilRetract = 0x0001;
// 复位线圈：写入 ON 清除设备可复位的状态或故障。
inline constexpr quint16 kCoilReset = 0x0007;

// 输入信号位 0：充电桩干接点已闭合，常用于确认车辆与充电桩的物理触发条件。
inline constexpr quint8 kInputDryContactBit = 0;
// 输入信号位 1：红外接收端检测到来自对端的红外信号。
inline constexpr quint8 kInputInfraredReceiverBit = 1;
// 输入信号位 2：充电机构已运动至伸出到位位置。
inline constexpr quint8 kInputExtendedBit = 2;
// 输入信号位 3：充电机构已运动至收回到位位置。
inline constexpr quint8 kInputRetractedBit = 3;
// 输入信号位 7：充电桩无线通信链路已连接。
inline constexpr quint8 kInputWirelessConnectedBit = 7;

// 输出信号位 0：充电桩红外发送端当前正在发送信号。
inline constexpr quint8 kOutputInfraredTransmitterBit = 0;
// 输出信号位 1：设备处于等待开始充电的状态。
inline constexpr quint8 kOutputWaitingChargeBit = 1;
// 输出信号位 2：设备当前存在故障状态。
inline constexpr quint8 kOutputFaultBit = 2;
// 输出信号位 3：设备报告电池已充满。
inline constexpr quint8 kOutputFullyChargedBit = 3;
// 输出信号位 4：充电桩处于工作状态。
inline constexpr quint8 kOutputWorkingBit = 4;
// 输出信号位 5：充电桩散热风扇正在运行。
inline constexpr quint8 kOutputFanBit = 5;
// 输出信号位 9：充电回路继电器已吸合。
inline constexpr quint8 kOutputRelayBit = 9;

/** 取帧结果的状态：Incomplete 不改变缓存，Invalid 同样保留缓存供调用方记录或恢复。 */
enum class FrameExtractStatus {
    Incomplete,
    Complete,
    Invalid
};

/**
 * @brief 从接收缓存中取出一帧后的结果。
 * frame 仅在 Complete 时保存完整 RTU 帧；exceptionCode 为 0 表示普通响应。
 * reason 用中文描述校验错误或 Modbus 异常，便于上层直接写入运行日志。
 */
struct FrameExtractResult {
    FrameExtractStatus status = FrameExtractStatus::Incomplete;
    QByteArray frame;
    quint8 exceptionCode = 0;
    QString reason;
};

/** @brief 按 Modbus CRC-16/A001 算法计算字节序列的 CRC 数值。 */
quint16 crc16(QByteArrayView bytes);

/** @brief 构造读保持/输入寄存器请求；仅接受 0x03 与 0x04，非法功能码返回空数组。 */
QByteArray buildReadRequest(quint8 slaveId, quint8 function, quint16 address, quint16 count);

/** @brief 构造功能码 0x06 的写单寄存器请求。 */
QByteArray buildWriteRegisterRequest(quint8 slaveId, quint16 address, quint16 value);

/** @brief 构造功能码 0x05 的写单线圈请求，ON 编码为 0xFF00，OFF 编码为 0x0000。 */
QByteArray buildWriteCoilRequest(quint8 slaveId, quint16 address, bool on);

/**
 * @brief 尝试从 buffer 头部拆出期望站号和功能码的一个响应帧。
 *
 * 读取响应按第三字节的字节数确定长度，写回显固定 8 字节，异常响应固定 5 字节。
 * 只有校验成功的完整帧才会从缓存移除，避免拆包、粘包或错误帧造成后续字节丢失。
 */
FrameExtractResult takeResponseFrame(QByteArray *buffer, quint8 expectedSlave, quint8 expectedFunction);

/** @brief 从输入信号状态字读取 bitIndex 位；位号不在 0 至 15 时返回 false。 */
bool inputSignalBit(quint16 signalWord, quint8 bitIndex);
/** @brief 从输出信号状态字读取 bitIndex 位；位号不在 0 至 15 时返回 false。 */
bool outputSignalBit(quint16 signalWord, quint8 bitIndex);
/** @brief 从事件状态字读取 bitIndex 位；位号不在 0 至 15 时返回 false。 */
bool eventBit(quint16 eventWord, quint8 bitIndex);
/** @brief 从故障状态字读取 bitIndex 位；位号不在 0 至 15 时返回 false。 */
bool faultBit(quint16 faultWord, quint8 bitIndex);

} // namespace ChargePileProtocol
