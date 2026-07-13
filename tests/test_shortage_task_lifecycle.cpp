#include "taskqueue.h"

#include <QFile>
#include <QTest>

namespace {

QString readSourceFile(const QString &relativePath)
{
    QFile file(QStringLiteral(PROJECT_SOURCE_DIR "/") + relativePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qFatal("无法读取源码契约文件");
    }
    return QString::fromUtf8(file.readAll());
}

int countOccurrences(const QString &haystack, const QString &needle)
{
    int count = 0;
    int from = 0;
    while ((from = haystack.indexOf(needle, from)) >= 0) {
        ++count;
        from += needle.size();
    }
    return count;
}

} // namespace

class ShortageTaskLifecycleTest final : public QObject
{
    Q_OBJECT

private slots:
    void taskSourceHasThreeDistinctChineseLabels();       // TK-01：三类来源必须有统一中文文案。
    void enqueueAlwaysAppendsAndTakeNextKeepsOrder();      // TK-02/RG-01：入队只追加，出队严格 FIFO。
    void unloadFactExistsOnlyAtArmUnloadTransition();      // TK-03/RG-04：唯一倒料事实只在倒料成功后发布。
    void systemErrorPublishesTerminalFactForCurrentTask(); // TK-06/RG-05：系统错误先发当前任务终态事实。
    void stopStillClearsPendingAndEntersError();           // RG-02：Stop 仍清 Pending 并进入 Error。
    void resetStillReturnsIdleWithoutStarting();           // RG-03：Reset 只回 Idle，不自动启动。
};

void ShortageTaskLifecycleTest::taskSourceHasThreeDistinctChineseLabels()
{
    const QString uiMock = taskSourceText(TaskSource::UiMock);
    const QString liveAutomatic = taskSourceText(TaskSource::LiveAutomatic);
    const QString liveManual = taskSourceText(TaskSource::LiveManual);

    QCOMPARE(uiMock, QStringLiteral("模拟"));
    QCOMPARE(liveAutomatic, QStringLiteral("真实自动"));
    QCOMPARE(liveManual, QStringLiteral("人工补料"));
    QVERIFY(uiMock != liveAutomatic);
    QVERIFY(uiMock != liveManual);
    QVERIFY(liveAutomatic != liveManual);
}

void ShortageTaskLifecycleTest::enqueueAlwaysAppendsAndTakeNextKeepsOrder()
{
    TaskQueue queue;

    const Task first = queue.enqueue(3, TaskSource::UiMock);
    const Task second = queue.enqueue(5, TaskSource::LiveAutomatic, quint64{202607140001});
    const Task third = queue.enqueue(5, TaskSource::LiveManual, quint64{202607140002});

    QCOMPARE(first.taskId, quint64{1});
    QCOMPARE(second.taskId, quint64{2});
    QCOMPARE(third.taskId, quint64{3});
    QCOMPARE(first.replenishmentOrderNo, quint64{0});
    QCOMPARE(second.replenishmentOrderNo, quint64{202607140001});
    QCOMPARE(third.replenishmentOrderNo, quint64{202607140002});

    const QList<Task> snapshot = queue.pendingSnapshot();
    QCOMPARE(snapshot.size(), 3);
    QCOMPARE(snapshot.at(0).taskId, first.taskId);
    QCOMPARE(snapshot.at(1).taskId, second.taskId);
    QCOMPARE(snapshot.at(2).taskId, third.taskId);

    const Task takenFirst = queue.takeNext();
    const Task takenSecond = queue.takeNext();
    const Task takenThird = queue.takeNext();
    QCOMPARE(takenFirst.taskId, first.taskId);
    QCOMPARE(takenSecond.taskId, second.taskId);
    QCOMPARE(takenThird.taskId, third.taskId);
    QCOMPARE(takenFirst.state, TaskState::Running);
    QCOMPARE(takenSecond.state, TaskState::Running);
    QCOMPARE(takenThird.state, TaskState::Running);
}

void ShortageTaskLifecycleTest::unloadFactExistsOnlyAtArmUnloadTransition()
{
    const QString header = readSourceFile(QStringLiteral("src/taskexecutor.h"));
    const QString source = readSourceFile(QStringLiteral("src/taskexecutor.cpp"));

    QVERIFY2(header.contains(QStringLiteral("void materialUnloaded(const Task &task);")),
             "TaskExecutor 必须声明完整 Task 快照的倒料事实信号");
    QCOMPARE(countOccurrences(source, QStringLiteral("emit materialUnloaded(m_task);")), 1);

    const int armUnloadCase = source.indexOf(QStringLiteral("case ExecState::ArmUnload:"));
    QVERIFY(armUnloadCase >= 0);
    const int emitIndex = source.indexOf(QStringLiteral("emit materialUnloaded(m_task);"), armUnloadCase);
    QVERIFY(emitIndex > armUnloadCase);
    const int stowIndex = source.indexOf(
        QStringLiteral("enterState(ExecState::StowAfterUnload, QStringLiteral(\"倒料完成，机械臂收姿态\"));"),
        armUnloadCase);
    QVERIFY(stowIndex > emitIndex);
    QVERIFY(source.indexOf(QStringLiteral("case ExecState::StowAfterUnload:"), armUnloadCase) > stowIndex);
}

void ShortageTaskLifecycleTest::systemErrorPublishesTerminalFactForCurrentTask()
{
    const QString header = readSourceFile(QStringLiteral("src/linemanager.h"));
    const QString source = readSourceFile(QStringLiteral("src/linemanager.cpp"));

    QVERIFY(header.contains(QStringLiteral("void shortageTaskTerminal(Task task, QString reason);")));

    const int handler = source.indexOf(QStringLiteral("void LineManager::onExecutorSystemError"));
    QVERIFY(handler >= 0);
    const int terminal = source.indexOf(QStringLiteral("emit shortageTaskTerminal("), handler);
    const int enterError = source.indexOf(QStringLiteral("enterError(reason);"), handler);
    QVERIFY(terminal > handler);
    QVERIFY(enterError > terminal);
    QVERIFY(source.mid(handler, enterError - handler).contains(QStringLiteral("systemError")));
}

void ShortageTaskLifecycleTest::stopStillClearsPendingAndEntersError()
{
    const QString source = readSourceFile(QStringLiteral("src/linemanager.cpp"));
    const int stop = source.indexOf(QStringLiteral("void LineManager::stop()"));
    QVERIFY(stop >= 0);
    const int clearPending = source.indexOf(QStringLiteral("clearPendingForError(reason);"), stop);
    const int setError = source.indexOf(QStringLiteral("setState(LineSystemState::Error"), stop);
    QVERIFY(clearPending > stop);
    QVERIFY(setError > clearPending);
}

void ShortageTaskLifecycleTest::resetStillReturnsIdleWithoutStarting()
{
    const QString source = readSourceFile(QStringLiteral("src/linemanager.cpp"));
    const int reset = source.indexOf(QStringLiteral("void LineManager::resetError()"));
    const int nextFunction = source.indexOf(QStringLiteral("void LineManager::reportShortage"), reset);
    QVERIFY(reset >= 0);
    QVERIFY(nextFunction > reset);

    const QString body = source.mid(reset, nextFunction - reset);
    QVERIFY(body.contains(QStringLiteral("setState(LineSystemState::Idle, QStringLiteral(\"未启动\"));")));
    QVERIFY(!body.contains(QStringLiteral("tryStartNext()")));
}

QTEST_MAIN(ShortageTaskLifecycleTest)
#include "test_shortage_task_lifecycle.moc"
