#pragma once

#include <QString>
#include <QTextStream>
#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QByteArrayView>
#include <QStringConverter>
#else
#include <QByteArray>
using QByteArrayView = QByteArray;
#endif

inline void setTextStreamUtf8(QTextStream &stream)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    stream.setEncoding(QStringConverter::Utf8);
#else
    stream.setCodec("UTF-8");
#endif
}

inline auto qtSkipEmptyParts()
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    return Qt::SkipEmptyParts;
#else
    return QString::SkipEmptyParts;
#endif
}

#if defined(_MSC_VER) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#undef QStringLiteral
// Qt 5.12 + MSVC 2017 的 u"" 字面量会按本地代码页解码源文件。
// 项目源码统一为 UTF-8，因此在该组合下显式按 UTF-8 构造字符串。
#define QStringLiteral(str) QString::fromUtf8(str)
#endif
