#include "handeyedialog.h"

#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>

HandEyeDialog::HandEyeDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("手眼矩阵加载向导"));
    setMinimumSize(520, 420);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(buildIntroPage());
    m_stack->addWidget(buildLoadPage());
    m_stack->addWidget(buildConfirmPage());

    m_btnPrev = new QPushButton(QStringLiteral("上一步"));
    m_btnNext = new QPushButton(QStringLiteral("下一步"));
    m_btnPrev->setFixedHeight(30);
    m_btnNext->setFixedHeight(30);
    m_btnPrev->setEnabled(false);

    auto *navRow = new QHBoxLayout();
    navRow->addStretch();
    navRow->addWidget(m_btnPrev);
    navRow->addWidget(m_btnNext);

    auto *root = new QVBoxLayout(this);
    root->addWidget(m_stack, 1);
    root->addLayout(navRow);

    connect(m_btnPrev, &QPushButton::clicked, this, &HandEyeDialog::onPrev);
    connect(m_btnNext, &QPushButton::clicked, this, &HandEyeDialog::onNext);
}

QWidget *HandEyeDialog::buildIntroPage()
{
    auto *page = new QWidget();
    auto *lay  = new QVBoxLayout(page);
    lay->setContentsMargins(24, 24, 24, 8);

    auto *title = new QLabel(QStringLiteral("<b>手眼矩阵加载向导</b>"));
    title->setStyleSheet("font-size:16px;");

    auto *desc = new QLabel(
        QStringLiteral(
            "本向导帮助你将手眼标定结果加载到运行中的视觉系统。\n\n"
            "手眼矩阵（T_tool_cam）描述相机坐标系到机器人末端坐标系的变换，\n"
            "用于将视觉检测到的目标位置转换为机器人可执行的运动指令。\n\n"
            "标定文件路径（默认）：\n"
            "  hand-eye/calibration_output/handeye_matrix.txt\n\n"
            "文件格式：\n"
            "  # 注释行（以 # 开头，由 numpy.savetxt 生成）\n"
            "  R11 R12 R13 tx\n"
            "  R21 R22 R23 ty\n"
            "  R31 R32 R33 tz\n"
            "  0.0 0.0 0.0 1.0\n\n"
            "点击「下一步」选择文件并预览矩阵，也可手动输入。"
        )
    );
    desc->setWordWrap(true);
    desc->setStyleSheet("font-size:13px; line-height:1.5;");

    lay->addWidget(title);
    lay->addSpacing(12);
    lay->addWidget(desc);
    lay->addStretch();
    return page;
}

QWidget *HandEyeDialog::buildLoadPage()
{
    auto *page = new QWidget();
    auto *lay  = new QVBoxLayout(page);
    lay->setContentsMargins(16, 16, 16, 8);

    auto *pathRow = new QHBoxLayout();
    m_editPath = new QLineEdit();
    m_editPath->setPlaceholderText(QStringLiteral("标定文件路径 (.txt)"));
    m_editPath->setText(QStringLiteral("../hand-eye/calibration_output/handeye_matrix.txt"));

    auto *btnBrowse = new QPushButton(QStringLiteral("浏览..."));
    btnBrowse->setFixedWidth(72);
    auto *btnParse  = new QPushButton(QStringLiteral("解析"));
    btnParse->setFixedWidth(56);

    pathRow->addWidget(m_editPath, 1);
    pathRow->addWidget(btnBrowse);
    pathRow->addWidget(btnParse);
    lay->addLayout(pathRow);

    m_lblStatus = new QLabel(QStringLiteral("请选择标定文件或直接在表格中填写矩阵。"));
    m_lblStatus->setStyleSheet("font-size:12px; color:#aaa;");
    lay->addWidget(m_lblStatus);

    // 4×4 可编辑矩阵表格
    m_tableLoad = new QTableWidget(4, 4, page);
    m_tableLoad->setHorizontalHeaderLabels({"col0","col1","col2","col3"});
    m_tableLoad->setVerticalHeaderLabels({"row0","row1","row2","row3"});
    m_tableLoad->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_tableLoad->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_tableLoad->verticalHeader()->setDefaultSectionSize(36);
    m_tableLoad->setFixedHeight(168);
    matrixToTable(m_tableLoad);
    lay->addWidget(m_tableLoad);

    lay->addStretch();

    connect(btnBrowse, &QPushButton::clicked, this, &HandEyeDialog::onBrowse);
    connect(btnParse,  &QPushButton::clicked, this, &HandEyeDialog::onParse);
    return page;
}

QWidget *HandEyeDialog::buildConfirmPage()
{
    auto *page = new QWidget();
    auto *lay  = new QVBoxLayout(page);
    lay->setContentsMargins(16, 16, 16, 8);

    auto *lbl = new QLabel(QStringLiteral("确认以下矩阵将被立即应用到运行中的视觉系统："));
    lbl->setStyleSheet("font-size:13px;");
    lay->addWidget(lbl);
    lay->addSpacing(8);

    m_tableConfirm = new QTableWidget(4, 4, page);
    m_tableConfirm->setHorizontalHeaderLabels({"col0","col1","col2","col3"});
    m_tableConfirm->setVerticalHeaderLabels({"row0","row1","row2","row3"});
    m_tableConfirm->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_tableConfirm->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_tableConfirm->verticalHeader()->setDefaultSectionSize(36);
    m_tableConfirm->setFixedHeight(168);
    m_tableConfirm->setEditTriggers(QAbstractItemView::NoEditTriggers);
    lay->addWidget(m_tableConfirm);

    auto *warn = new QLabel(
        QStringLiteral("⚠ 应用后将立即覆盖当前矩阵，影响所有后续推理结果。\n"
                       "  请确认矩阵来自最新一次手眼标定。"));
    warn->setStyleSheet("font-size:12px; color:#f0ad4e;");
    lay->addWidget(warn);
    lay->addStretch();
    return page;
}

// ── 槽 ──────────────────────────────────────────────────────

void HandEyeDialog::onBrowse()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择手眼标定文件"),
        m_editPath->text(),
        QStringLiteral("文本文件 (*.txt);;所有文件 (*)"));
    if (!path.isEmpty())
        m_editPath->setText(path);
}

void HandEyeDialog::onParse()
{
    if (parseFile(m_editPath->text())) {
        matrixToTable(m_tableLoad);
        m_lblStatus->setStyleSheet("font-size:12px; color:#5cb85c;");
        m_lblStatus->setText(QStringLiteral("解析成功，请检查矩阵后点击「下一步」。"));
    } else {
        m_lblStatus->setStyleSheet("font-size:12px; color:#d9534f;");
        m_lblStatus->setText(QStringLiteral("解析失败：文件格式不符或路径不存在，请手动填写。"));
    }
}

void HandEyeDialog::onNext()
{
    const int cur = m_stack->currentIndex();
    if (cur == 1) {
        // 从加载页前进到确认页：先从表格读取矩阵
        tableToMatrix();
        matrixToTable(m_tableConfirm);
        m_btnNext->setText(QStringLiteral("立即应用"));
    } else if (cur == 2) {
        // 确认页：发出信号并关闭
        emit matrixConfirmed(m_matrix);
        accept();
        return;
    }

    m_stack->setCurrentIndex(cur + 1);
    m_btnPrev->setEnabled(true);
    if (m_stack->currentIndex() == m_stack->count() - 1)
        m_btnNext->setText(QStringLiteral("立即应用"));
    else
        m_btnNext->setText(QStringLiteral("下一步"));
}

void HandEyeDialog::onPrev()
{
    const int cur = m_stack->currentIndex();
    m_stack->setCurrentIndex(cur - 1);
    m_btnPrev->setEnabled(m_stack->currentIndex() > 0);
    m_btnNext->setText(QStringLiteral("下一步"));
}

// ── 辅助 ─────────────────────────────────────────────────────

bool HandEyeDialog::parseFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QTextStream in(&f);
    QVector<float> vals;
    vals.reserve(16);

    while (!in.atEnd() && vals.size() < 16) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
        for (const QString &p : parts) {
            bool ok = false;
            const float v = p.toFloat(&ok);
            if (ok) vals.append(v);
        }
    }

    if (vals.size() < 16)
        return false;

    for (int i = 0; i < 16; ++i)
        m_matrix[i] = vals[i];
    return true;
}

void HandEyeDialog::tableToMatrix()
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            auto *item = m_tableLoad->item(r, c);
            if (item) {
                bool ok = false;
                const float v = item->text().toFloat(&ok);
                m_matrix[r * 4 + c] = ok ? v : m_matrix[r * 4 + c];
            }
        }
}

void HandEyeDialog::matrixToTable(QTableWidget *tbl)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            const float v = m_matrix[r * 4 + c];
            auto *item = new QTableWidgetItem(QString::number(static_cast<double>(v), 'f', 6));
            item->setTextAlignment(Qt::AlignCenter);
            tbl->setItem(r, c, item);
        }
}
