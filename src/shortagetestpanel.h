#ifndef SHORTAGETESTPANEL_H
#define SHORTAGETESTPANEL_H

#include <QGroupBox>
#include <QHash>
#include <QList>

#include "deviceindicator.h"
#include "shortagecalculator.h"
#include "shortageconfig.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

/**
 * @brief 缺料信号计算测试面板。
 *
 * 该控件只负责展示测试会话状态，不包含任何主调度授权或 FIFO 派单逻辑。
 * “开始检测/停止检测”只控制 ShortageTestSession 的轮询生命周期。
 */
class ShortageTestPanel : public QGroupBox
{
    Q_OBJECT
public:
    explicit ShortageTestPanel(QWidget *parent = nullptr);

    /// 只更新按钮启停状态，不代表主调度是否允许接收现场缺料。
    void setRunning(bool running);

public slots:
    /// 更新总体状态和现场系统指示灯；错误信息只留在测试面板内。
    void setStatus(const QString &text, bool healthy);
    /// 刷新 MES/PLC 当前样本；单件用量只作为测试解释，不驱动任何派单。
    void setSample(qint64 actualQty,
                   qint64 delta,
                   ProductModel product,
                   ProductionMode mode,
                   QHash<QString, bool> bits);
    /// 刷新 12 工位测试库存快照；状态列只解释测试结果，不显示生产任务状态。
    void setInventory(QList<StationConsumption> stations);

signals:
    void startTestRequested();
    void stopTestRequested();

private:
    QString stationStatusText(const StationConsumption &station) const;
    QString usageSummaryText(ProductionMode mode) const;
    void initializeTable();

    QPushButton *m_shortageTestStartBtn = nullptr;  ///< 只请求开始测试轮询。
    QPushButton *m_shortageTestStopBtn = nullptr;   ///< 只请求停止测试轮询。
    DeviceIndicator *m_shortageTestIndicator = nullptr; ///< 现场系统通信状态指示灯。
    QLineEdit *m_shortageTestActualQtyEdit = nullptr;   ///< MES actualQty 当前值。
    QLabel *m_shortageTestDeltaLabel = nullptr;         ///< 与上一有效轮次相比的产量增量。
    QLabel *m_shortageTestProductLabel = nullptr;       ///< 连续两轮确认后的产品文案。
    QLabel *m_shortageTestModeLabel = nullptr;          ///< 连续两轮确认后的生产方式文案。
    QLabel *m_shortageTestUsageLabel = nullptr;         ///< 当前计算策略说明。
    QLabel *m_shortageTestBitsLabel = nullptr;          ///< L68/L69/L71/L72/L73/L1998 当前值。
    QLabel *m_shortageTestStatusLabel = nullptr;        ///< 总体测试状态文案。
    QTableWidget *m_shortageTestTable = nullptr;        ///< 12 工位测试库存表。

    QString m_currentStatusText = QStringLiteral("等待首轮基线");
};

#endif // SHORTAGETESTPANEL_H
