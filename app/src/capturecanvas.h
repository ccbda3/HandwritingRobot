// 原生采集画布：数位板 / 鼠标写字，产出归一化笔画。
//
// 与网页采集器同一套坐标约定：原点在书写区左上角，x 向右，y 向下，0~1。
// 压感来自 QTabletEvent::pressure()；鼠标固定 0.5。
#pragma once

#include <QElapsedTimer>
#include <QWidget>

#include <vector>

#include "handwriting/trajectory.hpp"

class QMouseEvent;
class QPaintEvent;
class QPainter;
class QTabletEvent;

class CaptureCanvas : public QWidget {
    Q_OBJECT

public:
    explicit CaptureCanvas(QWidget* parent = nullptr);

    void setGuide(const QString& text, bool lineMode);
    void clear();
    void undoStroke();
    // 仅供快照/自测：注入一条演示笔画
    void setDemoStroke();

    [[nodiscard]] const std::vector<handwriting::Stroke>& strokes() const noexcept {
        return strokes_;
    }
    [[nodiscard]] bool pressureSupported() const noexcept { return pressure_supported_; }
    [[nodiscard]] int samplingRate() const noexcept;

signals:
    void strokesChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void tabletEvent(QTabletEvent* event) override;

private:
    [[nodiscard]] QRectF writingRect() const;
    [[nodiscard]] QPointF normalize(const QPointF& pos) const;
    void beginStroke(const QPointF& pos, double pressure);
    void extendStroke(const QPointF& pos, double pressure);
    void endStroke();
    void drawStroke(QPainter& painter,
                    const std::vector<handwriting::Point>& points) const;
    void drawGuide(QPainter& painter) const;

    QString guide_text_;
    bool line_mode_{false};
    std::vector<handwriting::Stroke> strokes_;
    std::vector<handwriting::Point> current_points_;
    bool drawing_{false};
    bool using_tablet_{false};
    bool pressure_supported_{false};
    QElapsedTimer clock_;
    bool clock_started_{false};
    QPointF last_pos_;
};
