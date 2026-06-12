#ifndef LINEORCHESTRATOR_H
#define LINEORCHESTRATOR_H

#include <QObject>
#include <QTimer>
#include "agvcontroller.h"   // AgvMonitorData 需完整定义

class HuayanScheduler;

/*!
 * \brief 整线流程编排器（AGV + 机械臂时序协调）
 *
 * 纯协调层，不调用任何 Modbus/SDK 原语；只调高层方法并监听完成信号。
 * 单次流程：初检 → AGV取料站 → 机械臂取料 → 收姿态 → AGV倒料站 →
 *           倒料 → 收姿态 → AGV回站1 → 完成。任一步出错就地停机。
 */
class LineOrchestrator : public QObject
{
    Q_OBJECT
public:
    explicit LineOrchestrator(AgvController *agv,
                              HuayanScheduler *arm,
                              QObject *parent = nullptr);

    /// 工位号（试测默认：待机1 / 取料3 / 倒料4），可经 setter 改
    void setStations(int home, int pickup, int unload);

    bool isRunning() const { return m_state != LineState::Idle; }

public slots:
    void start();  // 工具栏"开始"
    void stop();   // 工具栏"停止"（停机，不报错）

signals:
    void lineStarted();
    void lineFinished();                    // 一轮成功完成
    void lineStopped();                     // 手动停止
    void lineError(const QString &reason);  // 中止并报因
    void stepChanged(int stepIdx);          // 0-4 驱动流程图
    void lineLog(const QString &msg);

    /// 请求 AGV 前往某工位（DeviceManager 接此信号调 dispatchAgv）
    void agvDispatchRequested(int workstation);

private slots:
    void onAgvMonitor(const AgvMonitorData &d);
    void onArmCompleted(const QString &stageName);
    void onArmError(const QString &msg);

private:
    enum class LineState {
        Idle,
        InitCheck,
        AgvToPickup,
        ArmPicking,
        StowAfterPick,
        AgvToUnload,
        ArmUnloading,
        StowAfterUnload,
        AgvReturnHome
    };

    void enterState(LineState s);
    void haltDevices();              // 取消 AGV 导航 + 停机械臂 + 回 Idle
    void abort(const QString &reason);
    void emitStepForState(LineState s);

    AgvController   *m_agv = nullptr;
    HuayanScheduler *m_arm = nullptr;

    LineState m_state = LineState::Idle;
    AgvMonitorData m_lastMonitor;    // 最近一次 AGV 监控快照（初检读 curStation）
    QTimer *m_agvTimeout = nullptr;  // AGV 单步超时保护

    int  m_expectedStation = 0;    // 本次 AGV 移动目标站，到达判定校验用
    bool m_agvSeenMoving   = false; // 已观察到 AGV 进入导航，避免旧到达态误判
    bool m_monitorValid    = false; // 是否已收到首帧监控（初检前置）

    int m_homeStation   = 1;
    int m_pickupStation = 3;
    int m_unloadStation = 4;

    static constexpr int kAgvTimeoutMs = 120000; // AGV 单步 120s 超时
};

#endif // LINEORCHESTRATOR_H
