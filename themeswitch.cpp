#include "themeswitch.h"

#include <QPainter>
#include <QPropertyAnimation>
#include <QMouseEvent>

ThemeSwitch::ThemeSwitch(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(76, 30);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("切换日间 / 夜间主题"));

    m_anim = new QPropertyAnimation(this, "slidePos", this);
    m_anim->setDuration(200);
    m_anim->setEasingCurve(QEasingCurve::OutCubic);
}

void ThemeSwitch::setSlidePos(qreal p)
{
    m_slidePos = p;
    update();
}

void ThemeSwitch::setDark(bool dark, bool animated)
{
    if (m_dark == dark) return;
    m_dark = dark;

    const qreal target = dark ? 1.0 : 0.0;
    if (animated && isVisible()) {
        m_anim->stop();
        m_anim->setStartValue(m_slidePos);
        m_anim->setEndValue(target);
        m_anim->start();
    } else {
        setSlidePos(target);
    }
    emit toggled(m_dark);
}

void ThemeSwitch::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int W      = width();
    const int H      = height();
    const int iconW  = 16;         // 图标区域宽度
    const int tX     = iconW;      // 轨道起点 X
    const int tW     = W - 2 * iconW;  // 轨道宽度
    const int tH     = H - 8;     // 轨道高度
    const int tY     = (H - tH) / 2;
    const int circD  = tH - 2;    // 圆圈直径

    // ── 轨道：浅色(灰) ↔ 深色(蓝) 插值 ──────────────────────
    auto lerp = [](int a, int b, qreal t) { return int(a + t * (b - a)); };
    const QColor lightTrack(160, 160, 160);
    const QColor darkTrack (50,  115, 200);
    QColor track(
        lerp(lightTrack.red(),   darkTrack.red(),   m_slidePos),
        lerp(lightTrack.green(), darkTrack.green(), m_slidePos),
        lerp(lightTrack.blue(),  darkTrack.blue(),  m_slidePos)
    );
    p.setBrush(track);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(tX, tY, tW, tH, tH / 2.0, tH / 2.0);

    // ── 滑动圆圈 ─────────────────────────────────────────────
    const qreal cX = tX + 1.0 + m_slidePos * (tW - circD - 2.0);
    const qreal cY = (H - circD) / 2.0;
    p.setBrush(Qt::white);
    p.drawEllipse(QRectF(cX, cY, circD, circD));

    // ── 图标 ─────────────────────────────────────────────────
    QFont f = font();
    f.setPixelSize(13);
    p.setFont(f);

    // 左侧 ☀（浅色模式时高亮）
    p.setPen(m_slidePos < 0.5
             ? QColor(220, 160,   0)   // 激活：金黄
             : QColor(120, 120, 120));  // 非激活：灰
    p.drawText(QRect(0, 0, iconW, H), Qt::AlignCenter,
               QString::fromUtf8("☀")); // ☀

    // 右侧 ☽（深色模式时高亮）
    p.setPen(m_slidePos >= 0.5
             ? QColor(160, 190, 255)   // 激活：淡蓝
             : QColor(120, 120, 120));  // 非激活：灰
    p.drawText(QRect(W - iconW, 0, iconW, H), Qt::AlignCenter,
               QString::fromUtf8("☽")); // ☽
}

void ThemeSwitch::mousePressEvent(QMouseEvent *)
{
    setDark(!m_dark);
}
