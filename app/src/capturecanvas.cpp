#include "capturecanvas.h"

#include <QColor>
#include <QFont>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QTabletEvent>

#include <algorithm>
#include <cmath>

CaptureCanvas::CaptureCanvas(QWidget* parent) : QWidget(parent) {
    setMinimumSize(600, 360);
    setTabletTracking(true);
    setCursor(Qt::CrossCursor);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
}

void CaptureCanvas::setGuide(const QString& text, bool lineMode) {
    guide_text_ = text;
    line_mode_ = lineMode;
    update();
}

void CaptureCanvas::clear() {
    strokes_.clear();
    current_points_.clear();
    drawing_ = false;
    clock_started_ = false;
    emit strokesChanged();
    update();
}

void CaptureCanvas::setDemoStroke() {
    clear();
    std::vector<handwriting::Point> points;
    points.reserve(24);
    for (int i = 0; i < 24; ++i) {
        const double t = i / 23.0;
        points.push_back(handwriting::Point{
            0.2 + 0.6 * t, 0.2 + 0.6 * t,
            0.45 + 0.35 * std::sin(t * 3.14159265358979323846), i * 8});
    }
    strokes_.emplace_back(points);
    emit strokesChanged();
    update();
}

void CaptureCanvas::undoStroke() {
    if (drawing_) {
        drawing_ = false;
        current_points_.clear();
    } else if (!strokes_.empty()) {
        strokes_.pop_back();
    }
    emit strokesChanged();
    update();
}

QRectF CaptureCanvas::writingRect() const {
    return QRectF(rect()).adjusted(20.0, 20.0, -20.0, -20.0);
}

QPointF CaptureCanvas::normalize(const QPointF& pos) const {
    const QRectF area = writingRect();
    const double x = (pos.x() - area.left()) / std::max(1.0, area.width());
    const double y = (pos.y() - area.top()) / std::max(1.0, area.height());
    return QPointF(std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0));
}

int CaptureCanvas::samplingRate() const noexcept {
    int points = 0;
    int first_t = 0;
    int last_t = 0;
    bool have = false;
    for (const auto& stroke : strokes_) {
        for (const auto& point : stroke.points()) {
            if (!have) {
                first_t = point.t;
                have = true;
            }
            last_t = point.t;
            ++points;
        }
    }
    if (points < 2 || last_t <= first_t) {
        return 120;
    }
    return std::clamp(points * 1000 / (last_t - first_t), 30, 500);
}

void CaptureCanvas::beginStroke(const QPointF& pos, double pressure) {
    if (!clock_started_) {
        clock_.start();
        clock_started_ = true;
    }
    const QPointF normalized = normalize(pos);
    current_points_.clear();
    current_points_.push_back(handwriting::Point{
        normalized.x(), normalized.y(), std::clamp(pressure, 0.0, 1.0),
        static_cast<int>(clock_.elapsed())});
    drawing_ = true;
    last_pos_ = pos;
    update();
}

void CaptureCanvas::extendStroke(const QPointF& pos, double pressure) {
    if (!drawing_) {
        return;
    }
    if (QLineF(last_pos_, pos).length() < 2.0) {
        return;
    }
    const QPointF normalized = normalize(pos);
    current_points_.push_back(handwriting::Point{
        normalized.x(), normalized.y(), std::clamp(pressure, 0.0, 1.0),
        static_cast<int>(clock_.elapsed())});
    last_pos_ = pos;
    update();
}

void CaptureCanvas::endStroke() {
    if (!drawing_) {
        return;
    }
    drawing_ = false;
    if (!current_points_.empty()) {
        strokes_.emplace_back(current_points_);
        current_points_.clear();
        emit strokesChanged();
    }
    update();
}

void CaptureCanvas::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(0xff, 0xfd, 0xf5));

    const QRectF area = writingRect();
    painter.setPen(QPen(QColor(0xd8, 0xd2, 0xc4), 1));
    painter.drawRect(area);
    painter.setPen(QPen(QColor(0xee, 0xe7, 0xd8), 1));
    for (double y = area.top() + 32.0; y < area.bottom(); y += 32.0) {
        painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
    }

    drawGuide(painter);
    for (const auto& stroke : strokes_) {
        drawStroke(painter, stroke.points());
    }
    if (drawing_) {
        drawStroke(painter, current_points_);
    }
}

void CaptureCanvas::drawGuide(QPainter& painter) const {
    if (guide_text_.isEmpty()) {
        return;
    }
    const QRectF area = writingRect();
    QFont font(QStringLiteral("NSimSun"));
    font.setStyleHint(QFont::Monospace);
    painter.setPen(QColor(0xe3, 0xdc, 0xcc));

    if (line_mode_) {
        const int count = std::max(1, static_cast<int>(guide_text_.size()));
        const double cell = area.width() / count;
        font.setPixelSize(std::clamp(static_cast<int>(cell * 0.78), 12, 160));
        painter.setFont(font);
        for (int i = 0; i < guide_text_.size(); ++i) {
            const QRectF box(area.left() + i * cell, area.top(), cell, area.height());
            painter.drawText(box, Qt::AlignCenter, guide_text_.mid(i, 1));
        }
        return;
    }

    font.setPixelSize(std::clamp(
        static_cast<int>(std::min(area.width(), area.height()) * 0.75), 24, 400));
    painter.setFont(font);
    painter.drawText(area, Qt::AlignCenter, guide_text_.left(1));
}

void CaptureCanvas::drawStroke(QPainter& painter,
                               const std::vector<handwriting::Point>& points) const {
    if (points.empty()) {
        return;
    }
    const QRectF area = writingRect();
    const auto map = [&area](const handwriting::Point& point) {
        return QPointF(area.left() + point.x * area.width(),
                       area.top() + point.y * area.height());
    };

    if (points.size() == 1) {
        painter.setPen(QPen(QColor(0x1a, 0x1a, 0x1a), 2.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawPoint(map(points.front()));
        return;
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        QPen pen(QColor(0x1a, 0x1a, 0x1a));
        pen.setWidthF(1.2 + 3.0 * std::clamp(points[i].pressure, 0.0, 1.0));
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.drawLine(map(points[i - 1]), map(points[i]));
    }
}

void CaptureCanvas::mousePressEvent(QMouseEvent* event) {
    if (using_tablet_ || event->button() != Qt::LeftButton) {
        return;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    beginStroke(event->position(), 0.5);
#else
    beginStroke(event->localPos(), 0.5);
#endif
    event->accept();
}

void CaptureCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (using_tablet_ || !drawing_) {
        return;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    extendStroke(event->position(), 0.5);
#else
    extendStroke(event->localPos(), 0.5);
#endif
    event->accept();
}

void CaptureCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (using_tablet_ || event->button() != Qt::LeftButton) {
        return;
    }
    endStroke();
    event->accept();
}

void CaptureCanvas::tabletEvent(QTabletEvent* event) {
    using_tablet_ = true;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPointF pos = event->position();
#else
    const QPointF pos = event->posF();
#endif
    const double pressure = event->pressure() > 0.01 ? event->pressure() : 0.5;
    if (event->pressure() > 0.01) {
        pressure_supported_ = true;
    }
    switch (event->type()) {
        case QEvent::TabletPress:
            beginStroke(pos, pressure);
            break;
        case QEvent::TabletMove:
            extendStroke(pos, pressure);
            break;
        case QEvent::TabletRelease:
            endStroke();
            break;
        default:
            break;
    }
    event->accept();
}
