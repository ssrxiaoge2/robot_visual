/**
 * @file deviceindicator.cpp
 * @brief DeviceIndicator 设备状态指示灯控件实现
 */

#include "deviceindicator.h"

#include <QPainter>
#include <QFont>

// ── 构造 ─────────────────────────────────────────────────────

DeviceIndicator::DeviceIndicator(const QString &name, QWidget *parent)
    : QWidget(parent), m_name(name)
{
    setFixedSize(170, 56);               // 固定尺寸，防止布局拉伸导致变形
    setStatus(false, QStringLiteral("未知")); // 默认：红色 + "未知"
}

// ── 公开接口 ─────────────────────────────────────────────────

void DeviceIndicator::setStatus(bool ok, const QString &text)
{
    m_ok         = ok;
    m_statusText = text;
    update(); // 触发重绘（异步，在下一次事件循环中调用 paintEvent）
}

// ── 绘制 ─────────────────────────────────────────────────────

void DeviceIndicator::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing); // 开启抗锯齿，使圆角和圆圈边缘平滑

    // ── LED 颜色 / 背景色（根据状态切换）────────────────────
    //   正常：绿色 LED，深绿背景
    //   异常：红色 LED，深红背景
    const QColor led = m_ok ? QColor(0, 200, 80)  : QColor(220, 60, 60);
    const QColor bg  = m_ok ? QColor(40, 60, 48)  : QColor(60, 40, 44);

    // ── 圆角矩形背景 ─────────────────────────────────────────
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(rect(), 8, 8);

    // ── LED 圆点（左侧，垂直居中）───────────────────────────
    //   位置：x=10, y=15，直径 18px
    p.setBrush(led);
    p.drawEllipse(10, 15, 18, 18);

    // ── 设备名（右上，粗体 10pt）────────────────────────────
    QFont f = font();
    f.setPointSize(10);
    f.setBold(true);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(QRect(34, 8, 100, 22), Qt::AlignLeft | Qt::AlignVCenter, m_name);

    // ── 状态文字（下方，浅灰色 8pt）─────────────────────────
    f.setPointSize(8);
    f.setBold(false);
    p.setFont(f);
    p.setPen(QColor(180, 180, 180));
    p.drawText(QRect(14, 36, 118, 18), Qt::AlignCenter, m_statusText);
}
