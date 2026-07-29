// 独立验证工具：确认华沿机器人 SDK 对 Rx、Ry、Rz 的欧拉角组合顺序。
//
// 安全边界：
// 1. 本程序只允许调用连接、RPY 转四元数和断开连接接口；
// 2. 本程序不包含上电、使能、点动、轨迹、MoveJ、MoveL、Servo 等运动接口；
// 3. 输入的 (10°, 20°, 30°) 只用于数学换算，不会命令机器人运动。
//
// 使用方式：
//     verify_hans_rpy.exe <机器人控制器IP>

#include "HR_Pro.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

// 华沿 SDK 的固定连接参数。当前验证只使用 0 号控制箱和 0 号机器人。
constexpr unsigned int kBoxId = 0;
constexpr unsigned int kRobotId = 0;
constexpr unsigned short kSdkPort = 10003;

// 非对称测试角可以区分两种候选旋转顺序，角度单位为度。
constexpr double kRxDegrees = 10.0;
constexpr double kRyDegrees = 20.0;
constexpr double kRzDegrees = 30.0;
constexpr double kPi = 3.141592653589793238462643383279502884;

// 现场验证表明 SDK 四元数只保留三位小数。四个分量各自最多产生约
// 0.0005 的舍入误差，因此综合欧氏距离使用 0.001 作为匹配上限。
constexpr double kMatchTolerance = 1.0e-3;

// 四元数分量顺序严格对应华沿 SDK 的 W、X、Y、Z 输出顺序。
struct Quaternion
{
    double w = 1.0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

// 连接成功后保证程序离开 main 前执行断开。析构函数只负责通信清理，
// 不会改变机器人的使能、运行模式或位姿状态。
class RobotConnectionGuard
{
public:
    explicit RobotConnectionGuard(const unsigned int boxId)
        : m_boxId(boxId)
    {
    }

    RobotConnectionGuard(const RobotConnectionGuard &) = delete;
    RobotConnectionGuard &operator=(const RobotConnectionGuard &) = delete;

    ~RobotConnectionGuard()
    {
        const int disconnectResult = HRIF_DisConnect(m_boxId);
        std::cout << "断开连接返回码：" << disconnectResult << '\n';
    }

private:
    unsigned int m_boxId;
};

double degreesToRadians(const double degrees)
{
    return degrees * kPi / 180.0;
}

// 生成绕指定笛卡尔轴旋转的单位四元数。
Quaternion axisAngleQuaternion(
    const double axisX,
    const double axisY,
    const double axisZ,
    const double angleDegrees)
{
    const double halfAngleRadians = degreesToRadians(angleDegrees) / 2.0;
    const double sine = std::sin(halfAngleRadians);

    return {
        std::cos(halfAngleRadians),
        axisX * sine,
        axisY * sine,
        axisZ * sine
    };
}

// Hamilton 乘法：返回 lhs * rhs。对于列向量主动旋转，
// 组合 qz * qy * qx 表示依次应用 Rx、Ry、Rz。
Quaternion multiply(const Quaternion &lhs, const Quaternion &rhs)
{
    return {
        lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z,
        lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
        lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w
    };
}

double componentDistance(
    const Quaternion &actual,
    const Quaternion &candidate,
    const double candidateSign)
{
    const double deltaW = actual.w - candidateSign * candidate.w;
    const double deltaX = actual.x - candidateSign * candidate.x;
    const double deltaY = actual.y - candidateSign * candidate.y;
    const double deltaZ = actual.z - candidateSign * candidate.z;

    return std::sqrt(
        deltaW * deltaW
        + deltaX * deltaX
        + deltaY * deltaY
        + deltaZ * deltaZ);
}

// q 与 -q 表示同一个空间旋转，因此取两种符号下的较小分量欧氏距离。
double rotationDistance(const Quaternion &actual, const Quaternion &candidate)
{
    return std::min(
        componentDistance(actual, candidate, 1.0),
        componentDistance(actual, candidate, -1.0));
}

void printQuaternion(const std::string &label, const Quaternion &quaternion)
{
    std::cout
        << label
        << "W=" << quaternion.w
        << ", X=" << quaternion.x
        << ", Y=" << quaternion.y
        << ", Z=" << quaternion.z
        << '\n';
}

} // namespace

int main(int argc, char *argv[])
{
    if (argc != 2) {
        std::cerr << "用法：" << argv[0] << " <机器人控制器IP>\n";
        return 1;
    }

    const std::string robotIp = argv[1];
    const int connectResult = HRIF_Connect(kBoxId, robotIp.c_str(), kSdkPort);
    std::cout << "连接返回码：" << connectResult << '\n';

    if (connectResult != 0) {
        std::cerr << "连接失败，未执行欧拉角换算。\n";
        return 2;
    }

    RobotConnectionGuard connectionGuard(kBoxId);

    Quaternion sdkQuaternion;
    const int convertResult = HRIF_RPY2Quaternion(
        kBoxId,
        kRobotId,
        kRxDegrees,
        kRyDegrees,
        kRzDegrees,
        sdkQuaternion.w,
        sdkQuaternion.x,
        sdkQuaternion.y,
        sdkQuaternion.z);

    std::cout << "RPY 转四元数返回码：" << convertResult << '\n';
    if (convertResult != 0) {
        std::cerr << "换算失败，四元数输出不参与判断。\n";
        return 3;
    }

    const Quaternion qx = axisAngleQuaternion(1.0, 0.0, 0.0, kRxDegrees);
    const Quaternion qy = axisAngleQuaternion(0.0, 1.0, 0.0, kRyDegrees);
    const Quaternion qz = axisAngleQuaternion(0.0, 0.0, 1.0, kRzDegrees);

    // 两个候选值分别对应参考验证计划中的两种待确认组合。
    const Quaternion candidateRzRyRx = multiply(multiply(qz, qy), qx);
    const Quaternion candidateRxRyRz = multiply(multiply(qx, qy), qz);

    std::cout << std::fixed << std::setprecision(12);
    std::cout
        << "输入角度：Rx=" << kRxDegrees
        << ", Ry=" << kRyDegrees
        << ", Rz=" << kRzDegrees
        << '\n';
    printQuaternion("官方四元数：", sdkQuaternion);
    printQuaternion("候选 Rz·Ry·Rx：", candidateRzRyRx);
    printQuaternion("候选 Rx·Ry·Rz：", candidateRxRyRz);

    const double distanceRzRyRx = rotationDistance(sdkQuaternion, candidateRzRyRx);
    const double distanceRxRyRz = rotationDistance(sdkQuaternion, candidateRxRyRz);
    const bool matchesRzRyRx = distanceRzRyRx <= kMatchTolerance;
    const bool matchesRxRyRz = distanceRxRyRz <= kMatchTolerance;

    std::cout << "Rz·Ry·Rx 误差：" << distanceRzRyRx << '\n';
    std::cout << "Rx·Ry·Rz 误差：" << distanceRxRyRz << '\n';

    if (matchesRzRyRx && !matchesRxRyRz) {
        std::cout << "判定结果：官方换算对应 Rz(rz)·Ry(ry)·Rx(rx)。\n";
        return 0;
    }

    if (matchesRxRyRz && !matchesRzRyRx) {
        std::cout << "判定结果：官方换算对应 Rx(rx)·Ry(ry)·Rz(rz)。\n";
        return 0;
    }

    if (matchesRzRyRx && matchesRxRyRz) {
        std::cout << "判定结果：两种候选均匹配，当前输入不足以区分旋转顺序。\n";
        return 4;
    }

    std::cout << "判定结果：两种候选均不匹配，请保留完整输出继续分析。\n";
    return 5;
}
