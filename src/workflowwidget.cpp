/**
 * @file workflowwidget.cpp
 * @brief 工作流可视化控件实现
 *
 * 全部使用 QPainter 自绘，不依赖 QSS，在深色/浅色主题下均正常显示。
 * 布局参数在 paintEvent 中根据控件实际宽度动态计算，支持拉伸。
 */

#include "workflowwidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QFont>

// ── 默认步骤定义（SDK 调度阶段，索引与 HuayanScheduler::stepIndexFor 对应）──
// 此处仅用于 UI 展示，不含业务逻辑
static const QList<WorkflowStep> kDefaultSteps = {
    {QStringLiteral("视觉定位"),  QStringLiteral("Orbbec+SAM2"), QColor(65,  130, 220)},
    {QStringLiteral("SDK 取料"),  QStringLiteral("华沿 SDK"),    QColor(220, 160,  50)},
    {QStringLiteral("翻转卸料"),  QStringLiteral("华沿 SDK"),    QColor(180, 110, 210)},
    {QStringLiteral("AGV 运输"),  QStringLiteral("仙工 Modbus"), QColor(60,  180, 120)},
    {QStringLiteral("码垛复位"),  QStringLiteral("下一轮"),      QColor(130, 130, 130)},
};

// ── 构造 ─────────────────────────────────────────────────────

WorkflowWidget::WorkflowWidget(QWidget *parent)
    : QWidget(parent), m_steps(kDefaultSteps)
{
    setMinimumHeight(200); // 保证步骤方框和工位条都能完整显示
}

// ── 公开接口 ─────────────────────────────────────────────────

void WorkflowWidget::setActiveStep(int step)
{
    m_activeStep = step;
    update(); // 异步触发重绘
}

void WorkflowWidget::setStationHighlight(int station)
{
    m_highlightStation = station;
    update();
}

void WorkflowWidget::setStationMode(bool show)
{
    m_showStations = show;
    update();
}

// ── 主绘制入口 ────────────────────────────────────────────────

void WorkflowWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // 深色圆角背景（不随系统主题变化，始终保持固定配色）
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(30, 32, 38));
    p.drawRoundedRect(rect(), 10, 10);

    // ── 计算布局参数（随控件宽度自动缩放）────────────────────
    const int num    = m_steps.size();   // 步骤总数（5）
    const int w      = width();          // 控件实际宽度
    const int boxW   = 100;             // 每个步骤方框宽度（固定）
    const int boxH   = 56;             // 每个步骤方框高度（固定）
    const int arrowW = 30;             // 箭头区域宽度（两个方框之间的间距）
    // gap：将所有方框+箭头水平居中所需的左右边距
    const int gap    = (w - num * boxW - (num - 1) * arrowW) / 2;
    const int topY   = 40;             // 步骤方框距控件顶部的距离

    // 按层次顺序绘制：先填充方框，再绘制箭头（避免箭头被方框遮挡）
    drawStepBoxes(p, w, gap, boxW, boxH, arrowW, topY);
    drawArrows(p, w, gap, boxW, boxH, arrowW, topY);

    if (m_showStations)
        drawStations(p, w);
}

// ── 步骤方框绘制 ──────────────────────────────────────────────

void WorkflowWidget::drawStepBoxes(QPainter &p, int /*w*/, int gap,
                                    int boxW, int boxH, int arrowW, int topY)
{
    const int num = m_steps.size();

    for (int i = 0; i < num; ++i) {
        const int  x      = gap + i * (boxW + arrowW); // 方框左上角 X 坐标
        const bool active = (i == m_activeStep);        // 是否为当前激活步骤

        // ── 方框填充色：激活=全亮，非激活=变暗+半透明 ────────
        const auto &s = m_steps[i];
        QColor fill   = active ? s.color : s.color.darker(200);
        if (!active) fill.setAlpha(120); // 非激活步骤增加透明度，视觉上退到背景

        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(QRect(x, topY, boxW, boxH), 8, 8);

        // 激活步骤：额外绘制高亮边框（比方框略大 1px，形成光晕效果）
        if (active) {
            p.setPen(QPen(s.color.lighter(150), 2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRect(x - 1, topY - 1, boxW + 2, boxH + 2), 9, 9);
        }

        // ── 主标题（步骤名，10pt，激活时粗体）────────────────
        QFont f = font();
        f.setPointSize(10);
        f.setBold(active);
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(QRect(x, topY + 4, boxW, 24), Qt::AlignCenter, s.name);

        // ── 副标题（设备描述，7pt，浅灰色）──────────────────
        f.setPointSize(7);
        f.setBold(false);
        p.setFont(f);
        p.setPen(QColor(160, 160, 160));
        p.drawText(QRect(x, topY + 28, boxW, 22), Qt::AlignCenter, s.desc);
    }
}

// ── 连接箭头绘制 ──────────────────────────────────────────────

void WorkflowWidget::drawArrows(QPainter &p, int /*w*/, int gap,
                                 int boxW, int boxH, int arrowW, int topY)
{
    const int num = m_steps.size();
    const int y   = topY + boxH / 2; // 箭头水平中线 Y（方框垂直中心）

    for (int i = 0; i < num; ++i) {
        const int x1    = gap + i * (boxW + arrowW);
        const int x2    = (i + 1 < num) ? gap + (i + 1) * (boxW + arrowW) : gap;
        const int fromX = x1 + boxW;  // 当前方框右边缘
        const int toX   = x2;         // 下一个方框左边缘

        if (i < num - 1) {
            // ── 相邻步骤：水平实线箭头 ────────────────────────
            // 箭头颜色：当前激活步骤的"出口"箭头用其颜色高亮，其余灰色
            QColor ac(100, 100, 100); // 默认：灰色
            if (i == m_activeStep - 1 || (m_activeStep == 0 && i == num - 1))
                ac = m_steps[m_activeStep].color.lighter(130); // 高亮：激活步骤色

            // 箭头杆（水平线段）
            p.setPen(QPen(ac, 2));
            p.drawLine(QPoint(fromX, y), QPoint(toX, y));

            // 箭头头（填充三角形，尖端朝右）
            QPainterPath tip;
            tip.moveTo(toX,      y);      // 尖端（右侧顶点）
            tip.lineTo(toX - 8,  y - 5); // 左上角
            tip.lineTo(toX - 8,  y + 5); // 左下角
            tip.closeSubpath();
            p.setBrush(ac);
            p.setPen(Qt::NoPen);
            p.drawPath(tip);

        } else {
            // ── 最后一步→第一步：蓝色虚线折线（表示循环）────
            //   路径：步骤4右边 → 向下折 → 向左横穿 → 向上到步骤0左边
            const int loopX1 = x1 + boxW;       // 步骤4方框右边缘 X
            const int loopX2 = gap;              // 步骤0方框左边缘 X
            const int yy     = topY + boxH + 22; // 折线底部 Y（方框下方22px）

            p.setPen(QPen(QColor(100, 180, 255, 120), 2, Qt::DashLine));
            p.drawLine(QPointF(loopX1,      y),  QPointF(loopX1,       yy)); // 右竖线段（向下）
            p.drawLine(QPointF(loopX1,      yy), QPointF(loopX2 - 16,  yy)); // 底部横线（向左）
            p.drawLine(QPointF(loopX2 - 16, yy), QPointF(loopX2 - 16,  y));  // 左竖线段（向上）

            // 循环箭头头（向上的三角形，指向步骤0入口）
            QPainterPath tip;
            tip.moveTo(loopX2 - 16, y);      // 尖端（上方顶点）
            tip.lineTo(loopX2 - 20, y + 8);  // 左下角
            tip.lineTo(loopX2 - 12, y + 8);  // 右下角
            tip.closeSubpath();
            p.setBrush(QColor(100, 180, 255, 120));
            p.setPen(Qt::NoPen);
            p.drawPath(tip);
        }
    }
}

// ── 工位条绘制 ────────────────────────────────────────────────

void WorkflowWidget::drawStations(QPainter &p, int w)
{
    // 工位条位于控件底部，每格等宽，共 12 格
    const int stationY = height() - 36;      // 工位条顶部 Y
    const int stW      = (w - 40) / 12;     // 单个工位格宽度（左右各留 20px 边距）

    for (int s = 0; s < 12; ++s) {
        const int  sx   = 16 + s * stW;                        // 当前工位格左上角 X
        const QRect sr(sx, stationY, stW - 4, 26);             // 工位格矩形（格间留 4px 间距）
        const bool hl   = (s + 1 == m_highlightStation);       // 是否为当前高亮工位（编号从1开始）
        const QColor bg = hl ? QColor(50, 150, 250) : QColor(55, 57, 65); // 高亮=蓝，普通=暗灰

        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawRoundedRect(sr, 4, 4);

        // 工位编号文字（高亮=白色，普通=灰色）
        p.setPen(hl ? Qt::white : QColor(160, 160, 160));
        p.setFont(QFont(font().family(), 10));
        p.drawText(sr, Qt::AlignCenter, QString("工位%1").arg(s + 1));
    }
}
