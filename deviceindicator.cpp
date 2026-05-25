#include "deviceindicator.h"

#include <QPainter>
#include <QFont>

DeviceIndicator::DeviceIndicator(const QString &name, QWidget *parent)
    : QWidget(parent), m_name(name)
{
    setFixedSize(170, 56);
    setStatus(false, QStringLiteral("未知"));
}

void DeviceIndicator::setStatus(bool ok, const QString &text)
{
    m_ok = ok;
    m_statusText = text;
    update();
}

void DeviceIndicator::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QColor led = m_ok ? QColor(0, 200, 80) : QColor(220, 60, 60);
    const QColor bg  = m_ok ? QColor(40, 60, 48) : QColor(60, 40, 44);

    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(rect(), 8, 8);

    p.setBrush(led);
    p.drawEllipse(10, 15, 18, 18);

    QFont f = font();
    f.setPointSize(10);
    f.setBold(true);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(QRect(34, 8, 100, 22), Qt::AlignLeft | Qt::AlignVCenter, m_name);

    f.setPointSize(8);
    f.setBold(false);
    p.setFont(f);
    p.setPen(QColor(180, 180, 180));
    p.drawText(QRect(14, 36, 118, 18), Qt::AlignCenter, m_statusText);
}
