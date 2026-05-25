#ifndef DEVICEINDICATOR_H
#define DEVICEINDICATOR_H

#include <QWidget>

class DeviceIndicator : public QWidget
{
    Q_OBJECT
public:
    explicit DeviceIndicator(const QString &name, QWidget *parent = nullptr);
    void setStatus(bool ok, const QString &text);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QString m_name;
    bool m_ok = false;
    QString m_statusText;
};

#endif
