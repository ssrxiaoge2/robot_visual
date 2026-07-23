#ifndef RUNTIMESETTINGS_H
#define RUNTIMESETTINGS_H

#include <QStringList>

enum class SettingsCategory {
    Pickup,
    VisionClosedLoop,
    DepthDescent,
    Search,
    Motion,
    SafetyAndTimeouts
};

struct RuntimeSettings
{
    struct Pickup {
        double largeBasketGrabZClearanceMm = 417.0;
        double purpleBasketGrabZClearanceMm = 380.0;
        double grabXCompensationMm = 30.0;
        double grabYCompensationMm = -17.0;
        bool zDescendInvert = false;
    } pickup;

    struct VisionClosedLoop {
        double xyToleranceMm = 2.0;
        double rzToleranceDeg = 1.0;
        int maxGrabIterations = 15;
        int settleMs = 2000;
        double largeRzJumpThresholdDeg = 80.0;
        double largeRzDeltaToleranceDeg = 15.0;
        int maxLargeRzExecutions = 1;
        double stationRoiHalfXmm = 500.0;
        double stationRoiHalfYmm = 500.0;
        double anchorMaxTrustXmm = 300.0;
        double anchorMaxTrustYmm = 450.0;
        double anchorSameLayerToleranceMm = 20.0;
        double anchorSwitchMaxXyMm = 220.0;
        int lockMaxMissingFrames = 3;
        double lockTrackRadiusMm = 260.0;
        double lockSameLayerToleranceMm = 80.0;
    } vision;

    struct DepthDescent {
        bool enabled = true;
        double triggerDepthMm = 1200.0;
        double stepMm = 200.0;
        double maxAccumulatedMm = 400.0;
    } depthDescent;

    struct Search {
        double descendStepMm = 20.0;
        double maxAccumulatedMm = 80.0;
        int settleMs = 2000;
    } search;

    struct Motion {
        int speedPercent = 100;
        double velocity = 50.0;
        double acceleration = 100.0;
        double radius = 0.0;
        double scanRecoveryRotationDeg = 180.0;
    } motion;

    struct Safety {
        double maxSingleXyAdjustMm = 250.0;
        double maxZDescendMm = 1078.0;
        int normalMotionTimeoutMs = 30000;
        int longZMotionTimeoutMs = 120000;
        int commandReadyTimeoutMs = 8000;
        int resetSettleMs = 1000;
        int pollIntervalMs = 100;
        int shortMotionFallbackMs = 3000;
    } safety;

    static RuntimeSettings defaults();
};

struct SettingsValidation
{
    bool ok = false;
    QStringList errors;
};

struct DepthDescentDecision
{
    enum class Action {
        ContinuePickup,
        MoveDown,
        FailLimitReached
    } action = Action::ContinuePickup;

    double moveMm = 0.0;
};

SettingsValidation validateRuntimeSettings(const RuntimeSettings &settings);
RuntimeSettings restoreCategoryDefaults(const RuntimeSettings &current,
                                        SettingsCategory category);
DepthDescentDecision decideDepthDescent(
    double depthMm,
    double accumulatedMm,
    const RuntimeSettings::DepthDescent &settings);

#endif // RUNTIMESETTINGS_H
