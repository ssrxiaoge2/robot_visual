#include <QFile>

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

QString readUtf8File(const QString &path)
{
    QFile file(path);
    requireTrue(file.open(QIODevice::ReadOnly | QIODevice::Text),
                qPrintable(QStringLiteral("必须能读取 %1").arg(path)));
    return QString::fromUtf8(file.readAll());
}

int countOccurrences(const QString &haystack, const QString &needle)
{
    int count = 0;
    int pos = 0;
    while ((pos = haystack.indexOf(needle, pos)) >= 0) {
        ++count;
        pos += needle.size();
    }
    return count;
}

} // namespace

int main()
{
    const QString dialogHeader =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/palletparamdialog.h"));
    const QString dialogSource =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/palletparamdialog.cpp"));
    const QString mainWindowHeader =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/mainwindow.h"));
    const QString mainWindowSource =
        readUtf8File(QStringLiteral(PROJECT_SOURCE_DIR "/src/mainwindow.cpp"));

    requireTrue(dialogHeader.contains(
                    QStringLiteral("explicit PalletParamDialog(PalletScheduler *scheduler,\n"
                                   "                               HuayanScheduler *arm,\n"
                                   "                               QWidget *parent = nullptr);")),
                "PalletParamDialog 构造函数必须接收 PalletScheduler* 和 HuayanScheduler*");
    requireTrue(!dialogHeader.contains(QStringLiteral("VisionHttpClient")),
                "PalletParamDialog 头文件不应再依赖 VisionHttpClient");
    requireTrue(dialogHeader.contains(QStringLiteral("void runSinglePalletPlace(PalletArea area);")),
                "PalletParamDialog 必须声明真实单步码垛入口");
    requireTrue(!dialogHeader.contains(QStringLiteral("simulatePlaced")),
                "配置页不应保留绕过机械臂完成信号的模拟提交入口");
    requireTrue(dialogHeader.contains(QStringLiteral("void closeEvent(QCloseEvent *event) override;")),
                "PalletParamDialog 必须声明 closeEvent 关闭保护");
    requireTrue(dialogHeader.contains(QStringLiteral("bool m_debugRunning = false;")),
                "PalletParamDialog 必须跟踪调试运行态");
    requireTrue(dialogHeader.contains(QStringLiteral("HuayanScheduler *m_arm = nullptr;")),
                "PalletParamDialog 必须保存 HuayanScheduler 指针");
    requireTrue(!dialogHeader.contains(QStringLiteral("m_pendingRequests")),
                "PalletParamDialog 不应再保留视觉 pending request");
    requireTrue(dialogHeader.contains(
                    QStringLiteral("根据箱高写入目标层上方释放高度建议值；不影响相对 Z 偏移公式。")),
                "PalletParamDialog 头文件注释必须更新释放高度业务含义");

    requireTrue(dialogSource.contains(QStringLiteral("QStringLiteral(\"执行一次码垛\")")),
                "配置页必须提供执行一次码垛按钮");
    requireTrue(!dialogSource.contains(QStringLiteral("QStringLiteral(\"模拟放置完成\")")),
                "配置页不应提供会直接推进 placedCount 的模拟放置按钮");
    requireTrue(!dialogSource.contains(QStringLiteral("simulatePlaced")),
                "配置页源码不应保留 simulatePlaced 直接提交逻辑");
    requireTrue(dialogSource.contains(QStringLiteral("runSinglePalletPlace(area);")),
                "执行一次码垛按钮必须接入 runSinglePalletPlace");
    requireTrue(!dialogSource.contains(QStringLiteral("视觉检测")),
                "配置页源码不应再保留视觉检测分组");
    requireTrue(!dialogSource.contains(QStringLiteral("fetchPalletOccupancy")),
                "配置页不应再发起视觉占用请求");
    requireTrue(!dialogSource.contains(QStringLiteral("palletOccupancyReady")),
                "配置页不应再处理视觉占用成功回调");
    requireTrue(!dialogSource.contains(QStringLiteral("palletOccupancyError")),
                "配置页不应再处理视觉占用失败回调");
    requireTrue(!dialogSource.contains(QStringLiteral("detectArea(")),
                "配置页不应再保留 detectArea");
    requireTrue(dialogSource.contains(QStringLiteral("QStringLiteral(\"目标层上方释放高度:\")")),
                "配置页必须展示新的释放高度标签");
    requireTrue(!dialogSource.contains(QStringLiteral("QStringLiteral(\"释放高度参考:\")")),
                "配置页不应继续展示旧的释放高度参考标签");
    requireTrue(dialogSource.contains(
                    QStringLiteral("QStringLiteral(\"已按 1.2 倍箱高填入目标层上方释放高度建议值\")")),
                "释放高度建议提示语必须说明新的业务含义");
    requireTrue(dialogSource.contains(QStringLiteral("const PalletAreaTaskConfig *areaConfig = palletAreaConfig(area);")),
                "单步码垛必须读取 lineconfig 中的 PalletAreaTaskConfig");
    requireTrue(dialogSource.contains(QStringLiteral("m_arm->startPalletPlace(offset, cfg.releaseZOffset, cfg.robotBaseHeightFromGround);")),
                "单步码垛必须把目标层地面高度、释放高度和机器人基座离地高度传给 HuayanScheduler");
    requireTrue(dialogSource.contains(QStringLiteral("m_scheduler->commitPlaced(area, &error)")),
                "单步码垛完成后必须提交 placedCount");
    requireTrue(dialogSource.contains(QStringLiteral("HuayanScheduler::schedulerStopped")),
                "单步码垛停止收尾必须连接 HuayanScheduler::schedulerStopped");
    requireTrue(!dialogSource.contains(QStringLiteral("HuayanScheduler::logMessage")),
                "单步码垛停止收尾不应再依赖 HuayanScheduler::logMessage");
    requireTrue(!dialogSource.contains(QStringLiteral("msg != QStringLiteral(\"调度已停止\")")),
                "单步码垛停止收尾不应再解析固定日志文案");
    requireTrue(countOccurrences(dialogSource, QStringLiteral("if (!m_debugRunning)\n                return;")) >= 2,
                "完成/失败回调都必须先忽略非 debug run 的信号");
    requireTrue(countOccurrences(dialogSource,
                                 QStringLiteral("if (w && w->singlePlaceBtn)\n                w->singlePlaceBtn->setEnabled(true);")) >= 2,
                "完成/失败回调都必须恢复执行一次码垛按钮");
    requireTrue(dialogSource.contains(QStringLiteral("void PalletParamDialog::closeEvent(QCloseEvent *event)")),
                "PalletParamDialog 必须实现 closeEvent 关闭保护");
    requireTrue(dialogSource.contains(
                    QStringLiteral("QStringLiteral(\"真实单步码垛正在执行，请先在华研面板点击停止，并等待动作完成或停止提示后再关闭窗口。\")")),
                "关闭保护必须提示操作员走华研面板停止并等待完成");

    requireTrue(mainWindowHeader.contains(
                    QStringLiteral("打开空箱码垛配置窗口；窗口可执行真实单步码垛调试，停止统一使用华研面板停止按钮。")),
                "MainWindow 注释必须说明真实单步码垛和统一停止路径");
    requireTrue(mainWindowSource.contains(
                    QStringLiteral("m_devMgr ? m_devMgr->huayanScheduler() : nullptr")),
                "MainWindow 必须把 HuayanScheduler 传给 PalletParamDialog");
    requireTrue(!mainWindowSource.contains(
                    QStringLiteral("m_devMgr ? m_devMgr->visionClient() : nullptr")),
                "MainWindow 不应再把 VisionHttpClient 传给 PalletParamDialog");

    return 0;
}
