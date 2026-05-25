#ifndef WORKFLOWWIDGET_H
#define WORKFLOWWIDGET_H

#include <QWidget>
#include <QList>
#include "workflowstep.h"

class WorkflowWidget : public QWidget
{
    Q_OBJECT
public:
    explicit WorkflowWidget(QWidget *parent = nullptr);

    void setActiveStep(int step);
    void setStationHighlight(int station);
    void setStationMode(bool show);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void drawStepBoxes(QPainter &p, int w, int gap, int boxW, int boxH, int arrowW, int topY);
    void drawArrows(QPainter &p, int w, int gap, int boxW, int boxH, int arrowW, int topY);
    void drawStations(QPainter &p, int w);

    QList<WorkflowStep> m_steps;
    int m_activeStep       = -1;
    int m_highlightStation = -1;
    bool m_showStations    = true;
};

#endif
