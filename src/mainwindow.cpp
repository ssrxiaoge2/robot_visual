/**
 * @file mainwindow.cpp
 * @brief MainWindow 主窗口实现（纯 UI 层）
 *
 * ── 架构职责 ─────────────────────────────────────────────────
 *   MainWindow 只负责：
 *     • 构建并布局所有 UI 控件
 *     • 将用户操作转发给 WorkflowEngine / DeviceManager
 *     • 订阅业务层信号，更新指示灯/日志/标签等 UI 元素
 *
 *   MainWindow 不包含：
 *     • 任何 Modbus 读写逻辑
 *     • 工作流状态管理
 *     • 设备连接生命周期管理
 *
 * ── 信号连接顺序 ─────────────────────────────────────────────
 *   构造函数中：
 *     1. 创建业务层（WorkflowEngine + DeviceManager）
 *     2. 注入硬件控制器（engine.setRobotController / setAgvController）
 *     3. 调用 initUI() 构建所有控件
 *     4. 连接 WorkflowEngine 信号
 *     5. 连接 DeviceManager 信号（含 Modbus 状态透传）
 *     6. 连接 DeviceManager::debugRegistersRead（调试面板显示寄存器值）
 */

#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "camerawindow.h"
#include "workflowengine.h"

#include <QDate>
#include <QDir>
#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QDateTime>
#include <QFont>
#include <QFormLayout>
#include <QScrollArea>
#include <QTextStream>

#include "handeyedialog.h"
#include "huayanScheduler.h"

// ============================================================
//  构造 / 析构
// ============================================================

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // ── 0. 初始化日志文件（需在第一条 log() 调用之前完成）──
    // 用 PROJECT_SOURCE_DIR 宏（CMakeLists 第 12 行定义）保证绝对路径，
    // 与 fill_light 路径策略一致，不受 Qt Creator 工作目录设置影响
    {
        const QString logDir = QStringLiteral(PROJECT_SOURCE_DIR "/Log");
        QDir().mkpath(logDir);
        const QString path = logDir + "/"
            + QDate::currentDate().toString("yyyy-MM-dd") + " log.txt";
        m_logFile = new QFile(path, this);
        if (!m_logFile->open(QIODevice::Append | QIODevice::Text)) {
            qWarning() << "[日志] 无法创建日志文件:" << path
                       << m_logFile->errorString();
        } else {
            m_logStream = new QTextStream(m_logFile);
            m_logStream->setEncoding(QStringConverter::Utf8);
        }
    }

    // ── 1. 先构造业务层 ─────────────────────────────────────
    // 注意：必须在 initUI() 之前构造，因为 initUI() 中的部分代码
    // 可能通过 lambda 捕获 m_devMgr 指针（如 m_btnReadReg 槽函数）
    m_engine = new WorkflowEngine(this);
    m_devMgr = new DeviceManager(this);

    // ── 2. 注入硬件控制器到引擎 ─────────────────────────────
    // 注入后引擎从"纯仿真"模式升级为"有硬件时走 Modbus，无硬件时仿真"双模式
    m_engine->setRobotController(m_devMgr->robotController());
    m_engine->setAgvController(m_devMgr->agvController());
    // 注入视觉客户端：Step0 将主动调用 fetchInference()，
    // 返回坐标后自动调用 setCoordinates() 推进到 Step1
    m_engine->setVisionClient(m_devMgr->visionClient());

    // ── 3. 构建所有 UI 控件 ─────────────────────────────────
    initUI();

    // ── 4. 连接 WorkflowEngine 信号 ─────────────────────────
    connect(m_engine, &WorkflowEngine::stepActivated,
            this, &MainWindow::onStepActivated);
    connect(m_engine, &WorkflowEngine::started,
            this, &MainWindow::onEngineStarted);
    connect(m_engine, &WorkflowEngine::stopped,
            this, &MainWindow::onEngineStopped);
    // 引擎运行日志（步骤切换、Modbus 写入结果等）→ 转发到日志控件
    connect(m_engine, &WorkflowEngine::stepLog,
            this, [this](const QString &msg) { log(msg); });
    // 引擎错误（超时、AGV 故障等）→ 带 ⚠ 前缀显示
    connect(m_engine, &WorkflowEngine::engineError,
            this, [this](const QString &msg) { log(QString("⚠ %1").arg(msg)); });
    // 步进延时 SpinBox 实时同步到引擎（运行中也生效）
    connect(m_spinDelay, &QSpinBox::valueChanged,
            m_engine, &WorkflowEngine::setInterval);

    // AGV 故障：更新指示灯为故障状态（与引擎 stopped 信号的"待机"区分）
    connect(m_engine, &WorkflowEngine::agvFaultDetected, this, [this]() {
        m_indAGV->setStatus(false, QStringLiteral("故障"));
        log(QStringLiteral("[AGV] 检测到故障，已停止流程并复位机器人"));
    });

    // ── 5. 连接 DeviceManager 信号 ──────────────────────────
    // 日志消息（配置应用、连通性测试结果等）
    connect(m_devMgr, &DeviceManager::logMessage,
            this, [this](const QString &msg) { log(msg); });

    // TCP 连通性测试结果 → 更新对应设备的状态指示灯
    connect(m_devMgr, &DeviceManager::robotStatusChanged,
            this, [this](bool ok, const QString &s) { m_indRobot->setStatus(ok, s); });
    connect(m_devMgr, &DeviceManager::agvStatusChanged,
            this, [this](bool ok, const QString &s) { m_indAGV->setStatus(ok, s); });
    connect(m_devMgr, &DeviceManager::cameraStatusChanged,
            this, [this](bool ok, const QString &s) { m_indCameraDebug->setStatus(ok, s); });
    connect(m_devMgr, &DeviceManager::scannerStatusChanged,
            this, [this](bool ok, const QString &s) { m_indScanner->setStatus(ok, s); });

    // 补光灯切换结果 / 配置应用结果
    connect(m_devMgr, &DeviceManager::lightChanged,
            this, &MainWindow::onLightChanged);
    connect(m_devMgr, &DeviceManager::configApplied,
            this, &MainWindow::onConfigApplied);

    // ── 6. 机械臂 Modbus 连接状态 ───────────────────────────
    // 连接成功：更新 Modbus 调试面板指示灯 + 主状态指示灯
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

    // ── 7. AGV Modbus 连接状态 ──────────────────────────────
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

    // ── 8. 寄存器读取结果显示（调试面板专用）──────────────
    connect(m_devMgr, &DeviceManager::debugRegistersRead, this,
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

    // ── 9. 华沿 SDK 信号连接 ─────────────────────────────────
    {
        HuayanScheduler *hs = m_devMgr->huayanScheduler();
        Q_ASSERT(hs != nullptr);
        connect(hs, &HuayanScheduler::logMessage,
                this, &MainWindow::onHuayanLog);
        connect(hs, &HuayanScheduler::stageStarted,
                this, &MainWindow::onHuayanStageStarted);
        connect(hs, &HuayanScheduler::stageCompleted,
                this, &MainWindow::onHuayanStageCompleted);
        connect(hs, &HuayanScheduler::stageError,
                this, &MainWindow::onHuayanStageError);
    }

    log(QStringLiteral("===== 上位机可视化系统启动 ====="));
    log(QStringLiteral("请配置设备IP后连接"));
}

MainWindow::~MainWindow()
{
    if (m_logFile && m_logFile->isOpen())
        m_logFile->close();
    delete ui;
}

// ============================================================
//  UI 构建
// ============================================================

/**
 * @brief 初始化整个主窗口布局
 *
 * 布局结构：
 *   VBoxLayout (root)
 *     ├── HBoxLayout (toolbar)     工具栏：开始/停止/单步/延时/循环计数/主题开关
 *     ├── HBoxLayout (mainArea)
 *     │     ├── QScrollArea        左侧面板（设备状态+配置+调试面板，宽度受限340px）
 *     │     └── WorkflowWidget     右侧流程图（弹性填充）
 *     └── QPlainTextEdit (logView) 底部日志区
 */
void MainWindow::initUI()
{
    setWindowTitle(QStringLiteral("仓储机器人可视化上位机"));
    setMinimumSize(1100, 750);
    resize(1200, 850);

    auto *cw   = new QWidget(this);
    auto *root = new QVBoxLayout(cw);
    root->setContentsMargins(10, 10, 10, 10);
    setCentralWidget(cw);

    // ── 工具栏 ──────────────────────────────────────────────
    auto *toolbar = new QHBoxLayout();
    m_btnStart = new QPushButton(QStringLiteral("▶  开始运行"));
    m_btnStop  = new QPushButton(QStringLiteral("■  停止"));
    m_btnStep  = new QPushButton(QStringLiteral("▶| 单步运行"));
    m_btnStart->setFixedHeight(32);
    m_btnStop ->setFixedHeight(32);
    m_btnStop ->setEnabled(false);  // 初始状态：停止按钮不可用
    m_btnStep ->setFixedHeight(32);

    m_spinDelay = new QSpinBox();
    m_spinDelay->setRange(200, 10000);
    m_spinDelay->setValue(2000);
    m_spinDelay->setSuffix(QStringLiteral(" ms"));
    m_spinDelay->setFixedHeight(32);
    m_spinDelay->setToolTip(QStringLiteral("仿真模式：每步等待时长；真实模式：步骤超时参考值"));

    // 循环计数标签（数字）和当前步骤标签（文字）
    m_lblCycle = new QLabel(QStringLiteral("0"));
    m_lblCycle->setStyleSheet("font-size:14px; font-weight:bold; color:#4af;");
    m_lblStep = new QLabel(QStringLiteral("就绪"));
    m_lblStep->setStyleSheet("font-size:13px; color:#aaa;");

    // 主题切换开关（放在工具栏最右侧）
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
    toolbar->addStretch();       // 将主题开关推到最右侧
    toolbar->addWidget(m_themeSwitch);
    toolbar->addSpacing(6);
    root->addLayout(toolbar);

    // ── 主体区域 ─────────────────────────────────────────────
    auto *mainArea      = new QHBoxLayout();
    auto *leftContainer = new QWidget();
    auto *leftPanel     = new QVBoxLayout(leftContainer);
    leftPanel->setContentsMargins(0, 0, 4, 0);

    // 设备状态指示灯组（2×2 网格）
    auto *gbStatus     = new QGroupBox(QStringLiteral("设备状态"));
    auto *statusLayout = new QGridLayout(gbStatus);
    statusLayout->setSpacing(4);
    m_indCamera = new DeviceIndicator(QStringLiteral("奥比相机"));
    m_indRobot  = new DeviceIndicator(QStringLiteral("华沿机器人"));
    m_indAGV    = new DeviceIndicator(QStringLiteral("仙工AGV"));
    m_indLight  = new DeviceIndicator(QStringLiteral("补光灯"));
    statusLayout->addWidget(m_indCamera, 0, 0);
    statusLayout->addWidget(m_indRobot,  0, 1);
    statusLayout->addWidget(m_indAGV,    1, 0);
    statusLayout->addWidget(m_indLight,  1, 1);
    leftPanel->addWidget(gbStatus);

    // 各功能面板（按功能分组，内部实现见 initXxxPanel）
    initConfigPanel(leftPanel);   // 网络配置 + 补光灯
    initCameraPanel(leftPanel);   // 相机调试
    initScannerPanel(leftPanel);  // 扫码器调试
    initRobotPanel(leftPanel);    // 机械臂 Modbus 控制
    initHuayanPanel(leftPanel);   // 华沿 SDK 调试

    leftPanel->addStretch(); // 底部弹性填充
    leftContainer->setMinimumWidth(280);

    // 左侧面板加滚动条（屏幕高度不够时可滚动查看）
    auto *scrollArea = new QScrollArea();
    scrollArea->setWidgetResizable(true);
    scrollArea->setWidget(leftContainer);
    scrollArea->setMaximumWidth(340);
    scrollArea->setStyleSheet("QScrollArea { border:none; }");
    mainArea->addWidget(scrollArea);

    // 右侧流程图（弹性填充，stretch=1 占满剩余宽度）
    m_flow = new WorkflowWidget();
    m_flow->setMinimumSize(700, 280);
    mainArea->addWidget(m_flow, 1);
    root->addLayout(mainArea, 1);

    // ── 底部日志区 ───────────────────────────────────────────
    m_logView = new QPlainTextEdit();
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(200); // 最多保留 200 行，防止内存无限增长
    root->addWidget(m_logView, 1);

    // ── 按钮信号连接 ─────────────────────────────────────────
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
    connect(m_btnHandEye,        &QPushButton::clicked, this, &MainWindow::onHandEyeCalib);

    menuBar()->setVisible(false);   // 隐藏菜单栏（工业界面不需要）
    statusBar()->setVisible(false); // 隐藏状态栏
    // 按钮对象名用于 QSS 中的 #id 选择器精确着色
    m_btnStop->setObjectName("btnStop");
    m_btnStep->setObjectName("btnStep");

    // 最后统一应用默认深色主题
    applyTheme(true);
}

// ── 左侧面板初始化（按功能分组）─────────────────────────────

/**
 * @brief 初始化"网络配置"和"补光灯"面板
 *
 * 包含：机器人 IP:端口 + 测试按钮、AGV IP + 测试按钮、
 *       应用配置按钮、补光灯切换按钮。
 */
void MainWindow::initConfigPanel(QVBoxLayout *leftPanel)
{
    auto *gbCfg = new QGroupBox(QStringLiteral("网络配置"));
    auto *form  = new QFormLayout(gbCfg);
    form->setLabelAlignment(Qt::AlignRight);

    // 机器人 IP 和端口输入行
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

    // AGV IP 输入行（端口固定 502，不暴露给用户）
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
    m_btnApply->setObjectName("btnApply"); // QSS 用蓝色区分
    m_btnApply->setFixedHeight(28);
    form->addRow(m_btnApply);
    leftPanel->addWidget(gbCfg);

    // ── 补光灯控制（单独一组）────────────────────────────────
    auto *gbLight     = new QGroupBox(QStringLiteral("补光灯"));
    auto *lightLayout = new QVBoxLayout(gbLight);
    // 补光灯默认关闭：按钮文字/样式与 m_lightOn=false 初始状态保持一致
    m_btnLight = new QPushButton(QStringLiteral("○ 开启补光灯"));
    m_btnLight->setObjectName("btnLight");
    m_btnLight->setFixedHeight(30);
    m_btnLight->setStyleSheet(
        "#btnLight { background:#555; color:#aaa; border:none; "
        "border-radius:4px; padding:4px 16px; font-size:13px; }"
        "#btnLight:hover { background:#666; }");
    lightLayout->addWidget(m_btnLight);
    m_indLight->setStatus(false, QStringLiteral("已关闭"));
    leftPanel->addWidget(gbLight);
}

/**
 * @brief 初始化"奥比相机调试"面板
 *
 * 包含：相机 IP 输入、连接测试按钮、打开实时画面按钮、状态指示灯。
 */
void MainWindow::initCameraPanel(QVBoxLayout *leftPanel)
{
    auto *gbCamera = new QGroupBox(QStringLiteral("奥比相机调试"));
    auto *form     = new QFormLayout(gbCamera);
    form->setLabelAlignment(Qt::AlignRight);

    m_editCameraIP = new QLineEdit("192.168.1.10");
    m_editCameraIP->setFixedWidth(130);

    auto *camRow    = new QHBoxLayout();
    // "连接"：TCP 连通性测试
    m_btnTestCamera = new QPushButton(QStringLiteral("连接"));
    m_btnTestCamera->setFixedSize(56, 24);
    m_btnTestCamera->setObjectName("btnCamTest");
    m_btnTestCamera->setStyleSheet(
        "#btnCamTest { background:#5bc0de; color:white; border:none; "
        "border-radius:3px; font-size:11px; }"
        "#btnCamTest:hover { background:#6ed0ee; }");

    // "相机"：打开 CameraWindow 查看实时画面
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

    // 手眼标定按钮：打开向导加载标定矩阵
    m_btnHandEye = new QPushButton(QStringLiteral("手眼标定矩阵"));
    m_btnHandEye->setFixedHeight(26);
    m_btnHandEye->setObjectName("btnHandEye");
    m_btnHandEye->setStyleSheet(
        "#btnHandEye { background:#8e44ad; color:white; border:none; "
        "border-radius:3px; font-size:12px; }"
        "#btnHandEye:hover { background:#a85cc5; }");
    form->addRow(m_btnHandEye);
    leftPanel->addWidget(gbCamera);
}

/**
 * @brief 初始化"扫码器调试"面板
 *
 * 包含：扫码器 IP 输入、测试按钮、状态指示灯。
 */
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

/**
 * @brief 初始化"机械臂 Modbus 控制"调试面板
 *
 * 包含：Modbus 连接状态指示灯、寄存器读取（地址+数量+读取按钮）、
 *       寄存器写入（地址+值+写入按钮）、寄存器显示文本框。
 *
 * 注：寄存器读写通过 DeviceManager 调试接口转发，不直接访问 RobotController。
 */
void MainWindow::initRobotPanel(QVBoxLayout *leftPanel)
{
    auto *gb   = new QGroupBox(QStringLiteral("机械臂 Modbus 控制"));
    auto *form = new QFormLayout(gb);
    form->setLabelAlignment(Qt::AlignRight);

    // Modbus 连接状态指示灯（显示在面板顶部）
    m_indRobotConn = new DeviceIndicator(QStringLiteral("连接状态"));
    m_indRobotConn->setFixedSize(180, 48);
    form->addRow(m_indRobotConn);

    // ── 寄存器读取行 ─────────────────────────────────────────
    auto *readRow = new QHBoxLayout();
    m_editRegAddr = new QLineEdit("0");         // 起始地址输入
    m_editRegAddr->setFixedWidth(50);
    m_spinRegCount = new QSpinBox();            // 读取数量（1-20）
    m_spinRegCount->setRange(1, 20);
    m_spinRegCount->setValue(4);
    m_btnReadReg = new QPushButton(QStringLiteral("读取"));
    m_btnReadReg->setFixedSize(56, 24);
    connect(m_btnReadReg, &QPushButton::clicked, this, [this]() {
        bool ok;
        int addr = m_editRegAddr->text().toInt(&ok);
        if (!ok || addr < 0) return;
        m_devMgr->debugReadRobotRegisters(addr, m_spinRegCount->value());
    });
    readRow->addWidget(new QLabel(QStringLiteral("地址:")));
    readRow->addWidget(m_editRegAddr);
    readRow->addWidget(new QLabel(QStringLiteral("数量:")));
    readRow->addWidget(m_spinRegCount);
    readRow->addWidget(m_btnReadReg);
    readRow->addStretch();
    form->addRow(QStringLiteral("读取寄存器:"), readRow);

    // ── 寄存器写入行 ─────────────────────────────────────────
    auto *writeRow = new QHBoxLayout();
    m_editRegValue = new QLineEdit();           // 写入值输入（十进制）
    m_editRegValue->setFixedWidth(60);
    m_btnWriteReg = new QPushButton(QStringLiteral("写入"));
    m_btnWriteReg->setFixedSize(56, 24);
    connect(m_btnWriteReg, &QPushButton::clicked, this, [this]() {
        bool okAddr, okVal;
        int addr = m_editRegAddr->text().toInt(&okAddr);
        int val  = m_editRegValue->text().toInt(&okVal);
        if (!okAddr || !okVal || addr < 0) return;
        m_devMgr->debugWriteRobotRegister(addr, static_cast<quint16>(val));
    });
    writeRow->addWidget(new QLabel(QStringLiteral("值:")));
    writeRow->addWidget(m_editRegValue);
    writeRow->addWidget(m_btnWriteReg);
    writeRow->addStretch();
    form->addRow(QStringLiteral("写入寄存器:"), writeRow);

    // ── 寄存器值显示文本框（terminal 风格，样式由 applyTheme 管理）
    m_regView = new QPlainTextEdit();
    m_regView->setObjectName("regView"); // QSS #id 选择器
    m_regView->setReadOnly(true);
    m_regView->setMaximumBlockCount(50);
    m_regView->setFixedHeight(120);
    form->addRow(m_regView);

    leftPanel->addWidget(gb);
}

void MainWindow::initHuayanPanel(QVBoxLayout *leftPanel)
{
    auto *gb   = new QGroupBox(QStringLiteral("华沿 SDK 调试"));
    auto *vbox = new QVBoxLayout(gb);

    auto *row1 = new QHBoxLayout();
    m_huayanConnectBtn    = new QPushButton(QStringLiteral("连接机器人"));
    m_huayanDisconnectBtn = new QPushButton(QStringLiteral("断开"));
    m_huayanIndicator     = new DeviceIndicator(QStringLiteral("华沿 SDK"));
    m_huayanIndicator->setStatus(false, QStringLiteral("未连接"));
    m_huayanDisconnectBtn->setEnabled(false);
    m_huayanConnectBtn->setFixedHeight(28);
    m_huayanDisconnectBtn->setFixedHeight(28);
    row1->addWidget(m_huayanConnectBtn);
    row1->addWidget(m_huayanDisconnectBtn);
    row1->addStretch();
    row1->addWidget(m_huayanIndicator);
    vbox->addLayout(row1);

    auto *line = new QFrame();
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    vbox->addWidget(line);

    auto *row2 = new QHBoxLayout();
    m_huayanStartBtn = new QPushButton(QStringLiteral("启动取料（阶段一）"));
    m_huayanStopBtn  = new QPushButton(QStringLiteral("停止"));
    m_huayanStartBtn->setEnabled(false);
    m_huayanStopBtn->setEnabled(false);
    m_huayanStartBtn->setFixedHeight(28);
    m_huayanStopBtn->setFixedHeight(28);
    row2->addWidget(m_huayanStartBtn);
    row2->addWidget(m_huayanStopBtn);
    vbox->addLayout(row2);

    connect(m_huayanConnectBtn,    &QPushButton::clicked,
            this, &MainWindow::onHuayanConnect);
    connect(m_huayanDisconnectBtn, &QPushButton::clicked,
            this, &MainWindow::onHuayanDisconnect);
    connect(m_huayanStartBtn,      &QPushButton::clicked,
            this, &MainWindow::onHuayanStartStageOne);
    connect(m_huayanStopBtn,       &QPushButton::clicked,
            this, &MainWindow::onHuayanStop);

    leftPanel->addWidget(gb);
}

// ── 日志工具 ─────────────────────────────────────────────────

/**
 * @brief 向日志控件追加一条带时间戳的日志
 * @param msg 日志内容（不含时间戳）
 */
void MainWindow::log(const QString &msg)
{
    const QString ts   = QDateTime::currentDateTime().toString("hh:mm:ss");
    const QString line = QString("[%1] %2").arg(ts, msg);
    m_logView->appendPlainText(line);
    if (m_logStream) {
        *m_logStream << line << '\n';
        m_logStream->flush();
    }
}

// ── 主题切换 ─────────────────────────────────────────────────

/**
 * @brief 应用深色或浅色主题
 * @param dark true=深色，false=浅色
 *
 * 通过 setStyleSheet() 应用全局 QSS，覆盖所有子控件的默认样式。
 * 关键：必须包含 "QWidget { background:... }" 规则，
 *       否则 Linux 系统主题会让 QGroupBox/QScrollArea 内的
 *       QWidget 背景变成系统默认白色（Ubuntu 白色背景问题）。
 *
 * 部分控件（logView/regView/lblCycle/lblStep）有独立颜色需求，
 * 通过内联 setStyleSheet() 在此方法中同步更新。
 */
void MainWindow::applyTheme(bool dark)
{
    // ── 深色主题 QSS ─────────────────────────────────────────
    static const char *kDark =
        // ⚠ 关键：覆盖所有 QWidget 背景，修复 Linux 系统主题干扰
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
        "QPushButton#btnStop  { background:#d9534f; }"       // 停止按钮：红色
        "QPushButton#btnStop:hover  { background:#e86562; }"
        "QPushButton#btnStep  { background:#5cb85c; }"       // 单步按钮：绿色
        "QPushButton#btnStep:hover  { background:#6ec86e; }"
        "QPushButton#btnApply { background:#5bc0de; }"       // 应用按钮：蓝色
        "QPushButton#btnApply:hover { background:#6ed0ee; }"
        "QScrollBar:vertical { background:#2b2d33; width:8px; }"
        "QScrollBar::handle:vertical { background:#555; border-radius:4px; }"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical { height:0; }";

    // ── 浅色主题 QSS ─────────────────────────────────────────
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

    setStyleSheet(dark ? kDark : kLight);

    // ── 部分控件独立样式（terminal/强调配色）────────────────
    // 日志控件：始终使用等宽字体，深浅色各自配色
    m_logView->setStyleSheet(dark
        ? "QPlainTextEdit { background:#1e1f24; color:#c0c0c0; font-family:monospace; }"
        : "QPlainTextEdit { background:#fafafa;  color:#333;    font-family:monospace; }");

    // 寄存器显示：绿底黑字（terminal 风格），两个主题分别配色
    m_regView->setStyleSheet(dark
        ? "QPlainTextEdit { background:#1a1f1a; color:#80ff80; font-family:monospace; font-size:11px; }"
        : "QPlainTextEdit { background:#f0fff0; color:#006600; font-family:monospace; font-size:11px; }");

    // 工具栏标签颜色跟随主题
    m_lblCycle->setStyleSheet(dark
        ? "font-size:14px; font-weight:bold; color:#4af;"
        : "font-size:14px; font-weight:bold; color:#0078d4;");
    m_lblStep->setStyleSheet(dark
        ? "font-size:13px; color:#aaa;"
        : "font-size:13px; color:#666;");
}

// ── 读取 UI 控件 → Config 结构体 ────────────────────────────

/**
 * @brief 从 UI 输入控件读取当前配置，填充 DeviceManager::Config 结构体
 *
 * 集中读取避免在多个槽函数中重复读取 IP/端口文本，
 * 所有需要配置的操作（onStart/onApplyConfig/onTestXxx）都先调用此方法。
 */
void MainWindow::buildConfig(DeviceManager::Config &cfg) const
{
    cfg.robotIP   = m_editRobotIP->text().trimmed();
    cfg.robotPort = m_spinRobotPort->value();
    cfg.agvIP     = m_editAgvIP->text().trimmed();
    cfg.cameraIP   = m_editCameraIP->text().trimmed();
    cfg.cameraPort = 8080; // 视觉服务固定端口（Flask HTTP）
    cfg.scannerIP  = m_editScannerIP->text().trimmed();
    // agvPort 默认 502，无对应 UI 控件（Modbus 标准端口）
}

// ── 转发用户操作到业务层 ─────────────────────────────────────

/**
 * @brief "▶ 开始运行"按钮回调
 *
 * 先应用配置（触发 Modbus 重连），再启动工作流引擎。
 * 引擎启动后通过 onEngineStarted() 更新 UI 按钮状态。
 */
void MainWindow::onStart()
{
    DeviceManager::Config cfg;
    buildConfig(cfg);
    m_devMgr->setConfig(cfg);
    m_devMgr->applyConfig();               // 触发 Modbus 重连
    m_engine->start(m_spinDelay->value()); // 启动五步循环
}

/// "■ 停止"按钮回调：停止引擎（内部会向机器人发复位命令）
void MainWindow::onStop()
{
    m_engine->stop();
}

/// "▶| 单步运行"按钮回调：手动推进一步（仅在停止状态下有效）
void MainWindow::onStep()
{
    m_engine->singleStep();
}

/// 补光灯切换按钮回调
void MainWindow::onLightToggle()
{
    m_devMgr->toggleLight();
}

/// "应用配置"按钮回调：保存配置并重连所有 Modbus 设备
void MainWindow::onApplyConfig()
{
    DeviceManager::Config cfg;
    buildConfig(cfg);
    m_devMgr->setConfig(cfg);
    m_devMgr->applyConfig();
}

/// 各设备测试按钮回调：先更新配置，再执行 TCP 连通性测试
void MainWindow::onTestRobot()
{
    DeviceManager::Config cfg; buildConfig(cfg);
    m_devMgr->setConfig(cfg);
    m_devMgr->testRobot();
}

void MainWindow::onTestAgv()
{
    DeviceManager::Config cfg; buildConfig(cfg);
    m_devMgr->setConfig(cfg);
    m_devMgr->testAgv();
}

void MainWindow::onTestCamera()
{
    DeviceManager::Config cfg; buildConfig(cfg);
    m_devMgr->setConfig(cfg);
    m_devMgr->testCamera();
}

/**
 * @brief "相机"按钮回调：打开奥比相机实时画面对话框
 *
 * 创建 CameraWindow（内部启动 Python 子进程），设置 WA_DeleteOnClose
 * 确保关闭时自动释放资源（包括终止 Python 进程）。
 */
void MainWindow::onTestCameraOpen()
{
    const QString ip = m_editCameraIP->text().trimmed();
    if (ip.isEmpty()) { log(QStringLiteral("[相机] IP 为空")); return; }

    log(QString("[相机] 正在打开视觉标注画面 %1:8080 ...").arg(ip));
    m_indCameraDebug->setStatus(false, QStringLiteral("连接中..."));

    auto *camWin = new CameraWindow(ip, 8080, this);
    connect(camWin, &QDialog::finished, this, [this](int) {
        m_indCameraDebug->setStatus(true, QStringLiteral("待机"));
        log(QStringLiteral("[相机] 画面窗口已关闭"));
    });
    camWin->setAttribute(Qt::WA_DeleteOnClose); // 关闭时自动 delete
    camWin->show();

    m_indCameraDebug->setStatus(true, QStringLiteral("画面已打开"));
    log(QStringLiteral("[相机] 视觉标注画面窗口已开启（HTTP /frame/annotated）"));
}

void MainWindow::onTestScanner()
{
    DeviceManager::Config cfg; buildConfig(cfg);
    m_devMgr->setConfig(cfg);
    m_devMgr->testScanner();
}

/// "手眼标定矩阵"按钮回调：打开向导，用户确认后将矩阵写入视觉系统
void MainWindow::onHandEyeCalib()
{
    auto *dlg = new HandEyeDialog(this);
    connect(dlg, &HandEyeDialog::matrixConfirmed,
            m_devMgr, &DeviceManager::applyHandEyeMatrix);
    connect(m_devMgr, &DeviceManager::handEyeMatrixApplied, this, [this]() {
        log(QStringLiteral("[手眼] 矩阵已成功应用，后续推理将使用新矩阵"));
    });
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->exec();
}

// ── 响应业务层信号（纯 UI 更新）────────────────────────────

/**
 * @brief 响应 WorkflowEngine::stepActivated 信号
 * @param idx     刚激活的步骤索引（0-4）
 * @param station 当前工位编号（1-12）
 * @param cycle   已完成的循环次数
 *
 * 更新：流程图高亮、工位条高亮、工具栏标签、设备状态指示灯、日志。
 */
void MainWindow::onStepActivated(int idx, int station, int cycle)
{
    // 更新流程图控件
    m_lblStep->setText(WorkflowEngine::kSteps[idx].name);
    m_lblCycle->setText(QString::number(cycle));
    m_flow->setActiveStep(idx);
    m_flow->setStationHighlight(station);

    // 根据当前步骤更新各设备状态指示灯和日志
    switch (idx) {
    case 0: // 相机拍照
        m_indCamera->setStatus(true, QStringLiteral("拍照中"));
        log(QString("→ 工位%1 相机拍照").arg(station));
        break;
    case 1: // 机器人抓取
        m_indRobot->setStatus(true, QStringLiteral("抓取中"));
        m_indCamera->setStatus(true, QStringLiteral("就绪"));
        log(QStringLiteral("→ 华沿机器人执行抓取"));
        break;
    case 2: // AGV 行走
        m_indRobot->setStatus(true, QStringLiteral("已抓取"));
        m_indAGV->setStatus(true, QStringLiteral("运行中"));
        log(QString("→ 仙工AGV前往工位%1上料站").arg(station));
        break;
    case 3: // 机器人放料
        m_indAGV->setStatus(true, QStringLiteral("已到位"));
        m_indRobot->setStatus(true, QStringLiteral("放料中"));
        log(QStringLiteral("→ AGV到位，机器人翻转卸料"));
        break;
    case 4: // 复位等待
        m_indRobot->setStatus(true, QStringLiteral("就绪"));
        m_indAGV->setStatus(true, QStringLiteral("复位中"));
        if (!m_engine->isRunning())
            log(QString("→ 第%1轮完成，等待下一轮").arg(cycle));
        break;
    }
}

/// 响应引擎启动：禁用开始按钮，启用停止按钮，更新指示灯
void MainWindow::onEngineStarted()
{
    m_btnStart->setEnabled(false);
    m_btnStop->setEnabled(true);

    log(QStringLiteral("========== 自动循环启动 =========="));
    const auto &cfg = m_devMgr->config();
    log(QString("机器人 %1:%2  AGV %3")
            .arg(cfg.robotIP).arg(cfg.robotPort).arg(cfg.agvIP));

    // 初始状态：所有设备显示"就绪/已连接"
    m_indCamera->setStatus(true, QStringLiteral("就绪"));
    m_indRobot->setStatus(true, QStringLiteral("已连接"));
    m_indAGV->setStatus(true, QStringLiteral("已连接"));
}

/// 响应引擎停止：恢复按钮状态，流程图清除高亮
void MainWindow::onEngineStopped()
{
    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);
    m_flow->setActiveStep(-1);        // 清除流程图高亮
    m_lblStep->setText(QStringLiteral("已停止"));
    m_indCamera->setStatus(true, QStringLiteral("待机"));
    m_indRobot->setStatus(true, QStringLiteral("待机"));
    m_indAGV->setStatus(true, QStringLiteral("待机"));

    log(QStringLiteral("========== 循环已停止 =========="));
}

/**
 * @brief 响应补光灯状态变化（DeviceManager::lightChanged 信号）
 * @param on      目标状态（true=开启）
 * @param success 操作是否成功（Linux GPIO 控制结果，Windows 始终 true）
 *
 * 注意 m_indLight 的状态逻辑：
 *   - 灯开启成功 → 绿色（on=true, success=true）
 *   - 灯开启失败 → 红色（on=true, success=false，灯实际未亮）
 *   - 灯关闭成功 → 红色（on=false，灯已灭，用红灯表示"无光"）
 *   - 灯关闭失败 → 绿色（on=false, success=false，灯还亮着）
 */
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
        // 成功关灯 → 指示灯红（灯已灭）；关灯失败 → 指示灯绿（灯还亮着）
        m_indLight->setStatus(!success,
            success ? QStringLiteral("已关闭") : QStringLiteral("关闭失败"));
    }
}

/**
 * @brief 响应配置应用（DeviceManager::configApplied 信号）
 *
 * 配置刚应用、Modbus 尚未连接，将指示灯更新为"待连接"过渡状态。
 * 真正的连接成功/失败由 robotModbusConnected/Disconnected 信号处理。
 */
void MainWindow::onConfigApplied(const QString &robotIP, int /*port*/, const QString &agvIP)
{
    m_indRobot->setStatus(true, QString("待连接 %1").arg(robotIP));
    m_indAGV->setStatus(true, QString("待连接 %1").arg(agvIP));
}

// ── 华沿 SDK 调试面板槽 ──────────────────────────────────────

void MainWindow::onHuayanConnect()
{
    m_huayanConnectBtn->setEnabled(false);
    m_huayanIndicator->setStatus(false, QStringLiteral("连接中..."));
    log(QStringLiteral("[华沿] 正在连接机器人..."));
    m_devMgr->huayanScheduler()->connectRobot();
}

void MainWindow::onHuayanDisconnect()
{
    m_devMgr->huayanScheduler()->disconnectRobot();
    m_huayanConnectBtn->setEnabled(true);
    m_huayanDisconnectBtn->setEnabled(false);
    m_huayanStartBtn->setEnabled(false);
    m_huayanStopBtn->setEnabled(false);
    m_huayanIndicator->setStatus(false, QStringLiteral("未连接"));
    log(QStringLiteral("[华沿] 已断开连接"));
}

void MainWindow::onHuayanStartStageOne()
{
    m_huayanStartBtn->setEnabled(false);
    m_devMgr->huayanScheduler()->startStageOne();
}

void MainWindow::onHuayanStop()
{
    m_devMgr->huayanScheduler()->stop();
    m_huayanStopBtn->setEnabled(false);
    m_huayanStartBtn->setEnabled(true);
    m_huayanIndicator->setStatus(false, QStringLiteral("已停止"));
}

void MainWindow::onHuayanLog(const QString &msg)
{
    log(msg);
    if (msg.contains(QStringLiteral("已连接"))) {
        m_huayanConnectBtn->setEnabled(false);
        m_huayanDisconnectBtn->setEnabled(true);
        m_huayanStartBtn->setEnabled(true);
        m_huayanStopBtn->setEnabled(false);
        m_huayanIndicator->setStatus(true, QStringLiteral("已连接"));
    }
}

void MainWindow::onHuayanStageStarted(const QString &stageName)
{
    m_huayanStartBtn->setEnabled(false);
    m_huayanStopBtn->setEnabled(true);
    m_huayanIndicator->setStatus(true, QStringLiteral("运行中"));
    log(QStringLiteral("[华沿] 阶段启动：%1").arg(stageName));
}

void MainWindow::onHuayanStageCompleted(const QString &stageName)
{
    m_huayanStartBtn->setEnabled(true);
    m_huayanStopBtn->setEnabled(false);
    m_huayanIndicator->setStatus(true, QStringLiteral("完成"));
    log(QStringLiteral("[华沿] 阶段完成：%1").arg(stageName));
}

void MainWindow::onHuayanStageError(const QString &msg)
{
    m_huayanStartBtn->setEnabled(true);
    m_huayanStopBtn->setEnabled(false);
    m_huayanIndicator->setStatus(false, QStringLiteral("错误"));
    log(QStringLiteral("[华沿] 错误：%1").arg(msg));
}
