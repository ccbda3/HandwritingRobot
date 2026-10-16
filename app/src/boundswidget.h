// 工作区边界示意图：实时显示机头位置与已标定的边界线。
//
// 这是软限位的可视化端。坐标系沿用机床约定：X 向右、Y 向上（远离操作者）。
// 画布按矩形自适应缩放并居中，保持长宽比，所以无论标定出的矩形是什么比例
// 都不会变形。机头一旦落到边界外会立刻变成红色告警色。
#pragma once

#include <QWidget>

#include <optional>

#include "handwriting/bounds.hpp"

class BoundsWidget : public QWidget {
    Q_OBJECT

public:
    explicit BoundsWidget(QWidget* parent = nullptr);

    void setWorkspace(const handwriting::bounds::Workspace& ws);
    void setHead(double x, double y);
    /// 当前正在标定的角（高亮提示）；传 nullopt 表示标定结束/不在标定时。
    void setActiveCorner(std::optional<handwriting::bounds::Corner> corner);

    [[nodiscard]] QSize minimumSizeHint() const override { return {220, 160}; }
    [[nodiscard]] QSize sizeHint() const override { return {260, 180}; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    /// 把机床坐标映射到控件像素，处理缩放、居中与 Y 轴翻转。
    struct Transform {
        double scale{1.0};
        double ox{0.0};
        double oy{0.0};
    };
    [[nodiscard]] Transform computeTransform() const;
    [[nodiscard]] QPointF toScreen(double x, double y, const Transform& t) const;
    [[nodiscard]] bool headOutside() const;

    handwriting::bounds::Workspace workspace_;
    std::optional<std::pair<double, double>> head_;
    std::optional<handwriting::bounds::Corner> active_corner_;
};
