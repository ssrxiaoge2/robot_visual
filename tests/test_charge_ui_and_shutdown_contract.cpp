#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTextStream>

#include <stdexcept>
#include <cstdio>

namespace {

QString readUtf8File(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        throw std::runtime_error(
            QStringLiteral("无法读取契约文件：%1").arg(path).toStdString());
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    return stream.readAll();
}

void requireContains(const QString &source,
                     const QString &needle,
                     const QString &message)
{
    if (!source.contains(needle))
        throw std::runtime_error(message.toStdString());
}

void requireNotContains(const QString &source,
                        const QString &needle,
                        const QString &message)
{
    if (source.contains(needle))
        throw std::runtime_error(message.toStdString());
}

void requireBefore(const QString &source,
                   const QString &first,
                   const QString &second,
                   const QString &message)
{
    const qsizetype firstIndex = source.indexOf(first);
    const qsizetype secondIndex = source.indexOf(second, firstIndex + 1);
    if (firstIndex < 0 || secondIndex < 0 || firstIndex >= secondIndex)
        throw std::runtime_error(message.toStdString());
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    try {
        const QString root = QStringLiteral(PROJECT_SOURCE_DIR);
        const QString mainHeader =
            readUtf8File(root + QStringLiteral("/src/mainwindow.h"));
        const QString mainSource =
            readUtf8File(root + QStringLiteral("/src/mainwindow.cpp"));
        const QString deviceHeader =
            readUtf8File(root + QStringLiteral("/src/devicemanager.h"));
        const QString deviceSource =
            readUtf8File(root + QStringLiteral("/src/devicemanager.cpp"));
        const QString agvHeader =
            readUtf8File(root + QStringLiteral("/src/agvcontroller.h"));
        const QString shutdownPolicyHeader =
            readUtf8File(root + QStringLiteral("/src/chargeshutdownpolicy.h"));
        const QString shutdownPolicySource =
            readUtf8File(root + QStringLiteral("/src/chargeshutdownpolicy.cpp"));

        // 主窗口第一次收到关闭事件时必须先阻止析构，并把安全收尾交给
        // DeviceManager 所拥有的唯一控制器，不能只断开 TCP 或直接删除控制器。
        requireContains(mainHeader,
                        QStringLiteral("void closeEvent(QCloseEvent *event) override;"),
                        QStringLiteral("MainWindow 必须覆盖 closeEvent"));
        requireContains(mainSource,
                        QStringLiteral("void MainWindow::closeEvent(QCloseEvent *event)"),
                        QStringLiteral("MainWindow 必须实现 closeEvent"));
        requireContains(mainSource, QStringLiteral("event->ignore();"),
                        QStringLiteral("不安全关闭必须忽略首次关闭事件"));
        requireContains(mainSource,
                        QStringLiteral("m_devMgr->requestApplicationShutdown();"),
                        QStringLiteral("关闭必须进入 DeviceManager 的唯一安全收尾入口"));
        requireContains(mainSource,
                        QStringLiteral("m_chargeShutdownPolicy"),
                        QStringLiteral("关闭流程必须使用显式代次策略防重入"));
        requireContains(mainSource,
                        QStringLiteral("canRunQueuedClose("),
                        QStringLiteral("排队关闭必须实时复核安全状态"));
        requireContains(mainSource,
                        QStringLiteral("applicationShutdownFinished"),
                        QStringLiteral("主窗口必须消费应用关闭最终结果"));
        requireContains(mainSource,
                        QStringLiteral("onApplicationShutdownFinished("),
                        QStringLiteral("应用关闭结果必须交给当前代次消费"));
        requireContains(mainSource,
                        QStringLiteral("QTimer::singleShot"),
                        QStringLiteral("安全完成后应排队二次关闭以避免同步重入"));
        requireContains(mainSource,
                        QStringLiteral("正在安全停止"),
                        QStringLiteral("关闭处理中必须给操作员明确提示"));
        requireContains(mainSource,
                        QStringLiteral("状态未知，必须人工检查"),
                        QStringLiteral("安全收尾失败必须保持状态未知提示"));

        // 关闭处理中，所有可能发起新查询、开始或改参的入口必须被统一锁住。
        requireContains(mainSource,
                        QStringLiteral("chargeApplicationShutdownInProgress()"),
                        QStringLiteral("控件刷新必须读取 DeviceManager 关闭冻结门禁"));
        requireContains(mainSource,
                        QStringLiteral("!closePending && !parametersLocked"),
                        QStringLiteral("关闭处理中必须锁定参数入口"));
        requireContains(mainSource,
                        QStringLiteral("!closePending && !busy"),
                        QStringLiteral("关闭处理中必须锁定只读查询入口"));
        requireContains(mainSource,
                        QStringLiteral("!closePending && lineIdle"),
                        QStringLiteral("关闭处理中必须锁定手动开始入口"));

        // 强制退出只在安全收尾已经失败后开放，并要求连续两次明确确认。
        requireContains(mainSource,
                        QStringLiteral("停止结果未知"),
                        QStringLiteral("第一次强制退出确认必须说明停止结果未知"));
        requireContains(mainSource,
                        QStringLiteral("退出程序不代表已经停止或缩回"),
                        QStringLiteral("第二次确认必须重复说明退出不代表安全"));
        requireContains(mainSource,
                        QStringLiteral("CloseAction::OfferForceExit"),
                        QStringLiteral("强退只能由失败关闭代次策略授予"));
        requireContains(mainSource,
                        QStringLiteral("[充电强制退出][确认前快照]"),
                        QStringLiteral("确认前必须写结构化安全快照日志"));
        for (const QString &field : {
                 QStringLiteral("电压="),
                 QStringLiteral("电流="),
                 QStringLiteral("伸缩状态="),
                 QStringLiteral("事件="),
                 QStringLiteral("故障=")}) {
            requireContains(mainSource, field,
                            QStringLiteral("强制退出日志缺少字段：%1").arg(field));
        }
        requireContains(mainSource,
                        QStringLiteral("[充电强制退出][人工选择]"),
                        QStringLiteral("确认后必须记录操作人员选择"));

        requireNotContains(
            mainSource,
            QStringLiteral("delete m_devMgr->chargePileController"),
            QStringLiteral("普通关闭路径不得直接删除唯一控制器"));
        requireNotContains(
            mainSource,
            QStringLiteral("chargePileController()->disconnectFromHost"),
            QStringLiteral("普通关闭路径不得只断开充电桩 TCP"));
        requireNotContains(mainHeader, QStringLiteral("DO0"),
                           QStringLiteral("DO0不得增加主窗口接口"));
        requireNotContains(mainSource, QStringLiteral("DO0"),
                           QStringLiteral("DO0不得增加主窗口控件或逻辑"));

        // DeviceManager 只做所有权边界和转发。人工会话在不安全终态结束后，
        // “停止充电”必须可重试保守恢复，不能因没有活动 flow 退化为 no-op。
        requireContains(deviceHeader,
                        QStringLiteral("bool stopChargePile(QString *error"),
                        QStringLiteral("人工停止入口必须向 UI 返回接受/拒绝结果"));
        requireContains(deviceHeader,
                        QStringLiteral("void requestApplicationShutdown();"),
                        QStringLiteral("DeviceManager 必须提供关闭收尾入口"));
        requireContains(deviceHeader,
                        QStringLiteral("chargeApplicationShutdownInProgress()"),
                        QStringLiteral("DeviceManager 必须公开只读关闭冻结状态"));
        requireContains(deviceHeader,
                        QStringLiteral("void applicationShutdownFinished(bool safe,"),
                        QStringLiteral("DeviceManager 必须转发关闭最终结果"));
        requireContains(deviceSource,
                        QStringLiteral("m_chargePileController->shutdownRequired()"),
                        QStringLiteral("人工停止必须识别无活动 flow 的不安全终态"));
        requireContains(deviceSource,
                        QStringLiteral("selectStopRecoveryOrigin(automaticSessionActive)"),
                        QStringLiteral("自动会话的人工恢复必须保留 Automatic 来源"));
        requireContains(deviceSource,
                        QStringLiteral("只读查询或其他非充电操作正在执行，不能启动安全恢复"),
                        QStringLiteral("无活动充电 flow 的 busy 状态必须明确拒绝恢复"));
        requireContains(
            deviceSource,
            QStringLiteral("requestConservativeRecovery("),
            QStringLiteral("人工停止必须进入唯一控制器的 Manual 保守恢复"));
        requireContains(
            deviceSource,
            QStringLiteral("ChargePileController::SessionOrigin::Manual"),
            QStringLiteral("人工保守恢复必须保留 Manual 会话来源"));
        requireContains(deviceSource,
                        QStringLiteral("m_chargePileController->requestApplicationShutdown();"),
                        QStringLiteral("DeviceManager 关闭入口必须转发到唯一控制器"));
        requireContains(deviceSource,
                        QStringLiteral("m_chargeApplicationShutdownGate.beginRequest();"),
                        QStringLiteral("关闭必须先冻结所有新充电动作"));
        for (const QString &blockedAction : {
                 QStringLiteral("不能修改充电参数"),
                 QStringLiteral("不能开始新会话"),
                 QStringLiteral("不能开始状态查询"),
                 QStringLiteral("不能改变自动充电授权")}) {
            requireContains(
                deviceSource, blockedAction,
                QStringLiteral("应用关闭门禁缺少业务拒绝：%1").arg(blockedAction));
        }
        requireContains(deviceSource,
                        QStringLiteral("m_autoChargeCoordinator->setEnabled(false);"),
                        QStringLiteral("应用关闭必须先撤销自动充电授权"));
        requireBefore(
            deviceSource,
            QStringLiteral("m_chargeApplicationShutdownGate.beginRequest();"),
            QStringLiteral("m_autoChargeCoordinator->setEnabled(false);"),
            QStringLiteral("关闭冻结必须先于撤销自动授权"));
        requireBefore(
            deviceSource,
            QStringLiteral("m_autoChargeCoordinator->setEnabled(false);"),
            QStringLiteral("m_chargePileController->requestApplicationShutdown();"),
            QStringLiteral("撤销自动授权必须先于控制器关闭收尾"));
        requireContains(shutdownPolicyHeader,
                        QStringLiteral("quint64 m_generation"),
                        QStringLiteral("强退资格必须绑定明确关闭代次"));
        requireContains(shutdownPolicySource,
                        QStringLiteral("ControllerOperationInProgress"),
                        QStringLiteral("新控制器操作必须清除旧强退资格"));
        requireNotContains(
            mainSource,
            QStringLiteral("m_chargeCloseSafeConfirmed"),
            QStringLiteral("历史 safe 布尔值不得绕过实时 shutdownRequired 复核"));

        // 单个DO0只允许在既有AgvController和DeviceManager边界内接入，不得
        // 新增跨设备协调器或让手动/自动入口形成两套启动顺序。
        requireContains(agvHeader,
                        QStringLiteral("bool ensureDo0(bool high"),
                        QStringLiteral("DO0写入和确认必须由AgvController拥有"));
        requireContains(deviceSource,
                        QStringLiteral("requestChargeStartWithDo0"),
                        QStringLiteral("手动和自动开始必须汇入同一DO0入口"));
        requireBefore(deviceSource,
                      QStringLiteral("ensureDo0(true"),
                      QStringLiteral("m_chargePileController->startCharge("),
                      QStringLiteral("DO0确认必须发生在充电桩启动之前"));
        requireContains(deviceSource, QStringLiteral("setInterval(30000)"),
                        QStringLiteral("充电期间DO0监控周期必须为30秒"));

        // 手动开始、自动授权、主调度自动开始必须汇入 DeviceManager 的同一异步预检，
        // MainWindow 只显示结果，不能持有预检意图或越过预检直接控制 DO0。
        requireContains(deviceHeader,
                        QStringLiteral("enum class ChargePreflightIntent"),
                        QStringLiteral("DeviceManager 必须持有统一的充电预检意图"));
        for (const QString &intent : {
                 QStringLiteral("ChargePreflightIntent::ManualStart"),
                 QStringLiteral("ChargePreflightIntent::EnableAutomatic"),
                 QStringLiteral("ChargePreflightIntent::AutomaticStart")}) {
            requireContains(deviceSource, intent,
                            QStringLiteral("缺少充电预检入口：%1").arg(intent));
        }
        requireContains(deviceSource,
                        QStringLiteral("handleChargePreflightFinished"),
                        QStringLiteral("查询结果必须由统一预检完成处理器消费"));
        requireContains(deviceSource,
                        QStringLiteral("raiseExternalSystemError"),
                        QStringLiteral("自动开始的设备安全失败必须交给主调度 Error"));
        requireContains(mainSource,
                        QStringLiteral("manualChargePreflightFinished"),
                        QStringLiteral("主窗口必须异步显示手动预检结果"));
        requireNotContains(mainHeader,
                           QStringLiteral("ChargePreflightIntent"),
                           QStringLiteral("主窗口不得持有充电预检意图"));
        requireBefore(deviceSource,
                      QStringLiteral("beginChargePreflight"),
                      QStringLiteral("ensureDo0(true"),
                      QStringLiteral("充电桩只读预检必须先于 DO0 置高"));

        const QDir sourceDir(root + QStringLiteral("/src"));
        const QStringList unexpectedCoordinators =
            sourceDir.entryList(
                {QStringLiteral("*charge*do0*coordinator*"),
                 QStringLiteral("*do0*charge*coordinator*")},
                QDir::Files);
        if (!unexpectedCoordinators.isEmpty()) {
            throw std::runtime_error(
                QStringLiteral("DO0不得新增生产协调器：%1")
                    .arg(unexpectedCoordinators.join(QStringLiteral(", ")))
                    .toStdString());
        }

        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        qCritical().noquote() << error.what();
        return 1;
    }
}
