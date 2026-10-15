// 预览画布：把字格与字形轨迹画出来。
//
// 只负责绘制，不做任何生成逻辑。字形与版面由 MainWindow 传进来，
// 保证「画布上看到的」与「导出的 G 代码」用的是同一份数据。
//
// 笔压用线宽表达，因此预览能同时反映提按合成是否合理。
#pragma once

#include <QPointF>
#include <QWidget>

#include <vector>

#include "handwriting/layout.hpp"
#include "handwriting/trajectory.hpp"

class QMouseEvent;
class QWheelEvent;

class PreviewCanvas : public QWidget {
    Q_OBJECT

public:
    explicit PreviewCanvas(QWidget* parent = nullptr);

    // 设置要显示的内容。glyphs 与 page.cells 按书写顺序一一对应。
    void setContent(handwriting::layout::Page page,
                    std::vector<handwriting::StrokeSet> glyphs);
    void clearContent();

    void setShowCells(bool show);
    void setShowRules(bool show);
    [[nodiscard]] bool showCells() const noexcept { return show_cells_; }
    [[nodiscard]] bool showRules() const noexcept { return show_rules_; }

    void zoomToFit();
    [[nodiscard]] double zoom() const noexcept { return scale_; }

    [[nodiscard]] bool hasContent() const noexcept { return !glyphs_.empty(); }

signals:
    void zoomChanged(double scale);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void drawPaper(QPainter& painter);
    void drawRules(QPainter& painter);
    void drawCells(QPainter& painter);
    void drawGlyphs(QPainter& painter);
    void drawPlaceholder(QPainter& painter);

    handwriting::layout::Page page_;
    std::vector<handwriting::StrokeSet> glyphs_;

    double scale_{3.0};         // 像素 / 毫米
    QPointF origin_{0.0, 0.0};  // 页面左上角在控件坐标中的位置

    // 自动适应窗口。用户一旦手动缩放或平移就关闭，
    // 否则每次窗口大小变化都会把用户的视图冲掉。
    bool auto_fit_{true};

    bool show_cells_{true};
    bool show_rules_{true};

    bool panning_{false};
    QPoint last_mouse_;
};
