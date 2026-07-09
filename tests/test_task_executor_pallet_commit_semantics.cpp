#include <QFile>

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

} // namespace

int main()
{
    QFile taskExecutorHeader(QStringLiteral(PROJECT_SOURCE_DIR "/src/taskexecutor.h"));
    requireTrue(taskExecutorHeader.open(QIODevice::ReadOnly | QIODevice::Text),
                "必须能读取 taskexecutor.h");
    const QString taskExecutorHeaderSource = QString::fromUtf8(taskExecutorHeader.readAll());

    QFile taskExecutorFile(QStringLiteral(PROJECT_SOURCE_DIR "/src/taskexecutor.cpp"));
    requireTrue(taskExecutorFile.open(QIODevice::ReadOnly | QIODevice::Text),
                "必须能读取 taskexecutor.cpp");
    const QString taskExecutorSource = QString::fromUtf8(taskExecutorFile.readAll());

    requireTrue(taskExecutorHeaderSource.contains(
                    QStringLiteral("ArmPalletPlace,          ///< 等待机械臂完成标准码垛动作，动作内已包含松爪后回运行安全位。")),
                "ArmPalletPlace 注释必须说明标准码垛动作内已回运行安全位");
    requireTrue(taskExecutorHeaderSource.contains(
                    QStringLiteral("CommitPallet,            ///< 标准码垛动作完整成功后，将已放点位提交到缓存。")),
                "CommitPallet 注释必须说明提交发生在完整码垛动作成功后");
    requireTrue(taskExecutorSource.contains(
                    QStringLiteral("已提交 %1 码垛数量，机械臂已在码垛动作内回运行安全位")),
                "commit 成功日志必须说明机械臂已在码垛动作内回运行安全位");
    requireTrue(taskExecutorSource.contains(QStringLiteral("HuayanScheduler::schedulerStopped")),
                "主流程必须监听华研面板停止信号，避免 ArmPalletPlace 停止后卡住");
    requireTrue(taskExecutorSource.contains(QStringLiteral("m_state == ExecState::ArmPalletPlace")),
                "华研面板停止只应在码垛动作等待态触发码垛失败收尾");
    requireTrue(taskExecutorSource.contains(QStringLiteral("码垛动作已被华研面板停止")),
                "主流程码垛停止收尾必须给出明确失败原因");
    requireTrue(taskExecutorSource.contains(QStringLiteral("finishTaskSuccess();")),
                "commit 成功后必须直接结束任务");
    requireTrue(!taskExecutorSource.contains(
                    QStringLiteral("enterState(ExecState::StowAfterPallet, QStringLiteral(\"码垛提交完成，机械臂收姿态\"));")),
                "commit 成功后不能再次进入 StowAfterPallet");

    return 0;
}
