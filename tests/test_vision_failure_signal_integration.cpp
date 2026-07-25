#include <QtTest/QtTest>

#include <QSignalSpy>

#define private public
#include "taskexecutor.h"
#include "huayanScheduler.h"
#undef private

// TaskExecutor 的本测试路径不会启动扫码，但其目标文件仍引用状态文本函数；
// 在测试目标内提供等价轻量实现，避免为了一个纯文本分支加载扫码厂商运行库。
QString NScanScheduler::statusText(NScanScheduler::ScanResult::Status status)
{
    Q_UNUSED(status);
    return QStringLiteral("测试扫码状态");
}

class VisionFailureSignalIntegrationTest : public QObject
{
    Q_OBJECT

private slots:
    void visionAlignmentFailureTerminatesTaskWithoutCleanupStow()
    {
        HuayanScheduler scheduler;
        TaskExecutor executor(nullptr, &scheduler, nullptr, nullptr);

        executor.m_task.taskId = 7;
        executor.m_task.state = TaskState::Running;
        executor.m_task.step = TaskStep::ArmPickup;
        executor.m_state = TaskExecutor::ExecState::ArmPickup;

        QSignalSpy failedSpy(&executor, &TaskExecutor::taskFailed);
        QSignalSpy stageStartedSpy(&scheduler, &HuayanScheduler::stageStarted);

        scheduler.m_stage = HuayanScheduler::Stage::StageOne;
        scheduler.m_stageStep =
            HuayanScheduler::StageStep::ValidateVisionAlignment;
        bool stoppedBeforeNotification = false;
        connect(&scheduler, &HuayanScheduler::visionAlignmentFailed,
                this, [&] {
            stoppedBeforeNotification =
                scheduler.m_stage == HuayanScheduler::Stage::None
                && scheduler.m_stageStep == HuayanScheduler::StageStep::None
                && !scheduler.m_pollTimer->isActive()
                && !scheduler.m_timeoutTimer->isActive();
        });

        scheduler.onVisionErrorForPickup(
            QStringLiteral("联合对准未收敛"));

        QCOMPARE(failedSpy.count(), 1);
        QVERIFY(stoppedBeforeNotification);
        QCOMPARE(stageStartedSpy.count(), 0);
        QCOMPARE(executor.currentTask().state, TaskState::Failed);
        QCOMPARE(executor.currentTask().step, TaskStep::Done);
        QVERIFY(!executor.isBusy());
        QVERIFY(!executor.m_cleanupAfterTaskFailure);
        QCOMPARE(executor.m_state, TaskExecutor::ExecState::Idle);
    }
};

QTEST_APPLESS_MAIN(VisionFailureSignalIntegrationTest)

#include "test_vision_failure_signal_integration.moc"
