#include "visiongriptestdialog.h"

#include "HR_Pro.h"
#include "visionclient.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <string>
#include <vector>

namespace {

constexpr double kDefaultBasketZClearanceMm = 417.0;
constexpr double kMaxZDescendMm = 1078.0;
constexpr double kCompensationIgnoreDistanceMm = 0.5;
constexpr double kZDescendIgnoreDistanceMm = 1.0;

QDoubleSpinBox *createOffsetSpin(QWidget *parent, const QString &suffix)
{
    auto *spin = new QDoubleSpinBox(parent);
    spin->setRange(-2000.0, 2000.0);
    spin->setDecimals(3);
    spin->setSingleStep(1.0);
    spin->setSuffix(suffix);
    spin->setKeyboardTracking(false);
    return spin;
}

} // namespace

VisionGripTestDialog::VisionGripTestDialog(const QString &cameraIp,
                                           int cameraPort,
                                           const QString &robotIp,
                                           int robotPort,
                                           QWidget *parent)
    : QDialog(parent)
    , m_visionClient(new VisionHttpClient(this))
    , m_visionPollTimer(new QTimer(this))
{
    buildUi();
    m_cameraIpEdit->setText(cameraIp);
    m_cameraPortSpin->setValue(cameraPort);
    m_robotIpEdit->setText(robotIp);
    m_robotPortSpin->setValue(robotPort);

    connect(m_visionClient,
            qOverload<double, double, double, double>(&VisionHttpClient::rawCoordinatesReady),
            this,
            &VisionGripTestDialog::onVisionCoordinates);
    connect(m_visionClient, &VisionHttpClient::selectionLogMessage,
            this, [this](const QString &message) {
                // 100ms 连续模式下只保留每秒一次的候选摘要，避免日志刷新反过来影响 UI。
                if (!m_visionPollingActive || m_visionPollingResultCount % 10 == 0)
                    appendLog(message);
            });
    connect(m_visionClient, &VisionHttpClient::noObjectDetected,
            this, [this] {
                m_visionRequestInFlight = false;
                ++m_visionPollingResultCount;
                m_fetchVisionButton->setEnabled(!m_visionPollingActive);
                m_visionStatusLabel->setText(QStringLiteral("未检测到目标"));
                if (!m_visionPollingActive || m_visionPollingResultCount % 10 == 0)
                    appendLog(QStringLiteral("[视觉] 本次响应没有可用目标"));
            });
    connect(m_visionClient, &VisionHttpClient::errorOccurred,
            this, [this](const QString &message) {
                m_visionRequestInFlight = false;
                ++m_visionPollingResultCount;
                m_fetchVisionButton->setEnabled(!m_visionPollingActive);
                m_visionStatusLabel->setText(QStringLiteral("请求失败"));
                if (!m_visionPollingActive || m_visionPollingResultCount % 10 == 0)
                    appendLog(message);
            });
    m_visionPollTimer->setSingleShot(false);
    connect(m_visionPollTimer, &QTimer::timeout,
            this, &VisionGripTestDialog::onVisionPollTimeout);

    appendLog(QStringLiteral("[测试页] 已创建独立视觉客户端和机械臂控制入口"));
    appendLog(QStringLiteral("[安全] 视觉 X/Y/Rz 按基坐标增量执行；Z 下探与抓取补偿按工具坐标执行"));
}

VisionGripTestDialog::~VisionGripTestDialog()
{
    if (m_ownsRobotConnection && HRIF_IsConnected(kBoxId))
        HRIF_DisConnect(kBoxId);
}

void VisionGripTestDialog::setHandEyeMatrix(const float matrix[16])
{
    if (!matrix)
        return;
    m_visionClient->setHandEyeMatrix(matrix);
    appendLog(QStringLiteral("[视觉] 已同步主业务当前生效的手眼矩阵快照"));
}

void VisionGripTestDialog::buildUi()
{
    setWindowTitle(QStringLiteral("视觉抓取独立测试"));
    setMinimumSize(780, 680);
    resize(880, 760);

    auto *root = new QVBoxLayout(this);

    auto *warning = new QLabel(
        QStringLiteral("独立测试会真实控制机械臂。使用前请停止主流程并断开主面板华沿连接；"
                       "视觉 X/Y/Rz 使用基坐标，Z 下探及抓取补偿使用工具坐标。"),
        this);
    warning->setWordWrap(true);
    warning->setStyleSheet(QStringLiteral(
        "QLabel { color: #9c3b13; background: #fff2df; border: 1px solid #e3a25f; "
        "padding: 8px; }"));
    root->addWidget(warning);

    auto *visionGroup = new QGroupBox(QStringLiteral("1. 获取视觉结果"), this);
    auto *visionLayout = new QGridLayout(visionGroup);
    m_cameraIpEdit = new QLineEdit(visionGroup);
    m_cameraPortSpin = new QSpinBox(visionGroup);
    m_cameraPortSpin->setRange(1, 65535);
    m_fetchVisionButton = new QPushButton(QStringLiteral("获取 /inference"), visionGroup);
    m_startVisionPollingButton = new QPushButton(QStringLiteral("开启连续获取"), visionGroup);
    m_stopVisionPollingButton = new QPushButton(QStringLiteral("停止连续获取"), visionGroup);
    m_visionPollingIntervalSpin = new QSpinBox(visionGroup);
    m_visionPollingIntervalSpin->setRange(50, 5000);
    m_visionPollingIntervalSpin->setValue(100);
    m_visionPollingIntervalSpin->setSuffix(QStringLiteral(" ms"));
    m_visionStatusLabel = new QLabel(QStringLiteral("待请求"), visionGroup);
    m_frameInfoLabel = new QLabel(QStringLiteral("frame_id: -   timestamp: -"), visionGroup);
    visionLayout->addWidget(new QLabel(QStringLiteral("相机 IP"), visionGroup), 0, 0);
    visionLayout->addWidget(m_cameraIpEdit, 0, 1);
    visionLayout->addWidget(new QLabel(QStringLiteral("端口"), visionGroup), 0, 2);
    visionLayout->addWidget(m_cameraPortSpin, 0, 3);
    visionLayout->addWidget(m_fetchVisionButton, 0, 4);
    visionLayout->addWidget(m_visionStatusLabel, 1, 0, 1, 2);
    visionLayout->addWidget(m_frameInfoLabel, 1, 2, 1, 3);
    visionLayout->addWidget(new QLabel(QStringLiteral("连续周期"), visionGroup), 2, 0);
    visionLayout->addWidget(m_visionPollingIntervalSpin, 2, 1);
    visionLayout->addWidget(m_startVisionPollingButton, 2, 2);
    visionLayout->addWidget(m_stopVisionPollingButton, 2, 3);
    root->addWidget(visionGroup);

    auto *offsetGroup = new QGroupBox(QStringLiteral("2. 视觉结果与抓取补偿"), this);
    auto *offsetLayout = new QGridLayout(offsetGroup);
    m_xSpin = createOffsetSpin(offsetGroup, QStringLiteral(" mm"));
    m_ySpin = createOffsetSpin(offsetGroup, QStringLiteral(" mm"));
    m_zSpin = createOffsetSpin(offsetGroup, QStringLiteral(" mm"));
    m_rzSpin = createOffsetSpin(offsetGroup, QStringLiteral(" deg"));
    m_xCompensationSpin = createOffsetSpin(offsetGroup, QStringLiteral(" mm"));
    m_yCompensationSpin = createOffsetSpin(offsetGroup, QStringLiteral(" mm"));
    m_zClearanceSpin = createOffsetSpin(offsetGroup, QStringLiteral(" mm"));
    m_xCompensationSpin->setValue(0.0);
    m_yCompensationSpin->setValue(0.0);
    m_zClearanceSpin->setValue(kDefaultBasketZClearanceMm);
    m_zDescendPreviewLabel = new QLabel(offsetGroup);
    m_zDescendPreviewLabel->setMinimumWidth(120);

    offsetLayout->addWidget(new QLabel(QStringLiteral("视觉 X"), offsetGroup), 0, 0);
    offsetLayout->addWidget(m_xSpin, 0, 1);
    offsetLayout->addWidget(new QLabel(QStringLiteral("视觉 Y"), offsetGroup), 0, 2);
    offsetLayout->addWidget(m_ySpin, 0, 3);
    offsetLayout->addWidget(new QLabel(QStringLiteral("视觉深度 Z"), offsetGroup), 1, 0);
    offsetLayout->addWidget(m_zSpin, 1, 1);
    offsetLayout->addWidget(new QLabel(QStringLiteral("视觉 Rz"), offsetGroup), 1, 2);
    offsetLayout->addWidget(m_rzSpin, 1, 3);
    offsetLayout->addWidget(new QLabel(QStringLiteral("X 补偿"), offsetGroup), 2, 0);
    offsetLayout->addWidget(m_xCompensationSpin, 2, 1);
    offsetLayout->addWidget(new QLabel(QStringLiteral("Y 补偿"), offsetGroup), 2, 2);
    offsetLayout->addWidget(m_yCompensationSpin, 2, 3);
    offsetLayout->addWidget(new QLabel(QStringLiteral("Z 工位余量"), offsetGroup), 3, 0);
    offsetLayout->addWidget(m_zClearanceSpin, 3, 1);
    offsetLayout->addWidget(new QLabel(QStringLiteral("计算下探量"), offsetGroup), 3, 2);
    offsetLayout->addWidget(m_zDescendPreviewLabel, 3, 3);
    root->addWidget(offsetGroup);

    auto *robotGroup = new QGroupBox(QStringLiteral("3. 机械臂与动作"), this);
    auto *robotLayout = new QVBoxLayout(robotGroup);
    auto *connectionRow = new QHBoxLayout();
    m_robotIpEdit = new QLineEdit(robotGroup);
    m_robotPortSpin = new QSpinBox(robotGroup);
    m_robotPortSpin->setRange(1, 65535);
    m_connectButton = new QPushButton(QStringLiteral("连接"), robotGroup);
    m_disconnectButton = new QPushButton(QStringLiteral("断开"), robotGroup);
    m_robotStatusLabel = new QLabel(QStringLiteral("未连接"), robotGroup);
    connectionRow->addWidget(new QLabel(QStringLiteral("机器人 IP"), robotGroup));
    connectionRow->addWidget(m_robotIpEdit, 1);
    connectionRow->addWidget(new QLabel(QStringLiteral("端口"), robotGroup));
    connectionRow->addWidget(m_robotPortSpin);
    connectionRow->addWidget(m_connectButton);
    connectionRow->addWidget(m_disconnectButton);
    connectionRow->addWidget(m_robotStatusLabel);
    robotLayout->addLayout(connectionRow);

    auto *motionParameters = new QHBoxLayout();
    m_overrideSpin = new QSpinBox(robotGroup);
    m_overrideSpin->setRange(1, 100);
    m_overrideSpin->setValue(60);
    m_overrideSpin->setSuffix(QStringLiteral(" %"));
    m_velocitySpin = createOffsetSpin(robotGroup, QStringLiteral(" mm/s"));
    m_velocitySpin->setRange(1.0, 2000.0);
    m_velocitySpin->setValue(30.0);
    m_accelerationSpin = createOffsetSpin(robotGroup, QStringLiteral(" mm/s^2"));
    m_accelerationSpin->setRange(1.0, 5000.0);
    m_accelerationSpin->setValue(60.0);
    m_radiusSpin = createOffsetSpin(robotGroup, QStringLiteral(" mm"));
    m_radiusSpin->setRange(0.0, 1000.0);
    m_radiusSpin->setValue(0.0);
    motionParameters->addWidget(new QLabel(QStringLiteral("全局速度"), robotGroup));
    motionParameters->addWidget(m_overrideSpin);
    motionParameters->addWidget(new QLabel(QStringLiteral("MoveL 速度"), robotGroup));
    motionParameters->addWidget(m_velocitySpin);
    motionParameters->addWidget(new QLabel(QStringLiteral("加速度"), robotGroup));
    motionParameters->addWidget(m_accelerationSpin);
    motionParameters->addWidget(new QLabel(QStringLiteral("圆滑半径"), robotGroup));
    motionParameters->addWidget(m_radiusSpin);
    robotLayout->addLayout(motionParameters);

    auto *moveRow = new QHBoxLayout();
    m_moveAllButton = new QPushButton(QStringLiteral("联合移动 X/Y/Rz"), robotGroup);
    m_moveXyButton = new QPushButton(QStringLiteral("视觉 X/Y"), robotGroup);
    m_moveRzButton = new QPushButton(QStringLiteral("视觉 Rz"), robotGroup);
    moveRow->addWidget(m_moveAllButton, 2);
    moveRow->addWidget(m_moveXyButton);
    moveRow->addWidget(m_moveRzButton);
    robotLayout->addLayout(moveRow);

    auto *grabAdjustmentRow = new QHBoxLayout();
    m_moveZButton = new QPushButton(QStringLiteral("Z 下探"), robotGroup);
    m_moveXCompensationButton = new QPushButton(QStringLiteral("X 补偿"), robotGroup);
    m_moveYCompensationButton = new QPushButton(QStringLiteral("Y 补偿"), robotGroup);
    grabAdjustmentRow->addWidget(m_moveZButton, 2);
    grabAdjustmentRow->addWidget(m_moveXCompensationButton);
    grabAdjustmentRow->addWidget(m_moveYCompensationButton);
    robotLayout->addLayout(grabAdjustmentRow);

    auto *gripperRow = new QHBoxLayout();
    m_gripFunctionEdit = new QLineEdit(QStringLiteral("Func_jiajin"), robotGroup);
    m_releaseFunctionEdit = new QLineEdit(QStringLiteral("Func_songzhua"), robotGroup);
    m_gripButton = new QPushButton(QStringLiteral("夹紧"), robotGroup);
    m_releaseButton = new QPushButton(QStringLiteral("松开"), robotGroup);
    m_stopButton = new QPushButton(QStringLiteral("停止机械臂"), robotGroup);
    m_stopButton->setStyleSheet(QStringLiteral("QPushButton { color: #a40000; font-weight: 600; }"));
    gripperRow->addWidget(new QLabel(QStringLiteral("夹紧函数"), robotGroup));
    gripperRow->addWidget(m_gripFunctionEdit);
    gripperRow->addWidget(m_gripButton);
    gripperRow->addWidget(new QLabel(QStringLiteral("松开函数"), robotGroup));
    gripperRow->addWidget(m_releaseFunctionEdit);
    gripperRow->addWidget(m_releaseButton);
    gripperRow->addWidget(m_stopButton);
    robotLayout->addLayout(gripperRow);
    root->addWidget(robotGroup);

    m_logView = new QPlainTextEdit(this);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(1000);
    m_logView->setPlaceholderText(QStringLiteral("视觉、SDK 返回和动作命令会记录在这里"));
    root->addWidget(m_logView, 1);

    connect(m_fetchVisionButton, &QPushButton::clicked,
            this, &VisionGripTestDialog::fetchVisionResult);
    connect(m_startVisionPollingButton, &QPushButton::clicked,
            this, &VisionGripTestDialog::startVisionPolling);
    connect(m_stopVisionPollingButton, &QPushButton::clicked,
            this, &VisionGripTestDialog::stopVisionPolling);
    connect(m_connectButton, &QPushButton::clicked,
            this, &VisionGripTestDialog::connectRobot);
    connect(m_disconnectButton, &QPushButton::clicked,
            this, &VisionGripTestDialog::disconnectRobot);
    connect(m_moveAllButton, &QPushButton::clicked,
            this, &VisionGripTestDialog::moveAllAxes);
    connect(m_moveXyButton, &QPushButton::clicked,
            this, &VisionGripTestDialog::moveXY);
    connect(m_moveZButton, &QPushButton::clicked,
            this, &VisionGripTestDialog::moveZ);
    connect(m_moveRzButton, &QPushButton::clicked,
            this, &VisionGripTestDialog::moveRz);
    connect(m_moveXCompensationButton, &QPushButton::clicked,
            this, &VisionGripTestDialog::moveXCompensation);
    connect(m_moveYCompensationButton, &QPushButton::clicked,
            this, &VisionGripTestDialog::moveYCompensation);
    connect(m_zSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &VisionGripTestDialog::updateZDescendPreview);
    connect(m_zClearanceSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &VisionGripTestDialog::updateZDescendPreview);
    connect(m_gripButton, &QPushButton::clicked,
            this, &VisionGripTestDialog::grip);
    connect(m_releaseButton, &QPushButton::clicked,
            this, &VisionGripTestDialog::release);
    connect(m_stopButton, &QPushButton::clicked,
            this, &VisionGripTestDialog::stopRobot);

    setVisionPollingUi(false);
    setRobotConnectedUi(false);
    updateZDescendPreview();
}

void VisionGripTestDialog::fetchVisionResult()
{
    if (m_visionRequestInFlight)
        return;

    const QString ip = m_cameraIpEdit->text().trimmed();
    if (ip.isEmpty()) {
        appendLog(QStringLiteral("[视觉] 相机 IP 不能为空"));
        return;
    }

    m_visionClient->setServerUrl(ip, m_cameraPortSpin->value());
    m_visionRequestInFlight = true;
    m_fetchVisionButton->setEnabled(false);
    m_visionStatusLabel->setText(QStringLiteral("请求中..."));
    if (!m_visionPollingActive) {
        appendLog(QStringLiteral("[视觉] 请求 http://%1:%2/inference")
                      .arg(ip)
                      .arg(m_cameraPortSpin->value()));
    }
    m_visionClient->fetchInference();
}

void VisionGripTestDialog::startVisionPolling()
{
    if (m_visionPollingActive)
        return;
    if (m_cameraIpEdit->text().trimmed().isEmpty()) {
        appendLog(QStringLiteral("[视觉] 相机 IP 不能为空，无法开启连续获取"));
        return;
    }

    m_visionPollingActive = true;
    m_visionPollingResultCount = 0;
    m_visionPollTimer->start(m_visionPollingIntervalSpin->value());
    setVisionPollingUi(true);
    appendLog(QStringLiteral("[视觉] 已开启连续获取，周期=%1ms；请求未完成时自动跳过本轮")
                  .arg(m_visionPollingIntervalSpin->value()));
    // 点击后立即取第一帧，不必先等待一个定时周期。
    fetchVisionResult();
}

void VisionGripTestDialog::stopVisionPolling()
{
    if (!m_visionPollingActive && !m_visionRequestInFlight)
        return;

    m_visionPollTimer->stop();
    m_visionPollingActive = false;
    m_visionClient->invalidateInferenceRequests();
    m_visionRequestInFlight = false;
    setVisionPollingUi(false);
    m_visionStatusLabel->setText(QStringLiteral("连续获取已停止"));
    appendLog(QStringLiteral("[视觉] 已停止连续获取"));
}

void VisionGripTestDialog::onVisionPollTimeout()
{
    if (m_visionPollingActive && !m_visionRequestInFlight)
        fetchVisionResult();
}

void VisionGripTestDialog::onVisionCoordinates(double x, double y, double z, double rz)
{
    m_xSpin->setValue(x);
    m_ySpin->setValue(y);
    // VisionHttpClient 输出的是用于抓取计算的正深度。Z 的方向和余量统一在“Z 下探”动作中处理，
    // 这里保留原始值，避免把视觉深度与基坐标有符号偏移混为一谈。
    m_zSpin->setValue(z);
    m_rzSpin->setValue(rz);
    m_visionRequestInFlight = false;
    ++m_visionPollingResultCount;
    m_fetchVisionButton->setEnabled(!m_visionPollingActive);
    m_visionStatusLabel->setText(
        m_visionPollingActive
            ? QStringLiteral("连续获取中（%1ms）").arg(m_visionPollingIntervalSpin->value())
            : QStringLiteral("已接收并填入偏移"));
    m_frameInfoLabel->setText(QStringLiteral("frame_id: %1   timestamp(ms): %2")
                                  .arg(m_visionClient->lastInferenceFrameId())
                                  .arg(m_visionClient->lastInferenceTimestampMs()));
    if (!m_visionPollingActive || m_visionPollingResultCount % 10 == 0) {
        appendLog(QStringLiteral("[视觉] X=%1mm, Y=%2mm, 深度Z=%3mm, Rz=%4deg")
                      .arg(x, 0, 'f', 3)
                      .arg(y, 0, 'f', 3)
                      .arg(z, 0, 'f', 3)
                      .arg(rz, 0, 'f', 3));
    }
}

void VisionGripTestDialog::connectRobot()
{
    if (m_ownsRobotConnection && HRIF_IsConnected(kBoxId)) {
        appendLog(QStringLiteral("[机械臂] 测试页已经连接"));
        return;
    }
    if (HRIF_IsConnected(kBoxId)) {
        appendLog(QStringLiteral("[机械臂] boxID=0 已被其他模块占用，测试页拒绝接管；请先断开主面板连接"));
        return;
    }

    const QString ip = m_robotIpEdit->text().trimmed();
    if (ip.isEmpty()) {
        appendLog(QStringLiteral("[机械臂] 机器人 IP 不能为空"));
        return;
    }

    appendLog(QStringLiteral("[机械臂] 正在连接 %1:%2")
                  .arg(ip)
                  .arg(m_robotPortSpin->value()));
    int ret = HRIF_Connect(kBoxId, ip.toStdString().c_str(), m_robotPortSpin->value());
    if (ret != 0) {
        appendLog(QStringLiteral("[机械臂] 连接失败：ret=%1，%2")
                      .arg(ret)
                      .arg(describeSdkError(ret)));
        return;
    }

    m_ownsRobotConnection = true;
    ret = HRIF_SetOverride(kBoxId, kRobotId, m_overrideSpin->value() / 100.0);
    if (ret != 0)
        appendLog(QStringLiteral("[机械臂] 设置全局速度失败：ret=%1，%2")
                      .arg(ret)
                      .arg(describeSdkError(ret)));

    ret = HRIF_XToStandby(kBoxId, kRobotId);
    if (ret != 0) {
        appendLog(QStringLiteral("[机械臂] 进入 Standby 任务下发失败：ret=%1，%2")
                      .arg(ret)
                      .arg(describeSdkError(ret)));
    } else {
        appendLog(QStringLiteral("[机械臂] Standby 任务已下发，请确认示教器状态后再运动"));
    }
    setRobotConnectedUi(true);
}

void VisionGripTestDialog::disconnectRobot()
{
    if (!m_ownsRobotConnection) {
        appendLog(QStringLiteral("[机械臂] 测试页没有可断开的自有连接"));
        setRobotConnectedUi(false);
        return;
    }

    const int ret = HRIF_DisConnect(kBoxId);
    if (ret != 0) {
        appendLog(QStringLiteral("[机械臂] 断开失败：ret=%1，%2")
                      .arg(ret)
                      .arg(describeSdkError(ret)));
        return;
    }
    m_ownsRobotConnection = false;
    setRobotConnectedUi(false);
    appendLog(QStringLiteral("[机械臂] 已断开测试页连接"));
}

void VisionGripTestDialog::moveAllAxes()
{
    // 平面视觉调整保持一次下发；Z 按原抓取逻辑使用工具坐标单独下探。
    executeBaseRelativeMove(QStringLiteral("联合移动 X/Y/Rz"), {true, true, false, true});
}

void VisionGripTestDialog::moveXY()
{
    executeBaseRelativeMove(QStringLiteral("只动 X/Y"), {true, true, false, false});
}

void VisionGripTestDialog::moveZ()
{
    const double plannedDescend = m_zSpin->value() - m_zClearanceSpin->value();
    if (plannedDescend > kMaxZDescendMm) {
        appendLog(QStringLiteral("[机械臂] Z 下探失败：计划下探 %1mm 超过硬上限 %2mm")
                      .arg(plannedDescend, 0, 'f', 3)
                      .arg(kMaxZDescendMm, 0, 'f', 1));
        return;
    }

    // 与 HuayanScheduler::calculateGrabDescend 一致：负值按 0 处理，异常大值拒绝下发。
    const double descend = qBound(0.0, plannedDescend, kMaxZDescendMm);
    executeToolRelativeAxisMove(
        QStringLiteral("Z 下探（深度 %1 - 余量 %2）")
            .arg(m_zSpin->value(), 0, 'f', 3)
            .arg(m_zClearanceSpin->value(), 0, 'f', 3),
        2,
        descend,
        kZDescendIgnoreDistanceMm);
}

void VisionGripTestDialog::moveRz()
{
    executeBaseRelativeMove(QStringLiteral("只转 Rz"), {false, false, false, true});
}

void VisionGripTestDialog::moveXCompensation()
{
    executeToolRelativeAxisMove(QStringLiteral("X 补偿"),
                                0,
                                m_xCompensationSpin->value(),
                                kCompensationIgnoreDistanceMm);
}

void VisionGripTestDialog::moveYCompensation()
{
    executeToolRelativeAxisMove(QStringLiteral("Y 补偿"),
                                1,
                                m_yCompensationSpin->value(),
                                kCompensationIgnoreDistanceMm);
}

void VisionGripTestDialog::updateZDescendPreview()
{
    const double plannedDescend = m_zSpin->value() - m_zClearanceSpin->value();
    if (plannedDescend > kMaxZDescendMm) {
        m_zDescendPreviewLabel->setText(
            QStringLiteral("%1 mm（超限）").arg(plannedDescend, 0, 'f', 3));
        m_zDescendPreviewLabel->setStyleSheet(QStringLiteral("QLabel { color: #a40000; }"));
        return;
    }

    m_zDescendPreviewLabel->setText(
        QStringLiteral("%1 mm").arg(qMax(0.0, plannedDescend), 0, 'f', 3));
    m_zDescendPreviewLabel->setStyleSheet(QString());
}

void VisionGripTestDialog::grip()
{
    runRobotFunction(QStringLiteral("夹紧"), m_gripFunctionEdit->text().trimmed());
}

void VisionGripTestDialog::release()
{
    runRobotFunction(QStringLiteral("松开"), m_releaseFunctionEdit->text().trimmed());
}

void VisionGripTestDialog::stopRobot()
{
    if (!ensureOwnedConnection(QStringLiteral("停止机械臂")))
        return;
    const int ret = HRIF_GrpStop(kBoxId, kRobotId);
    appendLog(ret == 0
                  ? QStringLiteral("[机械臂] 停止指令已下发")
                  : QStringLiteral("[机械臂] 停止指令失败：ret=%1，%2")
                        .arg(ret)
                        .arg(describeSdkError(ret)));
}

void VisionGripTestDialog::closeEvent(QCloseEvent *event)
{
    m_visionPollTimer->stop();
    m_visionPollingActive = false;
    m_visionRequestInFlight = false;
    m_visionClient->invalidateInferenceRequests();
    if (m_ownsRobotConnection)
        disconnectRobot();
    QDialog::closeEvent(event);
}

void VisionGripTestDialog::appendLog(const QString &message)
{
    const QString line = QStringLiteral("[%1] %2")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")),
                                  message);
    m_logView->appendPlainText(line);
    emit logMessage(QStringLiteral("[视觉抓取测试] %1").arg(message));
}

void VisionGripTestDialog::setVisionPollingUi(bool polling)
{
    m_startVisionPollingButton->setEnabled(!polling);
    m_stopVisionPollingButton->setEnabled(polling);
    m_visionPollingIntervalSpin->setEnabled(!polling);
    m_cameraIpEdit->setEnabled(!polling);
    m_cameraPortSpin->setEnabled(!polling);
    m_fetchVisionButton->setEnabled(!polling && !m_visionRequestInFlight);
}

void VisionGripTestDialog::setRobotConnectedUi(bool connected)
{
    m_connectButton->setEnabled(!connected);
    m_disconnectButton->setEnabled(connected);
    m_moveAllButton->setEnabled(connected);
    m_moveXyButton->setEnabled(connected);
    m_moveZButton->setEnabled(connected);
    m_moveRzButton->setEnabled(connected);
    m_moveXCompensationButton->setEnabled(connected);
    m_moveYCompensationButton->setEnabled(connected);
    m_gripButton->setEnabled(connected);
    m_releaseButton->setEnabled(connected);
    m_stopButton->setEnabled(connected);
    m_robotIpEdit->setEnabled(!connected);
    m_robotPortSpin->setEnabled(!connected);
    m_robotStatusLabel->setText(connected ? QStringLiteral("测试页已连接")
                                          : QStringLiteral("未连接"));
}

bool VisionGripTestDialog::ensureOwnedConnection(const QString &operation)
{
    if (m_ownsRobotConnection && HRIF_IsConnected(kBoxId))
        return true;

    m_ownsRobotConnection = false;
    setRobotConnectedUi(false);
    appendLog(QStringLiteral("[机械臂] %1失败：测试页未持有有效连接").arg(operation));
    return false;
}

void VisionGripTestDialog::executeBaseRelativeMove(const QString &label, const AxisMask &mask)
{
    if (!ensureOwnedConnection(label))
        return;

    const double x = mask.x ? m_xSpin->value() : 0.0;
    const double y = mask.y ? m_ySpin->value() : 0.0;
    const double z = mask.z ? m_zSpin->value() : 0.0;
    const double rz = mask.rz ? m_rzSpin->value() : 0.0;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(rz)) {
        appendLog(QStringLiteral("[机械臂] %1失败：偏移包含非有限数值").arg(label));
        return;
    }

    const int overrideRet = HRIF_SetOverride(
        kBoxId, kRobotId, m_overrideSpin->value() / 100.0);
    if (overrideRet != 0) {
        appendLog(QStringLiteral("[机械臂] %1前设置全局速度失败：ret=%2，%3")
                      .arg(label)
                      .arg(overrideRet)
                      .arg(describeSdkError(overrideRet)));
        return;
    }

    // 厂商 WayPointRel 示例定义：nType=1 为 MoveL，nrelMoveType=1 为叠加值，
    // UCS="Base" 使启用的轴按基坐标解释。联合与分项按钮复用同一接口，
    // 只通过轴掩码决定哪些分量进入本次轨迹。
    const int ret = HRIF_WayPointRel(
        kBoxId, kRobotId,
        1,
        0,
        0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0,
        1,
        mask.x ? 1 : 0,
        mask.y ? 1 : 0,
        mask.z ? 1 : 0,
        0,
        0,
        mask.rz ? 1 : 0,
        x, y, z, 0, 0, rz,
        std::string("TCP"), std::string("Base"),
        m_velocitySpin->value(),
        m_accelerationSpin->value(),
        m_radiusSpin->value(),
        0, 0, 0, 0,
        nextCommandId().toStdString());

    if (ret != 0) {
        appendLog(QStringLiteral("[机械臂] %1下发失败：ret=%2，%3")
                      .arg(label)
                      .arg(ret)
                      .arg(describeSdkError(ret)));
        return;
    }
    appendLog(QStringLiteral("[机械臂] %1已下发：Base dX=%2, dY=%3, dZ=%4, dRz=%5")
                  .arg(label)
                  .arg(x, 0, 'f', 3)
                  .arg(y, 0, 'f', 3)
                  .arg(z, 0, 'f', 3)
                  .arg(rz, 0, 'f', 3));
}

void VisionGripTestDialog::executeToolRelativeAxisMove(const QString &label,
                                                       int poseId,
                                                       double signedDistance,
                                                       double ignoreDistance)
{
    if (!ensureOwnedConnection(label))
        return;
    if (!std::isfinite(signedDistance)) {
        appendLog(QStringLiteral("[机械臂] %1失败：距离不是有限数值").arg(label));
        return;
    }
    if (std::abs(signedDistance) < ignoreDistance) {
        appendLog(QStringLiteral("[机械臂] %1无需执行：距离 %2mm 小于动作阈值 %3mm")
                      .arg(label)
                      .arg(signedDistance, 0, 'f', 3)
                      .arg(ignoreDistance, 0, 'f', 1));
        return;
    }

    const int overrideRet = HRIF_SetOverride(
        kBoxId, kRobotId, m_overrideSpin->value() / 100.0);
    if (overrideRet != 0) {
        appendLog(QStringLiteral("[机械臂] %1前设置全局速度失败：ret=%2，%3")
                      .arg(label)
                      .arg(overrideRet)
                      .arg(describeSdkError(overrideRet)));
        return;
    }

    // 与原抓取流程保持相同的 MoveRelL 参数：poseId 0/1/2 对应工具 X/Y/Z，
    // direction=1 为正向、0 为反向，nToolMotion=1 表示按当前 TCP/工具坐标运动。
    const int direction = signedDistance >= 0.0 ? 1 : 0;
    const int ret = HRIF_MoveRelL(kBoxId,
                                  kRobotId,
                                  poseId,
                                  direction,
                                  std::abs(signedDistance),
                                  1);
    if (ret != 0) {
        appendLog(QStringLiteral("[机械臂] %1下发失败：ret=%2，%3")
                      .arg(label)
                      .arg(ret)
                      .arg(describeSdkError(ret)));
        return;
    }

    static const char *const kAxisNames[] = {"X", "Y", "Z"};
    const QString axis = poseId >= 0 && poseId < 3
        ? QString::fromLatin1(kAxisNames[poseId])
        : QString::number(poseId);
    appendLog(QStringLiteral("[机械臂] %1已下发：工具坐标 %2=%3mm")
                  .arg(label)
                  .arg(axis)
                  .arg(signedDistance, 0, 'f', 3));
}

void VisionGripTestDialog::runRobotFunction(const QString &label,
                                            const QString &functionName)
{
    if (!ensureOwnedConnection(label))
        return;
    if (functionName.isEmpty()) {
        appendLog(QStringLiteral("[机械臂] %1失败：示教器函数名为空").arg(label));
        return;
    }

    std::vector<std::string> parameters;
    const int ret = HRIF_RunFunc(kBoxId, functionName.toStdString(), parameters);
    appendLog(ret == 0
                  ? QStringLiteral("[机械臂] %1函数已下发：%2").arg(label, functionName)
                  : QStringLiteral("[机械臂] %1失败：ret=%2，%3")
                        .arg(label)
                        .arg(ret)
                        .arg(describeSdkError(ret)));
}

QString VisionGripTestDialog::describeSdkError(int code) const
{
    std::string message;
    if (HRIF_GetErrorCodeStr(kBoxId, code, message) == 0 && !message.empty())
        return QString::fromStdString(message);
    return QStringLiteral("SDK 未返回错误说明");
}

QString VisionGripTestDialog::nextCommandId()
{
    ++m_commandSequence;
    return QStringLiteral("vision-test-%1-%2")
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(m_commandSequence);
}
