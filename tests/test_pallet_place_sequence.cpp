#include <QtTest/QtTest>

#include "palletplacesequence.h"

class PalletPlaceSequenceTest : public QObject
{
    Q_OBJECT

private slots:
    void buildsStandardSingleBoxSequence()
    {
        PalletPose target;
        target.x = 120.0;
        target.y = -80.0;
        target.z = 150.0;
        target.rz = 0.0;

        const QList<PalletPlaceStep> steps =
            buildPalletPlaceSequence(target, 50.0, 850.0, 500.0);

        QCOMPARE(steps.size(), 7);
        QCOMPARE(steps.at(0).kind, PalletPlaceStepKind::ClampAtSafety);
        QCOMPARE(steps.at(1).kind, PalletPlaceStepKind::RunPalletBaseFunction);
        QCOMPARE(steps.at(2).kind, PalletPlaceStepKind::MoveXYAboveTarget);
        QCOMPARE(steps.at(2).offset.x, 120.0);
        QCOMPARE(steps.at(2).offset.y, -80.0);
        QCOMPARE(steps.at(2).offset.z, 0.0);
        QCOMPARE(steps.at(3).kind, PalletPlaceStepKind::DescendToReleaseHeight);
        QCOMPARE(steps.at(3).offset.z, -730.0);
        QCOMPARE(steps.at(4).kind, PalletPlaceStepKind::ReleaseGripper);
        QCOMPARE(steps.at(5).kind, PalletPlaceStepKind::LiftAfterRelease);
        QCOMPARE(steps.at(5).offset.z, 730.0);
        QCOMPARE(steps.at(6).kind, PalletPlaceStepKind::RunStowFunction);
    }

    void rejectsNegativeReleaseHeight()
    {
        PalletPose target;
        target.z = 10.0;
        const QList<PalletPlaceStep> steps = buildPalletPlaceSequence(target, -1.0, 850.0, 500.0);
        QVERIFY2(steps.isEmpty(), "释放高度不能为负数，规划必须失败关闭");
    }

    void rejectsReleaseTcpHigherThanPalletBaseTcp()
    {
        PalletPose target;
        target.z = 600.0;

        QString error;
        const QList<PalletPlaceStep> steps =
            buildPalletPlaceSequence(target, 40.0, 850.0, 180.0, &error);

        QVERIFY2(steps.isEmpty(), "释放目标 TCP Z 高于码垛初始点位时必须拒绝，避免向奇异点抬升");
        QVERIFY(error.contains(QStringLiteral("高于码垛初始点位")));
    }
};

QTEST_MAIN(PalletPlaceSequenceTest)
#include "test_pallet_place_sequence.moc"
