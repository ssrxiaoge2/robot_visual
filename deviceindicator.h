/**
 * @file deviceindicator.h
 * @brief 设备状态指示灯控件
 *
 * 自定义绘制的小型状态卡片，用于在左侧面板实时显示单个设备的连接状态。
 * 绿色 LED = 正常/在线，红色 LED = 异常/离线。
 */

#ifndef DEVICEINDICATOR_H
#define DEVICEINDICATOR_H

#include <QWidget>

/**
 * @brief 单设备状态指示灯
 *
 * 固定大小 170×56 px，完全自绘（不受全局 QSS 背景影响）。
 * 布局：左侧彩色 LED 圆点 | 右上角设备名 | 右下角状态文字
 *
 * 使用示例：
 * @code
 *   auto *ind = new DeviceIndicator("华沿机器人");
 *   ind->setStatus(true,  "已连接");   // 绿色
 *   ind->setStatus(false, "离线");     // 红色
 * @endcode
 */
class DeviceIndicator : public QWidget
{
    Q_OBJECT
public:
    /**
     * @brief 构造函数
     * @param name    设备名称，显示在指示灯右上方（例："华沿机器人"）
     * @param parent  父控件
     *
     * 初始状态：红色 LED + 状态文字"未知"
     */
    explicit DeviceIndicator(const QString &name, QWidget *parent = nullptr);

    /**
     * @brief 更新设备状态并重绘
     * @param ok    true=正常（绿色），false=异常（红色）
     * @param text  状态说明文字，显示在 LED 下方（例："已连接"/"离线"/"测试中..."）
     */
    void setStatus(bool ok, const QString &text);

protected:
    /// 完全自绘：圆角背景 + LED 圆点 + 设备名 + 状态文字
    void paintEvent(QPaintEvent *) override;

private:
    QString m_name;        ///< 设备名称（构造时设定，不可更改）
    bool    m_ok = false;  ///< 当前状态：true=正常，false=异常
    QString m_statusText;  ///< 当前状态文字
};

#endif // DEVICEINDICATOR_H
