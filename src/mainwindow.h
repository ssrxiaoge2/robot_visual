#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "deviceindicator.h"
#include "workflowwidget.h"
#include "devicemanager.h"
#include "themeswitch.h"

class WorkflowEngine;

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
    void onStep();
    void onLightToggle();
    void onApplyConfig();
    void onTestRobot();
    void onTestAgv();
    void onTestCamera();
    void onTestCameraOpen();
    void onTestScanner();

    // ── 响应业务层信号（纯 UI 更新）────────────────────────────
    void onStepActivated(int idx, int station, int cycle);
    void onEngineStarted();
    void onEngineStopped();
    void onLightChanged(bool on, bool success);
    void onConfigApplied(const QString &robotIP, int port, const QString &agvIP);

private:
    void initUI();
    void initConfigPanel(QVBoxLayout *leftPanel);
    void initCameraPanel(QVBoxLayout *leftPanel);
    void initScannerPanel(QVBoxLayout *leftPanel);
    void initRobotPanel(QVBoxLayout *leftPanel);

    void log(const QString &msg);
    /// 应用主题（true=深色，false=浅色），同时刷新所有有内联样式的控件
    void applyTheme(bool dark);

    /// 从 UI 控件读取当前配置并填充 Config 结构体
    void buildConfig(DeviceManager::Config &cfg) const;

    Ui::MainWindow *ui = nullptr;

    // ── 业务层（不含 UI 逻辑）──────────────────────────────────
    WorkflowEngine *m_engine = nullptr;
    DeviceManager  *m_devMgr = nullptr;

    // ── 工具栏控件 ───────────────────────────────────────────
    QPushButton *m_btnStart  = nullptr;
    QPushButton *m_btnStop   = nullptr;
    QPushButton *m_btnStep   = nullptr;
    QSpinBox    *m_spinDelay = nullptr;
    QLabel      *m_lblCycle  = nullptr;
    QLabel      *m_lblStep   = nullptr;

    // ── 设备状态指示灯 ──────────────────────────────────────
    DeviceIndicator *m_indCamera = nullptr;
    DeviceIndicator *m_indRobot  = nullptr;
    DeviceIndicator *m_indAGV    = nullptr;
    DeviceIndicator *m_indLight  = nullptr;

    // ── 网络配置面板 ────────────────────────────────────────
    QLineEdit   *m_editRobotIP   = nullptr;
    QSpinBox    *m_spinRobotPort = nullptr;
    QLineEdit   *m_editAgvIP     = nullptr;
    QPushButton *m_btnLight      = nullptr;
    QPushButton *m_btnApply      = nullptr;
    QPushButton *m_btnTestRobot  = nullptr;
    QPushButton *m_btnTestAgv    = nullptr;

    // ── 相机调试面板 ────────────────────────────────────────
    QLineEdit       *m_editCameraIP      = nullptr;
    QPushButton     *m_btnTestCamera     = nullptr;
    QPushButton     *m_btnTestCameraOpen = nullptr;
    DeviceIndicator *m_indCameraDebug    = nullptr;

    // ── 扫码器调试面板 ──────────────────────────────────────
    QLineEdit       *m_editScannerIP  = nullptr;
    QPushButton     *m_btnTestScanner = nullptr;
    DeviceIndicator *m_indScanner     = nullptr;

    // ── 机械臂 Modbus 控制面板 ──────────────────────────────
    DeviceIndicator *m_indRobotConn = nullptr;
    QLineEdit       *m_editRegAddr  = nullptr;
    QSpinBox        *m_spinRegCount = nullptr;
    QPushButton     *m_btnReadReg   = nullptr;
    QLineEdit       *m_editRegValue = nullptr;
    QPushButton     *m_btnWriteReg  = nullptr;
    QPlainTextEdit  *m_regView      = nullptr;

    // ── 主题开关 ─────────────────────────────────────────────
    ThemeSwitch    *m_themeSwitch = nullptr;

    // ── 流程图 / 日志 ────────────────────────────────────────
    WorkflowWidget *m_flow    = nullptr;
    QPlainTextEdit *m_logView = nullptr;
};

#endif // MAINWINDOW_H
