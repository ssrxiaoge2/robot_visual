#ifndef THEMESWITCH_H
#define THEMESWITCH_H

#include <QWidget>

class QPropertyAnimation;

/*!
 * \brief 日间 / 夜间主题滑动开关
 *
 * 自定义绘制的滑动按钮，左侧☀右侧☽，点击时圆圈滑动并发出 toggled 信号。
 * 大小固定 76×30，不受 QSS 背景色影响（完全自绘）。
 */
class ThemeSwitch : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(qreal slidePos READ slidePos WRITE setSlidePos)

public:
    explicit ThemeSwitch(QWidget *parent = nullptr);

    bool  isDark()    const { return m_dark; }
    qreal slidePos()  const { return m_slidePos; }

    void setSlidePos(qreal p);
    /// 切换主题；animated=false 时圆圈瞬间跳到目标位置
    void setDark(bool dark, bool animated = true);

signals:
    void toggled(bool dark);   ///< dark=true 切为深色，false 切为浅色

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;

private:
    bool   m_dark     = true;   ///< 当前是否深色模式
    qreal  m_slidePos = 1.0;    ///< 0.0=浅色(左)，1.0=深色(右)
    QPropertyAnimation *m_anim  = nullptr;
};

#endif // THEMESWITCH_H
