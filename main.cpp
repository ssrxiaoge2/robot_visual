/**
 * @file main.cpp
 * @brief 程序入口
 *
 * 创建 QApplication 和主窗口，进入 Qt 事件循环。
 * 所有业务逻辑均在 MainWindow 及其下属组件中完成。
 */

#include "mainwindow.h"
#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);  // 初始化 Qt 应用程序（事件循环、字体、样式等）
    MainWindow w;
    w.show();
    return QApplication::exec(); // 进入事件循环，直到主窗口关闭才返回
}
