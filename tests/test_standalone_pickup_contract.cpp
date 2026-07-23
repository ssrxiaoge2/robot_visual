#include <QFile>
#include <QString>

#include <cstdlib>
#include <iostream>

namespace {

void requireTrue(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << std::endl;
        std::exit(1);
    }
}

QString readSource(const QString &relativePath)
{
    QFile file(QStringLiteral(PROJECT_SOURCE_DIR "/") + relativePath);
    requireTrue(file.open(QIODevice::ReadOnly | QIODevice::Text),
                qPrintable(QStringLiteral("必须能读取 %1").arg(relativePath)));
    return QString::fromUtf8(file.readAll());
}

} // namespace

int main()
{
    const QString schedulerHeader = readSource(QStringLiteral("src/huayanScheduler.h"));
    const QString schedulerSource = readSource(QStringLiteral("src/huayanScheduler.cpp"));
    const QString deviceHeader = readSource(QStringLiteral("src/devicemanager.h"));
    const QString deviceSource = readSource(QStringLiteral("src/devicemanager.cpp"));
    const QString lineManagerSource = readSource(QStringLiteral("src/linemanager.cpp"));
    const QString oldLineSource = readSource(QStringLiteral("src/lineorchestrator.cpp"));
    const QString mainHeader = readSource(QStringLiteral("src/mainwindow.h"));
    const QString mainSource = readSource(QStringLiteral("src/mainwindow.cpp"));
    const QString taskSource = readSource(QStringLiteral("src/taskexecutor.cpp"));
    const QString lineHeader = readSource(QStringLiteral("src/linemanager.h"));
    const QString taskHeader = readSource(QStringLiteral("src/taskexecutor.h"));

    requireTrue(schedulerHeader.contains(QStringLiteral("bool isBusy() const;")),
                "HuayanScheduler 必须公开只读忙碌判定");
    requireTrue(schedulerSource.contains(QStringLiteral("m_stage != Stage::None"))
                    && schedulerSource.contains(QStringLiteral("m_action != Action::None"))
                    && schedulerSource.contains(QStringLiteral("m_pendingCommand.kind != PendingCommandKind::None")),
                "忙碌判定必须覆盖阶段、独立动作和待下发命令");

    requireTrue(deviceHeader.contains(
                    QStringLiteral("bool startStandaloneStageOne(int stationId);")),
                "DeviceManager 必须提供带工位号的单独测试业务入口");
    requireTrue(deviceSource.contains(
                    QStringLiteral("stationTaskConfig(stationId, runtimeSettings())")),
                "单独测试必须从统一工位配置表加载配置并应用运行参数");
    requireTrue(deviceSource.contains(QStringLiteral("stationFuncs.captureFunc = config->captureFunc"))
                    && deviceSource.contains(QStringLiteral("stationFuncs.afterGripMode = config->afterGripMode"))
                    && deviceSource.contains(QStringLiteral("stationFuncs.afterGripFunc = config->afterGripFunc"))
                    && deviceSource.contains(QStringLiteral("stationFuncs.grabZClearance = config->grabZClearance")),
                "单独测试必须显式注入工位拍照、夹后策略和 Z 余量");
    requireTrue(deviceSource.contains(QStringLiteral("setPreGripScanEnabled(false)")),
                "单独阶段一测试必须明确关闭扫码");
    requireTrue(taskSource.contains(QStringLiteral("setPreGripScanEnabled(true)")),
                "生产 TaskExecutor 必须继续明确开启扫码");

    requireTrue(lineManagerSource.contains(QStringLiteral("m_arm->isBusy()"))
                    && lineManagerSource.contains(QStringLiteral("机械臂正被单独测试占用")),
                "总调度必须在业务层拒绝占用中的机械臂");
    requireTrue(oldLineSource.contains(QStringLiteral("m_arm->isBusy()")),
                "兼容旧整线入口也必须拒绝占用中的机械臂");
    requireTrue(deviceSource.contains(QStringLiteral("setExternalWorkflowRunning"))
                    && deviceSource.contains(QStringLiteral("m_lineOrch && m_lineOrch->isRunning()"))
                    && deviceSource.contains(QStringLiteral("m_lineManager->state() != LineSystemState::Idle")),
                "DeviceManager 必须注入新旧顶层调度互斥判定");
    requireTrue(lineManagerSource.contains(QStringLiteral("兼容整线流程正在运行")),
                "新总调度必须在业务层拒绝兼容整线运行期间启动");
    requireTrue(oldLineSource.contains(QStringLiteral("新总调度正在运行")),
                "兼容整线也必须在业务层拒绝新总调度运行期间启动");
    requireTrue(deviceSource.contains(QStringLiteral("LineSystemState::Idle"))
                    && deviceSource.contains(QStringLiteral("currentTask().taskId != 0")),
                "单独测试必须检查新总调度完全空闲");

    requireTrue(mainHeader.contains(QStringLiteral("QComboBox *m_huayanStationCombo")),
                "华研测试面板必须保存工位选择下拉框");
    requireTrue(mainSource.contains(QStringLiteral("for (int stationId = 1; stationId <= 12; ++stationId)"))
                    && mainSource.contains(QStringLiteral("m_huayanStationCombo->currentData().toInt()")),
                "UI 必须提供 1-12 工位并把所选编号传给业务层");
    requireTrue(mainSource.contains(QStringLiteral("updateStandalonePickupControls()")),
                "连接、阶段和整线状态变化必须集中刷新测试互斥控件");
    requireTrue(mainSource.contains(QStringLiteral("schedulerStopped"))
                    && mainSource.contains(QStringLiteral("&MainWindow::updateStandalonePickupControls")),
                "机械臂 stop 清空忙碌状态后必须再次刷新测试互斥控件");
    requireTrue(mainSource.contains(QStringLiteral("lineCompletelyIdle && armIdle && legacyIdle")),
                "总调度 Start 按钮必须同时考虑兼容整线空闲状态");

    requireTrue(deviceHeader.contains(QStringLiteral("m_settingsManager = nullptr"))
                    && deviceHeader.contains(QStringLiteral("const RuntimeSettings &runtimeSettings() const"))
                    && deviceHeader.contains(QStringLiteral("bool runtimeSettingsLocked() const"))
                    && deviceHeader.contains(QStringLiteral("applyRuntimeSettingsCandidate")),
                "DeviceManager 必须唯一拥有设置管理器并公开事务应用接口");
    requireTrue(deviceSource.contains(QStringLiteral("m_settingsManager->load()"))
                    && deviceSource.contains(QStringLiteral("m_huayanScheduler->applyRuntimeSettings"))
                    && deviceSource.contains(QStringLiteral("m_lineManager->applyRuntimeSettings"))
                    && deviceSource.contains(QStringLiteral("stageCandidate(candidate"))
                    && deviceSource.contains(QStringLiteral("commitStaged(candidate")),
                "启动加载和保存必须把完整配置快照事务式应用到机械臂及生产工位");
    requireTrue(lineHeader.contains(QStringLiteral("applyRuntimeSettings(const RuntimeSettings &settings)"))
                    && taskHeader.contains(QStringLiteral("applyRuntimeSettings(const RuntimeSettings &settings)"))
                    && taskSource.contains(QStringLiteral("stationTaskConfig(m_task.stationId, m_runtimeSettings)")),
                "生产 TaskExecutor 必须使用运行配置覆盖工位抓取余量");
    requireTrue(mainHeader.contains(QStringLiteral("QPushButton *m_btnSettings"))
                    && mainSource.contains(QStringLiteral("m_btnSettings = new QPushButton"))
                    && mainSource.indexOf(QStringLiteral("toolbar->addWidget(m_btnSettings)"))
                        > mainSource.indexOf(QStringLiteral("toolbar->addWidget(m_btnReset)")),
                "设置按钮必须位于复位按钮右侧");
    requireTrue(mainSource.contains(QStringLiteral("SettingsDialog dialog("))
                    && mainSource.contains(QStringLiteral("&SettingsDialog::saveRequested"))
                    && mainSource.contains(QStringLiteral("applyRuntimeSettingsCandidate")),
                "主窗口必须打开分类设置窗口并提交候选配置");

    return 0;
}
