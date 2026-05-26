/**
 * @file themeswitch.cpp
 * @brief ThemeSwitch 主题切换开关控件实现
 *
 * 绘制布局（宽 76px，高 30px）：
 *   [☀ iconW][── tX ── 轨道 tW ──][☽ iconW]
 *   左图标区 │  胶囊形轨道 + 白色滑块圆圈  │ 右图标区
 *
 * slidePos 取值范围：0.0（浅色/左）到 1.0（深色/右）
 *   - 轨道颜色：在灰色(0.0)和蓝色(1.0)之间线性插值
 *   - 圆圈位置：从轨道左端到右端线性移动
 *   - 图标颜色：slidePos<0.5 时☀高亮，slidePos≥0.5 时☽高亮
 */

#include "themeswitch.h"

#include <QPainter>
#include <QPropertyAnimation>
#include <QMouseEvent>

// ── 构造 ─────────────────────────────────────────────────────

ThemeSwitch::ThemeSwitch(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(76, 30);                              // 固定尺寸，防止布局拉伸
    setCursor(Qt::PointingHandCursor);                 // 手形光标提示可点击
    setToolTip(QStringLiteral("切换日间 / 夜间主题"));

    // QPropertyAnimation 对 Q_PROPERTY(slidePos) 进行动画补间
    // 动画系统会以 200ms OutCubic 缓动调用 setSlidePos()
    m_anim = new QPropertyAnimation(this, "slidePos", this);
    m_anim->setDuration(200);
    m_anim->setEasingCurve(QEasingCurve::OutCubic);
}

// ── 属性写入（由动画系统或直接赋值调用）─────────────────────

void ThemeSwitch::setSlidePos(qreal p)
{
    m_slidePos = p;
    update(); // 位置变化后立即触发重绘
}

// ── 公开接口 ─────────────────────────────────────────────────

void ThemeSwitch::setDark(bool dark, bool animated)
{
    if (m_dark == dark) return; // 状态未变，不重复触发
    m_dark = dark;

    const qreal target = dark ? 1.0 : 0.0; // 目标 slidePos
    if (animated && isVisible()) {
        // 有动画：从当前位置平滑过渡到目标位置
        m_anim->stop();
        m_anim->setStartValue(m_slidePos);
        m_anim->setEndValue(target);
        m_anim->start();
    } else {
        // 无动画（控件不可见时，或调用方明确要求）：瞬间跳转
        setSlidePos(target);
    }
    emit toggled(m_dark); // 通知 MainWindow::applyTheme()
}

// ── 绘制 ─────────────────────────────────────────────────────

void ThemeSwitch::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int W     = width();
    const int H     = height();
    const int iconW = 16;              // 左/右图标区各占宽度
    const int tX    = iconW;          // 轨道起始 X
    const int tW    = W - 2 * iconW;  // 轨道宽度
    const int tH    = H - 8;          // 轨道高度（上下留 4px）
    const int tY    = (H - tH) / 2;   // 轨道顶部 Y（垂直居中）
    const int circD = tH - 2;         // 圆圈直径（比轨道矮 2px，留 1px 上下边距）

    // ── 轨道颜色插值（灰 → 蓝，随 slidePos 变化）────────────
    auto lerp = [](int a, int b, qreal t) { return int(a + t * (b - a)); };
    const QColor lightTrack(160, 160, 160); // slidePos=0 时的轨道色
    const QColor darkTrack (50,  115, 200); // slidePos=1 时的轨道色
    QColor track(
        lerp(lightTrack.red(),   darkTrack.red(),   m_slidePos),
        lerp(lightTrack.green(), darkTrack.green(), m_slidePos),
        lerp(lightTrack.blue(),  darkTrack.blue(),  m_slidePos)
    );
    p.setBrush(track);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(tX, tY, tW, tH, tH / 2.0, tH / 2.0); // 胶囊形

    // ── 白色滑块圆圈（位置随 slidePos 在轨道内线性移动）──────
    // slidePos=0: cX = tX+1（左端边距）
    // slidePos=1: cX = tX+tW-circD-1（右端边距）
    const qreal cX = tX + 1.0 + m_slidePos * (tW - circD - 2.0);
    const qreal cY = (H - circD) / 2.0; // 垂直居中
    p.setBrush(Qt::white);
    p.drawEllipse(QRectF(cX, cY, circD, circD));

    // ── 图标区域（13px 像素字体）────────────────────────────
    QFont f = font();
    f.setPixelSize(13);
    p.setFont(f);

    // 左侧 ☀：浅色模式（slidePos<0.5）时金黄色，否则灰色
    p.setPen(m_slidePos < 0.5 ? QColor(220, 160, 0) : QColor(120, 120, 120));
    p.drawText(QRect(0, 0, iconW, H), Qt::AlignCenter, QString::fromUtf8("☀"));

    // 右侧 ☽：深色模式（slidePos≥0.5）时淡蓝色，否则灰色
    p.setPen(m_slidePos >= 0.5 ? QColor(160, 190, 255) : QColor(120, 120, 120));
    p.drawText(QRect(W - iconW, 0, iconW, H), Qt::AlignCenter, QString::fromUtf8("☽"));
}

// ── 鼠标事件 ─────────────────────────────────────────────────

void ThemeSwitch::mousePressEvent(QMouseEvent *)
{
    setDark(!m_dark); // 点击任意位置切换，无需判断坐标
}
