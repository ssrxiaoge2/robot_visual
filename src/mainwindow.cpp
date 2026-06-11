/**
 * @file mainwindow.cpp
 * @brief MainWindow 主窗口实现（纯 UI 层）
 *
 * ── 架构职责 ─────────────────────────────────────────────────
 *   MainWindow 只负责：
 *     • 构建并布局所有 UI 控件
 *     • 将用户操作转发给 DeviceManager（含 HuayanScheduler 驱动的机械臂流程）
 *     • 订阅业务层信号，更新指示灯/日志/标签等 UI 元素
 *
 *   MainWindow 不包含：
 *     • 任何 Modbus 读写逻辑
 *     • 工作流状态管理
 *     • 设备连接生命周期管理
 *
 * ── 信号连接顺序 ─────────────────────────────────────────────
 *   构造函数中：
 *     1. 创建业务层（DeviceManager）
 *     2. 调用 initUI() 构建所有控件
 *     3. 连接 DeviceManager 信号（含 AGV Modbus 状态透传）
 *     4. 连接 HuayanScheduler 信号（机械臂 SDK 调试面板）
 */

#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "camerawindow.h"

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
    m_devMgr = new DeviceManager(this);

    // ── 2. 构建所有 UI 控件 ─────────────────────────────────
    initUI();

    // ── 3. 连接 DeviceManager 信号 ──────────────────────────
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

    // ── 4. AGV Modbus 连接状态 ──────────────────────────────
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

    // ── 5. 华沿 SDK 信号连接 ─────────────────────────────────
    {
        HuayanScheduler *hs = m_devMgr->huayanScheduler();
        Q_ASSERT(hs != nullptr);
        connect(hs, &HuayanScheduler::connected,
                this, &MainWindow::onHuayanConnected);
        connect(hs, &HuayanScheduler::disconnected,
                this, &MainWindow::onHuayanDisconnected);
        connect(hs, &HuayanScheduler::logMessage,
                this, &MainWindow::onHuayanLog);
        connect(hs, &HuayanScheduler::stageStarted,
                this, &MainWindow::onHuayanStageStarted);
        connect(hs, &HuayanScheduler::stageCompleted,
                this, &MainWindow::onHuayanStageCompleted);
        connect(hs, &HuayanScheduler::stageError,
                this, &MainWindow::onHuayanStageError);
        connect(hs, &HuayanScheduler::stepChanged,
                this, &MainWindow::onSdkStepChanged);
        // 顶部"机械臂"指示灯跟随 SDK 连接状态
        connect(hs, &HuayanScheduler::connected, this, [this]() {
            m_indRobot->setStatus(true, QStringLiteral("SDK 已连接"));
        });
        connect(hs, &HuayanScheduler::disconnected, this, [this]() {
            m_indRobot->setStatus(false, QStringLiteral("离线"));
        });
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
    m_btnStart->setFixedHeight(32);
    m_btnStop ->setFixedHeight(32);
    m_btnStop ->setEnabled(false);  // 初始状态：停止按钮不可用

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
    m_indRobot  = new DeviceIndicator(QStringLiteral("机械臂 SDK"));
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

    // 机械臂 IP 输入行
    m_editRobotIP = new QLineEdit("192.168.1.11");
    m_editRobotIP->setFixedWidth(130);

    auto *robotRow = new QHBoxLayout();
    m_btnTestRobot = new QPushButton(QStringLiteral("测试"));
    m_btnTestRobot->setFixedSize(56, 24);
    robotRow->addWidget(m_editRobotIP);
    robotRow->addStretch();
    robotRow->addWidget(m_btnTestRobot);
    form->addRow(QStringLiteral("机械臂 IP:"), robotRow);

    // AGV IP 输入行（端口固定 502，不暴露给用户）
    m_editAgvIP = new QLineEdit("192.168.1.100");
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

    m_editCameraIP = new QLineEdit("127.0.0.1");
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

    m_editScannerIP = new QLineEdit("192.168.1.12");
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
    m_huayanStartBtn   = new QPushButton(QStringLiteral("启动取料（阶段一）"));
    m_huayanStopBtn    = new QPushButton(QStringLiteral("停止"));
    m_huayanReleaseBtn = new QPushButton(QStringLiteral("松开夹爪"));
    m_huayanStartBtn->setEnabled(false);
    m_huayanStopBtn->setEnabled(false);
    m_huayanReleaseBtn->setEnabled(false);
    m_huayanStartBtn->setFixedHeight(28);
    m_huayanStopBtn->setFixedHeight(28);
    m_huayanReleaseBtn->setFixedHeight(28);
    row2->addWidget(m_huayanStartBtn);
    row2->addWidget(m_huayanStopBtn);
    row2->addWidget(m_huayanReleaseBtn);
    vbox->addLayout(row2);

    auto *row3 = new QHBoxLayout();
    m_huayanSpeedSlider = new QSlider(Qt::Horizontal);
    m_huayanSpeedSlider->setRange(1, 100);
    m_huayanSpeedSlider->setValue(100);
    m_huayanSpeedLabel = new QLabel(QStringLiteral("100%"));
    m_huayanSpeedLabel->setMinimumWidth(40);
    row3->addWidget(new QLabel(QStringLiteral("速度")));
    row3->addWidget(m_huayanSpeedSlider);
    row3->addWidget(m_huayanSpeedLabel);
    vbox->addLayout(row3);

    connect(m_huayanConnectBtn,    &QPushButton::clicked,
            this, &MainWindow::onHuayanConnect);
    connect(m_huayanDisconnectBtn, &QPushButton::clicked,
            this, &MainWindow::onHuayanDisconnect);
    connect(m_huayanStartBtn,      &QPushButton::clicked,
            this, &MainWindow::onHuayanStartStageOne);
    connect(m_huayanStopBtn,       &QPushButton::clicked,
            this, &MainWindow::onHuayanStop);
    connect(m_huayanReleaseBtn,    &QPushButton::clicked,
            this, &MainWindow::onHuayanRelease);
    connect(m_huayanSpeedSlider,   &QSlider::valueChanged,
            this, &MainWindow::onHuayanSpeedChanged);

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
    cfg.agvIP     = m_editAgvIP->text().trimmed();
    cfg.cameraIP   = m_editCameraIP->text().trimmed();
    cfg.cameraPort = 8080; // 视觉服务固定端口（Flask HTTP）
    cfg.scannerIP  = m_editScannerIP->text().trimmed();
    // agvPort 默认 502，无对应 UI 控件（Modbus 标准端口）
    // 机械臂 SDK 复用 robotIP（端口固定 10003）
    cfg.huayanIP   = cfg.robotIP;
    cfg.huayanPort = 10003;
}

// ── 转发用户操作到业务层 ─────────────────────────────────────

void MainWindow::onStart()
{
    m_btnStart->setEnabled(false); // 连接为阻塞调用，先禁用防止窗口期重复点击
    DeviceManager::Config cfg;
    buildConfig(cfg);
    m_devMgr->setConfig(cfg);
    m_devMgr->applyConfig();                       // 重连 AGV Modbus + 更新视觉 URL
    m_devMgr->huayanScheduler()->startStageOne();  // SDK 取料流程
}

void MainWindow::onStop()
{
    m_devMgr->huayanScheduler()->stop();
    m_devMgr->cancelAgvNav();   // AGV 行走中一并取消，避免停止后空跑
    m_flow->setActiveStep(-1);
    m_lblStep->setText(QStringLiteral("已停止"));
    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);
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
 * 配置刚应用，将指示灯更新为"待连接"过渡状态。
 * AGV 真正的连接成功/失败由 agvModbusConnected/Disconnected 信号处理。
 */
void MainWindow::onConfigApplied(const QString &robotIP, const QString &agvIP)
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
    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);
}

void MainWindow::onHuayanConnected()
{
    m_huayanConnectBtn->setEnabled(false);
    m_huayanDisconnectBtn->setEnabled(true);
    m_huayanStartBtn->setEnabled(true);
    m_huayanStopBtn->setEnabled(false);
    m_huayanReleaseBtn->setEnabled(true);
    m_huayanIndicator->setStatus(true, QStringLiteral("已连接"));
}

void MainWindow::onHuayanDisconnected()
{
    m_huayanConnectBtn->setEnabled(true);
    m_huayanDisconnectBtn->setEnabled(false);
    m_huayanStartBtn->setEnabled(false);
    m_huayanStopBtn->setEnabled(false);
    m_huayanReleaseBtn->setEnabled(false);
    m_huayanIndicator->setStatus(false, QStringLiteral("未连接"));
}

void MainWindow::onHuayanLog(const QString &msg)
{
    log(msg);
}

void MainWindow::onHuayanRelease()
{
    m_devMgr->huayanScheduler()->releaseGripper();
}

void MainWindow::onHuayanSpeedChanged(int percent)
{
    m_huayanSpeedLabel->setText(QStringLiteral("%1%").arg(percent));
    m_devMgr->huayanScheduler()->setSpeedOverride(percent);
}

/// SDK 调度步骤推进：高亮流程图节点并更新工具栏当前步骤标签
void MainWindow::onSdkStepChanged(int stepIdx)
{
    m_flow->setActiveStep(stepIdx);
    if (stepIdx >= 0 && stepIdx < m_flow->steps().size())
        m_lblStep->setText(m_flow->steps()[stepIdx].name);
}

void MainWindow::onHuayanStageStarted(const QString &stageName)
{
    m_huayanStartBtn->setEnabled(false);
    m_huayanStopBtn->setEnabled(true);
    m_huayanIndicator->setStatus(true, QStringLiteral("运行中"));
    log(QStringLiteral("[华沿] 阶段启动：%1").arg(stageName));

    m_btnStart->setEnabled(false);
    m_btnStop->setEnabled(true);
}

void MainWindow::onHuayanStageCompleted(const QString &stageName)
{
    m_huayanStartBtn->setEnabled(true);
    m_huayanStopBtn->setEnabled(false);
    m_huayanIndicator->setStatus(true, QStringLiteral("完成"));
    log(QStringLiteral("[华沿] 阶段完成：%1").arg(stageName));

    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);
    m_flow->setActiveStep(-1);
    if (stageName.startsWith(QStringLiteral("阶段一")))
        m_lblCycle->setText(QString::number(++m_cycleCount));
    m_lblStep->setText(QStringLiteral("完成"));
}

void MainWindow::onHuayanStageError(const QString &msg)
{
    m_huayanStartBtn->setEnabled(true);
    m_huayanStopBtn->setEnabled(false);
    m_huayanIndicator->setStatus(false, QStringLiteral("错误"));
    log(QStringLiteral("[华沿] 错误：%1").arg(msg));

    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);
    m_flow->setActiveStep(-1);
    m_lblStep->setText(QStringLiteral("错误"));
}
