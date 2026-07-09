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
        target.z = 216.0;
        target.rz = 0.0;

        const QList<PalletPlaceStep> steps = buildPalletPlaceSequence(target, 35.0);

        QCOMPARE(steps.size(), 7);
        QCOMPARE(steps.at(0).kind, PalletPlaceStepKind::ClampAtSafety);
        QCOMPARE(steps.at(1).kind, PalletPlaceStepKind::RunPalletBaseFunction);
        QCOMPARE(steps.at(2).kind, PalletPlaceStepKind::MoveXYAboveTarget);
        QCOMPARE(steps.at(2).offset.x, 120.0);
        QCOMPARE(steps.at(2).offset.y, -80.0);
        QCOMPARE(steps.at(2).offset.z, 0.0);
        QCOMPARE(steps.at(3).kind, PalletPlaceStepKind::DescendToReleaseHeight);
        QCOMPARE(steps.at(3).offset.z, 251.0);
        QCOMPARE(steps.at(4).kind, PalletPlaceStepKind::ReleaseGripper);
        QCOMPARE(steps.at(5).kind, PalletPlaceStepKind::LiftAfterRelease);
        QCOMPARE(steps.at(5).offset.z, -251.0);
        QCOMPARE(steps.at(6).kind, PalletPlaceStepKind::RunStowFunction);
    }

    void rejectsNegativeReleaseHeight()
    {
        PalletPose target;
        target.z = 10.0;
        const QList<PalletPlaceStep> steps = buildPalletPlaceSequence(target, -1.0);
        QVERIFY2(steps.isEmpty(), "释放高度不能为负数，规划必须失败关闭");
    }
};

QTEST_MAIN(PalletPlaceSequenceTest)
#include "test_pallet_place_sequence.moc"
