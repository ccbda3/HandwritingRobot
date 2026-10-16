#include "boundswidget.h"

#include <QPainter>
#include <QPaintEvent>

#include <algorithm>
#include <cmath>

namespace {

// 暗色主题下的配色（与 IDE 暗色主题约定保持一致）。
constexpr QRgb kBackground = 0x1e1e24;
constexpr QRgb kBorder = 0x3ddc97;       // 允许范围边界：绿
constexpr QRgb kBorderWarn = 0xff6b6b;    // 机头越界：红
constexpr QRgb kHead = 0x4aa3ff;          // 机头
constexpr QRgb kHeadWarn = 0xff6b6b;      // 越界机头
constexpr QRgb kCorner = 0x8a8a96;        // 角标记
constexpr QRgb kCornerActive = 0xffd166;   // 当前待记录的角
constexpr QRgb kGrid = 0x2a2a32;
constexpr QRgb kText = 0xb8b8c0;

const QString corner_label(handwriting::bounds::Corner corner) {
    using handwriting::bounds::Corner;
    switch (corner) {
        case Corner::TopLeft: return QStringLiteral("左上");
        case Corner::TopRight: return QStringLiteral("右上");
        case Corner::BottomRight: return QStringLiteral("右下");
        case Corner::BottomLeft: return QStringLiteral("左下");
    }
    return QString();
}

// 选一个"好看"的网格步长（1/2/5 × 10^n）。
double nice_step(double span) {
    if (span <= 0.0) {
        return 1.0;
    }
    const double raw = span / 5.0;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double norm = raw / mag;
    double step;
    if (norm < 1.5) {
        step = 1.0;
    } else if (norm < 3.5) {
        step = 2.0;
    } else if (norm < 7.5) {
        step = 5.0;
    } else {
        step = 10.0;
    }
    return step * mag;
}

}  // namespace

BoundsWidget::BoundsWidget(QWidget* parent) : QWidget(parent) {
    setAutoFillBackground(true);
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, QColor::fromRgb(kBackground));
    setPalette(palette);
}

void BoundsWidget::setWorkspace(const handwriting::bounds::Workspace& ws) {
    workspace_ = ws;
    update();
}

void BoundsWidget::setHead(double x, double y) {
    head_ = std::make_pair(x, y);
    update();
}

void BoundsWidget::setActiveCorner(std::optional<handwriting::bounds::Corner> corner) {
    active_corner_ = corner;
    update();
}

BoundsWidget::Transform BoundsWidget::computeTransform() const {
    Transform t;
    const double min_x = workspace_.valid ? workspace_.min_x : 0.0;
    const double max_x = workspace_.valid ? workspace_.max_x : 100.0;
    const double min_y = workspace_.valid ? workspace_.min_y : 0.0;
    const double max_y = workspace_.valid ? workspace_.max_y : 100.0;

    // 留 12% 边距，让边界线不贴到控件边缘
    const double pad = 0.12;
    const double data_w = std::max(1.0, max_x - min_x);
    const double data_h = std::max(1.0, max_y - min_y);
    const double view_w = data_w * (1.0 + pad * 2.0);
    const double view_h = data_h * (1.0 + pad * 2.0);

    const double cw = static_cast<double>(width());
    const double ch = static_cast<double>(height());
    t.scale = std::min(cw / view_w, ch / view_h);

    // 数据原点（min_x, min_y）映射到控件居中区域；Y 翻转：机床 Y 向上，屏幕 y 向下
    const double drawn_w = data_w * t.scale;
    const double drawn_h = data_h * t.scale;
    t.ox = (cw - drawn_w) / 2.0 - min_x * t.scale;
    t.oy = (ch - drawn_h) / 2.0 + max_y * t.scale;
    return t;
}

QPointF BoundsWidget::toScreen(double x, double y, const Transform& t) const {
    return QPointF(t.ox + x * t.scale, t.oy - y * t.scale);
}

bool BoundsWidget::headOutside() const {
    if (!head_.has_value() || !workspace_.valid) {
        return false;
    }
    return !workspace_.contains(head_->first, head_->second, 0.0);
}

void BoundsWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const Transform t = computeTransform();

    // ---- 网格 ----
    if (workspace_.valid) {
        painter.setPen(QPen(QColor::fromRgb(kGrid), 1));
        const double step_x = nice_step(workspace_.width());
        const double step_y = nice_step(workspace_.height());
        for (double x = std::ceil(workspace_.min_x / step_x) * step_x; x <= workspace_.max_x;
             x += step_x) {
            painter.drawLine(toScreen(x, workspace_.min_y, t), toScreen(x, workspace_.max_y, t));
        }
        for (double y = std::ceil(workspace_.min_y / step_y) * step_y; y <= workspace_.max_y;
             y += step_y) {
            painter.drawLine(toScreen(workspace_.min_x, y, t), toScreen(workspace_.max_x, y, t));
        }
    }

    // ---- 边界矩形 ----
    const QPointF tl = toScreen(workspace_.valid ? workspace_.min_x : 0.0,
                                workspace_.valid ? workspace_.max_y : 100.0, t);
    const QPointF br = toScreen(workspace_.valid ? workspace_.max_x : 100.0,
                                workspace_.valid ? workspace_.min_y : 0.0, t);
    const QRectF rect = QRectF(tl, br).normalized();
    if (workspace_.valid) {
        QPen border_pen(QColor::fromRgb(kBorder), 2);
        border_pen.setStyle(Qt::DashLine);
        painter.setPen(border_pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect);
    }

    // ---- 角标记 ----
    if (workspace_.valid) {
        const auto corners = workspace_.corners();
        for (int i = 0; i < 4; ++i) {
            const auto corner = static_cast<handwriting::bounds::Corner>(i);
            const QPointF p = toScreen(corners[i].first, corners[i].second, t);
            const bool active = active_corner_.has_value() && *active_corner_ == corner;
            painter.setPen(QPen(QColor::fromRgb(active ? kCornerActive : kCorner), active ? 2 : 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(p.x() - 4, p.y() - 4, 8, 8));
            if (active) {
                painter.setPen(QColor::fromRgb(kCornerActive));
                painter.drawText(p.x() + 8, p.y() - 6,
                                corner_label(corner) + QStringLiteral(" ←记录"));
            }
        }
    }

    // ---- 机头 ----
    if (head_.has_value()) {
        const QPointF p = toScreen(head_->first, head_->second, t);
        const bool outside = headOutside();
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor::fromRgb(outside ? kHeadWarn : kHead));
        painter.drawEllipse(p, 5, 5);
        painter.setPen(QColor::fromRgb(kText));
        painter.setBrush(Qt::NoBrush);
        painter.drawText(p.x() + 9, p.y() + 4,
                        QStringLiteral("X %1  Y %2")
                            .arg(head_->first, 0, 'f', 1)
                            .arg(head_->second, 0, 'f', 1));
        if (outside) {
            painter.setPen(QColor::fromRgb(kBorderWarn));
            painter.drawText(8, height() - 10, QStringLiteral("⚠ 机头已超出边界！"));
        }
    } else {
        painter.setPen(QColor::fromRgb(kText));
        painter.drawText(8, height() - 10, QStringLiteral("未连接或暂无位置"));
    }
}
