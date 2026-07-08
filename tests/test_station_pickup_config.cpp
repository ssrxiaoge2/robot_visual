#include "lineconfig.h"
#include "huayanScheduler.h"

#include <QFile>
#include <QtGlobal>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void requireTrue(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << std::endl;
        std::exit(1);
    }
}

void requireNear(double actual, double expected, double tolerance, const char *message)
{
    if (std::fabs(actual - expected) > tolerance) {
        std::cerr << message << " actual=" << actual << " expected=" << expected << std::endl;
        std::exit(1);
    }
}

} // namespace

int main()
{
    const StationTaskConfig *s12 = stationConfig(12);
    requireTrue(s12 != nullptr, "工位12配置必须存在");
    requireTrue(s12->captureFunc == QStringLiteral("Func_capture12"), "工位12拍照函数必须保持 Func_capture12");
    requireTrue(s12->unloadPointFunc == QStringLiteral("Func_daoliao12"), "工位12倒料点位函数必须保持 Func_daoliao12");
    requireTrue(s12->afterGripMode == AfterGripMode::CaptureFunc, "工位12夹紧后必须保持复用拍照函数回安全位");
    const StationTaskConfig *s3 = stationConfig(3);
    requireTrue(s3 != nullptr, "工位3配置必须存在");
    requireTrue(s3->stowAfterUnloadFunc == QStringLiteral("Func_yun_xing_zhong_s3"),
                "工位3倒料后收姿态必须使用带过渡点的新函数");

    const StationTaskConfig *s1 = stationConfig(1);
    const StationTaskConfig *s2 = stationConfig(2);
    const StationTaskConfig *s11 = stationConfig(11);
    requireTrue(s1 && s2 && s11, "工位1/2/11配置必须存在");
    requireTrue(s1->afterGripMode == AfterGripMode::None, "工位1夹紧后不回安全位");
    requireTrue(s2->afterGripMode == AfterGripMode::None, "工位2夹紧后不回安全位");
    requireTrue(s11->afterGripMode == AfterGripMode::None, "工位11夹紧后不回安全位");

    for (int station = 3; station <= 10; ++station) {
        const StationTaskConfig *cfg = stationConfig(station);
        requireTrue(cfg != nullptr, "工位3-10配置必须存在");
        requireTrue(cfg->afterGripMode == AfterGripMode::CaptureFunc, "工位3-10默认复用拍照函数回安全位");
    }

    for (int station = 1; station <= 11; ++station) {
        const StationTaskConfig *cfg = stationConfig(station);
        requireTrue(cfg != nullptr, "工位1-11配置必须存在");
        requireNear(cfg->grabZClearance, 417.0, 0.001, "工位1-11篮筐余量必须保持原生产值 417.0");
    }
    for (int station = 1; station <= 12; ++station) {
        const StationTaskConfig *cfg = stationConfig(station);
        requireTrue(cfg != nullptr, "12工位配置必须完整");
        requireTrue(!cfg->stowAfterUnloadFunc.isEmpty(),
                    "每个工位都必须显式配置倒料后收姿态函数");
        if (station != 3) {
            requireTrue(cfg->stowAfterUnloadFunc == QStringLiteral("Func_yun_xing_zhong"),
                        "非工位3默认使用原全局收姿态函数");
        }
    }
    requireTrue(s12->grabZClearance < 425.0, "工位12紫框余量应小于旧值以增加下探");

    requireNear(HuayanScheduler::calculateGrabDescend(798.1, 450.0, 1078.0), 348.1, 0.001,
                "Z 下探计算必须使用 visionZ - clearance");
    requireNear(HuayanScheduler::calculateGrabDescend(300.0, 450.0, 1078.0), 0.0, 0.001,
                "Z 下探计算不能返回负值");
    requireNear(HuayanScheduler::calculateGrabDescend(2000.0, 400.0, 1078.0), 1078.0, 0.001,
                "Z 下探计算必须受最大下探量限制");

    bool shouldRun = true;
    QString func = HuayanScheduler::resolveAfterGripFunction(AfterGripMode::None,
                                                             QStringLiteral("Func_capture12"),
                                                             QString(),
                                                             &shouldRun);
    requireTrue(!shouldRun, "None 策略不应调用夹后函数");
    requireTrue(func.isEmpty(), "None 策略返回函数名必须为空");

    func = HuayanScheduler::resolveAfterGripFunction(AfterGripMode::CaptureFunc,
                                                     QStringLiteral("Func_capture3"),
                                                     QString(),
                                                     &shouldRun);
    requireTrue(shouldRun, "CaptureFunc 策略应调用函数");
    requireTrue(func == QStringLiteral("Func_capture3"), "CaptureFunc 策略应返回拍照函数");

    func = HuayanScheduler::resolveAfterGripFunction(AfterGripMode::CustomFunc,
                                                     QStringLiteral("Func_capture7"),
                                                     QStringLiteral("Func_after_grip7"),
                                                     &shouldRun);
    requireTrue(shouldRun, "CustomFunc 策略应调用函数");
    requireTrue(func == QStringLiteral("Func_after_grip7"), "CustomFunc 策略应返回自定义夹后函数");

    QFile schedulerFile(QStringLiteral(PROJECT_SOURCE_DIR "/src/huayanScheduler.cpp"));
    requireTrue(schedulerFile.open(QIODevice::ReadOnly | QIODevice::Text),
                "必须能读取 huayanScheduler.cpp 进行运行路径静态回归检查");
    const QString schedulerSource = QString::fromUtf8(schedulerFile.readAll());
    QFile taskExecutorFile(QStringLiteral(PROJECT_SOURCE_DIR "/src/taskexecutor.cpp"));
    requireTrue(taskExecutorFile.open(QIODevice::ReadOnly | QIODevice::Text),
                "必须能读取 taskexecutor.cpp 进行调度日志静态回归检查");
    const QString taskExecutorSource = QString::fromUtf8(taskExecutorFile.readAll());
    QFile schedulerHeaderFile(QStringLiteral(PROJECT_SOURCE_DIR "/src/huayanScheduler.h"));
    requireTrue(schedulerHeaderFile.open(QIODevice::ReadOnly | QIODevice::Text),
                "必须能读取 huayanScheduler.h 进行延迟回调保护检查");
    const QString schedulerHeaderSource = QString::fromUtf8(schedulerHeaderFile.readAll());

    requireTrue(schedulerHeaderSource.contains(QStringLiteral("quint64 nextCallbackSeq();")),
                "huayanScheduler.h 必须声明 nextCallbackSeq()");
    requireTrue(schedulerHeaderSource.contains(QStringLiteral("enum class CommandReadiness")),
                "huayanScheduler.h 必须提供可测试的 CommandReadiness 判定枚举");
    requireTrue(schedulerHeaderSource.contains(QStringLiteral("static CommandReadiness evaluateCommandReadiness(")),
                "huayanScheduler.h 必须提供可测试的 evaluateCommandReadiness()");
    requireTrue(schedulerHeaderSource.contains(QStringLiteral("static bool canQueuePendingCommand(bool hasPendingCommand,")),
                "huayanScheduler.h 必须提供可测试的待命令占用判定 helper");
    requireTrue(schedulerSource.contains(QStringLiteral("quint64 HuayanScheduler::nextCallbackSeq()")),
                "huayanScheduler.cpp 必须定义 nextCallbackSeq()");
    requireTrue(schedulerSource.contains(QStringLiteral("return ++m_commandSeq;")),
                "nextCallbackSeq() 必须递增 m_commandSeq");
    requireTrue(schedulerSource.contains(QStringLiteral("return pollCommandReady();")),
                "beginCommandWhenReady() 必须把同步失败传回调用方");
    requireTrue(schedulerSource.contains(QStringLiteral("待下发命令仍未执行，拒绝覆盖")),
                "beginCommandWhenReady() 必须拒绝覆盖尚未下发的待命令");
    requireTrue(schedulerSource.contains(QStringLiteral("机械臂仍有命令执行中，拒绝插入新命令")),
                "beginCommandWhenReady() 必须拒绝在已有执行中命令时插入新命令");
    requireTrue(schedulerSource.contains(QStringLiteral("启动失败：机械臂仍有命令执行中")),
                "新阶段启动前必须拒绝清掉正在执行中的机械臂命令");
    requireTrue(schedulerSource.contains(QStringLiteral("if (m_stage == Stage::StageOne && m_stageStep == StageStep::SearchDescend)")),
                "搜索下移命令门控失败后，只能在阶段状态仍有效时回滚搜索计数");
    requireTrue(schedulerSource.contains(QStringLiteral("m_timeoutTimer->isActive() && m_stageStep != StageStep::WaitForVision")),
                "执行中命令判定不能把 WaitForVision 的超时定时器误算成机械臂命令");
    requireTrue(schedulerSource.contains(QStringLiteral("已收到视觉结果，停止 WaitForVision 超时定时器")),
                "视觉结果到达后必须停止 WaitForVision 的超时定时器");
    requireTrue(schedulerSource.contains(QStringLiteral("命令前读取 FSM 失败")),
                "pollCommandReady() 在 FSM 读取失败时必须显式报错而不是放行");
    requireTrue(schedulerSource.contains(QStringLiteral("PendingCommandKind::MoveJ")),
                "MoveJ 必须纳入统一待命令类型");
    requireTrue(schedulerSource.contains(QStringLiteral("cmd.kind = PendingCommandKind::MoveJ")),
                "executeMoveJ() 必须通过统一待命令门控下发 MoveJ");
    requireTrue(taskExecutorSource.contains(QStringLiteral("取料完成后无需回拍照安全高度")),
                "同 LM 工位日志必须区分是否真的回过拍照安全高度");
    requireTrue(taskExecutorSource.contains(QStringLiteral("isPickupCompletionState")),
                "TaskExecutor 必须集中判断阶段一完成状态");
    requireTrue(taskExecutorSource.contains(QStringLiteral("case ExecState::PreGripScanSearchReturn:")),
                "扫码搜索成功回原夹取位后，阶段一完成必须继续推进");
    requireTrue(taskExecutorSource.contains(QStringLiteral("if (isPickupCompletionState(m_state))")),
                "onArmStageCompleted 必须使用取料完成状态 helper");
    requireTrue(schedulerSource.contains(QStringLiteral("++m_commandSeq; // 让已经排队的 singleShot 回调全部失效")),
                "stop() 必须显式失效已排队的 singleShot 回调");
    requireTrue(schedulerSource.contains(QStringLiteral("nextCallbackSeq()")),
                "resetAndProceed() 必须在 singleShot 前生成新的回调序号");
    requireTrue(schedulerSource.contains(QStringLiteral("HRIF_GrpReset(m_boxID, m_rbtID)")),
                "resetAndProceed() 必须调用 GrpReset");
    requireTrue(schedulerSource.contains(QStringLiteral("resetAndProceed 调用 GrpReset 失败")),
                "resetAndProceed() 应记录 GrpReset 失败日志");
    requireTrue(schedulerSource.contains(QStringLiteral("const quint64 seq = nextCallbackSeq();\n            QTimer::singleShot(300, this, [this, seq] {")),
                "MoveToGrab/码垛 offset 的 300ms 回调必须使用 nextCallbackSeq()");
    requireTrue(schedulerSource.contains(QStringLiteral("const quint64 seq = nextCallbackSeq();\n            QTimer::singleShot(kVisionSettleMs, this, [this, seq] {")),
                "视觉稳定回调必须使用 nextCallbackSeq()");
    requireTrue(schedulerSource.contains(QStringLiteral("const quint64 seq = nextCallbackSeq();\n    QTimer::singleShot(1500, this, [this, seq] {")),
                "setGripper() 的 1500ms 回调必须使用 nextCallbackSeq()");
    requireTrue(schedulerSource.contains(QStringLiteral("calculateGrabDescend(m_grabOffset.z, m_grabZClearance, kMaxDescend)")),
                "DescendZ 必须使用按工位注入的 Z 余量");
    requireTrue(schedulerSource.contains(QStringLiteral("const QString afterGripFunc = resolveAfterGripFunction(")),
                "LiftLoad 必须按 AfterGripMode 解析夹后函数");
    requireTrue(schedulerSource.contains(QStringLiteral("夹紧后配置为不回安全位，直接完成取料阶段")),
                "LiftLoad 必须支持配置为不回安全位");
    requireTrue(!schedulerSource.contains(QStringLiteral("抬升（调用拍照位函数")),
                "LiftLoad 不应再保留固定复用拍照函数的旧日志");
    requireTrue(schedulerSource.contains(QStringLiteral("setNextStowFunction")),
                "HuayanScheduler 必须提供下一次收姿态函数选择接口");
    requireTrue(taskExecutorSource.contains(QStringLiteral("setNextStowFunction(m_stationCfg->stowAfterUnloadFunc)")),
                "TaskExecutor 必须在倒料后收姿态前注入当前工位函数");
    requireTrue(schedulerSource.contains(QStringLiteral("if (rejectStageStartWhileActionRunning(stageName(Stage::Stow))) {\n        m_nextStowFuncName.clear();\n        return;\n    }")),
                "startStow() 在已有动作运行而拒绝启动时必须清空一次性收姿态覆盖");
    requireTrue(schedulerSource.contains(QStringLiteral("if (!ensureConnected()) {\n        m_nextStowFuncName.clear();\n        return;\n    }")),
                "startStow() 在未连接而拒绝启动时必须清空一次性收姿态覆盖");
    requireTrue(!schedulerHeaderSource.contains(QStringLiteral("QString stowAfterUnloadFunc;")),
                "HuayanScheduler::StationArmFunctions 不应保留未使用的 stowAfterUnloadFunc 注入状态");

    requireTrue(HuayanScheduler::evaluateCommandReadiness(0, 0, 0, QStringLiteral("ProgramStopped"))
                    == HuayanScheduler::CommandReadiness::Wait,
                "ProgramStopped 时门控必须等待，不能直接放行");
    requireTrue(HuayanScheduler::evaluateCommandReadiness(0, 0, 0, QStringLiteral("RobotInMoving"))
                    == HuayanScheduler::CommandReadiness::Wait,
                "RobotInMoving 时门控必须等待，不能直接放行");
    requireTrue(HuayanScheduler::evaluateCommandReadiness(0, 0, -1, QStringLiteral("unknown"))
                    == HuayanScheduler::CommandReadiness::Error,
                "FSM 读取失败时门控必须 fail-closed");
    requireTrue(HuayanScheduler::evaluateCommandReadiness(0, 0, 0, QStringLiteral("Auto"))
                    == HuayanScheduler::CommandReadiness::ReadyToDispatch,
                "非错误、非暂停、非 moving 且非 ProgramStopped 时必须允许放行");
    requireTrue(HuayanScheduler::canQueuePendingCommand(false, false),
                "待命令槽位为空时必须允许登记新命令");
    requireTrue(!HuayanScheduler::canQueuePendingCommand(true, false),
                "已有待命令时必须拒绝覆盖，避免串台");
    requireTrue(!HuayanScheduler::canQueuePendingCommand(false, true),
                "已有执行中命令时必须拒绝插入新命令");

    return 0;
}
