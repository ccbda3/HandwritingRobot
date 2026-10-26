// 横条本编辑器：在带横线的纸面上编辑文本。
//
// 背景用 QPalette::Base 画一张与行高对齐的横线纸，文字直接压在上面；
// 这样编辑时看到的就是最终版面的样子，不用在「纯文本框」和「预览」之间来回猜。
#pragma once

#include <QPlainTextEdit>

class QResizeEvent;

class NotebookEditor : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit NotebookEditor(QWidget* parent = nullptr);

    void setCharsPerLine(int chars);
    [[nodiscard]] int charsPerLine() const noexcept { return chars_per_line_; }

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void rebuildPaper();

    int chars_per_line_{15};
};
