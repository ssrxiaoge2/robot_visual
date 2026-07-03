#include <QtTest>

#include "taskqueue.h"

class LiveShortageBaselineTest : public QObject
{
    Q_OBJECT

private slots:
    void mixedSourcesStillKeepGlobalFifo();
};

void LiveShortageBaselineTest::mixedSourcesStillKeepGlobalFifo()
{
    TaskQueue queue;

    // 这个基线测试专门保护 e1ffb3f 主流程已经成立的 FIFO 语义：
    // 后续把真实缺料接入主调度时，即使队列里混入“模拟缺料”和“现场缺料”，
    // 也不能因为来源不同而改写全局先入先出顺序，否则会把既有主流程调乱。
    const Task first = queue.enqueue(8, TaskSource::UiMock);
    const Task second = queue.enqueue(3, TaskSource::CustomerSystem);
    const Task third = queue.enqueue(3, TaskSource::CustomerSystem);

    QVERIFY(queue.hasPending());

    const Task next1 = queue.takeNext();
    QCOMPARE(next1.taskId, first.taskId);
    QCOMPARE(next1.stationId, first.stationId);
    QCOMPARE(next1.source, first.source);

    const Task next2 = queue.takeNext();
    QCOMPARE(next2.taskId, second.taskId);
    QCOMPARE(next2.stationId, second.stationId);
    QCOMPARE(next2.source, second.source);

    const Task next3 = queue.takeNext();
    QCOMPARE(next3.taskId, third.taskId);
    QCOMPARE(next3.stationId, third.stationId);
    QCOMPARE(next3.source, third.source);

    QVERIFY(!queue.hasPending());
}

QTEST_MAIN(LiveShortageBaselineTest)

#include "test_live_shortage_baseline.moc"
