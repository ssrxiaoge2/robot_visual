#include <QtTest/QtTest>

#include "visionalignmentdecision.h"

Q_DECLARE_METATYPE(VisionAlignment::WindowAction)

class VisionAlignmentDecisionTest : public QObject
{
    Q_OBJECT

private slots:
    void planarAlignmentHonorsInclusiveTolerances()
    {
        const VisionAlignment::WindowPolicy policy;

        // 防止把阈值边界误写成严格比较，导致恰好落在允许窗口内的目标被拒绝。
        QVERIFY(VisionAlignment::isPlanarAligned({2.0, -2.0, 100.0, 1.0, true}, policy));
        QVERIFY(!VisionAlignment::isPlanarAligned({2.01, 0.0, 100.0, 0.0, true}, policy));
        QVERIFY(!VisionAlignment::isPlanarAligned({0.0, 0.0, 100.0, 1.01, true}, policy));
    }

    void toolCorrectionUsesRobotToolDirectionConvention()
    {
        // 防止错误复用视觉坐标方向：工具坐标系中 X 同向，Y 与 Rz 均需取反。
        const auto correction = VisionAlignment::toToolCorrection(
            {10.0, 20.0, 100.0, 30.0, true});

        QCOMPARE(correction.xMm, 10.0);
        QCOMPARE(correction.yMm, -20.0);
        QCOMPARE(correction.rzDeg, -30.0);
    }

    void stableDepthRejectsIncompleteWindow()
    {
        const auto result = VisionAlignment::evaluateStableDepth(
            {943.1, 946.2, 947.9}, 5, 5.0);

        QVERIFY(!result.stable);
    }

    void stableDepthIgnoresOneHighAndOneLowOutlier()
    {
        // 对应现场失败日志中的真实模式：核心三帧在 4.9mm 内，但两端各有一个异常值。
        const auto result = VisionAlignment::evaluateStableDepth(
            {947.0, 943.5, 942.1, 934.4, 948.7}, 5, 5.0);

        QVERIFY(result.stable);
        QCOMPARE(result.filteredZMm, 943.5);
        QVERIFY(qAbs(result.coreRangeMm - 4.9) < 1e-9);
    }

    void stableDepthStillRejectsBroadCoreDistribution()
    {
        const auto result = VisionAlignment::evaluateStableDepth(
            {933.5, 939.6, 940.9, 948.2, 948.7}, 5, 5.0);

        QVERIFY(!result.stable);
        QVERIFY(result.coreRangeMm > 5.0);
    }

    void decisionWithinObservationWindow_data()
    {
        QTest::addColumn<qint64>("elapsedMs");
        QTest::addColumn<bool>("planarAligned");
        QTest::addColumn<bool>("zStable");
        QTest::addColumn<int>("completedFineCorrectionCount");
        QTest::addColumn<VisionAlignment::WindowAction>("expectedAction");

        QTest::newRow("四秒前继续观察")
            << 3999LL << true << true << 0
            << VisionAlignment::WindowAction::ContinueObserving;
        QTest::newRow("四秒后对准且Z稳定则下探")
            << 4000LL << true << true << 0
            << VisionAlignment::WindowAction::Descend;
        QTest::newRow("四秒后失准且有余量则精修")
            << 4000LL << false << true << 0
            << VisionAlignment::WindowAction::FineCorrect;
        QTest::newRow("精修次数耗尽则停止")
            << 4000LL << false << true << 1
            << VisionAlignment::WindowAction::Stop;
        QTest::newRow("八秒边界即使完全稳定也停止")
            << 8000LL << true << true << 0
            << VisionAlignment::WindowAction::Stop;
        QTest::newRow("超过八秒即使完全稳定也停止")
            << 8001LL << true << true << 0
            << VisionAlignment::WindowAction::Stop;
        QTest::newRow("八秒Z仍不稳定则停止")
            << 8000LL << true << false << 0
            << VisionAlignment::WindowAction::Stop;
    }

    void decisionWithinObservationWindow()
    {
        QFETCH(qint64, elapsedMs);
        QFETCH(bool, planarAligned);
        QFETCH(bool, zStable);
        QFETCH(int, completedFineCorrectionCount);
        QFETCH(VisionAlignment::WindowAction, expectedAction);

        VisionAlignment::WindowInput input;
        input.latest = planarAligned
            ? VisionAlignment::Sample{1.0, -1.0, 100.0, 0.5, true}
            : VisionAlignment::Sample{2.1, 0.0, 100.0, 0.0, true};
        input.zStable = zStable;
        input.elapsedMs = elapsedMs;
        input.completedFineCorrectionCount = completedFineCorrectionCount;

        const auto decision = VisionAlignment::decideWindow(
            input, VisionAlignment::WindowPolicy());

        QCOMPARE(decision.action, expectedAction);
    }

    void invalidTargetStopsImmediatelyWithReason()
    {
        // 防止观察窗口或Z稳定标志掩盖已丢失的锁定目标。
        VisionAlignment::WindowInput input;
        input.latest = {0.0, 0.0, 100.0, 0.0, false};
        input.zStable = true;
        input.elapsedMs = 0;

        const auto decision = VisionAlignment::decideWindow(
            input, VisionAlignment::WindowPolicy());

        QCOMPARE(decision.action, VisionAlignment::WindowAction::Stop);
        QVERIFY(!decision.reason.isEmpty());
    }
};

QTEST_MAIN(VisionAlignmentDecisionTest)
#include "test_vision_alignment_decision.moc"
