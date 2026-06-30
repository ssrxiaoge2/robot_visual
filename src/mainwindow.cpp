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
#include "customSysScheduler.h"

#include <QAbstractItemView>
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
#include <QHeaderView>
#include <QIntValidator>
#include <QMessageBox>
#include <QScrollArea>
#include <QSizePolicy>
#include <QTextStream>

#include <algorithm>
#include <utility>

#include "handeyedialog.h"
#include "huayanScheduler.h"
#include "lineorchestrator.h"
#include "palletparamdialog.h"
#include "palletscheduler.h"

namespace {

constexpr int kLineStationCount = 12;

QString fallbackLineStateText(LineSystemState state)
{
    switch (state) {
    case LineSystemState::Idle:
        return QStringLiteral("未启动");
    case LineSystemState::Running:
        return QStringLiteral("等待缺料");
    case LineSystemState::ReturningHome:
        return QStringLiteral("回待机点");
    case LineSystemState::Error:
        return QStringLiteral("系统报警");
    }
    return QStringLiteral("未知状态");
}

QString lineTaskPhaseText(const Task &task)
{
    switch (task.state) {
    case TaskState::Pending:
        return QStringLiteral("等待执行");
    case TaskState::Running:
        switch (task.step) {
        case TaskStep::Waiting:
            return QStringLiteral("开始送料");
        case TaskStep::AgvToPickup:
        case TaskStep::StowAfterPickup:
        case TaskStep::AgvToUnload:
            return QStringLiteral("正在送料");
        case TaskStep::ArmPickup:
        case TaskStep::GripAndLift:
            return QStringLiteral("正在取料");
        case TaskStep::PreGripScan:
            return QStringLiteral("正在扫码");
        case TaskStep::ArmUnload:
            return QStringLiteral("正在倒料");
        case TaskStep::StowAfterUnload:
        case TaskStep::AgvToPallet:
        case TaskStep::PreparePalletPoint:
        case TaskStep::ArmPalletPlace:
        case TaskStep::CommitPallet:
        case TaskStep::StowAfterPallet:
            return QStringLiteral("正在放空箱");
        case TaskStep::ReturningHome:
            return QStringLiteral("回待机点");
        case TaskStep::Done:
            break;
        }
        break;
    case TaskState::Succeeded:
        return QStringLiteral("送料完成");
    case TaskState::Failed:
        return QStringLiteral("送料失败");
    case TaskState::Canceled:
        return QStringLiteral("任务已取消");
    }

    return taskStateText(task.state);
}

QString lineTaskSummaryText(const Task &task)
{
    if (task.taskId == 0) {
        return QStringLiteral("当前无任务");
    }

    const QString station = QStringLiteral("工位%1").arg(task.stationId);
    switch (task.state) {
    case TaskState::Pending:
        return QStringLiteral("%1已加入送料队列").arg(station);
    case TaskState::Running:
        switch (task.step) {
        case TaskStep::Waiting:
            return QStringLiteral("%1开始送料").arg(station);
        case TaskStep::ArmPickup:
        case TaskStep::GripAndLift:
            return QStringLiteral("%1正在取料").arg(station);
        case TaskStep::PreGripScan:
            return QStringLiteral("%1正在扫码").arg(station);
        case TaskStep::ArmUnload:
            return QStringLiteral("%1正在倒料").arg(station);
        case TaskStep::StowAfterUnload:
        case TaskStep::AgvToPallet:
        case TaskStep::PreparePalletPoint:
        case TaskStep::ArmPalletPlace:
        case TaskStep::CommitPallet:
        case TaskStep::StowAfterPallet:
            return QStringLiteral("%1正在放空箱").arg(station);
        case TaskStep::ReturningHome:
            return QStringLiteral("送料完成，系统回待机点");
        case TaskStep::AgvToPickup:
        case TaskStep::StowAfterPickup:
        case TaskStep::AgvToUnload:
            return QStringLiteral("%1正在送料").arg(station);
        case TaskStep::Done:
            break;
        }
        return QStringLiteral("%1%2").arg(station, lineTaskPhaseText(task));
    case TaskState::Succeeded:
        return QStringLiteral("%1送料完成").arg(station);
    case TaskState::Failed:
        if (!task.lastError.isEmpty()) {
            return QStringLiteral("%1送料失败：%2").arg(station, task.lastError);
        }
        return QStringLiteral("%1送料失败").arg(station);
    case TaskState::Canceled:
        if (!task.lastError.isEmpty()) {
            return QStringLiteral("%1任务已取消：%2").arg(station, task.lastError);
        }
        return QStringLiteral("%1任务已取消").arg(station);
    }

    return QStringLiteral("当前无任务");
}

QString lineTaskTooltipText(const Task &task)
{
    if (task.taskId == 0) {
        return QString();
    }

    QStringList lines;
    lines << QStringLiteral("任务号：T#%1").arg(task.taskId)
          << QStringLiteral("工位：%1").arg(task.stationId)
          << QStringLiteral("状态：%1").arg(lineTaskPhaseText(task));

    if (!task.statusText.isEmpty()) {
        lines << QStringLiteral("执行详情：%1").arg(task.statusText);
    }
    if (!task.lastError.isEmpty()) {
        lines << QStringLiteral("失败原因：%1").arg(task.lastError);
    }
    return lines.join(QLatin1Char('\n'));
}

QString lineTaskAnnouncement(const Task &task)
{
    if (task.taskId == 0 || task.state != TaskState::Running) {
        return QString();
    }

    switch (task.step) {
    case TaskStep::Waiting:
        return QString();
    case TaskStep::AgvToPickup:
    case TaskStep::StowAfterPickup:
    case TaskStep::AgvToUnload:
        return QStringLiteral("工位%1正在送料").arg(task.stationId);
    case TaskStep::ArmPickup:
    case TaskStep::GripAndLift:
        return QStringLiteral("工位%1正在取料").arg(task.stationId);
    case TaskStep::PreGripScan:
        return QStringLiteral("工位%1正在扫码").arg(task.stationId);
    case TaskStep::ArmUnload:
        return QStringLiteral("工位%1正在倒料").arg(task.stationId);
    case TaskStep::StowAfterUnload:
    case TaskStep::AgvToPallet:
    case TaskStep::PreparePalletPoint:
    case TaskStep::ArmPalletPlace:
    case TaskStep::CommitPallet:
    case TaskStep::StowAfterPallet:
        return QStringLiteral("工位%1正在放空箱").arg(task.stationId);
    case TaskStep::ReturningHome:
        return QStringLiteral("系统正在回待机点");
    case TaskStep::Done:
        break;
    }

    return QString();
}

QString lineQueueStatusText(const Task &task)
{
    if (task.state == TaskState::Running) {
        return QStringLiteral("%1（%2）")
            .arg(taskStateText(task.state), lineTaskPhaseText(task));
    }
    return taskStateText(task.state);
}

QString lineTaskTimeText(const Task &task)
{
    if (!task.createdAt.isValid()) {
        return QStringLiteral("-");
    }
    return task.createdAt.toLocalTime().toString(QStringLiteral("HH:mm:ss"));
}

QString latestTaskResultText(const QString &prefix, const Task &task, const QString &suffix)
{
    return QStringLiteral("%1 T#%2 工位%3 %4 %5")
        .arg(prefix)
        .arg(task.taskId)
        .arg(task.stationId)
        .arg(suffix)
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")));
}

} // namespace

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
    // 码垛参数 UI 与调度流程必须共用同一个 PalletScheduler，避免两边参数不同步。
    m_palletScheduler = m_devMgr->palletScheduler();
    Q_ASSERT(m_palletScheduler != nullptr);

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
    connect(m_devMgr, &DeviceManager::nscanTestStarted, this, [this] {
        setNScanInputsEnabled(false);
        m_nscanVisualState = QStringLiteral("running");
        m_nscanIndicator->setStatus(false, QStringLiteral("扫码中..."));
        m_nscanResultEdit->setText(QStringLiteral("等待扫码结果..."));
        applyTheme(m_darkTheme);
    });
    connect(m_devMgr, &DeviceManager::nscanTestFinished,
            this, &MainWindow::onNScanFinished);
    connect(m_devMgr, &DeviceManager::nscanTestIdle,
            this, &MainWindow::onNScanIdle);

    connect(m_devMgr, &DeviceManager::customSystemRequestStarted,
            this, &MainWindow::onCustomSystemRequestStarted);
    connect(m_devMgr, &DeviceManager::customSystemStatusChanged,
            this, [this](bool ok, const QString &statusText) {
        m_customSysVisualState = ok ? QStringLiteral("success") : QStringLiteral("error");
        m_customSysIndicator->setStatus(
            ok, ok ? QStringLiteral("连接正常") : statusText);
        m_customSysInfoLabel->setText(
            ok ? QStringLiteral("连接正常；HTTP 为无状态请求，读取数据会再次访问接口")
               : QStringLiteral("连接失败：%1").arg(statusText));
        m_customSysRawLabel->setText(QStringLiteral("原始 JSON 将打印到日志区"));
        setCustomSystemInputsEnabled(true);
        applyTheme(m_darkTheme);
    });
    connect(m_devMgr, &DeviceManager::customSystemDayDataReady,
            this, &MainWindow::onCustomSystemDayDataReady);
    connect(m_devMgr, &DeviceManager::customSystemRequestFailed,
            this, &MainWindow::onCustomSystemRequestFailed);

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
        // 顶部"机械臂"指示灯跟随 SDK 连接状态
        connect(hs, &HuayanScheduler::connected, this, [this]() {
            m_indRobot->setStatus(true, QStringLiteral("SDK 已连接"));
        });
        connect(hs, &HuayanScheduler::disconnected, this, [this]() {
            m_indRobot->setStatus(false, QStringLiteral("离线"));
        });
    }

    // ── 整线流程编排器信号连接 ────────────────────────────────
    {
        LineOrchestrator *lo = m_devMgr->lineOrchestrator();
        Q_ASSERT(lo != nullptr);
        connect(lo, &LineOrchestrator::stepChanged,
                this, &MainWindow::onLineStepChanged);
        connect(lo, &LineOrchestrator::lineStarted,
                this, &MainWindow::onLineStarted);
        connect(lo, &LineOrchestrator::lineFinished,
                this, &MainWindow::onLineFinished);
        connect(lo, &LineOrchestrator::lineStopped,
                this, &MainWindow::onLineStopped);
        connect(lo, &LineOrchestrator::lineError,
                this, &MainWindow::onLineError);
    }

    // ── 整线调度看板信号连接 ────────────────────────────────
    {
        LineManager *lm = m_devMgr->lineManager();
        Q_ASSERT(lm != nullptr);
        connect(lm, &LineManager::systemStateChanged,
                this, &MainWindow::updateLineSystemState);
        connect(lm, &LineManager::queueChanged,
                this, &MainWindow::updateLineQueue);
        connect(lm, &LineManager::currentTaskChanged,
                this, &MainWindow::updateLineCurrentTask);
        connect(lm, &LineManager::alarmRaised, this, [this](const QString &reason) {
            if (m_lineAlarmLabel) {
                m_lineAlarmLabel->setText(QStringLiteral("系统报警：%1\n请立即处理并 Reset Error").arg(reason));
                m_lineAlarmLabel->setToolTip(reason);
                m_lineAlarmLabel->setStyleSheet(QStringLiteral(
                    "QLabel { color: #b71c1c; font-weight: 700; background: #ffebee; "
                    "border: 1px solid #d32f2f; padding: 4px 6px; }"));
            }

            const QString lastAlarmReason = property("lineAlarmLastReason").toString();
            if (lastAlarmReason == reason) {
                return;
            }

            setProperty("lineAlarmLastReason", reason);
            QMessageBox::warning(this,
                                 QStringLiteral("系统报警"),
                                 QStringLiteral("系统报警：%1\n\n请现场处理后再执行 Reset Error。")
                                     .arg(reason));
        });

        updateLineSystemState(lm->state(), fallbackLineStateText(lm->state()));
        updateLineQueue(lm->queueSnapshot());
        updateLineCurrentTask(lm->currentTask());
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
    m_btnReset = new QPushButton(QStringLiteral("复位"));
    m_btnStart->setFixedHeight(32);
    m_btnStop ->setFixedHeight(32);
    m_btnReset->setFixedHeight(32);
    m_btnReset->setObjectName("btnReset");
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
    toolbar->addWidget(m_btnReset);
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
    initLineDispatchPanel(leftPanel);

    // 各功能面板（按功能分组，内部实现见 initXxxPanel）
    initConfigPanel(leftPanel);   // 网络配置 + 补光灯
    initCameraPanel(leftPanel);   // 相机调试
    initScannerPanel(leftPanel);  // 扫码器调试
    initNScanPanel(leftPanel);    // N-ScanHub SDK 主动扫码验证
    initCustomSystemPanel(leftPanel); // 客户系统 REST API 通信测试
    initPalletPanel(leftPanel);   // 空箱码垛配置
    initAgvPanel(leftPanel);      // AGV 调试
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
    connect(m_btnReset,          &QPushButton::clicked, this, &MainWindow::onReset);
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

void MainWindow::initLineDispatchPanel(QVBoxLayout *leftPanel)
{
    // 这是客户/现场关注的任务看板，不替代现有设备测试面板。
    auto *gbLine = new QGroupBox(QStringLiteral("调度监控"));
    auto *layout = new QVBoxLayout(gbLine);
    layout->setSpacing(6);

    auto makeValueLabel = []() {
        auto *label = new QLabel();
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return label;
    };

    auto addInfoRow = [layout, &makeValueLabel](const QString &title, QLabel *&valueLabel) {
        auto *row = new QHBoxLayout();
        auto *titleLabel = new QLabel(title);
        titleLabel->setMinimumWidth(68);
        valueLabel = makeValueLabel();
        row->addWidget(titleLabel, 0, Qt::AlignTop);
        row->addWidget(valueLabel, 1);
        layout->addLayout(row);
    };

    addInfoRow(QStringLiteral("系统状态"), m_lineStateLabel);
    addInfoRow(QStringLiteral("当前任务"), m_lineCurrentLabel);
    addInfoRow(QStringLiteral("待送料数量"), m_lineQueueCountLabel);
    addInfoRow(QStringLiteral("报警信息"), m_lineAlarmLabel);

    auto *controlRow = new QHBoxLayout();
    m_lineStartBtn = new QPushButton(QStringLiteral("Start"));
    m_lineStopBtn = new QPushButton(QStringLiteral("Stop"));
    m_lineResetBtn = new QPushButton(QStringLiteral("Reset Error"));
    m_lineStopBtn->setObjectName("btnStop");
    controlRow->addWidget(m_lineStartBtn);
    controlRow->addWidget(m_lineStopBtn);
    controlRow->addWidget(m_lineResetBtn);
    layout->addLayout(controlRow);

    auto *stationTitle = new QLabel(QStringLiteral("模拟缺料"));
    layout->addWidget(stationTitle);

    auto *stationGrid = new QGridLayout();
    stationGrid->setHorizontalSpacing(4);
    stationGrid->setVerticalSpacing(4);
    for (int stationId = 1; stationId <= kLineStationCount; ++stationId) {
        auto *button = new QPushButton(QStringLiteral("工位%1").arg(stationId));
        button->setProperty("stationId", stationId);
        button->setMinimumHeight(28);
        // 按钮只上报一次缺料事件；任务编号、FIFO 入队和执行时机由 LineManager 决定。
        connect(button, &QPushButton::clicked, this, [this, button]() {
            LineManager *lm = m_devMgr ? m_devMgr->lineManager() : nullptr;
            if (!lm) {
                return;
            }
            const int stationId = button->property("stationId").toInt();
            lm->reportShortage(stationId);
        });
        stationGrid->addWidget(button, (stationId - 1) / 3, (stationId - 1) % 3);
        m_stationButtons.append(button);
    }
    layout->addLayout(stationGrid);

    auto *queueTitle = new QLabel(QStringLiteral("FIFO 队列"));
    layout->addWidget(queueTitle);

    // 仅展示 Running + Pending；长期历史写日志，避免表格随运行时间无限增长。
    m_lineQueueTable = new QTableWidget(0, 4, gbLine);
    m_lineQueueTable->setHorizontalHeaderLabels(
        {QStringLiteral("任务号"), QStringLiteral("工位"), QStringLiteral("状态"), QStringLiteral("入队时间")});
    m_lineQueueTable->verticalHeader()->setVisible(false);
    m_lineQueueTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_lineQueueTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_lineQueueTable->setFocusPolicy(Qt::NoFocus);
    m_lineQueueTable->setAlternatingRowColors(true);
    m_lineQueueTable->setMinimumHeight(180);
    m_lineQueueTable->horizontalHeader()->setStretchLastSection(true);
    m_lineQueueTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(m_lineQueueTable);

    addInfoRow(QStringLiteral("最近完成"), m_lineLastDoneLabel);
    addInfoRow(QStringLiteral("最近失败"), m_lineLastFailedLabel);

    connect(m_lineStartBtn, &QPushButton::clicked, this, &MainWindow::onStart);
    connect(m_lineStopBtn, &QPushButton::clicked, this, &MainWindow::onStop);
    connect(m_lineResetBtn, &QPushButton::clicked, this, [this]() {
        if (m_devMgr && m_devMgr->lineManager()) {
            m_devMgr->lineManager()->resetError();
        }
    });

    m_lineStateLabel->setText(QStringLiteral("未启动"));
    m_lineCurrentLabel->setText(QStringLiteral("当前无任务"));
    m_lineQueueCountLabel->setText(QStringLiteral("0 单"));
    m_lineAlarmLabel->setText(QStringLiteral("无"));
    m_lineLastDoneLabel->setText(QStringLiteral("暂无"));
    m_lineLastFailedLabel->setText(QStringLiteral("暂无"));

    leftPanel->addWidget(gbLine);
}

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

/**
 * @brief 初始化独立的 N-ScanHub 网络扫码验证面板。
 *
 * 该面板只通过 DeviceManager 的异步测试入口发起任务，不直接调用 SDK。
 */
void MainWindow::initNScanPanel(QVBoxLayout *leftPanel)
{
    auto *group = new QGroupBox(QStringLiteral("N-ScanHub SDK 调试"));
    group->setObjectName(QStringLiteral("nscanPanel"));
    auto *layout = new QVBoxLayout(group);
    layout->setSpacing(5);

    auto *addressRow = new QHBoxLayout();
    m_nscanIpEdit = new QLineEdit(QStringLiteral("192.168.1.12"));
    m_nscanIpEdit->setObjectName(QStringLiteral("nscanIpEdit"));
    m_nscanIpEdit->setPlaceholderText(QStringLiteral("扫码枪 IP"));
    m_nscanPortEdit = new QLineEdit(QStringLiteral("30000"));
    m_nscanPortEdit->setObjectName(QStringLiteral("nscanPortEdit"));
    m_nscanPortEdit->setValidator(new QIntValidator(1, 65535, m_nscanPortEdit));
    m_nscanPortEdit->setAlignment(Qt::AlignRight);
    m_nscanPortEdit->setFixedWidth(82);
    addressRow->addWidget(new QLabel(QStringLiteral("IP:")));
    addressRow->addWidget(m_nscanIpEdit, 1);
    addressRow->addWidget(new QLabel(QStringLiteral("端口:")));
    addressRow->addWidget(m_nscanPortEdit);
    layout->addLayout(addressRow);

    auto *parameterGrid = new QGridLayout();
    parameterGrid->setHorizontalSpacing(5);
    parameterGrid->setVerticalSpacing(4);
    m_nscanTimeoutEdit = new QLineEdit(QStringLiteral("2000"));
    m_nscanTimeoutEdit->setObjectName(QStringLiteral("nscanTimeoutEdit"));
    m_nscanTimeoutEdit->setValidator(new QIntValidator(1, 60000, m_nscanTimeoutEdit));
    m_nscanTimeoutEdit->setAlignment(Qt::AlignRight);
    m_nscanAttemptsEdit = new QLineEdit(QStringLiteral("3"));
    m_nscanAttemptsEdit->setObjectName(QStringLiteral("nscanAttemptsEdit"));
    m_nscanAttemptsEdit->setValidator(new QIntValidator(1, 20, m_nscanAttemptsEdit));
    m_nscanAttemptsEdit->setAlignment(Qt::AlignRight);
    m_nscanRetryIntervalEdit = new QLineEdit(QStringLiteral("500"));
    m_nscanRetryIntervalEdit->setObjectName(QStringLiteral("nscanRetryIntervalEdit"));
    m_nscanRetryIntervalEdit->setValidator(new QIntValidator(0, 10000, m_nscanRetryIntervalEdit));
    m_nscanRetryIntervalEdit->setAlignment(Qt::AlignRight);
    parameterGrid->addWidget(new QLabel(QStringLiteral("单次超时(ms):")), 0, 0);
    parameterGrid->addWidget(m_nscanTimeoutEdit, 0, 1);
    parameterGrid->addWidget(new QLabel(QStringLiteral("最大尝试:")), 1, 0);
    parameterGrid->addWidget(m_nscanAttemptsEdit, 1, 1);
    parameterGrid->addWidget(new QLabel(QStringLiteral("重试间隔(ms):")), 2, 0);
    parameterGrid->addWidget(m_nscanRetryIntervalEdit, 2, 1);
    parameterGrid->setColumnStretch(1, 1);
    layout->addLayout(parameterGrid);

    auto *actionRow = new QHBoxLayout();
    m_nscanTriggerBtn = new QPushButton(QStringLiteral("主动扫码"));
    m_nscanClearBtn = new QPushButton(QStringLiteral("清空结果"));
    m_nscanTriggerBtn->setObjectName(QStringLiteral("btnNScanTrigger"));
    m_nscanClearBtn->setObjectName(QStringLiteral("btnNScanClear"));
    m_nscanTriggerBtn->setFixedHeight(28);
    m_nscanClearBtn->setFixedHeight(28);
    actionRow->addWidget(m_nscanTriggerBtn, 1);
    actionRow->addWidget(m_nscanClearBtn);
    layout->addLayout(actionRow);

    m_nscanIndicator = new DeviceIndicator(QStringLiteral("N-ScanHub"));
    m_nscanIndicator->setStatus(false, QStringLiteral("待测试"));
    m_nscanIndicator->setFixedSize(180, 48);
    layout->addWidget(m_nscanIndicator, 0, Qt::AlignHCenter);

    auto *resultForm = new QFormLayout();
    resultForm->setLabelAlignment(Qt::AlignRight | Qt::AlignTop);
    m_nscanResultEdit = new QLineEdit();
    m_nscanResultEdit->setObjectName(QStringLiteral("nscanResultEdit"));
    m_nscanResultEdit->setReadOnly(true);
    m_nscanResultEdit->setPlaceholderText(QStringLiteral("尚未扫码"));
    m_nscanAttemptsLabel = new QLabel(QStringLiteral("0"));
    m_nscanAttemptsLabel->setObjectName(QStringLiteral("nscanAttemptsLabel"));
    m_nscanRawLabel = new QLabel(QStringLiteral("<empty>"));
    m_nscanRawLabel->setObjectName(QStringLiteral("nscanRawLabel"));
    m_nscanRawLabel->setWordWrap(true);
    m_nscanRawLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_nscanRawLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_nscanRawLabel->setMaximumHeight(48);
    m_nscanSuccessLabel = new QLabel(QStringLiteral("0"));
    m_nscanSuccessLabel->setObjectName(QStringLiteral("nscanSuccessLabel"));
    resultForm->addRow(QStringLiteral("最新条码:"), m_nscanResultEdit);
    resultForm->addRow(QStringLiteral("实际尝试:"), m_nscanAttemptsLabel);
    resultForm->addRow(QStringLiteral("原始数据:"), m_nscanRawLabel);
    resultForm->addRow(QStringLiteral("成功计数:"), m_nscanSuccessLabel);
    layout->addLayout(resultForm);

    connect(m_nscanTriggerBtn, &QPushButton::clicked,
            this, &MainWindow::onNScanTrigger);
    connect(m_nscanClearBtn, &QPushButton::clicked,
            this, &MainWindow::onNScanClear);

    leftPanel->addWidget(group);
}

/**
 * @brief 初始化客户系统 REST API 通信测试面板。
 *
 * 面板只通过 DeviceManager 发起请求；HTTP 和 JSON 解析由 CustomSysScheduler 处理。
 */
void MainWindow::initCustomSystemPanel(QVBoxLayout *leftPanel)
{
    auto *group = new QGroupBox(QStringLiteral("客户系统通信测试"));
    auto *layout = new QVBoxLayout(group);
    layout->setSpacing(5);

    auto *row1 = new QHBoxLayout();
    m_customSysConnectBtn = new QPushButton(QStringLiteral("测试连接"));
    m_customSysFetchBtn = new QPushButton(QStringLiteral("读取数据"));
    m_customSysIndicator = new DeviceIndicator(QStringLiteral("客户系统"));
    m_customSysIndicator->setStatus(false, QStringLiteral("待测试"));
    m_customSysConnectBtn->setFixedHeight(28);
    m_customSysFetchBtn->setFixedHeight(28);
    row1->addWidget(m_customSysConnectBtn);
    row1->addWidget(m_customSysFetchBtn);
    row1->addStretch();
    row1->addWidget(m_customSysIndicator);
    layout->addLayout(row1);

    auto *line = new QFrame();
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    layout->addWidget(line);

    auto *form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignTop);
    m_customSysEndpointEdit = new QLineEdit(
        CustomSysScheduler::defaultEndpoint().toString());
    m_customSysEndpointEdit->setPlaceholderText(QStringLiteral("客户系统接口 URL"));
    m_customSysEndpointEdit->setToolTip(QStringLiteral(
        "现场电脑需先连接 WiFi WDAS_PA01；程序只验证该 HTTP 接口。"));

    m_customSysActualQtyEdit = new QLineEdit();
    m_customSysActualQtyEdit->setReadOnly(true);
    m_customSysActualQtyEdit->setPlaceholderText(QStringLiteral("尚未读取"));
    m_customSysInfoLabel = new QLabel(QStringLiteral("未读取"));
    m_customSysInfoLabel->setWordWrap(true);
    m_customSysRawLabel = new QLabel(QStringLiteral("原始 JSON 将打印到日志区"));
    m_customSysRawLabel->setWordWrap(true);
    m_customSysRawLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_customSysRawLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_customSysRawLabel->setMaximumHeight(58);

    form->addRow(QStringLiteral("接口:"), m_customSysEndpointEdit);
    form->addRow(QStringLiteral("actualQty:"), m_customSysActualQtyEdit);
    form->addRow(QStringLiteral("解析:"), m_customSysInfoLabel);
    form->addRow(QStringLiteral("日志:"), m_customSysRawLabel);
    layout->addLayout(form);

    connect(m_customSysConnectBtn, &QPushButton::clicked,
            this, &MainWindow::onCustomSystemConnect);
    connect(m_customSysFetchBtn, &QPushButton::clicked,
            this, &MainWindow::onCustomSystemFetch);

    leftPanel->addWidget(group);
}


void MainWindow::initPalletPanel(QVBoxLayout *leftPanel)
{
    auto *group = new QGroupBox(QStringLiteral("码垛配置"));
    auto *layout = new QVBoxLayout(group);
    m_btnPalletConfig = new QPushButton(QStringLiteral("打开码垛配置"));
    m_btnPalletConfig->setFixedHeight(28);
    layout->addWidget(m_btnPalletConfig);
    connect(m_btnPalletConfig, &QPushButton::clicked,
            this, &MainWindow::onPalletConfig);
    leftPanel->addWidget(group);
}

/**
 * @brief 初始化"AGV 调试"面板
 *
 * 三块：派单行（工位号→站点解析+四按钮）、工位→站点映射表（编辑即存 QSettings）、
 * 实时监控区（AgvController 1s 轮询经 monitorUpdated 刷新）。
 */
void MainWindow::initAgvPanel(QVBoxLayout *leftPanel)
{
    auto *gb   = new QGroupBox(QStringLiteral("AGV 调试"));
    auto *vbox = new QVBoxLayout(gb);

    // ── 派单行 ───────────────────────────────────────────────
    auto *dispatchRow = new QHBoxLayout();
    m_editWorkstation = new QLineEdit(QStringLiteral("1"));
    m_editWorkstation->setValidator(new QIntValidator(1, 65535, m_editWorkstation));
    m_editWorkstation->setAlignment(Qt::AlignRight);
    m_editWorkstation->setFixedWidth(70);
    m_lblResolved = new QLabel();
    m_btnAgvGo = new QPushButton(QStringLiteral("出发"));
    m_btnAgvGo->setFixedSize(56, 24);
    dispatchRow->addWidget(new QLabel(QStringLiteral("工位:")));
    dispatchRow->addWidget(m_editWorkstation);
    dispatchRow->addWidget(m_lblResolved);
    dispatchRow->addStretch();
    dispatchRow->addWidget(m_btnAgvGo);
    vbox->addLayout(dispatchRow);

    auto *navRow = new QHBoxLayout();
    m_btnAgvCancel = new QPushButton(QStringLiteral("取消"));
    m_btnAgvPause  = new QPushButton(QStringLiteral("暂停"));
    m_btnAgvResume = new QPushButton(QStringLiteral("继续"));
    for (auto *b : {m_btnAgvCancel, m_btnAgvPause, m_btnAgvResume})
        b->setFixedHeight(24);
    navRow->addWidget(m_btnAgvCancel);
    navRow->addWidget(m_btnAgvPause);
    navRow->addWidget(m_btnAgvResume);
    navRow->addStretch();
    vbox->addLayout(navRow);

    // ── 工位 → 站点映射表 ───────────────────────────────────
    m_tblStationMap = new QTableWidget(0, 2);
    m_tblStationMap->setHorizontalHeaderLabels(
        {QStringLiteral("工位号"), QStringLiteral("站点 id")});
    m_tblStationMap->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_tblStationMap->verticalHeader()->setVisible(false);
    m_tblStationMap->setFixedHeight(110);
    vbox->addWidget(m_tblStationMap);

    auto *mapBtnRow = new QHBoxLayout();
    m_btnMapAdd = new QPushButton(QStringLiteral("+ 添加"));
    m_btnMapDel = new QPushButton(QStringLiteral("- 删除"));
    m_btnMapAdd->setFixedSize(56, 22);
    m_btnMapDel->setFixedSize(56, 22);
    mapBtnRow->addWidget(m_btnMapAdd);
    mapBtnRow->addWidget(m_btnMapDel);
    mapBtnRow->addStretch();
    vbox->addLayout(mapBtnRow);

    // ── 实时监控区 ───────────────────────────────────────────
    auto *monForm = new QFormLayout();
    m_lblAgvNav     = new QLabel(QStringLiteral("-"));
    m_lblAgvStation = new QLabel(QStringLiteral("-"));
    m_lblAgvLoc     = new QLabel(QStringLiteral("-"));
    m_lblAgvBattery = new QLabel(QStringLiteral("-"));
    m_lblAgvCtrl    = new QLabel(QStringLiteral("-"));
    monForm->addRow(QStringLiteral("导航状态:"), m_lblAgvNav);
    monForm->addRow(QStringLiteral("当前站点:"), m_lblAgvStation);
    monForm->addRow(QStringLiteral("定位状态:"), m_lblAgvLoc);
    monForm->addRow(QStringLiteral("电池电量:"), m_lblAgvBattery);
    monForm->addRow(QStringLiteral("控制权:"),   m_lblAgvCtrl);
    vbox->addLayout(monForm);

    leftPanel->addWidget(gb);

    // ── 信号连接 ─────────────────────────────────────────────
    connect(m_btnAgvGo, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const int ws = m_editWorkstation->text().trimmed().toInt(&ok);
        if (!ok || ws < 1 || ws > 65535) {
            log(QStringLiteral("[AGV] 工位号必须是 1-65535 的整数"));
            return;
        }
        m_devMgr->dispatchAgv(ws);
        m_flow->setStationHighlight(ws);
    });
    connect(m_btnAgvCancel, &QPushButton::clicked, m_devMgr, &DeviceManager::cancelAgvNav);
    connect(m_btnAgvPause,  &QPushButton::clicked, m_devMgr, &DeviceManager::pauseAgvNav);
    connect(m_btnAgvResume, &QPushButton::clicked, m_devMgr, &DeviceManager::resumeAgvNav);
    connect(m_editWorkstation, &QLineEdit::textChanged,
            this, [this](const QString &) { refreshResolvedLabel(); });

    connect(m_btnMapAdd, &QPushButton::clicked, this, [this]() {
        const int row = m_tblStationMap->rowCount();
        m_tblStationMap->insertRow(row);
        m_tblStationMap->setItem(row, 0, new QTableWidgetItem(QStringLiteral("1")));
        m_tblStationMap->setItem(row, 1, new QTableWidgetItem(QStringLiteral("1")));
    });
    connect(m_btnMapDel, &QPushButton::clicked, this, [this]() {
        const int row = m_tblStationMap->currentRow();
        if (row >= 0) m_tblStationMap->removeRow(row);
        rebuildStationMapFromTable();
    });
    connect(m_tblStationMap, &QTableWidget::cellChanged,
            this, [this](int, int) { rebuildStationMapFromTable(); });

    connect(m_devMgr->agvController(), &AgvController::monitorUpdated,
            this, [this](const AgvMonitorData &d) { updateAgvMonitor(d); });

    loadStationMapToTable();
    refreshResolvedLabel();
}

/// QSettings 中的映射 → 填充表格（QSignalBlocker 防止 cellChanged 递归回写）
void MainWindow::loadStationMapToTable()
{
    const QSignalBlocker blocker(m_tblStationMap);
    const QHash<int, int> map = m_devMgr->stationMap();
    QList<int> keys = map.keys();
    std::sort(keys.begin(), keys.end());
    m_tblStationMap->setRowCount(0);
    for (int w : keys) {
        const int row = m_tblStationMap->rowCount();
        m_tblStationMap->insertRow(row);
        m_tblStationMap->setItem(row, 0, new QTableWidgetItem(QString::number(w)));
        m_tblStationMap->setItem(row, 1, new QTableWidgetItem(QString::number(map[w])));
    }
}

/// 表格 → 映射哈希并持久化；非法行（非 1-65535 整数）跳过
void MainWindow::rebuildStationMapFromTable()
{
    QHash<int, int> map;
    for (int row = 0; row < m_tblStationMap->rowCount(); ++row) {
        const auto *itemW = m_tblStationMap->item(row, 0);
        const auto *itemS = m_tblStationMap->item(row, 1);
        if (!itemW || !itemS) continue;
        bool okW = false, okS = false;
        const int w = itemW->text().toInt(&okW);
        const int s = itemS->text().toInt(&okS);
        if (okW && okS && w > 0 && s > 0 && w <= 65535 && s <= 65535)
            map.insert(w, s);
    }
    m_devMgr->setStationMap(map);
    refreshResolvedLabel();
}

void MainWindow::refreshResolvedLabel()
{
    bool ok = false;
    const int ws = m_editWorkstation ? m_editWorkstation->text().trimmed().toInt(&ok) : 0;
    m_lblResolved->setText(ok ? QString("→ 站点 %1").arg(m_devMgr->resolveStation(ws))
                              : QStringLiteral("→ 站点 -"));
}

/// 监控快照 → 五行标签；导航状态着色：到达=绿 失败/取消/超时=红
void MainWindow::updateAgvMonitor(const AgvMonitorData &d)
{
    static const QStringList kNavText = {
        QStringLiteral("无"), QStringLiteral("等待"), QStringLiteral("执行中"),
        QStringLiteral("暂停"), QStringLiteral("到达"), QStringLiteral("失败"),
        QStringLiteral("取消"), QStringLiteral("超时")};
    static const QStringList kLocText = {
        QStringLiteral("失败"), QStringLiteral("正确"),
        QStringLiteral("重定位中"), QStringLiteral("完成(需确认)")};

    const QString nav = int(d.navStatus) < kNavText.size()
        ? kNavText[d.navStatus] : QString::number(d.navStatus);
    m_lblAgvNav->setText(QString("%1 (%2)").arg(nav).arg(d.navStatus));

    QString color = m_darkTheme ? "#ccc" : "#333";
    if (d.navStatus == 4) color = m_darkTheme ? "#5cb85c" : "#107c10";
    else if (d.navStatus >= 5 && d.navStatus <= 7) color = m_darkTheme ? "#d9534f" : "#c42b1c";
    m_lblAgvNav->setStyleSheet(QString("color:%1; font-weight:bold;").arg(color));

    m_lblAgvStation->setText(QString("%1（导航目标 %2）")
                                 .arg(d.curStation).arg(d.navStation));
    m_lblAgvLoc->setText(int(d.locStatus) < kLocText.size()
        ? kLocText[d.locStatus] : QString::number(d.locStatus));
    m_lblAgvBattery->setText(QString("%1%").arg(d.battery));
    m_lblAgvCtrl->setText(d.ctrlSeized ? QStringLiteral("被外部抢占")
                                       : QStringLiteral("正常"));

    // 指示灯随导航状态恢复，避免一次异常后粘滞在"导航异常"
    if (d.navStatus >= 5 && d.navStatus <= 7)
        m_indAGV->setStatus(false, QStringLiteral("导航异常"));
    else
        m_indAGV->setStatus(true, QStringLiteral("已连接"));
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
    m_darkTheme = dark;

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
        "QPushButton        { background:#3a8fd4; color:white; border:none;"
        "                     border-radius:4px; padding:4px 16px; font-size:13px; }"
        "QPushButton:hover  { background:#4da3e8; }"
        "QPushButton:disabled { background:#444; color:#888; }"
        "QPushButton#btnStop  { background:#d9534f; }"       // 停止按钮：红色
        "QPushButton#btnStop:hover  { background:#e86562; }"
        "QPushButton#btnReset  { background:#d9534f; }"       // 复位按钮：红色
        "QPushButton#btnReset:hover  { background:#e86562; }"
        "QPushButton#btnApply { background:#5bc0de; }"       // 应用按钮：蓝色
        "QPushButton#btnApply:hover { background:#6ed0ee; }"
        "QTableWidget { background:#3a3d44; color:#ddd; gridline-color:#555;"
        "               border:1px solid #555; font-size:12px; }"
        "QHeaderView::section { background:#2f3138; color:#ccc; border:none; padding:2px 4px; }"
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
        "QPushButton        { background:#0078d4; color:white; border:none;"
        "                     border-radius:4px; padding:4px 16px; font-size:13px; }"
        "QPushButton:hover  { background:#106ebe; }"
        "QPushButton:disabled { background:#ccc; color:#888; }"
        "QPushButton#btnStop  { background:#c42b1c; }"
        "QPushButton#btnStop:hover  { background:#d83b2e; }"
        "QPushButton#btnReset  { background:#c42b1c; }"
        "QPushButton#btnReset:hover  { background:#d83b2e; }"
        "QPushButton#btnApply { background:#0078d4; }"
        "QPushButton#btnApply:hover { background:#106ebe; }"
        "QTableWidget { background:#ffffff; color:#222; gridline-color:#ccc;"
        "               border:1px solid #bbb; font-size:12px; }"
        "QHeaderView::section { background:#e8e8e8; color:#333; border:none; padding:2px 4px; }"
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

    // N-ScanHub 面板包含状态色，主题切换时必须重新计算，避免颜色粘滞。
    if (m_nscanTriggerBtn) {
        m_nscanTriggerBtn->setStyleSheet(dark
            ? "#btnNScanTrigger { background:#2f8fdd; color:white; font-weight:bold; }"
              "#btnNScanTrigger:hover { background:#49a3e8; }"
              "#btnNScanTrigger:disabled { background:#444; color:#888; }"
            : "#btnNScanTrigger { background:#0078d4; color:white; font-weight:bold; }"
              "#btnNScanTrigger:hover { background:#106ebe; }"
              "#btnNScanTrigger:disabled { background:#ccc; color:#777; }");
        m_nscanClearBtn->setStyleSheet(dark
            ? "#btnNScanClear { background:#555; color:#ddd; }"
              "#btnNScanClear:hover { background:#666; }"
            : "#btnNScanClear { background:#666; color:white; }"
              "#btnNScanClear:hover { background:#555; }");

        QString stateColor = dark ? QStringLiteral("#b8b8b8")
                                  : QStringLiteral("#444444");
        if (m_nscanVisualState == QStringLiteral("running"))
            stateColor = dark ? QStringLiteral("#66b7ff") : QStringLiteral("#0067b8");
        else if (m_nscanVisualState == QStringLiteral("success"))
            stateColor = dark ? QStringLiteral("#65d887") : QStringLiteral("#107c10");
        else if (m_nscanVisualState == QStringLiteral("timeout"))
            stateColor = dark ? QStringLiteral("#f3c969") : QStringLiteral("#9a6700");
        else if (m_nscanVisualState == QStringLiteral("error"))
            stateColor = dark ? QStringLiteral("#ff7b72") : QStringLiteral("#c42b1c");

        m_nscanResultEdit->setStyleSheet(QStringLiteral(
            "QLineEdit { color:%1; font-weight:bold; }").arg(stateColor));
        m_nscanAttemptsLabel->setStyleSheet(QStringLiteral(
            "color:%1; font-weight:bold;").arg(stateColor));
        m_nscanSuccessLabel->setStyleSheet(dark
            ? "color:#65d887; font-weight:bold;"
            : "color:#107c10; font-weight:bold;");
        m_nscanRawLabel->setStyleSheet(dark
            ? "color:#9fc5e8; background:#202226; border:1px solid #444;"
              "border-radius:3px; padding:3px; font-family:monospace; font-size:10px;"
            : "color:#35536f; background:#f7f7f7; border:1px solid #ccc;"
              "border-radius:3px; padding:3px; font-family:monospace; font-size:10px;");
    }

    if (m_customSysActualQtyEdit) {
        QString stateColor = dark ? QStringLiteral("#b8b8b8")
                                  : QStringLiteral("#444444");
        if (m_customSysVisualState == QStringLiteral("running"))
            stateColor = dark ? QStringLiteral("#66b7ff") : QStringLiteral("#0067b8");
        else if (m_customSysVisualState == QStringLiteral("success"))
            stateColor = dark ? QStringLiteral("#65d887") : QStringLiteral("#107c10");
        else if (m_customSysVisualState == QStringLiteral("error"))
            stateColor = dark ? QStringLiteral("#ff7b72") : QStringLiteral("#c42b1c");

        m_customSysActualQtyEdit->setStyleSheet(QStringLiteral(
            "QLineEdit { color:%1; font-weight:bold; }").arg(stateColor));
        m_customSysInfoLabel->setStyleSheet(QStringLiteral(
            "color:%1; font-weight:bold;").arg(stateColor));
        m_customSysRawLabel->setStyleSheet(dark
            ? "color:#9fc5e8; background:#202226; border:1px solid #444;"
              "border-radius:3px; padding:3px; font-family:monospace; font-size:10px;"
            : "color:#35536f; background:#f7f7f7; border:1px solid #ccc;"
              "border-radius:3px; padding:3px; font-family:monospace; font-size:10px;");
    }
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
    if (m_customSysEndpointEdit)
        cfg.customSysEndpoint = m_customSysEndpointEdit->text().trimmed();
    // agvPort 默认 502，无对应 UI 控件（Modbus 标准端口）
    // 机械臂 SDK 复用 robotIP（端口固定 10003）
    cfg.huayanIP   = cfg.robotIP;
    cfg.huayanPort = 10003;
}

// ── 转发用户操作到业务层 ─────────────────────────────────────

void MainWindow::onStart()
{
    // 顶部与看板 Start 统一进入新 LineManager，不再启动旧 LineOrchestrator。
    m_devMgr->lineManager()->start();
}

void MainWindow::onStop()
{
    // Stop 是清队列并进入 Error 的急停语义，不是可恢复暂停。
    m_devMgr->lineManager()->stop();
}

void MainWindow::onReset()
{
    m_devMgr->huayanScheduler()->resetArm();
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

void MainWindow::onNScanTrigger()
{
    bool portOk = false;
    bool timeoutOk = false;
    bool attemptsOk = false;
    bool retryIntervalOk = false;
    const int port = m_nscanPortEdit->text().trimmed().toInt(&portOk);
    const int timeoutMs = m_nscanTimeoutEdit->text().trimmed().toInt(&timeoutOk);
    const int maxAttempts = m_nscanAttemptsEdit->text().trimmed().toInt(&attemptsOk);
    const int retryIntervalMs = m_nscanRetryIntervalEdit->text().trimmed().toInt(&retryIntervalOk);

    QString parameterError;
    if (!portOk || port < 1 || port > 65535)
        parameterError = QStringLiteral("端口必须是 1-65535 的整数");
    else if (!timeoutOk || timeoutMs < 1 || timeoutMs > 60000)
        parameterError = QStringLiteral("单次超时必须是 1-60000 ms 的整数");
    else if (!attemptsOk || maxAttempts < 1 || maxAttempts > 20)
        parameterError = QStringLiteral("最大尝试必须是 1-20 的整数");
    else if (!retryIntervalOk || retryIntervalMs < 0 || retryIntervalMs > 10000)
        parameterError = QStringLiteral("重试间隔必须是 0-10000 ms 的整数");

    if (!parameterError.isEmpty()) {
        m_nscanVisualState = QStringLiteral("error");
        m_nscanResultEdit->setText(QStringLiteral("配置错误：%1").arg(parameterError));
        m_nscanResultEdit->setToolTip(parameterError);
        m_nscanIndicator->setStatus(false, QStringLiteral("配置错误"));
        log(QStringLiteral("[N-ScanHub] 配置错误：%1").arg(parameterError));
        applyTheme(m_darkTheme);
        return;
    }

    NScanScheduler::ScanOptions options;
    options.ip = m_nscanIpEdit->text().trimmed();
    options.port = static_cast<quint16>(port);
    options.timeoutMs = timeoutMs;
    options.maxAttempts = maxAttempts;
    options.retryIntervalMs = retryIntervalMs;
    m_devMgr->startNScanTest(options);
}

void MainWindow::onNScanClear()
{
    const bool running = m_devMgr->nscanTestRunning();
    m_nscanSuccessCount = 0;
    m_nscanSuccessLabel->setText(QStringLiteral("0"));
    m_nscanResultEdit->clear();
    m_nscanResultEdit->setToolTip(QString());
    m_nscanAttemptsLabel->setText(QStringLiteral("0"));
    m_nscanRawLabel->setText(QStringLiteral("<empty>"));
    m_nscanRawLabel->setToolTip(QString());
    m_nscanVisualState = running ? QStringLiteral("running") : QStringLiteral("idle");
    m_nscanIndicator->setStatus(
        false, running ? QStringLiteral("扫码中...") : QStringLiteral("待测试"));
    applyTheme(m_darkTheme);
}

void MainWindow::onNScanFinished(const NScanScheduler::ScanResult &result)
{
    const QByteArray fullHex = result.rawData.toHex(' ');
    constexpr qsizetype kRawSummaryBytes = 64;
    QString rawSummary = QString::fromLatin1(
        result.rawData.left(kRawSummaryBytes).toHex(' '));
    if (result.rawData.size() > kRawSummaryBytes)
        rawSummary += QStringLiteral(" ...");
    if (rawSummary.isEmpty())
        rawSummary = QStringLiteral("<empty>");

    m_nscanAttemptsLabel->setText(QString::number(result.attempts));
    m_nscanRawLabel->setText(rawSummary);
    m_nscanRawLabel->setToolTip(
        fullHex.isEmpty() ? QStringLiteral("无原始数据") : QString::fromLatin1(fullHex));

    QString displayText;
    bool success = false;
    switch (result.status) {
    case NScanScheduler::ScanResult::Status::Success:
        success = true;
        ++m_nscanSuccessCount;
        displayText = result.code;
        m_nscanVisualState = QStringLiteral("success");
        m_nscanIndicator->setStatus(true, QStringLiteral("扫码成功"));
        break;
    case NScanScheduler::ScanResult::Status::Timeout:
        displayText = QStringLiteral("扫码超时（已尝试 %1 次）").arg(result.attempts);
        if (!result.errorMessage.isEmpty())
            displayText += QStringLiteral("：%1").arg(result.errorMessage);
        m_nscanVisualState = QStringLiteral("timeout");
        m_nscanIndicator->setStatus(false, QStringLiteral("扫码超时"));
        break;
    case NScanScheduler::ScanResult::Status::ConnectError:
        displayText = QStringLiteral("连接失败：%1").arg(result.errorMessage);
        m_nscanVisualState = QStringLiteral("error");
        m_nscanIndicator->setStatus(false, QStringLiteral("连接失败"));
        break;
    case NScanScheduler::ScanResult::Status::SendError:
        displayText = QStringLiteral("触发失败：%1").arg(result.errorMessage);
        m_nscanVisualState = QStringLiteral("error");
        m_nscanIndicator->setStatus(false, QStringLiteral("触发失败"));
        break;
    case NScanScheduler::ScanResult::Status::ReadError:
        displayText = QStringLiteral("读取失败：%1").arg(result.errorMessage);
        m_nscanVisualState = QStringLiteral("error");
        m_nscanIndicator->setStatus(false, QStringLiteral("读取失败"));
        break;
    case NScanScheduler::ScanResult::Status::InvalidConfig:
        displayText = QStringLiteral("配置错误：%1").arg(result.errorMessage);
        m_nscanVisualState = QStringLiteral("error");
        m_nscanIndicator->setStatus(false, QStringLiteral("配置错误"));
        break;
    case NScanScheduler::ScanResult::Status::SdkUnavailable:
        displayText = QStringLiteral("SDK 不可用：%1").arg(result.errorMessage);
        m_nscanVisualState = QStringLiteral("error");
        m_nscanIndicator->setStatus(false, QStringLiteral("SDK 不可用"));
        break;
    }

    m_nscanResultEdit->setText(displayText);
    m_nscanResultEdit->setToolTip(displayText);
    m_nscanSuccessLabel->setText(QString::number(m_nscanSuccessCount));
    applyTheme(m_darkTheme);
}

void MainWindow::onNScanIdle()
{
    setNScanInputsEnabled(true);
}

void MainWindow::setNScanInputsEnabled(bool enabled)
{
    m_nscanIpEdit->setEnabled(enabled);
    m_nscanPortEdit->setEnabled(enabled);
    m_nscanTimeoutEdit->setEnabled(enabled);
    m_nscanAttemptsEdit->setEnabled(enabled);
    m_nscanRetryIntervalEdit->setEnabled(enabled);
    m_nscanTriggerBtn->setEnabled(enabled);
}

void MainWindow::onCustomSystemConnect()
{
    DeviceManager::Config cfg;
    buildConfig(cfg);
    m_devMgr->setConfig(cfg);
    m_devMgr->testCustomSystem();
}


void MainWindow::onPalletConfig()
{
    if (m_palletDialog) {
        m_palletDialog->raise();
        m_palletDialog->activateWindow();
        return;
    }

    // 码垛参数 UI 直接复用主流程里的调度器实例，保证配置与调度读取的是同一份状态。
    // 仅打开配置/仿真窗口；本期不接入整线流程，也不会自动提交机械臂放置完成。
    auto *dialog = new PalletParamDialog(
        m_palletScheduler,
        m_devMgr ? m_devMgr->visionClient() : nullptr,
        this);
    m_palletDialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QObject::destroyed, this, [this] { m_palletDialog = nullptr; });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MainWindow::onCustomSystemFetch()
{
    DeviceManager::Config cfg;
    buildConfig(cfg);
    m_devMgr->setConfig(cfg);
    m_devMgr->fetchCustomSystemDayData();
}

void MainWindow::onCustomSystemRequestStarted(const QString &operation)
{
    setCustomSystemInputsEnabled(false);
    m_customSysVisualState = QStringLiteral("running");
    m_customSysIndicator->setStatus(false, operation + QStringLiteral("中..."));
    m_customSysInfoLabel->setText(QStringLiteral("等待客户系统返回..."));
    m_customSysRawLabel->setText(QStringLiteral("原始 JSON 将打印到日志区"));
    m_customSysRawLabel->setToolTip(QString());
    if (operation.contains(QStringLiteral("读取")))
        m_customSysActualQtyEdit->setPlaceholderText(QStringLiteral("读取中..."));
    applyTheme(m_darkTheme);
}

void MainWindow::onCustomSystemDayDataReady(const CustomSysScheduler::DayRecord &record,
                                            const QString &rawJson)
{
    setCustomSystemInputsEnabled(true);
    m_customSysVisualState = QStringLiteral("success");
    m_customSysIndicator->setStatus(true, QStringLiteral("读取成功"));
    m_customSysActualQtyEdit->setText(QString::number(record.actualQty));
    m_customSysActualQtyEdit->setToolTip(QStringLiteral("客户接口 actualQty 字段"));

    const QString line = record.lineName.isEmpty() ? record.lineId : record.lineName;
    const QString dateText = record.statDate.isValid()
        ? record.statDate.toString(QStringLiteral("yyyy-MM-dd"))
        : QStringLiteral("-");
    const QString parsedSummary = QStringLiteral("line=%1 date=%2 plan=%3 ok=%4 ng=%5")
                                      .arg(line.isEmpty() ? QStringLiteral("-") : line)
                                      .arg(dateText)
                                      .arg(record.planQty)
                                      .arg(record.okQty)
                                      .arg(record.ngQty);
    m_customSysInfoLabel->setText(parsedSummary);

    m_customSysRawLabel->setText(QStringLiteral("完整原始 JSON 已打印到日志区"));
    m_customSysRawLabel->setToolTip(rawJson.isEmpty() ? QStringLiteral("无原始返回") : rawJson);
    log(QStringLiteral("[客户系统] 解析摘要：actualQty=%1，%2")
            .arg(record.actualQty)
            .arg(parsedSummary));
    log(QStringLiteral("[客户系统] 原始返回：%1")
            .arg(rawJson.isEmpty() ? QStringLiteral("<empty>") : rawJson));
    applyTheme(m_darkTheme);
}

void MainWindow::onCustomSystemRequestFailed(const QString &operation,
                                             const QString &errorMessage,
                                             const QString &rawJson)
{
    setCustomSystemInputsEnabled(true);
    m_customSysVisualState = QStringLiteral("error");
    m_customSysIndicator->setStatus(false, QStringLiteral("请求失败"));
    m_customSysInfoLabel->setText(QStringLiteral("%1失败：%2").arg(operation, errorMessage));
    m_customSysInfoLabel->setToolTip(errorMessage);
    // 失败时不覆盖旧 actualQty，避免把上一次成功数据误认为本次结果。
    m_customSysActualQtyEdit->setPlaceholderText(QStringLiteral("读取失败"));
    m_customSysRawLabel->setText(rawJson.isEmpty()
        ? QStringLiteral("无原始 JSON，错误详见日志")
        : QStringLiteral("失败时原始返回已打印到日志区"));
    m_customSysRawLabel->setToolTip(rawJson.isEmpty() ? errorMessage : rawJson);
    log(QStringLiteral("[客户系统] %1失败：%2").arg(operation, errorMessage));
    if (!rawJson.isEmpty())
        log(QStringLiteral("[客户系统] 失败原始返回：%1").arg(rawJson));
    applyTheme(m_darkTheme);
}

void MainWindow::setCustomSystemInputsEnabled(bool enabled)
{
    m_customSysEndpointEdit->setEnabled(enabled);
    m_customSysConnectBtn->setEnabled(enabled);
    m_customSysFetchBtn->setEnabled(enabled);
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

void MainWindow::updateLineSystemState(LineSystemState state, const QString &text)
{
    const QString displayText = text.isEmpty() ? fallbackLineStateText(state) : text;
    if (m_lineStateLabel) {
        m_lineStateLabel->setText(displayText);
        m_lineStateLabel->setToolTip(displayText);
    }

    const bool canStart = (state == LineSystemState::Idle);
    const bool canStop = (state == LineSystemState::Running
                          || state == LineSystemState::ReturningHome);
    const bool canReset = (state == LineSystemState::Error);
    const bool shortageEnabled = (state != LineSystemState::Error);

    m_btnStart->setEnabled(canStart);
    m_btnStop->setEnabled(canStop);
    if (m_lineStartBtn)
        m_lineStartBtn->setEnabled(canStart);
    if (m_lineStopBtn)
        m_lineStopBtn->setEnabled(canStop);
    if (m_lineResetBtn)
        m_lineResetBtn->setEnabled(canReset);
    for (QPushButton *button : std::as_const(m_stationButtons)) {
        button->setEnabled(shortageEnabled);
    }

    if (state != LineSystemState::Error && m_lineAlarmLabel) {
        m_lineAlarmLabel->setText(QStringLiteral("无"));
        m_lineAlarmLabel->setToolTip(QString());
        m_lineAlarmLabel->setStyleSheet(QString());
        setProperty("lineAlarmLastReason", QString());
    }

}

void MainWindow::updateLineQueue(const QList<Task> &tasks)
{
    int pendingCount = 0;
    int runningCount = 0;
    for (const Task &task : tasks) {
        if (task.state == TaskState::Pending) {
            ++pendingCount;
        } else if (task.state == TaskState::Running) {
            ++runningCount;
        }
    }

    if (m_lineQueueCountLabel) {
        QString text = QStringLiteral("%1 单").arg(pendingCount);
        if (runningCount > 0) {
            text += QStringLiteral("，当前执行 %1 单").arg(runningCount);
        }
        m_lineQueueCountLabel->setText(text);
    }

    if (!m_lineQueueTable) {
        return;
    }

    // 长期历史走日志，表格只显示未完成任务，避免长时间运行 UI 无限增长。
    m_lineQueueTable->setRowCount(tasks.size());
    for (int row = 0; row < tasks.size(); ++row) {
        const Task &task = tasks[row];
        auto *taskIdItem = new QTableWidgetItem(QString::number(task.taskId));
        auto *stationItem = new QTableWidgetItem(QStringLiteral("工位%1").arg(task.stationId));
        auto *stateItem = new QTableWidgetItem(lineQueueStatusText(task));
        auto *timeItem = new QTableWidgetItem(lineTaskTimeText(task));
        taskIdItem->setTextAlignment(Qt::AlignCenter);
        stationItem->setTextAlignment(Qt::AlignCenter);
        stateItem->setTextAlignment(Qt::AlignCenter);
        timeItem->setTextAlignment(Qt::AlignCenter);
        m_lineQueueTable->setItem(row, 0, taskIdItem);
        m_lineQueueTable->setItem(row, 1, stationItem);
        m_lineQueueTable->setItem(row, 2, stateItem);
        m_lineQueueTable->setItem(row, 3, timeItem);
    }
}

void MainWindow::updateLineCurrentTask(const Task &task)
{
    if (m_lineCurrentLabel) {
        m_lineCurrentLabel->setText(lineTaskSummaryText(task));
        m_lineCurrentLabel->setToolTip(lineTaskTooltipText(task));
    }

    const bool changed = !m_hasLastLineTask
        || task.taskId != m_lastLineTaskId
        || task.step != m_lastLineTaskStep
        || task.state != m_lastLineTaskState;
    if (changed) {
        const QString announcement = lineTaskAnnouncement(task);
        if (!announcement.isEmpty()) {
            log(announcement);
        }
    }

    if (task.state == TaskState::Succeeded && m_lineLastDoneLabel) {
        const QString text = latestTaskResultText(QStringLiteral("已完成"), task,
                                                  QStringLiteral("送料完成"));
        m_lineLastDoneLabel->setText(text);
        m_lineLastDoneLabel->setToolTip(task.statusText);
    } else if ((task.state == TaskState::Failed || task.state == TaskState::Canceled)
               && m_lineLastFailedLabel) {
        QString text = latestTaskResultText(QStringLiteral("失败"), task,
                                            task.state == TaskState::Canceled
                                                ? QStringLiteral("已取消")
                                                : QStringLiteral("送料失败"));
        if (!task.lastError.isEmpty()) {
            text += QStringLiteral("\n原因：%1").arg(task.lastError);
        }
        m_lineLastFailedLabel->setText(text);
        m_lineLastFailedLabel->setToolTip(task.lastError);
    }

    m_lastLineTaskId = task.taskId;
    m_lastLineTaskStep = task.step;
    m_lastLineTaskState = task.state;
    m_hasLastLineTask = (task.taskId != 0);
}

bool MainWindow::lineManagerOwnsTopLevelWorkflowUi() const
{
    if (!m_devMgr || !m_devMgr->lineManager()) {
        return false;
    }

    LineManager *lm = m_devMgr->lineManager();
    const LineSystemState state = lm->state();
    return state == LineSystemState::Running
        || state == LineSystemState::ReturningHome
        || state == LineSystemState::Error
        || lm->currentTask().taskId != 0;
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

/// 整线步骤推进：高亮流程图节点并更新工具栏当前步骤标签
void MainWindow::onLineStepChanged(int stepIdx)
{
    m_flow->setActiveStep(stepIdx);
    if (stepIdx >= 0 && stepIdx < m_flow->steps().size())
        m_lblStep->setText(m_flow->steps()[stepIdx].name);
}

void MainWindow::onLineStarted()
{
    m_btnStart->setEnabled(false);
    m_btnStop->setEnabled(true);
}

void MainWindow::onLineFinished()
{
    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);
    m_flow->setActiveStep(-1);
    m_lblCycle->setText(QString::number(++m_cycleCount));
    m_lblStep->setText(QStringLiteral("完成"));
}

void MainWindow::onLineStopped()
{
    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);
    m_flow->setActiveStep(-1);
    m_lblStep->setText(QStringLiteral("已停止"));
}

void MainWindow::onLineError(const QString &reason)
{
    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);
    m_flow->setActiveStep(-1);
    m_lblStep->setText(QStringLiteral("错误"));
    log(QStringLiteral("整线流程中止：%1").arg(reason));
}

void MainWindow::onHuayanStageStarted(const QString &stageName)
{
    m_huayanStartBtn->setEnabled(false);
    m_huayanStopBtn->setEnabled(true);
    m_huayanIndicator->setStatus(true, QStringLiteral("运行中"));
    log(QStringLiteral("[华沿] 阶段启动：%1").arg(stageName));

    if (lineManagerOwnsTopLevelWorkflowUi()) {
        return;
    }

    m_btnStart->setEnabled(false);
    m_btnStop->setEnabled(true);
}

void MainWindow::onHuayanStageCompleted(const QString &stageName)
{
    m_huayanStartBtn->setEnabled(true);
    m_huayanStopBtn->setEnabled(false);
    m_huayanIndicator->setStatus(true, QStringLiteral("完成"));
    log(QStringLiteral("[华沿] 阶段完成：%1").arg(stageName));

    if (lineManagerOwnsTopLevelWorkflowUi()) {
        return;
    }

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

    if (lineManagerOwnsTopLevelWorkflowUi()) {
        return;
    }

    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);
    m_flow->setActiveStep(-1);
    m_lblStep->setText(QStringLiteral("错误"));
}
