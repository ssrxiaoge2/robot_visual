/**
 * @file huayanScheduler.h
 * @brief HuayanScheduler 华研机器人调度逻辑接口
 *
 * 本类使用华研 SDK 进行机器人连接、取料、卸料和码垛调度。
 */

#ifndef HUAYANSCHEDULER_H
#define HUAYANSCHEDULER_H

#include <QObject>
#include <QString>
#include <QStringList>

class QTimer;

class HuayanScheduler : public QObject
{
    Q_OBJECT

public:
    enum class Stage {
        None,
        StageOne,
        StageTwo,
        StageThree
    };

    /**
     * @brief HuayanScheduler
     *
     * 三阶段调度说明：
     *   StageOne：取料阶段，机器人移动到取料位置，闭合夹爪后抬升物料。
     *   StageTwo：卸料阶段，机器人移动到卸料区，执行翻转卸料，再打开夹爪释放物料，
     *              最后移动空箱至 AGV 工位。
     *   StageThree：码垛阶段，调用华研端脚本函数执行码垛逻辑。
     * todo:对接算法姿态输出，以及示教器实际设置点位，和示教器设置的脚本
     */
    explicit HuayanScheduler(QObject *parent = nullptr);
    ~HuayanScheduler() override;

    /// 配置华研机器人连接参数
    void setConnectionParams(const QString &ip,
                             unsigned short port = 10003,
                             unsigned int boxID = 0,
                             unsigned int rbtID = 0);

    /// 使用华研 SDK 建立 CPS 连接，并进入 Standby
    bool connectRobot();
    /// 断开华研机器人连接
    bool disconnectRobot();
    /// 检查当前连接状态
    bool isConnected() const;

    /// 配置阶段三使用的码垛脚本函数名及参数
    void setStackingFunction(const QString &funcName,
                             const QStringList &params = QStringList());

    /**
     * @brief 启动机器人端在示教器/控制器上运行的脚本（Main）
     * @return true 表示调用成功（不保证脚本内部成功），false 表示调用失败
     */
    bool startRobotScript();

    /**
     * @brief 停止机器人端正在运行的脚本
     * @return true 表示调用成功，false 表示调用失败
     */
    bool stopRobotScript();

    /// 启动三阶段调度中的取料阶段
    void startStageOne();
    /// 启动三阶段调度中的卸料阶段
    void startStageTwo();
    /// 启动三阶段调度中的码垛阶段
    void startStageThree();
    /// 停止当前调度流程
    void stop();

signals:
    void stageStarted(const QString &stageName);
    void stageCompleted(const QString &stageName);
    void stageError(const QString &msg);
    void logMessage(const QString &msg);

private slots:
    void onStageTimerTimeout();

private:
    enum class StageStep {
        None,
        MoveToPickup,
        CloseGripper,
        LiftLoad,
        MoveToUnload,
        FlipUnload,
        ReleaseLoad,
        MoveEmptyBox,
        ExecuteStackingFunction
    };

    void proceedStage();
    void advanceStep();
    void executeCurrentStep();

    /** @brief 表示一个位姿：x,y,z,rx,ry,rz（单位与华研 SDK 保持一致） */
    struct Pose {
        double x = 0;
        double y = 0;
        double z = 0;
        double rx = 0;
        double ry = 0;
        double rz = 0;
    };

    /// 设置与查询各阶段需要用到的位姿（6 个参数），供外部模块注入
    void setPickupPose(const Pose &p);
    Pose pickupPose() const;

    void setPickupLiftPose(const Pose &p);
    Pose pickupLiftPose() const;

    void setUnloadPose(const Pose &p);
    Pose unloadPose() const;

    void setEmptyBoxPose(const Pose &p);
    Pose emptyBoxPose() const;

    /// 确保机器人已连接，如果未连接则尝试连接
    bool ensureConnected();
    /// 发送 MoveJ 运动命令给华研机器人
    bool executeMoveJ(double x, double y, double z,
                      double rx, double ry, double rz,
                      const QString &cmdId = QStringLiteral("0"));
    /// 控制夹爪开闭
    bool setGripper(bool open);
    /// 运行卸料翻转脚本函数
    bool executeFlipUnload();
    /// 运行阶段三的码垛脚本函数
    bool executeStackingFunction();

    /// 返回阶段名称字符串
    QString stageName(Stage stage) const;

    QTimer *m_stageTimer = nullptr;
    Stage m_stage = Stage::None;
    StageStep m_stageStep = StageStep::None;

    // 可由外部模块注入的位姿（6 参数）
    Pose m_pickupPose;
    Pose m_pickupLiftPose;
    Pose m_unloadPose;
    Pose m_emptyBoxPose;

    QString m_ip;
    unsigned short m_port = 10003;
    unsigned int m_boxID = 0;
    unsigned int m_rbtID = 0;
    bool m_connected = false;

    QString m_stackingFuncName;
    QStringList m_stackingParams;
};

#endif // HUAYANSCHEDULER_H
