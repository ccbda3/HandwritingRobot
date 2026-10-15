#include "previewcanvas.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QPen>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kMinScale = 0.5;
constexpr double kMaxScale = 40.0;

const QColor kDesk(0x2B, 0x2B, 0x31);
const QColor kPaper(0xFF, 0xFF, 0xFF);
const QColor kRule(0xC8, 0xD4, 0xE0);
const QColor kCell(0xE4, 0xE8, 0xEE);
const QColor kInk(0x14, 0x14, 0x14);
const QColor kHint(0x8A, 0x8A, 0x96);

// 笔压到线宽的映射。线宽单位是毫米，随缩放自动放大。
constexpr double kBaseWidthMm = 0.28;
constexpr double kPressureWidthMm = 0.42;

// 宽度量化级数。把相近的笔压合并成一段，避免每个采样点都生成一个图元 ——
// 20 个字约 6000 个采样点，逐段绘制既慢又看不出差异。
constexpr int kWidthLevels = 8;

int width_level(double pressure) {
    const double clamped = std::min(1.0, std::max(0.0, pressure));
    return std::min(kWidthLevels - 1,
                    static_cast<int>(std::lround(clamped * (kWidthLevels - 1))));
}

double width_mm(int level) {
    return kBaseWidthMm + kPressureWidthMm * (static_cast<double>(level) / (kWidthLevels - 1));
}

// QMouseEvent::pos() 在 Qt6 中已废弃，而 position() 是 Qt6 才有的。
// 项目同时支持 Qt5.15 与 Qt6，因此这里做一次显式分支。
QPoint event_position(const QMouseEvent* event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position().toPoint();
#else
    return event->pos();
#endif
}

}  // namespace

PreviewCanvas::PreviewCanvas(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 240);
    setAutoFillBackground(false);
    setCursor(Qt::OpenHandCursor);
    setFocusPolicy(Qt::StrongFocus);
}

void PreviewCanvas::setContent(handwriting::layout::Page page,
                               std::vector<handwriting::StrokeSet> glyphs) {
    const bool page_changed = std::fabs(page_.width_mm - page.width_mm) > 1e-9 ||
                              std::fabs(page_.height_mm - page.height_mm) > 1e-9;
    page_ = std::move(page);
    glyphs_ = std::move(glyphs);
    if (page_changed || scale_ <= kMinScale) {
        zoomToFit();
    }
    update();
}

void PreviewCanvas::clearContent() {
    page_ = {};
    glyphs_.clear();
    update();
}

void PreviewCanvas::setShowCells(bool show) {
    show_cells_ = show;
    update();
}

void PreviewCanvas::setShowRules(bool show) {
    show_rules_ = show;
    update();
}

void PreviewCanvas::zoomToFit() {
    if (page_.width_mm <= 0.0 || page_.height_mm <= 0.0) {
        return;
    }
    const double margin = 16.0;
    const double available_w = std::max(1.0, static_cast<double>(width()) - margin * 2.0);
    const double available_h = std::max(1.0, static_cast<double>(height()) - margin * 2.0);
    scale_ = std::min(available_w / page_.width_mm, available_h / page_.height_mm);
    scale_ = std::min(kMaxScale, std::max(kMinScale, scale_));

    origin_.setX((static_cast<double>(width()) - page_.width_mm * scale_) / 2.0);
    origin_.setY((static_cast<double>(height()) - page_.height_mm * scale_) / 2.0);

    auto_fit_ = true;
    emit zoomChanged(scale_);
    update();
}

void PreviewCanvas::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // 控件初始尺寸是 minimumSize，首次刷新时算出来的缩放会偏小。
    // 布局稳定后必须重算一次，否则页面又小又不居中。
    if (auto_fit_) {
        zoomToFit();
    }
}

void PreviewCanvas::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), kDesk);

    if (page_.width_mm <= 0.0) {
        drawPlaceholder(painter);
        return;
    }

    drawPaper(painter);
    drawRules(painter);
    drawCells(painter);
    drawGlyphs(painter);
}

void PreviewCanvas::drawPlaceholder(QPainter& painter) {
    painter.setPen(kHint);
    QFont font = painter.font();
    font.setPointSizeF(11.0);
    painter.setFont(font);
    painter.drawText(rect(), Qt::AlignCenter,
                     QStringLiteral("在左侧输入文字并确保字形已预取\n"
                                    "命令行执行：handwriting preload \"你的文字\""));
}

void PreviewCanvas::drawPaper(QPainter& painter) {
    const QRectF paper(origin_.x(), origin_.y(), page_.width_mm * scale_,
                       page_.height_mm * scale_);
    painter.fillRect(paper, kPaper);
    painter.setPen(QPen(QColor(0x55, 0x55, 0x5E), 1.0));
    painter.drawRect(paper);
}

void PreviewCanvas::drawRules(QPainter& painter) {
    if (!show_rules_ || page_.rule_lines_mm.empty()) {
        return;
    }
    // 缩得太小时横线会糊成一片，反而看不清字
    if (scale_ < 2.0) {
        return;
    }
    QPen pen(kRule);
    pen.setWidthF(1.0);
    painter.setPen(pen);
    for (const double y_mm : page_.rule_lines_mm) {
        const double y = origin_.y() + y_mm * scale_;
        painter.drawLine(QPointF(origin_.x(), y),
                         QPointF(origin_.x() + page_.width_mm * scale_, y));
    }
}

void PreviewCanvas::drawCells(QPainter& painter) {
    if (!show_cells_ || scale_ < 3.0) {
        return;
    }
    QPen pen(kCell);
    pen.setWidthF(1.0);
    pen.setStyle(Qt::DotLine);
    painter.setPen(pen);
    for (const handwriting::layout::Cell& cell : page_.cells) {
        painter.drawRect(QRectF(origin_.x() + cell.x_mm * scale_,
                                origin_.y() + cell.y_mm * scale_,
                                cell.size_mm * scale_, cell.size_mm * scale_));
    }
}

void PreviewCanvas::drawGlyphs(QPainter& painter) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(kInk);

    const std::size_t count = std::min(glyphs_.size(), page_.cells.size());
    for (std::size_t index = 0; index < count; ++index) {
        const handwriting::layout::Cell& cell = page_.cells[index];
        const handwriting::StrokeSet& glyph = glyphs_[index];

        const auto to_screen = [&](const handwriting::Point& point) {
            return QPointF(origin_.x() + (cell.x_mm + point.x * cell.size_mm) * scale_,
                           origin_.y() + (cell.y_mm + point.y * cell.size_mm) * scale_);
        };

        for (const handwriting::Stroke& stroke : glyph.strokes()) {
            const std::vector<handwriting::Point>& points = stroke.points();
            if (points.empty()) {
                continue;
            }

            if (points.size() == 1) {
                // width_mm 已经是毫米，乘 scale_ 即得像素，不要再乘字宽
                const double radius =
                    std::max(0.8, width_mm(width_level(points.front().pressure)) * scale_ / 2.0);
                painter.drawEllipse(to_screen(points.front()), radius, radius);
                continue;
            }

            // 按量化后的笔压把笔画切成若干段，每段一个 polyline
            std::vector<QPointF> segment;
            segment.push_back(to_screen(points.front()));
            int level = width_level(points.front().pressure);

            const auto flush = [&]() {
                if (segment.size() < 2) {
                    segment.clear();
                    return;
                }
                QPen pen(kInk);
                pen.setWidthF(std::max(0.8, width_mm(level) * scale_));
                pen.setCapStyle(Qt::RoundCap);
                pen.setJoinStyle(Qt::RoundJoin);
                painter.setPen(pen);
                painter.drawPolyline(segment.data(), static_cast<int>(segment.size()));
                segment.clear();
            };

            for (std::size_t i = 1; i < points.size(); ++i) {
                const int next_level =
                    width_level((points[i - 1].pressure + points[i].pressure) / 2.0);
                if (next_level != level && segment.size() > 1) {
                    const QPointF bridge = segment.back();
                    flush();
                    level = next_level;
                    segment.push_back(bridge);
                }
                segment.push_back(to_screen(points[i]));
            }
            flush();
        }
    }
    painter.setPen(Qt::NoPen);
}

void PreviewCanvas::wheelEvent(QWheelEvent* event) {
    if (page_.width_mm <= 0.0) {
        return;
    }
    const double steps = static_cast<double>(event->angleDelta().y()) / 120.0;
    if (std::fabs(steps) < 1e-9) {
        return;
    }

    // 以光标为锚点缩放：光标下的页面位置保持不动
    const QPointF cursor = event->position();
    const QPointF page_point((cursor.x() - origin_.x()) / scale_,
                             (cursor.y() - origin_.y()) / scale_);

    const double factor = std::pow(1.15, steps);
    const double next = std::min(kMaxScale, std::max(kMinScale, scale_ * factor));
    if (std::fabs(next - scale_) < 1e-9) {
        return;
    }
    scale_ = next;
    origin_.setX(cursor.x() - page_point.x() * scale_);
    origin_.setY(cursor.y() - page_point.y() * scale_);

    auto_fit_ = false;  // 用户已手动缩放，不再跟随窗口大小
    emit zoomChanged(scale_);
    update();
    event->accept();
}

void PreviewCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && page_.width_mm > 0.0) {
        panning_ = true;
        auto_fit_ = false;  // 用户已手动平移，不再跟随窗口大小
        last_mouse_ = event_position(event);
        setCursor(Qt::ClosedHandCursor);
    }
}

void PreviewCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (!panning_) {
        return;
    }
    const QPoint position = event_position(event);
    const QPoint delta = position - last_mouse_;
    last_mouse_ = position;
    origin_ += QPointF(delta.x(), delta.y());
    update();
}

void PreviewCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        panning_ = false;
        setCursor(Qt::OpenHandCursor);
    }
}
