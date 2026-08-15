#ifndef VISIONGRIPTESTDIALOG_H
#define VISIONGRIPTESTDIALOG_H

#include <QDialog>

class QCloseEvent;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;
class VisionHttpClient;

/**
 * @brief 视觉结果与机械臂动作的独立联调窗口。
 *
 * 本窗口只创建自己的 VisionHttpClient，并直接调用华沿 SDK。它不会启动阶段一、
 * 不会连接任务队列，也不会复用 HuayanScheduler 的状态机。机械臂连接使用 SDK
 * boxID=0，因此连接前会检查该编号是否已经被主业务占用；窗口只断开自己建立的连接。
 */
class VisionGripTestDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit VisionGripTestDialog(const QString &cameraIp,
                                  int cameraPort,
                                  const QString &robotIp,
                                  int robotPort,
                                  QWidget *parent = nullptr);
    ~VisionGripTestDialog() override;

    /// 将主业务当前生效的手眼矩阵复制到本窗口的独立视觉客户端。
    void setHandEyeMatrix(const float matrix[16]);

signals:
    /// 将关键操作同步到主窗口日志，便于一次现场测试结束后统一追溯。
    void logMessage(const QString &message);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void fetchVisionResult();
    void startVisionPolling();
    void stopVisionPolling();
    void onVisionPollTimeout();
    void onVisionCoordinates(double x, double y, double z, double rz);
    void connectRobot();
    void disconnectRobot();
    void moveAllAxes();
    void moveXY();
    void moveZ();
    void moveRz();
    void moveXCompensation();
    void moveYCompensation();
    void updateZDescendPreview();
    void grip();
    void release();
    void stopRobot();

private:
    struct AxisMask {
        bool x = false;
        bool y = false;
        bool z = false;
        bool rz = false;
    };

    void buildUi();
    void appendLog(const QString &message);
    void setVisionPollingUi(bool polling);
    void setRobotConnectedUi(bool connected);
    bool ensureOwnedConnection(const QString &operation);
    void executeBaseRelativeMove(const QString &label, const AxisMask &mask);
    void executeToolRelativeAxisMove(const QString &label,
                                     int poseId,
                                     double signedDistance,
                                     double ignoreDistance);
    void runRobotFunction(const QString &label, const QString &functionName);
    QString describeSdkError(int code) const;
    QString nextCommandId();

    static constexpr unsigned int kBoxId = 0;
    static constexpr unsigned int kRobotId = 0;

    VisionHttpClient *m_visionClient = nullptr;

    QLineEdit *m_cameraIpEdit = nullptr;
    QSpinBox *m_cameraPortSpin = nullptr;
    QPushButton *m_fetchVisionButton = nullptr;
    QPushButton *m_startVisionPollingButton = nullptr;
    QPushButton *m_stopVisionPollingButton = nullptr;
    QSpinBox *m_visionPollingIntervalSpin = nullptr;
    QLabel *m_visionStatusLabel = nullptr;
    QLabel *m_frameInfoLabel = nullptr;
    QTimer *m_visionPollTimer = nullptr;

    QDoubleSpinBox *m_xSpin = nullptr;
    QDoubleSpinBox *m_ySpin = nullptr;
    QDoubleSpinBox *m_zSpin = nullptr;
    QDoubleSpinBox *m_rzSpin = nullptr;
    QDoubleSpinBox *m_xCompensationSpin = nullptr;
    QDoubleSpinBox *m_yCompensationSpin = nullptr;
    QDoubleSpinBox *m_zClearanceSpin = nullptr;
    QLabel *m_zDescendPreviewLabel = nullptr;

    QLineEdit *m_robotIpEdit = nullptr;
    QSpinBox *m_robotPortSpin = nullptr;
    QSpinBox *m_overrideSpin = nullptr;
    QDoubleSpinBox *m_velocitySpin = nullptr;
    QDoubleSpinBox *m_accelerationSpin = nullptr;
    QDoubleSpinBox *m_radiusSpin = nullptr;
    QLineEdit *m_gripFunctionEdit = nullptr;
    QLineEdit *m_releaseFunctionEdit = nullptr;
    QLabel *m_robotStatusLabel = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_disconnectButton = nullptr;
    QPushButton *m_moveAllButton = nullptr;
    QPushButton *m_moveXyButton = nullptr;
    QPushButton *m_moveZButton = nullptr;
    QPushButton *m_moveRzButton = nullptr;
    QPushButton *m_moveXCompensationButton = nullptr;
    QPushButton *m_moveYCompensationButton = nullptr;
    QPushButton *m_gripButton = nullptr;
    QPushButton *m_releaseButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QPlainTextEdit *m_logView = nullptr;

    bool m_ownsRobotConnection = false;
    bool m_visionPollingActive = false;
    bool m_visionRequestInFlight = false;
    quint64 m_visionPollingResultCount = 0;
    quint64 m_commandSequence = 0;
};

#endif // VISIONGRIPTESTDIALOG_H
