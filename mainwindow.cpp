#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "camerawindow.h"
#include "workflowengine.h"
#include "robotcontroller.h"
#include "agvcontroller.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QDateTime>
#include <QFont>
#include <QFormLayout>
#include <QScrollArea>

// ============================================================
//  MainWindow
// ============================================================

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // ── 1. 先构造业务层，UI 初始化时可引用 m_devMgr ──────────
    m_engine = new WorkflowEngine(this);
    m_devMgr = new DeviceManager(this);

    // ── 注入硬件控制器，引擎从仿真模式升级为真实 Modbus 控制 ──
    m_engine->setRobotController(m_devMgr->robotController());
    m_engine->setAgvController(m_devMgr->agvController());

    // ── 2. 构建所有 UI 控件 ──────────────────────────────────
    initUI();

    // ── 3. 连接 WorkflowEngine 信号 ──────────────────────────
    connect(m_engine, &WorkflowEngine::stepActivated,
            this, &MainWindow::onStepActivated);
    connect(m_engine, &WorkflowEngine::started,
            this, &MainWindow::onEngineStarted);
    connect(m_engine, &WorkflowEngine::stopped,
            this, &MainWindow::onEngineStopped);
    // 引擎日志/错误消息转发到日志控件
    connect(m_engine, &WorkflowEngine::stepLog,
            this, [this](const QString &msg) { log(msg); });
    connect(m_engine, &WorkflowEngine::engineError,
            this, [this](const QString &msg) { log(QString("⚠ %1").arg(msg)); });
    // 步进延时 SpinBox 直接驱动引擎（运行中也生效）
    connect(m_spinDelay, &QSpinBox::valueChanged,
            m_engine, &WorkflowEngine::setInterval);

    // ── 4. 连接 DeviceManager 信号 ───────────────────────────
    connect(m_devMgr, &DeviceManager::logMessage,
            this, [this](const QString &msg) { log(msg); });

    connect(m_devMgr, &DeviceManager::robotStatusChanged,
            this, [this](bool ok, const QString &s) { m_indRobot->setStatus(ok, s); });
    connect(m_devMgr, &DeviceManager::agvStatusChanged,
            this, [this](bool ok, const QString &s) { m_indAGV->setStatus(ok, s); });
    connect(m_devMgr, &DeviceManager::cameraStatusChanged,
            this, [this](bool ok, const QString &s) { m_indCameraDebug->setStatus(ok, s); });
    connect(m_devMgr, &DeviceManager::scannerStatusChanged,
            this, [this](bool ok, const QString &s) { m_indScanner->setStatus(ok, s); });

    connect(m_devMgr, &DeviceManager::lightChanged,
            this, &MainWindow::onLightChanged);
    connect(m_devMgr, &DeviceManager::configApplied,
            this, &MainWindow::onConfigApplied);

    // ── 5. 透传 Modbus 连接状态到机械臂控制面板 ──────────────
    connect(m_devMgr, &DeviceManager::robotModbusConnected, this, [this]() {
        m_indRobotConn->setStatus(true, QStringLiteral("已连接"));
        m_indRobot->setStatus(true, QStringLiteral("已连接"));
        log(QStringLiteral("[机械臂] Modbus 连接成功 ✓"));
    });
    connect(m_devMgr, &DeviceManager::robotModbusDisconnected, this, [this]() {
        m_indRobotConn->setStatus(false, QStringLiteral("断开"));
        m_indRobot->setStatus(false, QStringLiteral("离线"));
    });
    connect(m_devMgr, &DeviceManager::robotModbusError,
            this, [this](const QString &msg) {
        log(QString("[机械臂错误] %1").arg(msg));
    });

    // ── AGV Modbus 连接状态 ────────────────────────────────────
    connect(m_devMgr, &DeviceManager::agvModbusConnected, this, [this]() {
        m_indAGV->setStatus(true, QStringLiteral("已连接"));
        log(QStringLiteral("[AGV] Modbus 连接成功 ✓"));
    });
    connect(m_devMgr, &DeviceManager::agvModbusDisconnected, this, [this]() {
        m_indAGV->setStatus(false, QStringLiteral("离线"));
    });
    connect(m_devMgr, &DeviceManager::agvModbusError,
            this, [this](const QString &msg) {
        log(QString("[AGV 错误] %1").arg(msg));
    });

    // ── 6. 寄存器读取结果显示 ─────────────────────────────────
    connect(m_devMgr->robotController(), &RobotController::registersRead, this,
            [this](int startAddr, const QList<quint16> &values) {
        QString text;
        for (int i = 0; i < values.size(); ++i)
            text += QString("  寄存器 %1:  %2 (0x%3)\n")
                        .arg(startAddr + i, 4)
                        .arg(values[i], 5)
                        .arg(values[i], 4, 16, QLatin1Char('0'));
        m_regView->setPlainText(text);
        log(QString("[机械臂] 读取 %1 个寄存器").arg(values.size()));
    });

    log(QStringLiteral("===== 上位机可视化系统启动 ====="));
    log(QStringLiteral("请配置设备IP后连接"));
}

MainWindow::~MainWindow()
{
    delete ui;
}

// ── UI 构建 ────────────────────────────────────────────────

void MainWindow::initUI()
{
    setWindowTitle(QStringLiteral("仓储机器人可视化上位机"));
    setMinimumSize(1100, 750);
    resize(1200, 850);

    auto *cw   = new QWidget(this);
    auto *root = new QVBoxLayout(cw);
    root->setContentsMargins(10, 10, 10, 10);
    setCentralWidget(cw);

    // ── 工具栏 ──
    auto *toolbar = new QHBoxLayout();
    m_btnStart = new QPushButton(QStringLiteral("▶  开始运行"));
    m_btnStop  = new QPushButton(QStringLiteral("■  停止"));
    m_btnStep  = new QPushButton(QStringLiteral("▶| 单步运行"));
    m_btnStart->setFixedHeight(32);
    m_btnStop ->setFixedHeight(32);
    m_btnStop ->setEnabled(false);
    m_btnStep ->setFixedHeight(32);

    m_spinDelay = new QSpinBox();
    m_spinDelay->setRange(200, 10000);
    m_spinDelay->setValue(2000);
    m_spinDelay->setSuffix(QStringLiteral(" ms"));
    m_spinDelay->setFixedHeight(32);
    m_spinDelay->setToolTip(QStringLiteral("每步间延时"));

    m_lblCycle = new QLabel(QStringLiteral("0"));
    m_lblCycle->setStyleSheet("font-size:14px; font-weight:bold; color:#4af;");
    m_lblStep = new QLabel(QStringLiteral("就绪"));
    m_lblStep->setStyleSheet("font-size:13px; color:#aaa;");

    // 主题滑动开关
    m_themeSwitch = new ThemeSwitch();
    connect(m_themeSwitch, &ThemeSwitch::toggled, this, &MainWindow::applyTheme);

    toolbar->addWidget(m_btnStart);
    toolbar->addWidget(m_btnStop);
    toolbar->addWidget(m_btnStep);
    toolbar->addSpacing(20);
    toolbar->addWidget(new QLabel(QStringLiteral("步进延时:")));
    toolbar->addWidget(m_spinDelay);
    toolbar->addSpacing(30);
    toolbar->addWidget(new QLabel(QStringLiteral("循环:")));
    toolbar->addWidget(m_lblCycle);
    toolbar->addSpacing(20);
    toolbar->addWidget(new QLabel(QStringLiteral("当前:")));
    toolbar->addWidget(m_lblStep);
    toolbar->addStretch();
    toolbar->addWidget(m_themeSwitch);
    toolbar->addSpacing(6);
    root->addLayout(toolbar);

    // ── 主体：左侧面板 + 右侧流程图 ──
    auto *mainArea      = new QHBoxLayout();
    auto *leftContainer = new QWidget();
    auto *leftPanel     = new QVBoxLayout(leftContainer);
    leftPanel->setContentsMargins(0, 0, 4, 0);

    // 设备状态指示灯组
    auto *gbStatus     = new QGroupBox(QStringLiteral("设备状态"));
    auto *statusLayout = new QVBoxLayout(gbStatus);
    m_indCamera = new DeviceIndicator(QStringLiteral("奥比相机"));
    m_indRobot  = new DeviceIndicator(QStringLiteral("华沿机器人"));
    m_indAGV    = new DeviceIndicator(QStringLiteral("仙工AGV"));
    m_indLight  = new DeviceIndicator(QStringLiteral("补光灯"));
    statusLayout->addWidget(m_indCamera);
    statusLayout->addWidget(m_indRobot);
    statusLayout->addWidget(m_indAGV);
    statusLayout->addWidget(m_indLight);
    leftPanel->addWidget(gbStatus);

    initConfigPanel(leftPanel);
    initCameraPanel(leftPanel);
    initScannerPanel(leftPanel);
    initRobotPanel(leftPanel);

    leftPanel->addStretch();
    leftContainer->setMinimumWidth(280);

    auto *scrollArea = new QScrollArea();
    scrollArea->setWidgetResizable(true);
    scrollArea->setWidget(leftContainer);
    scrollArea->setMaximumWidth(340);
    scrollArea->setStyleSheet("QScrollArea { border:none; }");
    mainArea->addWidget(scrollArea);

    m_flow = new WorkflowWidget();
    m_flow->setMinimumSize(700, 280);
    mainArea->addWidget(m_flow, 1);
    root->addLayout(mainArea, 1);

    // ── 日志 ──
    m_logView = new QPlainTextEdit();
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(200);
    root->addWidget(m_logView, 1);

    // ── 按钮信号 ──
    connect(m_btnStart,          &QPushButton::clicked, this, &MainWindow::onStart);
    connect(m_btnStop,           &QPushButton::clicked, this, &MainWindow::onStop);
    connect(m_btnStep,           &QPushButton::clicked, this, &MainWindow::onStep);
    connect(m_btnLight,          &QPushButton::clicked, this, &MainWindow::onLightToggle);
    connect(m_btnApply,          &QPushButton::clicked, this, &MainWindow::onApplyConfig);
    connect(m_btnTestRobot,      &QPushButton::clicked, this, &MainWindow::onTestRobot);
    connect(m_btnTestAgv,        &QPushButton::clicked, this, &MainWindow::onTestAgv);
    connect(m_btnTestCamera,     &QPushButton::clicked, this, &MainWindow::onTestCamera);
    connect(m_btnTestCameraOpen, &QPushButton::clicked, this, &MainWindow::onTestCameraOpen);
    connect(m_btnTestScanner,    &QPushButton::clicked, this, &MainWindow::onTestScanner);

    menuBar()->setVisible(false);
    statusBar()->setVisible(false);
    m_btnStop->setObjectName("btnStop");
    m_btnStep->setObjectName("btnStep");

    // 在所有控件创建完毕后统一应用深色主题（默认）
    applyTheme(true);
}

// ── 左侧面板初始化 ──────────────────────────────────────────

void MainWindow::initConfigPanel(QVBoxLayout *leftPanel)
{
    auto *gbCfg = new QGroupBox(QStringLiteral("网络配置"));
    auto *form  = new QFormLayout(gbCfg);
    form->setLabelAlignment(Qt::AlignRight);

    m_editRobotIP = new QLineEdit("192.168.10.10");
    m_editRobotIP->setFixedWidth(130);
    m_spinRobotPort = new QSpinBox();
    m_spinRobotPort->setRange(1, 65535);
    m_spinRobotPort->setValue(502);
    m_spinRobotPort->setFixedWidth(80);

    auto *robotRow = new QHBoxLayout();
    m_btnTestRobot = new QPushButton(QStringLiteral("测试"));
    m_btnTestRobot->setFixedSize(56, 24);
    robotRow->addWidget(m_editRobotIP);
    robotRow->addWidget(new QLabel(":"));
    robotRow->addWidget(m_spinRobotPort);
    robotRow->addStretch();
    robotRow->addWidget(m_btnTestRobot);
    form->addRow(QStringLiteral("机器人 IP:"), robotRow);

    m_editAgvIP = new QLineEdit("192.168.192.5");
    m_editAgvIP->setFixedWidth(130);
    auto *agvRow = new QHBoxLayout();
    m_btnTestAgv = new QPushButton(QStringLiteral("测试"));
    m_btnTestAgv->setFixedSize(56, 24);
    agvRow->addWidget(m_editAgvIP);
    agvRow->addStretch();
    agvRow->addWidget(m_btnTestAgv);
    form->addRow(QStringLiteral("AGV IP:"), agvRow);

    m_btnApply = new QPushButton(QStringLiteral("应用配置"));
    m_btnApply->setObjectName("btnApply");
    m_btnApply->setFixedHeight(28);
    form->addRow(m_btnApply);
    leftPanel->addWidget(gbCfg);

    // 补光灯控制
    auto *gbLight     = new QGroupBox(QStringLiteral("补光灯"));
    auto *lightLayout = new QVBoxLayout(gbLight);
    m_btnLight = new QPushButton(QStringLiteral("● 关闭补光灯"));
    m_btnLight->setObjectName("btnLight");
    m_btnLight->setFixedHeight(30);
    m_btnLight->setStyleSheet(
        "#btnLight { background:#f0ad4e; color:white; border:none; "
        "border-radius:4px; padding:4px 16px; font-size:13px; }"
        "#btnLight:hover { background:#f5c26b; }");
    lightLayout->addWidget(m_btnLight);
    leftPanel->addWidget(gbLight);
}

void MainWindow::initCameraPanel(QVBoxLayout *leftPanel)
{
    auto *gbCamera = new QGroupBox(QStringLiteral("奥比相机调试"));
    auto *form     = new QFormLayout(gbCamera);
    form->setLabelAlignment(Qt::AlignRight);

    m_editCameraIP = new QLineEdit("192.168.1.10");
    m_editCameraIP->setFixedWidth(130);

    auto *camRow     = new QHBoxLayout();
    m_btnTestCamera  = new QPushButton(QStringLiteral("连接"));
    m_btnTestCamera->setFixedSize(56, 24);
    m_btnTestCamera->setObjectName("btnCamTest");
    m_btnTestCamera->setStyleSheet(
        "#btnCamTest { background:#5bc0de; color:white; border:none; "
        "border-radius:3px; font-size:11px; }"
        "#btnCamTest:hover { background:#6ed0ee; }");

    m_btnTestCameraOpen = new QPushButton(QStringLiteral("相机"));
    m_btnTestCameraOpen->setFixedSize(56, 24);
    m_btnTestCameraOpen->setObjectName("btnCamOpen");
    m_btnTestCameraOpen->setStyleSheet(
        "#btnCamOpen { background:#5cb85c; color:white; border:none; "
        "border-radius:3px; font-size:11px; }"
        "#btnCamOpen:hover { background:#6ec86e; }");

    camRow->addWidget(m_editCameraIP);
    camRow->addStretch();
    camRow->addWidget(m_btnTestCamera);
    camRow->addWidget(m_btnTestCameraOpen);
    form->addRow(QStringLiteral("相机 IP:"), camRow);

    m_indCameraDebug = new DeviceIndicator(QStringLiteral("相机状态"));
    m_indCameraDebug->setFixedSize(180, 48);
    form->addRow(m_indCameraDebug);
    leftPanel->addWidget(gbCamera);
}

void MainWindow::initScannerPanel(QVBoxLayout *leftPanel)
{
    auto *gbScanner = new QGroupBox(QStringLiteral("扫码器调试"));
    auto *form      = new QFormLayout(gbScanner);
    form->setLabelAlignment(Qt::AlignRight);

    m_editScannerIP = new QLineEdit("192.168.2.100");
    m_editScannerIP->setFixedWidth(130);

    auto *scanRow    = new QHBoxLayout();
    m_btnTestScanner = new QPushButton(QStringLiteral("测试"));
    m_btnTestScanner->setFixedSize(56, 24);
    m_btnTestScanner->setObjectName("btnScanTest");
    m_btnTestScanner->setStyleSheet(
        "#btnScanTest { background:#5bc0de; color:white; border:none; "
        "border-radius:3px; font-size:11px; }"
        "#btnScanTest:hover { background:#6ed0ee; }");

    scanRow->addWidget(m_editScannerIP);
    scanRow->addStretch();
    scanRow->addWidget(m_btnTestScanner);
    form->addRow(QStringLiteral("扫码器 IP:"), scanRow);

    m_indScanner = new DeviceIndicator(QStringLiteral("扫码器状态"));
    m_indScanner->setFixedSize(180, 48);
    form->addRow(m_indScanner);
    leftPanel->addWidget(gbScanner);
}

void MainWindow::initRobotPanel(QVBoxLayout *leftPanel)
{
    // 注：RobotController 的生命周期由 DeviceManager 管理
    // 此处仅构建 UI 并连接操作按钮
    auto *gb   = new QGroupBox(QStringLiteral("机械臂 Modbus 控制"));
    auto *form = new QFormLayout(gb);
    form->setLabelAlignment(Qt::AlignRight);

    m_indRobotConn = new DeviceIndicator(QStringLiteral("连接状态"));
    m_indRobotConn->setFixedSize(180, 48);
    form->addRow(m_indRobotConn);

    // 寄存器读取
    auto *readRow = new QHBoxLayout();
    m_editRegAddr = new QLineEdit("0");
    m_editRegAddr->setFixedWidth(50);
    m_spinRegCount = new QSpinBox();
    m_spinRegCount->setRange(1, 20);
    m_spinRegCount->setValue(4);
    m_btnReadReg = new QPushButton(QStringLiteral("读取"));
    m_btnReadReg->setFixedSize(56, 24);
    connect(m_btnReadReg, &QPushButton::clicked, this, [this]() {
        bool ok;
        int addr = m_editRegAddr->text().toInt(&ok);
        if (!ok || addr < 0) return;
        m_devMgr->robotController()->readHoldingRegisters(addr, m_spinRegCount->value());
    });
    readRow->addWidget(new QLabel(QStringLiteral("地址:")));
    readRow->addWidget(m_editRegAddr);
    readRow->addWidget(new QLabel(QStringLiteral("数量:")));
    readRow->addWidget(m_spinRegCount);
    readRow->addWidget(m_btnReadReg);
    readRow->addStretch();
    form->addRow(QStringLiteral("读取寄存器:"), readRow);

    // 寄存器写入
    auto *writeRow = new QHBoxLayout();
    m_editRegValue = new QLineEdit();
    m_editRegValue->setFixedWidth(60);
    m_btnWriteReg = new QPushButton(QStringLiteral("写入"));
    m_btnWriteReg->setFixedSize(56, 24);
    connect(m_btnWriteReg, &QPushButton::clicked, this, [this]() {
        bool okAddr, okVal;
        int addr = m_editRegAddr->text().toInt(&okAddr);
        int val  = m_editRegValue->text().toInt(&okVal);
        if (!okAddr || !okVal || addr < 0) return;
        m_devMgr->robotController()->writeRegister(addr, static_cast<quint16>(val));
        log(QString("[机械臂] 写入寄存器 %1 = %2").arg(addr).arg(val));
    });
    writeRow->addWidget(new QLabel(QStringLiteral("值:")));
    writeRow->addWidget(m_editRegValue);
    writeRow->addWidget(m_btnWriteReg);
    writeRow->addStretch();
    form->addRow(QStringLiteral("写入寄存器:"), writeRow);

    // 寄存器显示（样式由 applyTheme 统一管理）
    m_regView = new QPlainTextEdit();
    m_regView->setObjectName("regView");
    m_regView->setReadOnly(true);
    m_regView->setMaximumBlockCount(50);
    m_regView->setFixedHeight(120);
    form->addRow(m_regView);

    leftPanel->addWidget(gb);
}

// ── 日志工具 ────────────────────────────────────────────────

void MainWindow::log(const QString &msg)
{
    const QString ts = QDateTime::currentDateTime().toString("hh:mm:ss");
    m_logView->appendPlainText(QString("[%1] %2").arg(ts, msg));
}

// ── 主题切换 ────────────────────────────────────────────────

void MainWindow::applyTheme(bool dark)
{
    // ── 深色主题 ──────────────────────────────────────────────
    static const char *kDark =
        // 关键：QWidget 覆盖所有容器背景，修复 Linux 白色背景问题
        "QWidget            { background:#2b2d33; color:#ccc; }"
        "QMainWindow        { background:#2b2d33; }"
        "QDialog            { background:#2b2d33; }"
        "QScrollArea        { border:none; background:transparent; }"
        "QGroupBox          { color:#ddd; border:1px solid #444; border-radius:6px;"
        "                     margin-top:10px; padding-top:12px; background:#242629; }"
        "QGroupBox::title   { subcontrol-origin:margin; left:12px; }"
        "QLabel             { color:#ccc; background:transparent; }"
        "QLineEdit          { background:#3a3d44; color:#ddd; border:1px solid #555;"
        "                     border-radius:4px; padding:3px 6px; font-size:12px; }"
        "QSpinBox           { background:#3a3d44; color:#ddd; border:1px solid #555;"
        "                     border-radius:4px; padding:3px; }"
        "QPushButton        { background:#3a8fd4; color:white; border:none;"
        "                     border-radius:4px; padding:4px 16px; font-size:13px; }"
        "QPushButton:hover  { background:#4da3e8; }"
        "QPushButton:disabled { background:#444; color:#888; }"
        "QPushButton#btnStop  { background:#d9534f; }"
        "QPushButton#btnStop:hover  { background:#e86562; }"
        "QPushButton#btnStep  { background:#5cb85c; }"
        "QPushButton#btnStep:hover  { background:#6ec86e; }"
        "QPushButton#btnApply { background:#5bc0de; }"
        "QPushButton#btnApply:hover { background:#6ed0ee; }"
        "QScrollBar:vertical { background:#2b2d33; width:8px; }"
        "QScrollBar::handle:vertical { background:#555; border-radius:4px; }"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical { height:0; }";

    // ── 浅色主题 ──────────────────────────────────────────────
    static const char *kLight =
        "QWidget            { background:#f0f0f0; color:#222; }"
        "QMainWindow        { background:#e8e8e8; }"
        "QDialog            { background:#f0f0f0; }"
        "QScrollArea        { border:none; background:transparent; }"
        "QGroupBox          { color:#333; border:1px solid #ccc; border-radius:6px;"
        "                     margin-top:10px; padding-top:12px; background:#ffffff; }"
        "QGroupBox::title   { subcontrol-origin:margin; left:12px; }"
        "QLabel             { color:#333; background:transparent; }"
        "QLineEdit          { background:#ffffff; color:#222; border:1px solid #bbb;"
        "                     border-radius:4px; padding:3px 6px; font-size:12px; }"
        "QSpinBox           { background:#ffffff; color:#222; border:1px solid #bbb;"
        "                     border-radius:4px; padding:3px; }"
        "QPushButton        { background:#0078d4; color:white; border:none;"
        "                     border-radius:4px; padding:4px 16px; font-size:13px; }"
        "QPushButton:hover  { background:#106ebe; }"
        "QPushButton:disabled { background:#ccc; color:#888; }"
        "QPushButton#btnStop  { background:#c42b1c; }"
        "QPushButton#btnStop:hover  { background:#d83b2e; }"
        "QPushButton#btnStep  { background:#107c10; }"
        "QPushButton#btnStep:hover  { background:#1a8f1a; }"
        "QPushButton#btnApply { background:#0078d4; }"
        "QPushButton#btnApply:hover { background:#106ebe; }"
        "QScrollBar:vertical { background:#e0e0e0; width:8px; }"
        "QScrollBar::handle:vertical { background:#aaa; border-radius:4px; }"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical { height:0; }";

    // 应用全局样式表
    setStyleSheet(dark ? kDark : kLight);

    // 日志控件（使用 termianl 风格，两个主题分别配色）
    m_logView->setStyleSheet(dark
        ? "QPlainTextEdit { background:#1e1f24; color:#c0c0c0; font-family:monospace; }"
        : "QPlainTextEdit { background:#fafafa;  color:#333;    font-family:monospace; }");

    // 寄存器显示控件（terminal 风格，始终深色）
    m_regView->setStyleSheet(dark
        ? "QPlainTextEdit { background:#1a1f1a; color:#80ff80; font-family:monospace; font-size:11px; }"
        : "QPlainTextEdit { background:#f0fff0; color:#006600; font-family:monospace; font-size:11px; }");

    // 循环计数标签颜色跟随主题
    m_lblCycle->setStyleSheet(dark
        ? "font-size:14px; font-weight:bold; color:#4af;"
        : "font-size:14px; font-weight:bold; color:#0078d4;");
    m_lblStep->setStyleSheet(dark
        ? "font-size:13px; color:#aaa;"
        : "font-size:13px; color:#666;");
}

// ── 读取 UI 控件 → Config 结构体 ────────────────────────────

void MainWindow::buildConfig(DeviceManager::Config &cfg) const
{
    cfg.robotIP   = m_editRobotIP->text().trimmed();
    cfg.robotPort = m_spinRobotPort->value();
    cfg.agvIP     = m_editAgvIP->text().trimmed();
    cfg.cameraIP  = m_editCameraIP->text().trimmed();
    cfg.scannerIP = m_editScannerIP->text().trimmed();
}

// ── 转发用户操作到业务层 ────────────────────────────────────

void MainWindow::onStart()
{
    DeviceManager::Config cfg;
    buildConfig(cfg);
    m_devMgr->setConfig(cfg);
    m_devMgr->applyConfig();
    m_engine->start(m_spinDelay->value());
}

void MainWindow::onStop()
{
    m_engine->stop();
}

void MainWindow::onStep()
{
    m_engine->singleStep();
}

void MainWindow::onLightToggle()
{
    m_devMgr->toggleLight();
}

void MainWindow::onApplyConfig()
{
    DeviceManager::Config cfg;
    buildConfig(cfg);
    m_devMgr->setConfig(cfg);
    m_devMgr->applyConfig();
}

void MainWindow::onTestRobot()
{
    DeviceManager::Config cfg;
    buildConfig(cfg);
    m_devMgr->setConfig(cfg);
    m_devMgr->testRobot();
}

void MainWindow::onTestAgv()
{
    DeviceManager::Config cfg;
    buildConfig(cfg);
    m_devMgr->setConfig(cfg);
    m_devMgr->testAgv();
}

void MainWindow::onTestCamera()
{
    DeviceManager::Config cfg;
    buildConfig(cfg);
    m_devMgr->setConfig(cfg);
    m_devMgr->testCamera();
}

void MainWindow::onTestCameraOpen()
{
    const QString ip = m_editCameraIP->text().trimmed();
    if (ip.isEmpty()) { log(QStringLiteral("[相机] IP 为空")); return; }

    log(QString("[相机] 正在打开实时画面 %1 ...").arg(ip));
    m_indCameraDebug->setStatus(false, QStringLiteral("连接中..."));

    auto *camWin = new CameraWindow(ip, this);
    connect(camWin, &QDialog::finished, this, [this](int) {
        m_indCameraDebug->setStatus(true, QStringLiteral("待机"));
        log(QStringLiteral("[相机] 画面窗口已关闭"));
    });
    camWin->setAttribute(Qt::WA_DeleteOnClose);
    camWin->show();

    m_indCameraDebug->setStatus(true, QStringLiteral("画面已打开"));
    log(QStringLiteral("[相机] 实时画面窗口已开启"));
}

void MainWindow::onTestScanner()
{
    DeviceManager::Config cfg;
    buildConfig(cfg);
    m_devMgr->setConfig(cfg);
    m_devMgr->testScanner();
}

// ── 响应业务层信号（纯 UI 更新）────────────────────────────

void MainWindow::onStepActivated(int idx, int station, int cycle)
{
    // 更新流程图和状态栏
    m_lblStep->setText(WorkflowEngine::kSteps[idx].name);
    m_lblCycle->setText(QString::number(cycle));
    m_flow->setActiveStep(idx);
    m_flow->setStationHighlight(station);

    // 根据当前步骤更新设备状态指示灯
    switch (idx) {
    case 0:
        m_indCamera->setStatus(true, QStringLiteral("拍照中"));
        log(QString("→ 工位%1 相机拍照").arg(station));
        break;
    case 1:
        m_indRobot->setStatus(true, QStringLiteral("抓取中"));
        m_indCamera->setStatus(true, QStringLiteral("就绪"));
        log(QStringLiteral("→ 华沿机器人执行抓取 MOVJ"));
        break;
    case 2:
        m_indRobot->setStatus(true, QStringLiteral("已抓取"));
        m_indAGV->setStatus(true, QStringLiteral("运行中"));
        log(QString("→ 仙工AGV前往工位%1上料站").arg(station));
        break;
    case 3:
        m_indAGV->setStatus(true, QStringLiteral("已到位"));
        m_indRobot->setStatus(true, QStringLiteral("放料中"));
        log(QStringLiteral("→ AGV到位，机器人翻转卸料"));
        break;
    case 4:
        m_indRobot->setStatus(true, QStringLiteral("就绪"));
        m_indAGV->setStatus(true, QStringLiteral("复位中"));
        if (!m_engine->isRunning())
            log(QString("→ 第%1轮完成，等待下一轮").arg(cycle));
        break;
    }
}

void MainWindow::onEngineStarted()
{
    m_btnStart->setEnabled(false);
    m_btnStop->setEnabled(true);

    log(QStringLiteral("========== 自动循环启动 =========="));
    const auto &cfg = m_devMgr->config();
    log(QString("机器人 %1:%2  AGV %3")
            .arg(cfg.robotIP).arg(cfg.robotPort).arg(cfg.agvIP));

    m_indCamera->setStatus(true, QStringLiteral("就绪"));
    m_indRobot->setStatus(true, QStringLiteral("已连接"));
    m_indAGV->setStatus(true, QStringLiteral("已连接"));
}

void MainWindow::onEngineStopped()
{
    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);
    m_flow->setActiveStep(-1);
    m_lblStep->setText(QStringLiteral("已停止"));
    m_indCamera->setStatus(true, QStringLiteral("待机"));
    m_indRobot->setStatus(true, QStringLiteral("待机"));
    m_indAGV->setStatus(true, QStringLiteral("待机"));

    log(QStringLiteral("========== 循环已停止 =========="));
}

void MainWindow::onLightChanged(bool on, bool success)
{
    if (on) {
        m_btnLight->setText(QStringLiteral("● 关闭补光灯"));
        m_btnLight->setStyleSheet(
            "#btnLight { background:#f0ad4e; color:white; border:none; "
            "border-radius:4px; padding:4px 16px; font-size:13px; }"
            "#btnLight:hover { background:#f5c26b; }");
        m_indLight->setStatus(success,
            success ? QStringLiteral("已开启") : QStringLiteral("开启失败"));
    } else {
        m_btnLight->setText(QStringLiteral("○ 开启补光灯"));
        m_btnLight->setStyleSheet(
            "#btnLight { background:#555; color:#aaa; border:none; "
            "border-radius:4px; padding:4px 16px; font-size:13px; }"
            "#btnLight:hover { background:#666; }");
        // 成功关灯 → 红灯（灯已灭）；关灯失败 → 绿灯（灯还亮着）
        m_indLight->setStatus(!success,
            success ? QStringLiteral("已关闭") : QStringLiteral("关闭失败"));
    }
}

void MainWindow::onConfigApplied(const QString &robotIP, int /*port*/, const QString &agvIP)
{
    m_indRobot->setStatus(true, QString("待连接 %1").arg(robotIP));
    m_indAGV->setStatus(true, QString("待连接 %1").arg(agvIP));
}
