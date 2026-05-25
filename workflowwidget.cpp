#include "workflowwidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QFont>

static const QList<WorkflowStep> kDefaultSteps = {
    {QStringLiteral("相机拍照"),   QStringLiteral("Orbbec 3D"), QColor(65, 130, 220)},
    {QStringLiteral("机器人抓取"), QStringLiteral("华沿TCP"),  QColor(220, 160, 50)},
    {QStringLiteral("AGV行走"),    QStringLiteral("仙工Modbus"), QColor(60, 180, 120)},
    {QStringLiteral("机器人放料"), QStringLiteral("翻转卸料"),  QColor(180, 110, 210)},
    {QStringLiteral("复位等待"),   QStringLiteral("下一轮"),   QColor(130, 130, 130)},
};

WorkflowWidget::WorkflowWidget(QWidget *parent)
    : QWidget(parent), m_steps(kDefaultSteps)
{
    setMinimumHeight(200);
}

void WorkflowWidget::setActiveStep(int step)
{
    m_activeStep = step;
    update();
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

void WorkflowWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(30, 32, 38));
    p.drawRoundedRect(rect(), 10, 10);

    const int num    = m_steps.size();
    const int w      = width();
    const int boxW   = 100;
    const int boxH   = 56;
    const int arrowW = 30;
    const int gap    = (w - num * boxW - (num - 1) * arrowW) / 2;
    const int topY   = 40;

    drawStepBoxes(p, w, gap, boxW, boxH, arrowW, topY);
    drawArrows(p, w, gap, boxW, boxH, arrowW, topY);

    if (m_showStations)
        drawStations(p, w);
}

void WorkflowWidget::drawStepBoxes(QPainter &p, int, int gap, int boxW, int boxH, int arrowW, int topY)
{
    const int num = m_steps.size();
    for (int i = 0; i < num; ++i) {
        const int x      = gap + i * (boxW + arrowW);
        const bool active = (i == m_activeStep);

        const auto &s   = m_steps[i];
        QColor fill     = active ? s.color : s.color.darker(200);
        if (!active) fill.setAlpha(120);

        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(QRect(x, topY, boxW, boxH), 8, 8);

        if (active) {
            p.setPen(QPen(s.color.lighter(150), 2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRect(x - 1, topY - 1, boxW + 2, boxH + 2), 9, 9);
        }

        QFont f = font();
        f.setPointSize(10);
        f.setBold(active);
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(QRect(x, topY + 4, boxW, 24), Qt::AlignCenter, s.name);

        f.setPointSize(7);
        f.setBold(false);
        p.setFont(f);
        p.setPen(QColor(160, 160, 160));
        p.drawText(QRect(x, topY + 28, boxW, 22), Qt::AlignCenter, s.desc);
    }
}

void WorkflowWidget::drawArrows(QPainter &p, int, int gap, int boxW, int boxH, int arrowW, int topY)
{
    const int num = m_steps.size();
    const int y   = topY + boxH / 2;

    for (int i = 0; i < num; ++i) {
        const int x1    = gap + i * (boxW + arrowW);
        const int x2    = (i + 1 < num) ? gap + (i + 1) * (boxW + arrowW) : gap;
        const int fromX = x1 + boxW;
        const int toX   = x2;

        if (i < num - 1) {
            QColor ac(100, 100, 100);
            if (i == m_activeStep - 1 || (m_activeStep == 0 && i == num - 1))
                ac = m_steps[m_activeStep].color.lighter(130);

            p.setPen(QPen(ac, 2));
            p.drawLine(QPoint(fromX, y), QPoint(toX, y));

            QPainterPath tip;
            tip.moveTo(toX, y);
            tip.lineTo(toX - 8, y - 5);
            tip.lineTo(toX - 8, y + 5);
            tip.closeSubpath();
            p.setBrush(ac);
            p.setPen(Qt::NoPen);
            p.drawPath(tip);
        } else {
            const int loopX1 = x1 + boxW;
            const int loopX2 = gap;
            const int yy     = topY + boxH + 22;

            p.setPen(QPen(QColor(100, 180, 255, 120), 2, Qt::DashLine));
            p.drawLine(QPointF(loopX1, y),  QPointF(loopX1, yy));
            p.drawLine(QPointF(loopX1, yy), QPointF(loopX2 - 16, yy));
            p.drawLine(QPointF(loopX2 - 16, yy), QPointF(loopX2 - 16, y));

            QPainterPath tip;
            tip.moveTo(loopX2 - 16, y);
            tip.lineTo(loopX2 - 20, y + 8);
            tip.lineTo(loopX2 - 12, y + 8);
            tip.closeSubpath();
            p.setBrush(QColor(100, 180, 255, 120));
            p.setPen(Qt::NoPen);
            p.drawPath(tip);
        }
    }
}

void WorkflowWidget::drawStations(QPainter &p, int w)
{
    const int stationY = height() - 36;
    const int stW      = (w - 40) / 12;
    for (int s = 0; s < 12; ++s) {
        const int sx   = 16 + s * stW;
        const QRect sr(sx, stationY, stW - 4, 26);
        const bool hl  = (s + 1 == m_highlightStation);
        const QColor bg = hl ? QColor(50, 150, 250) : QColor(55, 57, 65);

        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawRoundedRect(sr, 4, 4);
        p.setPen(hl ? Qt::white : QColor(160, 160, 160));
        p.setFont(QFont(font().family(), 10));
        p.drawText(sr, Qt::AlignCenter, QString("工位%1").arg(s + 1));
    }
}
