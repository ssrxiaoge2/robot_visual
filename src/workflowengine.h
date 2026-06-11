#ifndef WORKFLOWENGINE_H
#define WORKFLOWENGINE_H

#include <QObject>
#include <QTimer>
#include <QList>
#include "workflowstep.h"
#include "agvcontroller.h"   // 需要完整定义以使用 AgvController::NavStatus

class RobotController;
class VisionHttpClient;

/*!
 * \brief 工作流状态机（五步循环，支持真实 Modbus 硬件与仿真回退）
 *
 * 调用 setRobotController / setAgvController 注入硬件控制器后，
 * 状态机将通过真实 Modbus 指令驱动设备并等待状态字确认；
 * 未注入或设备离线时自动降级为基于定时器的仿真模式。
 *
 * 五步流程：
 *   Step 0  就位/视觉 — 写 CmdID=8 → 轮询 Input1132==1 → 触发视觉推理 → 等待坐标回调
 *   Step 1  机器人抓取 — 写坐标(Holding 901-906) → 轮询 Input 1132 == GRAB_DONE(2)
 *   Step 2  AGV 行走  — 写目标站点(AGV [4x]00001) → 轮询 [3x]00009 == Arrived(4)
 *   Step 3  机器人放料 — 写 CmdID=FLIP(Holding 900) → 轮询 Input 1132 == UNLOAD_DONE
 *   Step 4  复位等待  — 写 CmdID=RESET(Holding 900) → 轮询 Input 1132 == IDLE
 *
 * ── 机器人 Modbus 寄存器 ─────────────────────────────────────────
 *   Holding 900  CmdID：8=抓取, FLIP_CMD=翻转卸料, 0=复位
 *   Holding 901  X 坐标（原始寄存器值，由相机系统填充）
 *   Holding 902  Y 坐标
 *   Holding 903  Z 坐标
 *   Holding 904  Rx 姿态角
 *   Holding 905  Ry 姿态角
 *   Holding 906  Rz 姿态角
 *   Input   1132 状态字：0=空闲就绪, 1=请求拍照, 2=抓取完成, 3=卸料完成
 */
class WorkflowEngine : public QObject
{
    Q_OBJECT
public:
    // ── 步骤元数据（供 UI 层展示）────────────────────────────────
    static const QList<WorkflowStep> kSteps;

    // ── 机器人 Modbus 寄存器 / 命令常量 ─────────────────────────
    static constexpr int     REG_CMD_ID       = 900;  ///< Holding: 命令字
    static constexpr int     REG_COORD_START  = 901;  ///< Holding: 坐标起始地址 (901-906)
    static constexpr int     REG_ROBOT_STATUS = 1132; ///< Input:   机器人状态字

    static constexpr quint16 CMD_GRAB         =  8;   ///< 启动抓取程序
    static constexpr quint16 CMD_FLIP         =  9;   ///< 翻转卸料（可根据实际协议调整）
    static constexpr quint16 CMD_RESET        =  0;   ///< 复位回原点

    static constexpr quint16 STATUS_IDLE        = 0;  ///< 空闲就绪
    static constexpr quint16 STATUS_REQ_PHOTO   = 1;  ///< 请求拍照
    static constexpr quint16 STATUS_GRAB_DONE   = 2;  ///< 抓取完成
    static constexpr quint16 STATUS_UNLOAD_DONE = 3;  ///< 卸料完成

    explicit WorkflowEngine(QObject *parent = nullptr);

    // ── 硬件控制器注入（在 start() 前调用）──────────────────────
    void setRobotController(RobotController *ctrl);
    void setAgvController(AgvController *ctrl);
    /**
     * @brief 注入视觉 HTTP 客户端
     *
     * 注入后 Step0 将主动调用 fetchInference()，
     * 不再依赖外部通过 requestCameraCapture() 信号触发。
     * 未注入或服务未配置时，降级为 requestCameraCapture() + 仿真定时器。
     */
    void setVisionClient(VisionHttpClient *client);

    int  currentStep() const { return m_currentStep; }
    int  cycleCount()  const { return m_cycleCount;  }
    int  station()     const { return m_station;     }
    bool isRunning()   const { return m_running;     }

public slots:
    /// 启动自动循环，simIntervalMs 为仿真模式下每步时长
    void start(int simIntervalMs);
    /// 停止自动循环（向机器人发复位命令）
    void stop();
    /// 手动单步推进（仅在非运行状态下有效）
    void singleStep();
    /// 动态调整仿真步进间隔
    void setInterval(int ms);
    /// 由外部（相机系统）注入一组坐标寄存器值（6 个，对应 Holding 901-906）
    void setCoordinates(const QList<quint16> &coords);

signals:
    // ── 供 UI 层订阅 ─────────────────────────────────────────
    void stepActivated(int stepIdx, int station, int cycleCount);
    void started();
    void stopped();

    // ── 日志 / 错误 ───────────────────────────────────────────
    void stepLog(const QString &msg);
    void engineError(const QString &msg);

    // ── 相机触发 ──────────────────────────────────────────────
    /// 请求相机执行一次拍照，结果通过 setCoordinates() 槽回调
    void requestCameraCapture();

    // ── AGV 故障 ──────────────────────────────────────────────
    /// AGV 导航失败/取消/超时（[3x]00009 = 5/6/7）时发出，供 UI 更新指示灯为故障状态
    void agvFaultDetected();

private slots:
    void onPollTick();
    void onStepTimeout();
    void onRobotInputRegistersRead(int startAddr, const QList<quint16> &values);
    void onAgvStatusRead(AgvController::NavStatus status, int navStation);
    void onRobotWriteCompleted();

private:
    enum class EngineState {
        Idle,
        Step0_SendCmd,     ///< 已写 CmdID=8，轮询 Input1132 等待 status=REQ_PHOTO(1)
        Step0_WaitVision,  ///< 机器人已到拍照位，已触发视觉推理，等待坐标回调
        Step1_WriteCoords, ///< 正在写入坐标寄存器（等待 writeCompleted）
        Step1_Grabbing,    ///< 等待 Input 1132 == GRAB_DONE(2)
        Step2_AgvMoving,   ///< 等待 AGV 状态 == ARRIVED
        Step3_Unloading,   ///< 等待 Input 1132 == UNLOAD_DONE(3)
        Step4_Resetting,   ///< 等待 Input 1132 == IDLE(0)
    };

    enum class PendingCmd { None, WriteCoords };

    void enterStep(int idx);   ///< 统一入口：更新 m_currentStep，发 stepActivated，分发到 enterStepN()
    void enterStep0();
    void enterStep1();
    void enterStep2();
    void enterStep3();
    void enterStep4();
    void completeCycle();      ///< 一轮结束：更新计数，决定继续还是停止

    bool robotReady()  const;
    bool agvReady()    const;
    void startPollAndTimeout();

    // ── 控制器（弱引用，生命周期由 DeviceManager 管理）──────────
    RobotController  *m_robotCtrl    = nullptr;
    AgvController    *m_agvCtrl     = nullptr;
    VisionHttpClient *m_visionClient = nullptr;

    // ── 定时器 ────────────────────────────────────────────────
    QTimer *m_pollTimer    = nullptr;  ///< 300 ms，向硬件轮询状态
    QTimer *m_stepTimer    = nullptr;  ///< 仿真模式：每步自动推进；真实模式：步骤超时
    QTimer *m_timeoutTimer = nullptr;  ///< 单步最长等待时间（30 s）

    // ── 状态 ──────────────────────────────────────────────────
    EngineState m_state       = EngineState::Idle;
    PendingCmd  m_pendingCmd  = PendingCmd::None;
    int         m_currentStep = -1;
    int         m_cycleCount  = 0;
    int         m_station     = 0;
    bool        m_running     = false;
    int         m_simIntervalMs = 2000;

    // ── 坐标数据 ──────────────────────────────────────────────
    QList<quint16> m_coords;    ///< 6 个寄存器值（901-906）
    bool           m_hasCoords = false;
};

#endif // WORKFLOWENGINE_H
