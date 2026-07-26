#include <QCoreApplication>
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
                        QStringLiteral("m_chargeClosePending"),
                        QStringLiteral("关闭流程必须有防重入状态"));
        requireContains(mainSource,
                        QStringLiteral("m_chargeCloseSafeConfirmed"),
                        QStringLiteral("只有已确认安全才能触发第二次关闭"));
        requireContains(mainSource,
                        QStringLiteral("applicationShutdownFinished"),
                        QStringLiteral("主窗口必须消费应用关闭最终结果"));
        requireContains(mainSource,
                        QStringLiteral("m_chargeShutdownResultConsumed"),
                        QStringLiteral("应用关闭结果必须只消费一次"));
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
                        QStringLiteral("const bool closePending = m_chargeClosePending;"),
                        QStringLiteral("控件刷新必须统一读取关闭处理中状态"));
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
                        QStringLiteral("m_forceChargeExitFirstConfirmed"),
                        QStringLiteral("必须记录第一次强制退出确认"));
        requireContains(mainSource,
                        QStringLiteral("m_forceChargeExitConfirmed"),
                        QStringLiteral("必须记录第二次强制退出确认"));
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

        // DeviceManager 只做所有权边界和转发。人工会话在不安全终态结束后，
        // “停止充电”必须可重试保守恢复，不能因没有活动 flow 退化为 no-op。
        requireContains(deviceHeader,
                        QStringLiteral("bool stopChargePile(QString *error"),
                        QStringLiteral("人工停止入口必须向 UI 返回接受/拒绝结果"));
        requireContains(deviceHeader,
                        QStringLiteral("void requestApplicationShutdown();"),
                        QStringLiteral("DeviceManager 必须提供关闭收尾入口"));
        requireContains(deviceHeader,
                        QStringLiteral("void applicationShutdownFinished(bool safe,"),
                        QStringLiteral("DeviceManager 必须转发关闭最终结果"));
        requireContains(deviceSource,
                        QStringLiteral("m_chargePileController->shutdownRequired()"),
                        QStringLiteral("人工停止必须识别无活动 flow 的不安全终态"));
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

        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        qCritical().noquote() << error.what();
        return 1;
    }
}
