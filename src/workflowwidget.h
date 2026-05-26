/**
 * @file workflowwidget.h
 * @brief 工作流可视化控件
 *
 * 在主窗口右侧区域绘制五步流程图和底部12工位轨道。
 * 当某步骤被激活时，对应方框高亮显示；
 * 当某工位被选中时，底部工位条对应格高亮显示。
 */

#ifndef WORKFLOWWIDGET_H
#define WORKFLOWWIDGET_H

#include <QWidget>
#include <QList>
#include "workflowstep.h"

/**
 * @brief 工作流五步流程图 + 12工位状态条
 *
 * 布局（从上到下）：
 *   ┌─────┐  →  ┌─────┐  →  ┌─────┐  →  ┌─────┐  →  ┌─────┐
 *   │步骤0 │     │步骤1 │     │步骤2 │     │步骤3 │     │步骤4 │
 *   └─────┘     └─────┘     └─────┘     └─────┘     └─────┘
 *        └─────────────────────────────────────────────────┘ (循环箭头)
 *   [工位1][工位2][工位3]...[工位12]   ← 底部工位条
 *
 * 步骤方框颜色取自 WorkflowStep::color，激活时全亮，非激活时变暗透明。
 * 连接箭头：相邻步骤用实线箭头；最后一步回到第一步用虚线折线（表示循环）。
 */
class WorkflowWidget : public QWidget
{
    Q_OBJECT
public:
    explicit WorkflowWidget(QWidget *parent = nullptr);

    /**
     * @brief 高亮指定步骤
     * @param step 步骤索引（0-4），-1 表示全部取消高亮（停止状态）
     */
    void setActiveStep(int step);

    /**
     * @brief 高亮底部工位条中的指定工位
     * @param station 工位编号（1-12），-1 表示不高亮
     */
    void setStationHighlight(int station);

    /**
     * @brief 显示或隐藏底部工位条
     * @param show true=显示（默认），false=隐藏
     */
    void setStationMode(bool show);

protected:
    /// 主绘制入口：背景 → 步骤方框 → 连接箭头 → 工位条
    void paintEvent(QPaintEvent *) override;

private:
    /**
     * @brief 绘制所有步骤方框
     * @param p       画笔
     * @param w       控件总宽度
     * @param gap     左右边距（水平居中计算所得）
     * @param boxW    单个方框宽度
     * @param boxH    单个方框高度
     * @param arrowW  箭头区域宽度
     * @param topY    方框顶部 Y 坐标
     */
    void drawStepBoxes(QPainter &p, int w, int gap, int boxW, int boxH, int arrowW, int topY);

    /**
     * @brief 绘制步骤间的连接箭头（含末尾循环折线）
     * 参数含义同 drawStepBoxes
     */
    void drawArrows(QPainter &p, int w, int gap, int boxW, int boxH, int arrowW, int topY);

    /**
     * @brief 绘制底部 12 工位状态条
     * @param p  画笔
     * @param w  控件总宽度
     */
    void drawStations(QPainter &p, int w);

    QList<WorkflowStep> m_steps;             ///< 五步元数据列表（名称/描述/颜色）
    int  m_activeStep       = -1;            ///< 当前激活的步骤索引，-1=无激活
    int  m_highlightStation = -1;            ///< 当前高亮的工位编号，-1=无高亮
    bool m_showStations     = true;          ///< 是否显示底部工位条
};

#endif // WORKFLOWWIDGET_H
