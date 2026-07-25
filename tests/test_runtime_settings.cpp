#include <QtTest/QtTest>

#include <limits>

#include "runtimesettings.h"

class RuntimeSettingsTest : public QObject
{
    Q_OBJECT

private slots:
    void defaultsMatchApprovedSpec()
    {
        const RuntimeSettings settings = RuntimeSettings::defaults();

        QCOMPARE(settings.pickup.largeBasketGrabZClearanceMm, 417.0);
        QCOMPARE(settings.pickup.purpleBasketGrabZClearanceMm, 380.0);
        QVERIFY(settings.depthDescent.enabled);
        QCOMPARE(settings.depthDescent.triggerDepthMm, 1200.0);
        QCOMPARE(settings.depthDescent.stepMm, 200.0);
        QCOMPARE(settings.depthDescent.maxAccumulatedMm, 400.0);
    }

    void rejectsDepthStepGreaterThanMaximum()
    {
        RuntimeSettings settings = RuntimeSettings::defaults();
        settings.depthDescent.stepMm = 500.0;

        const SettingsValidation result = validateRuntimeSettings(settings);

        QVERIFY(!result.ok);
        QVERIFY(result.errors.join('\n').contains(QStringLiteral("单次下探")));
    }

    void rejectsNonPositiveTimeout()
    {
        RuntimeSettings settings = RuntimeSettings::defaults();
        settings.safety.normalMotionTimeoutMs = 0;

        const SettingsValidation result = validateRuntimeSettings(settings);

        QVERIFY(!result.ok);
        QVERIFY(result.errors.join('\n').contains(QStringLiteral("普通运动超时")));
    }

    void rejectsNonFiniteValues()
    {
        RuntimeSettings settings = RuntimeSettings::defaults();
        settings.vision.xyToleranceMm = std::numeric_limits<double>::quiet_NaN();

        QVERIFY(!validateRuntimeSettings(settings).ok);
    }

    void validatesFineCorrectionCountRange()
    {
        RuntimeSettings settings = RuntimeSettings::defaults();

        settings.vision.maxFineCorrectionCount = 0;
        QVERIFY(validateRuntimeSettings(settings).ok);

        settings.vision.maxFineCorrectionCount = 2;
        QVERIFY(validateRuntimeSettings(settings).ok);

        settings.vision.maxFineCorrectionCount = -1;
        SettingsValidation result = validateRuntimeSettings(settings);
        QVERIFY(!result.ok);
        QVERIFY(result.errors.join('\n').contains(QStringLiteral("联合精修正次数")));

        settings.vision.maxFineCorrectionCount = 3;
        result = validateRuntimeSettings(settings);
        QVERIFY(!result.ok);
        QVERIFY(result.errors.join('\n').contains(QStringLiteral("联合精修正次数")));
    }

    void restoresOnlyRequestedCategory()
    {
        RuntimeSettings settings = RuntimeSettings::defaults();
        settings.pickup.largeBasketGrabZClearanceMm = 430.0;
        settings.depthDescent.triggerDepthMm = 1350.0;

        const RuntimeSettings restored =
            restoreCategoryDefaults(settings, SettingsCategory::Pickup);

        QCOMPARE(restored.pickup.largeBasketGrabZClearanceMm, 417.0);
        QCOMPARE(restored.depthDescent.triggerDepthMm, 1350.0);
    }

    void depthPolicyDoesNotMoveAtThreshold()
    {
        const auto config = RuntimeSettings::defaults().depthDescent;

        const DepthDescentDecision decision =
            decideDepthDescent(1200.0, 0.0, config);

        QCOMPARE(decision.action, DepthDescentDecision::Action::ContinuePickup);
        QCOMPARE(decision.moveMm, 0.0);
    }

    void depthPolicyMovesOneStep()
    {
        const auto config = RuntimeSettings::defaults().depthDescent;

        const DepthDescentDecision decision =
            decideDepthDescent(1200.1, 0.0, config);

        QCOMPARE(decision.action, DepthDescentDecision::Action::MoveDown);
        QCOMPARE(decision.moveMm, 200.0);
    }

    void depthPolicyClampsToRemainingBudget()
    {
        RuntimeSettings::DepthDescent config = RuntimeSettings::defaults().depthDescent;
        config.stepMm = 180.0;
        config.maxAccumulatedMm = 400.0;

        const DepthDescentDecision decision =
            decideDepthDescent(1500.0, 350.0, config);

        QCOMPARE(decision.action, DepthDescentDecision::Action::MoveDown);
        QCOMPARE(decision.moveMm, 50.0);
    }

    void depthPolicyFailsWhenBudgetIsExhausted()
    {
        const auto config = RuntimeSettings::defaults().depthDescent;

        const DepthDescentDecision decision =
            decideDepthDescent(1500.0, 400.0, config);

        QCOMPARE(decision.action, DepthDescentDecision::Action::FailLimitReached);
        QCOMPARE(decision.moveMm, 0.0);
    }
};

QTEST_APPLESS_MAIN(RuntimeSettingsTest)
#include "test_runtime_settings.moc"
