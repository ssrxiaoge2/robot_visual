#include <QtTest/QtTest>

#include "huayanScheduler.h"

class VisionAlignmentSafetyTest : public QObject
{
    Q_OBJECT

private slots:
    void largeRzConfirmationRequiresTwoStrictlyNewFrames()
    {
        HuayanScheduler::LargeRzConfirmationState state;

        const auto first = HuayanScheduler::evaluateLargeRzConfirmation(
            state, 91.0, 100, 1000, 80.0, 15.0, 0, 1);
        QCOMPARE(first.action,
                 HuayanScheduler::LargeRzAction::WaitForNewFrame);
        QVERIFY(first.nextState.pending);
        QCOMPARE(first.nextState.frameId, qint64(100));
        QCOMPARE(first.nextState.timestampMs, qint64(1000));

        // 同一个 HTTP 缓存帧即使重复返回相同角度，也绝不能构成第二帧确认。
        const auto duplicate = HuayanScheduler::evaluateLargeRzConfirmation(
            first.nextState, 92.0, 100, 1000, 80.0, 15.0, 0, 1);
        QCOMPARE(duplicate.action,
                 HuayanScheduler::LargeRzAction::WaitForNewFrame);
        QCOMPARE(duplicate.nextState.frameId, qint64(100));

        const auto confirmed = HuayanScheduler::evaluateLargeRzConfirmation(
            duplicate.nextState, 92.0, 101, 1001, 80.0, 15.0, 0, 1);
        QCOMPARE(confirmed.action,
                 HuayanScheduler::LargeRzAction::AllowMotion);
        QCOMPARE(confirmed.measuredRzDeg, 92.0);
        QCOMPARE(confirmed.motionRzDeg, 92.0);
        QVERIFY(!confirmed.nextState.pending);
    }

    void oppositeOrDifferentNewFrameResetsCandidate()
    {
        HuayanScheduler::LargeRzConfirmationState state;
        state.pending = true;
        state.rzDeg = 90.0;
        state.frameId = 10;
        state.timestampMs = 500;

        const auto opposite = HuayanScheduler::evaluateLargeRzConfirmation(
            state, -91.0, 11, 501, 80.0, 15.0, 0, 1);
        QCOMPARE(opposite.action,
                 HuayanScheduler::LargeRzAction::WaitForNewFrame);
        QCOMPARE(opposite.nextState.rzDeg, -91.0);
        QCOMPARE(opposite.nextState.frameId, qint64(11));

        const auto differentMagnitude =
            HuayanScheduler::evaluateLargeRzConfirmation(
                opposite.nextState, -120.0, 12, 502,
                80.0, 15.0, 0, 1);
        QCOMPARE(differentMagnitude.action,
                 HuayanScheduler::LargeRzAction::WaitForNewFrame);
        QCOMPARE(differentMagnitude.nextState.rzDeg, -120.0);
        QCOMPARE(differentMagnitude.nextState.frameId, qint64(12));
    }

    void executionLimitKeepsMeasuredResidualAndBlocksMotion()
    {
        const auto limited = HuayanScheduler::evaluateLargeRzConfirmation(
            {}, 93.0, 20, 600, 80.0, 15.0, 1, 1);

        QCOMPARE(limited.action,
                 HuayanScheduler::LargeRzAction::ExecutionLimitReached);
        QCOMPARE(limited.measuredRzDeg, 93.0);
        QCOMPARE(limited.motionRzDeg, 0.0);
        QVERIFY(limited.blocksDescent);

        VisionAlignment::WindowPolicy policy;
        policy.xyToleranceMm = 5.0;
        policy.rzToleranceDeg = 5.0;
        policy.maxFineCorrectionCount = 1;
        policy.minElapsedMs = 4000;
        policy.maxElapsedMs = 8000;

        VisionAlignment::WindowInput input;
        input.latest = {0.0, 0.0, 500.0, limited.measuredRzDeg, true};
        input.zStable = true;
        input.elapsedMs = 4000;
        input.completedFineCorrectionCount = 0;
        const auto window = VisionAlignment::decideWindow(input, policy);
        QVERIFY(window.action != VisionAlignment::WindowAction::Descend);
    }

    void visualMotionNeedsOfficialOrActualPoseEvidence()
    {
        HuayanScheduler::VisionMotionCompletionInput idleWithResidualDone;
        idleWithResidualDone.moving = false;
        idleWithResidualDone.blendingQuerySucceeded = true;
        idleWithResidualDone.blendingDone = true;
        idleWithResidualDone.elapsedMs = 3000;
        idleWithResidualDone.shortMotionFallbackMs = 3000;
        QCOMPARE(HuayanScheduler::evaluateVisionMotionCompletion(
                     idleWithResidualDone),
                 HuayanScheduler::VisionMotionCompletion::Wait);

        auto actualPoseReached = idleWithResidualDone;
        actualPoseReached.actualPoseAtTarget = true;
        QCOMPARE(HuayanScheduler::evaluateVisionMotionCompletion(
                     actualPoseReached),
                 HuayanScheduler::VisionMotionCompletion::Complete);

        auto seenMoving = idleWithResidualDone;
        seenMoving.hasSeenMoving = true;
        seenMoving.elapsedMs = 100;
        QCOMPARE(HuayanScheduler::evaluateVisionMotionCompletion(seenMoving),
                 HuayanScheduler::VisionMotionCompletion::Complete);

        auto officialTransition = idleWithResidualDone;
        officialTransition.hasSeenBlendingNotDone = true;
        officialTransition.elapsedMs = 100;
        QCOMPARE(HuayanScheduler::evaluateVisionMotionCompletion(
                     officialTransition),
                 HuayanScheduler::VisionMotionCompletion::Complete);

        auto stillIdleAwayFromTarget = idleWithResidualDone;
        stillIdleAwayFromTarget.elapsedMs = 6000;
        stillIdleAwayFromTarget.actualPoseAtTarget = false;
        QCOMPARE(HuayanScheduler::evaluateVisionMotionCompletion(
                     stillIdleAwayFromTarget),
                 HuayanScheduler::VisionMotionCompletion::Wait);
    }
};

QTEST_APPLESS_MAIN(VisionAlignmentSafetyTest)

#include "test_vision_alignment_safety.moc"
