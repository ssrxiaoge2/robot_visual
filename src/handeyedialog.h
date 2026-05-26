#ifndef HANDEYEDIALOG_H
#define HANDEYEDIALOG_H

#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>

/*!
 * \brief 手眼矩阵加载向导
 *
 * 三步向导：说明 → 加载/编辑矩阵文件 → 确认应用
 * 解析 hand-eye/calibration_output/handeye_matrix.txt（numpy savetxt 格式），
 * 或手动在表格中填写 4×4 矩阵。
 * 点击"立即应用"后 emit matrixConfirmed，由 DeviceManager 写入 VisionHttpClient。
 */
class HandEyeDialog : public QDialog
{
    Q_OBJECT
public:
    explicit HandEyeDialog(QWidget *parent = nullptr);

signals:
    /// 用户确认后发出，m 为行主序 4×4 浮点矩阵（T_tool_cam）
    void matrixConfirmed(const float m[16]);

private slots:
    void onBrowse();
    void onParse();
    void onNext();
    void onPrev();

private:
    QWidget *buildIntroPage();
    QWidget *buildLoadPage();
    QWidget *buildConfirmPage();

    bool parseFile(const QString &path);
    void tableToMatrix();
    void matrixToTable(QTableWidget *tbl);

    QStackedWidget *m_stack        = nullptr;
    QPushButton    *m_btnPrev      = nullptr;
    QPushButton    *m_btnNext      = nullptr;

    QLineEdit    *m_editPath       = nullptr;
    QTableWidget *m_tableLoad      = nullptr;
    QTableWidget *m_tableConfirm   = nullptr;
    QLabel       *m_lblStatus      = nullptr;

    float m_matrix[16] = {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        0,0,0,1
    };
};

#endif // HANDEYEDIALOG_H
