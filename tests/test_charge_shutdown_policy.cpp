#include <QtTest>

#include "chargeshutdownpolicy.h"

class ChargeShutdownPolicyTest : public QObject
{
    Q_OBJECT

private slots:
    void closeInterceptionAllowsMissingBaselineWithoutChargeResponsibility()
    {
        ChargeCloseInterceptionContext context;
        context.controllerShutdownRequired = true;

        QVERIFY(!chargeCloseInterceptionRequired(context));

        context.automaticPolicyParticipatingInLine = true;
        QVERIFY(chargeCloseInterceptionRequired(context));
    }

    void closeInterceptionBlocksRealChargeResponsibilityAndUnsafeTerminal()
    {
        ChargeCloseInterceptionContext context;

        context.chargeSessionActive = true;
        QVERIFY(chargeCloseInterceptionRequired(context));

        context = ChargeCloseInterceptionContext{};
        context.automaticSessionActive = true;
        QVERIFY(chargeCloseInterceptionRequired(context));

        context = ChargeCloseInterceptionContext{};
        context.do0OpeningOrActive = true;
        QVERIFY(chargeCloseInterceptionRequired(context));

        context = ChargeCloseInterceptionContext{};
        context.applicationShutdownInProgress = true;
        QVERIFY(chargeCloseInterceptionRequired(context));

        context = ChargeCloseInterceptionContext{};
        context.controllerUnsafeTerminal = true;
        QVERIFY(chargeCloseInterceptionRequired(context));

        context = ChargeCloseInterceptionContext{};
        context.controllerUnsafeEvidence = true;
        QVERIFY(chargeCloseInterceptionRequired(context));
    }

    void closeInterceptionBlocksMissingDeviceManager()
    {
        ChargeCloseInterceptionContext context;
        context.deviceManagerAvailable = false;

        QVERIFY(chargeCloseInterceptionRequired(context));
    }

    void applicationGateFailureUnfreezesButSuccessStaysFrozen()
    {
        ChargeApplicationShutdownGate gate;
        QVERIFY(!gate.blocksNewActions());

        gate.beginRequest();
        QVERIFY(gate.blocksNewActions());
        gate.finishRequest(false);
        QVERIFY(!gate.blocksNewActions());

        gate.beginRequest();
        gate.finishRequest(true);
        QVERIFY(gate.blocksNewActions());
    }

    void failedGenerationLosesForceEligibilityWhenRecoveryStarts()
    {
        ChargeShutdownPolicy policy;
        QCOMPARE(policy.onCloseRequested(true, false),
                 ChargeShutdownPolicy::CloseAction::RequestApplicationShutdown);
        const quint64 failedGeneration = policy.generation();
        policy.onApplicationShutdownFinished(false, true);
        QVERIFY(policy.canOfferForceExit());

        policy.onControllerOperationStarted();
        QVERIFY(!policy.canOfferForceExit());
        QCOMPARE(policy.generation(), failedGeneration);
        policy.onControllerOperationFinished();
        QVERIFY(!policy.canOfferForceExit());

        // 人工恢复即使再次失败，也不能复用上一次应用关闭的强退资格；必须先
        // 发起一个新的应用关闭代次，由该代次自己的失败结果重新授予资格。
        QCOMPARE(policy.onCloseRequested(true, false),
                 ChargeShutdownPolicy::CloseAction::RequestApplicationShutdown);
        QVERIFY(policy.generation() > failedGeneration);
    }

    void closeDuringRecoveryRequestsTakeoverInsteadOfOfferingForceExit()
    {
        ChargeShutdownPolicy policy;
        QCOMPARE(policy.onCloseRequested(true, false),
                 ChargeShutdownPolicy::CloseAction::RequestApplicationShutdown);
        policy.onApplicationShutdownFinished(false, true);
        QVERIFY(policy.canOfferForceExit());

        policy.onControllerOperationStarted();
        QCOMPARE(policy.onCloseRequested(true, true),
                 ChargeShutdownPolicy::CloseAction::RequestApplicationShutdown);
        QVERIFY(!policy.canOfferForceExit());
        QCOMPARE(policy.phase(),
                 ChargeShutdownPolicy::Phase::ApplicationShutdownPending);
    }

    void queuedSafeCloseRechecksRealtimeShutdownRequirement()
    {
        ChargeShutdownPolicy policy;
        QCOMPARE(policy.onCloseRequested(true, false),
                 ChargeShutdownPolicy::CloseAction::RequestApplicationShutdown);
        policy.onApplicationShutdownFinished(true, false);
        QCOMPARE(policy.phase(), ChargeShutdownPolicy::Phase::SafeCloseQueued);

        // 排队回调执行前若又出现新的不安全上下文，历史 safe 结果立即失效。
        QVERIFY(!policy.canRunQueuedClose(true));
        QCOMPARE(policy.phase(), ChargeShutdownPolicy::Phase::Normal);
        QCOMPARE(policy.onCloseRequested(true, false),
                 ChargeShutdownPolicy::CloseAction::RequestApplicationShutdown);
        QCOMPARE(policy.generation(), quint64{2});
    }

    void automaticOwnershipSelectsAutomaticRecoveryOrigin()
    {
        QVERIFY(selectStopRecoveryOrigin(true)
                == ChargePileController::SessionOrigin::Automatic);
        QVERIFY(selectStopRecoveryOrigin(false)
                == ChargePileController::SessionOrigin::Manual);
    }
};

QTEST_APPLESS_MAIN(ChargeShutdownPolicyTest)
#include "test_charge_shutdown_policy.moc"
