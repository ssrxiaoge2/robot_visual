#ifndef PALLETPARAMDIALOG_H
#define PALLETPARAMDIALOG_H

#include <QDialog>
#include <QHash>
#include <QMap>
#include <QString>

#include "palletscheduler.h"

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QLineEdit;
class QTableWidget;
class QTabWidget;
class VisionHttpClient;

/**
 * @brief 空箱码垛参数配置与仿真窗口。
 *
 * 窗口负责编辑大箱/小箱两套 PalletConfig、显示配置校验、预览下一点、
 * 从空托盘仿真 8 层、人工修正已放数量，并通过 VisionHttpClient 做码垛区占用检测。
 * 它不接入真实机械臂；“模拟放置完成”只用于调试时手动推进 placedCount。
 */
class PalletParamDialog : public QDialog
{
    Q_OBJECT

public:
    /**
     * @brief 创建码垛配置窗口。
     * @param scheduler 码垛算法与状态缓存对象，不能为空时窗口可正常工作。
     * @param visionClient 可选视觉 HTTP 客户端；为空时禁用视觉检测能力。
     */
    explicit PalletParamDialog(PalletScheduler *scheduler,
                               VisionHttpClient *visionClient,
                               QWidget *parent = nullptr);

private:
    /** @brief 单个 tab 页上的全部控件引用，便于按大箱/小箱分别刷新。 */
    struct PageWidgets {
        QWidget *page = nullptr;
        QDoubleSpinBox *palletX = nullptr;
        QDoubleSpinBox *palletY = nullptr;
        QDoubleSpinBox *palletZ = nullptr;
        QDoubleSpinBox *boxX = nullptr;
        QDoubleSpinBox *boxY = nullptr;
        QDoubleSpinBox *boxZ = nullptr;
        QDoubleSpinBox *gapX = nullptr;
        QDoubleSpinBox *gapY = nullptr;
        QDoubleSpinBox *marginX = nullptr;
        QDoubleSpinBox *marginY = nullptr;
        QLineEdit *maxLayers = nullptr;
        QDoubleSpinBox *releaseZOffset = nullptr;
        QDoubleSpinBox *maxRobotZ = nullptr;
        QDoubleSpinBox *originX = nullptr;
        QDoubleSpinBox *originY = nullptr;
        QDoubleSpinBox *originZ = nullptr;
        QDoubleSpinBox *originRx = nullptr;
        QDoubleSpinBox *originRy = nullptr;
        QDoubleSpinBox *originRz = nullptr;
        QCheckBox *invertX = nullptr;
        QCheckBox *invertY = nullptr;
        QLineEdit *placedCount = nullptr;
        QPushButton *applyPlaced = nullptr;
        QPushButton *detectBtn = nullptr;
        QLabel *detectLabel = nullptr;
        QLabel *emptyCountLabel = nullptr;
        QLabel *capacityLabel = nullptr;
        QLabel *nextPoseLabel = nullptr;
        QLabel *statusLabel = nullptr;
        QPlainTextEdit *validationText = nullptr;
        QTableWidget *simulationTable = nullptr;
    };

    /** @brief 创建大箱或小箱配置页。 */
    QWidget *createAreaPage(PalletArea area);
    /** @brief 获取指定区域的控件集合。 */
    PageWidgets *widgets(PalletArea area) const;

    /** @brief 创建尺寸/距离输入框；滚轮不会修改数值。 */
    QDoubleSpinBox *createDistanceSpin(double max = 100000.0) const;
    /** @brief 创建坐标输入框；允许正负值，滚轮不会修改数值。 */
    QDoubleSpinBox *createPoseSpin() const;
    /** @brief 创建姿态角输入框；滚轮不会修改数值。 */
    QDoubleSpinBox *createAngleSpin() const;

    /** @brief 从当前页控件读取配置，不代表用户已经确认保存。 */
    PalletConfig readConfig(PalletArea area) const;
    /** @brief 把配置写入当前页控件；用于加载缓存或填入默认现场值。 */
    void writeConfig(PalletArea area, const PalletConfig &config);
    /** @brief 刷新容量、已放数量、视觉空计数等状态展示。 */
    void refreshPage(PalletArea area);
    /** @brief 保存当前页控件配置到 PalletScheduler 和 QSettings。 */
    void savePage(PalletArea area);
    /** @brief 校验当前页配置并展示错误/建议。 */
    void validatePage(PalletArea area);
    /** @brief 将当前箱型的默认现场值写入 UI；不自动保存配置。 */
    void fillFieldDefaults(PalletArea area);
    /** @brief 根据箱高写入推荐释放高度参考值；不影响相对 Z 偏移公式。 */
    void recommendReleaseHeight(PalletArea area);
    /** @brief 预览下一点；只计算，不推进已放数量。 */
    void showNextPose(PalletArea area);
    /** @brief 仿真按钮使用：手动模拟机械臂完成一次放置并推进缓存。 */
    void simulatePlaced(PalletArea area);
    /** @brief 从空托盘开始生成完整点位表；不修改真实缓存。 */
    void simulateArea(PalletArea area);
    /** @brief 清空右侧结果、校验和仿真表格；不修改配置或缓存。 */
    void clearDisplayResults(PalletArea area);
    /** @brief 二次确认后清零当前区域已放数量。 */
    void resetArea(PalletArea area);
    /** @brief 二次确认后人工修正当前区域已放数量。 */
    void applyPlacedCount(PalletArea area);
    /** @brief 发起当前区域视觉占用检测请求。 */
    void detectArea(PalletArea area);
    /** @brief 设置状态提示文字和颜色。 */
    void setStatus(PageWidgets *w, const QString &text, const QString &state);
    /** @brief 将 PalletArea 转成 m_pages 使用的稳定 key。 */
    static int areaKey(PalletArea area);

    PalletScheduler *m_scheduler = nullptr;
    VisionHttpClient *m_visionClient = nullptr;
    QTabWidget *m_tabs = nullptr;
    QMap<int, PageWidgets *> m_pages;
    QHash<QString, PalletArea> m_pendingRequests;
};

#endif // PALLETPARAMDIALOG_H
