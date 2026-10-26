#include "notebookeditor.h"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QResizeEvent>
#include <QTextDocument>

NotebookEditor::NotebookEditor(QWidget* parent) : QPlainTextEdit(parent) {
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setFrameShape(QFrame::NoFrame);
    document()->setDocumentMargin(14.0);

    // 中文等宽字体：每个字占同样宽度，缩进/居中才能和版面一致
    QFont font(QStringLiteral("NSimSun"));
    font.setStyleHint(QFont::Monospace);
    font.setPointSizeF(14.0);
    setFont(font);
    setTabStopDistance(fontMetrics().horizontalAdvance(QChar(0x3000)));

    // 应用是深色主题，直接继承会得到「白字 + 米色纸」= 看不见。
    // 这里显式把纸面文字设为深色，选中态用浅蓝底。
    QPalette palette = viewport()->palette();
    palette.setColor(QPalette::Text, QColor(0x1a, 0x1a, 0x1a));
    palette.setColor(QPalette::PlaceholderText, QColor(0x9a, 0x9a, 0x9a));
    palette.setColor(QPalette::Highlight, QColor(0x67, 0xd4, 0xff));
    palette.setColor(QPalette::HighlightedText, QColor(0x10, 0x10, 0x10));
    viewport()->setPalette(palette);
    setPalette(palette);

    rebuildPaper();
}

void NotebookEditor::setCharsPerLine(int chars) {
    chars_per_line_ = chars > 0 ? chars : 15;
}

void NotebookEditor::rebuildPaper() {
    const QSize size = viewport()->size();
    if (size.isEmpty()) {
        return;
    }

    QPixmap paper(size);
    paper.fill(QColor(0xff, 0xfd, 0xf5));

    QPainter painter(&paper);
    const int line_height = fontMetrics().lineSpacing();
    const int top = static_cast<int>(document()->documentMargin());
    painter.setPen(QPen(QColor(0xe6, 0xe0, 0xd0), 1));
    for (int y = top + line_height; y < size.height(); y += line_height) {
        painter.drawLine(0, y, size.width(), y);
    }

    // 左侧页边线，提示「从这一列开始写」
    painter.setPen(QPen(QColor(0xf0, 0xc0, 0xc0), 1));
    const int margin_x = qMax(2, top - 4);
    painter.drawLine(margin_x, 0, margin_x, size.height());
    painter.end();

    QPalette palette = viewport()->palette();
    palette.setBrush(QPalette::Base, QBrush(paper));
    viewport()->setPalette(palette);
    viewport()->setAutoFillBackground(true);
}

void NotebookEditor::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    rebuildPaper();
}
