#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QComboBox>
#include <QMainWindow>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>

#include "deviceindicator.h"
#include "workflowwidget.h"
#include "devicemanager.h"
#include "themeswitch.h"
#include "agvcontroller.h"

class HandEyeDialog;
class HuayanScheduler;
class PalletParamDialog;
class PalletScheduler;
class QCheckBox;
class QSpinBox;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    // ── 转发用户操作到业务层 ──────────────────────────────────
    void onStart();
    void onStop();
    void onReset();
    void onLightToggle();
    void onApplyConfig();
    void onTestRobot();
    void onTestAgv();
    void onTestCamera();
    void onTestCameraOpen();
    void onTestScanner();
    void onNScanTrigger();
    void onNScanClear();
    void onNScanFinished(const NScanScheduler::ScanResult &result);
    void onNScanIdle();
    void onCustomSystemConnect();
    void onCustomSystemFetch();
    // 打开空箱码垛配置窗口；窗口可执行真实单步码垛调试，停止统一使用华研面板停止按钮。
    void onPalletConfig();
    void onCustomSystemRequestStarted(const QString &operation);
    void onCustomSystemDayDataReady(const CustomSysScheduler::DayRecord &record,
                                    const QString &rawJson);
    void onCustomSystemRequestFailed(const QString &operation,
                                     const QString &errorMessage,
                                     const QString &rawJson);
    void onHandEyeCalib();
    void onHuayanConnect();
    void onHuayanDisconnect();
    void onHuayanConnected();
    void onHuayanDisconnected();
    void onHuayanStartStageOne();
    void onHuayanStop();
    void onHuayanRelease();
    void onHuayanSpeedChanged(int percent);
    void onHuayanLog(const QString &msg);
    void onHuayanStageStarted(const QString &stageName);
    void onHuayanStageCompleted(const QString &stageName);
    void onHuayanStageError(const QString &msg);
    void onLineStepChanged(int stepIdx);
    void onLineStarted();
    void onLineFinished();
    void onLineStopped();
    void onLineError(const QString &reason);

    // ── 响应业务层信号（纯 UI 更新）────────────────────────────
    void onLightChanged(bool on, bool success);
    void onConfigApplied(const QString &robotIP, const QString &agvIP);

private:
    void initUI();
    /// 创建客户/现场使用的 12 工位任务看板；不替代下方设备调试面板。
    void initLineDispatchPanel(QVBoxLayout *leftPanel);
    void initConfigPanel(QVBoxLayout *leftPanel);
    void initCameraPanel(QVBoxLayout *leftPanel);
    void initScannerPanel(QVBoxLayout *leftPanel);
    void initNScanPanel(QVBoxLayout *leftPanel);
    void initCustomSystemPanel(QVBoxLayout *leftPanel);
    // 在左侧控制面板添加空箱码垛配置入口。
    void initPalletPanel(QVBoxLayout *leftPanel);
    void initHuayanPanel(QVBoxLayout *leftPanel);
    void initAgvPanel(QVBoxLayout *leftPanel);
    /// 在 AGV 与华沿面板之间创建紧凑充电调试区，高级参数保持在独立对话框。
    void initChargePanel(QVBoxLayout *leftPanel);
    /// 用 DeviceManager 的已生效快照刷新全部充电文案，不从控件反推业务状态。
    void updateChargePanel();
    /// 统一根据调度、AGV、控制器和自动授权状态计算按钮及阈值可用性。
    void updateChargeControls();
    /// 将主面板三阈值与其余已生效参数组成完整候选，经事务入口保存并应用。
    void applyChargeThresholdCandidate();
    /// 打开独立通信/电气/超时设置窗口，保存失败时保留窗口和已生效参数。
    void showChargeSettingsDialog();
    void loadStationMapToTable();
    void rebuildStationMapFromTable();
    void refreshResolvedLabel();
    void updateAgvMonitor(const AgvMonitorData &d);
    void setNScanInputsEnabled(bool enabled);
    void setCustomSystemInputsEnabled(bool enabled);
    void updateLineSystemState(LineSystemState state, const QString &text); ///< 只更新状态/报警控件。
    void updateLineQueue(const QList<Task> &tasks);                         ///< 只展示未完成任务快照。
    void updateLineCurrentTask(const Task &task);                           ///< 更新当前任务和最近结果文案。
    /// 根据机械臂连接/忙碌状态与整线状态，集中刷新手工阶段一和总调度入口的互斥状态。
    void updateStandalonePickupControls();
    bool lineManagerOwnsTopLevelWorkflowUi() const;

    void log(const QString &msg);
    /// 应用主题（true=深色，false=浅色），同时刷新所有有内联样式的控件
    void applyTheme(bool dark);

    /// 从 UI 控件读取当前配置并填充 Config 结构体
    void buildConfig(DeviceManager::Config &cfg) const;

    Ui::MainWindow *ui = nullptr;

    // ── 业务层（不含 UI 逻辑）──────────────────────────────────
    DeviceManager   *m_devMgr = nullptr;
    // 码垛参数 UI 与主流程共用 DeviceManager 内的同一个调度器，避免参数不同步。
    PalletScheduler *m_palletScheduler = nullptr;

    // ── 工具栏控件 ───────────────────────────────────────────
    QPushButton *m_btnStart  = nullptr;
    QPushButton *m_btnStop   = nullptr;
    QPushButton *m_btnReset  = nullptr;
    QPushButton *m_btnSettings = nullptr;
    QLabel      *m_lblCycle  = nullptr;
    QLabel      *m_lblStep   = nullptr;
    int          m_cycleCount = 0;

    // ── 设备状态指示灯 ──────────────────────────────────────
    DeviceIndicator *m_indCamera = nullptr;
    DeviceIndicator *m_indRobot  = nullptr;
    DeviceIndicator *m_indAGV    = nullptr;
    DeviceIndicator *m_indLight  = nullptr;

    // ── 调度监控看板：所有成员只保存 UI 状态，业务真值在 LineManager ─────────
    QLabel       *m_lineStateLabel      = nullptr;
    QLabel       *m_lineCurrentLabel    = nullptr;
    QLabel       *m_lineQueueCountLabel = nullptr;
    QLabel       *m_lineAlarmLabel      = nullptr;
    QLabel       *m_lineLastDoneLabel   = nullptr;
    QLabel       *m_lineLastFailedLabel = nullptr;
    QTableWidget *m_lineQueueTable      = nullptr;
    QPushButton  *m_lineStartBtn        = nullptr;
    QPushButton  *m_lineStopBtn         = nullptr;
    QPushButton  *m_lineResetBtn        = nullptr;
    QList<QPushButton *> m_stationButtons; ///< 12 个模拟缺料入口，property 保存 stationId。
    quint64       m_lastLineTaskId      = 0; ///< 用于抑制同一任务文案重复记录。
    TaskStep      m_lastLineTaskStep    = TaskStep::Waiting;
    TaskState     m_lastLineTaskState   = TaskState::Pending;
    bool          m_hasLastLineTask     = false;

    // ── 网络配置面板 ────────────────────────────────────────
    QLineEdit   *m_editRobotIP   = nullptr;
    QLineEdit   *m_editAgvIP     = nullptr;
    QPushButton *m_btnLight      = nullptr;
    QPushButton *m_btnApply      = nullptr;
    QPushButton *m_btnTestRobot  = nullptr;
    QPushButton *m_btnTestAgv    = nullptr;

    // ── 相机调试面板 ────────────────────────────────────────
    QLineEdit       *m_editCameraIP      = nullptr;
    QPushButton     *m_btnTestCamera     = nullptr;
    QPushButton     *m_btnTestCameraOpen = nullptr;
    QPushButton     *m_btnHandEye        = nullptr;
    DeviceIndicator *m_indCameraDebug    = nullptr;

    // ── 扫码器调试面板 ──────────────────────────────────────
    QLineEdit       *m_editScannerIP  = nullptr;
    QPushButton     *m_btnTestScanner = nullptr;
    DeviceIndicator *m_indScanner     = nullptr;

    // ── N-ScanHub SDK 验证面板 ──────────────────────────────
    QLineEdit       *m_nscanIpEdit            = nullptr;
    QLineEdit       *m_nscanPortEdit          = nullptr;
    QLineEdit       *m_nscanTimeoutEdit       = nullptr;
    QLineEdit       *m_nscanAttemptsEdit      = nullptr;
    QLineEdit       *m_nscanRetryIntervalEdit = nullptr;
    QPushButton     *m_nscanTriggerBtn        = nullptr;
    QPushButton     *m_nscanClearBtn          = nullptr;
    DeviceIndicator *m_nscanIndicator         = nullptr;
    QLineEdit       *m_nscanResultEdit        = nullptr;
    QLabel          *m_nscanAttemptsLabel     = nullptr;
    QLabel          *m_nscanRawLabel          = nullptr;
    QLabel          *m_nscanSuccessLabel      = nullptr;
    int              m_nscanSuccessCount      = 0;
    QString          m_nscanVisualState       = QStringLiteral("idle");

    // ── 客户系统通信测试面板 ────────────────────────────────
    QLineEdit       *m_customSysEndpointEdit  = nullptr;
    QPushButton     *m_customSysConnectBtn    = nullptr;
    QPushButton     *m_customSysFetchBtn      = nullptr;
    DeviceIndicator *m_customSysIndicator     = nullptr;
    QLineEdit       *m_customSysActualQtyEdit = nullptr;
    QLabel          *m_customSysInfoLabel     = nullptr;
    QLabel          *m_customSysRawLabel      = nullptr;
    QString          m_customSysVisualState   = QStringLiteral("idle");

    // ── 空箱码垛配置面板：只持有配置入口和当前打开的单例对话框 ─────────
    QPushButton *m_btnPalletConfig = nullptr;
    QPointer<PalletParamDialog> m_palletDialog;

    // ── 华沿 SDK 调试面板 ───────────────────────────────────────
    QPushButton     *m_huayanConnectBtn    = nullptr;
    QPushButton     *m_huayanDisconnectBtn = nullptr;
    DeviceIndicator *m_huayanIndicator     = nullptr;
    QPushButton     *m_huayanStartBtn      = nullptr;
    QComboBox *m_huayanStationCombo = nullptr; ///< 仅供手工阶段一测试选择 1～12 号工位。
    QPushButton     *m_huayanStopBtn       = nullptr;
    QPushButton     *m_huayanReleaseBtn    = nullptr;
    QSlider         *m_huayanSpeedSlider   = nullptr;
    QLabel          *m_huayanSpeedLabel    = nullptr;

    // ── AGV 调试面板 ────────────────────────────────────────
    QLineEdit    *m_editWorkstation = nullptr;
    QLabel       *m_lblResolved     = nullptr;
    QPushButton  *m_btnAgvGo        = nullptr;
    QPushButton  *m_btnAgvCancel    = nullptr;
    QPushButton  *m_btnAgvPause     = nullptr;
    QPushButton  *m_btnAgvResume    = nullptr;
    QTableWidget *m_tblStationMap   = nullptr;
    QPushButton  *m_btnMapAdd       = nullptr;
    QPushButton  *m_btnMapDel       = nullptr;
    QLabel       *m_lblAgvNav       = nullptr;
    QLabel       *m_lblAgvStation   = nullptr;
    QLabel       *m_lblAgvLoc       = nullptr;
    QLabel       *m_lblAgvBattery   = nullptr;
    QLabel       *m_lblAgvCtrl      = nullptr;
    bool          m_darkTheme       = true;

    // ── 紧凑充电调试面板：只显示高频状态/阈值，通信等高级参数放独立窗口 ──
    QCheckBox *m_autoChargeSwitch = nullptr; ///< 每次进程启动默认关闭，不参与参数持久化。
    QLabel *m_chargeModeNotice = nullptr;    ///< 醒目显示自动授权及安全收尾状态。
    QLabel *m_chargeDecisionLabel = nullptr; ///< 协调器最近一次自动决策说明。
    QLabel *m_chargeBatteryLabel = nullptr;  ///< DeviceManager 完整 AGV 快照中的电量。
    QLabel *m_chargeStationLabel = nullptr;  ///< 严格使用 AGV 当前站点，不使用导航目标。
    QSpinBox *m_chargeStartPercentSpin = nullptr;
    QSpinBox *m_chargeDispatchPercentSpin = nullptr;
    QSpinBox *m_chargeStopPercentSpin = nullptr;
    QLabel *m_chargeControllerStateLabel = nullptr;
    QLabel *m_chargeElectricalLabel = nullptr;
    QLabel *m_chargeActuatorLabel = nullptr;
    QLabel *m_chargeFaultLabel = nullptr;
    QPushButton *m_chargeSettingsButton = nullptr;
    QPushButton *m_chargeQueryButton = nullptr;
    QPushButton *m_chargeStartButton = nullptr;
    QPushButton *m_chargeStopButton = nullptr;
    QString m_chargeDecisionText =
        QStringLiteral("自动充电未授权，主调度逻辑保持原样");

    // ── 主题开关 ─────────────────────────────────────────────
    ThemeSwitch    *m_themeSwitch = nullptr;

    // ── 流程图 / 日志 ────────────────────────────────────────
    WorkflowWidget *m_flow      = nullptr;
    QPlainTextEdit *m_logView   = nullptr;
    QFile          *m_logFile   = nullptr;
    QTextStream    *m_logStream = nullptr;
};

#endif // MAINWINDOW_H
