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

    struct Pose {
        double x = 0;
        double y = 0;
        double z = 0;
        double rx = 0;
        double ry = 0;
        double rz = 0;
    };

    explicit HuayanScheduler(QObject *parent = nullptr);
    ~HuayanScheduler() override;

    void setConnectionParams(const QString &ip,
                             unsigned short port = 10003,
                             unsigned int boxID = 0,
                             unsigned int rbtID = 0);

    bool connectRobot();
    bool disconnectRobot();
    bool isConnected() const;

    void setStackingFunction(const QString &funcName,
                             const QStringList &params = QStringList());

    bool startRobotScript();
    bool stopRobotScript();

    void startStageOne();
    void startStageTwo();
    void startStageThree();
    void stop();

    void setSurveyPose(const Pose &p);
    void releaseGripper();   // 手动松开夹爪（UI 按钮调用）

public slots:
    void setGrabOffset(double x, double y, double z, double rz);

signals:
    void connected();
    void disconnected();
    void stageStarted(const QString &stageName);
    void stageCompleted(const QString &stageName);
    void stageError(const QString &msg);
    void logMessage(const QString &msg);
    void surveyReady();

private slots:
    void onPollTick();
    void onStepTimeout();

private:
    enum class StageStep {
        None,
        MoveToSurvey,
        WaitForVision,
        MoveToGrab,
        MoveToPickup,
        CloseGripper,
        LiftLoad,
        MoveToUnload,
        FlipUnload,
        ReleaseLoad,
        MoveEmptyBox,
        ExecuteStackingFunction
    };

    // 工具坐标系单轴相对运动（HRIF_MoveRelL），用于视觉偏移的分轴串联微调
    struct RelMove {
        int poseId;     // 0~5 = X/Y/Z/Rx/Ry/Rz
        int direction;  // 0=负向, 1=正向
        double distance;
    };

    void proceedStage();
    void advanceStep();
    void executeCurrentStep();
    bool executeNextGrabMove();

    void setPickupPose(const Pose &p);
    Pose pickupPose() const;

    void setPickupLiftPose(const Pose &p);
    Pose pickupLiftPose() const;

    void setUnloadPose(const Pose &p);
    Pose unloadPose() const;

    void setEmptyBoxPose(const Pose &p);
    Pose emptyBoxPose() const;

    bool ensureConnected();
    bool executeMoveJ(double x, double y, double z,
                      double rx, double ry, double rz,
                      const QString &cmdId   = QStringLiteral("0"),
                      const QString &ucsName = QStringLiteral("Base"));
    bool setGripper(bool open);
    bool executeRunFunc(const QString &funcName, int timeoutMs = 30000);
    bool executeGripFunc();
    bool executeFlipUnload();
    bool executeStackingFunction();

    void startWaitForIdle(int timeoutMs = 30000);

    QString stageName(Stage stage) const;

    QTimer *m_pollTimer    = nullptr;
    QTimer *m_timeoutTimer = nullptr;
    int     m_pollCount    = 0;

    Stage m_stage = Stage::None;
    StageStep m_stageStep = StageStep::None;

    Pose m_surveyPose;
    Pose m_grabOffset;
    QList<RelMove> m_grabMoves;
    int m_grabMoveIdx = 0;
    int m_grabIterations = 0;   // 闭环视觉矫正的迭代计数
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

    // 示教器脚本与函数名（方案 C：上位机调用示教器内已示教好的函数）
    QString m_scriptName      = QStringLiteral("dashiceshi");
    QString m_captureFuncName = QStringLiteral("Func_capture");
    QString m_gripFuncName    = QStringLiteral("Func_jiajin");
    QString m_releaseFuncName = QStringLiteral("Func_songzhua");
};

#endif // HUAYANSCHEDULER_H
