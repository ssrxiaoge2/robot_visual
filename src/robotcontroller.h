#ifndef ROBOTCONTROLLER_H
#define ROBOTCONTROLLER_H

#include <QObject>
#include <QModbusTcpClient>
#include <QModbusDataUnit>
#include <QTimer>

class RobotController : public QObject
{
    Q_OBJECT

public:
    explicit RobotController(QObject *parent = nullptr);
    ~RobotController() override;

    void connectToHost(const QString &ip, int port = 502);
    void disconnectFromHost();
    bool isConnected() const;

    void readHoldingRegisters(int startAddr, int count);
    /// 多寄存器批量写入（FC16），写完坐标后再写 CmdID 必须分两次调用
    void writeRegisters(int startAddr, const QList<quint16> &values);
    void writeRegister(int addr, quint16 value);
    void writeCoil(int addr, bool on);
    void readCoils(int startAddr, int count);
    /// Input Register（只读，FC04），用于读取机器人状态字 Input 1132
    void readInputRegisters(int startAddr, int count);

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString &msg);
    /// Holding 寄存器读取结果（FC03）
    void registersRead(int startAddr, const QList<quint16> &values);
    /// Input 寄存器读取结果（FC04），与 registersRead 分开以避免地址冲突
    void inputRegistersRead(int startAddr, const QList<quint16> &values);
    void coilsRead(int startAddr, const QList<bool> &values);
    void writeCompleted();

private slots:
    void onStateChanged(QModbusDevice::State state);
    void onReadReady();

private:
    QModbusTcpClient *m_client = nullptr;
    QTimer           *m_reconnectTimer = nullptr;
    QString           m_ip;
    int               m_port = 502;
};

#endif
