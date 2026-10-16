#include "handwriting/bounds.hpp"

#include <cstdio>

#include <array>
#include <filesystem>

#include "check.hpp"  // 测试辅助宏（HR_CHECK / HR_CHECK_NEAR）

namespace {

using handwriting::bounds::Corner;
using handwriting::bounds::Workspace;
using Pair = std::pair<double, double>;

// 一个顺时针矩形：左上(0,100)→右上(200,100)→右下(200,0)→左下(0,0)
std::array<Pair, 4> square_corners() {
    return {Pair{0.0, 100.0}, Pair{200.0, 100.0}, Pair{200.0, 0.0}, Pair{0.0, 0.0}};
}

}  // namespace

int main() {
    {
        // 四个角正常构造矩形
        const Workspace ws = handwriting::bounds::from_corners(square_corners());
        HR_CHECK(ws.valid);
        HR_CHECK_NEAR(ws.min_x, 0.0);
        HR_CHECK_NEAR(ws.max_x, 200.0);
        HR_CHECK_NEAR(ws.min_y, 0.0);
        HR_CHECK_NEAR(ws.max_y, 100.0);
        HR_CHECK_NEAR(ws.width(), 200.0);
        HR_CHECK_NEAR(ws.height(), 100.0);
    }

    {
        // 角顺序打乱也应得到同样的矩形（取 min/max 归一化）
        const std::array<Pair, 4> shuffled = {Pair{200.0, 0.0}, Pair{0.0, 0.0},
                                              Pair{200.0, 100.0}, Pair{0.0, 100.0}};
        const Workspace ws = handwriting::bounds::from_corners(shuffled);
        HR_CHECK(ws.valid);
        HR_CHECK_NEAR(ws.min_x, 0.0);
        HR_CHECK_NEAR(ws.max_x, 200.0);
        HR_CHECK_NEAR(ws.min_y, 0.0);
        HR_CHECK_NEAR(ws.max_y, 100.0);
    }

    {
        // 四个点重合 → 不是有效矩形
        const std::array<Pair, 4> same = {Pair{5.0, 5.0}, Pair{5.0, 5.0}, Pair{5.0, 5.0},
                                          Pair{5.0, 5.0}};
        const Workspace ws = handwriting::bounds::from_corners(same);
        HR_CHECK(!ws.valid);
    }

    {
        // contains：中心在内，边界在内（margin=0），外面在外
        const Workspace ws = handwriting::bounds::from_corners(square_corners());
        HR_CHECK(ws.contains(100.0, 50.0, 0.0));
        HR_CHECK(ws.contains(0.0, 0.0, 0.0));       // 左下属边界，算在界内
        HR_CHECK(ws.contains(200.0, 100.0, 0.0));   // 右上也算在界内
        HR_CHECK(!ws.contains(-1.0, 50.0, 0.0));
        HR_CHECK(!ws.contains(201.0, 50.0, 0.0));
        HR_CHECK(!ws.contains(100.0, 101.0, 0.0));
    }

    {
        // margin 收缩：缩小允许范围
        const Workspace ws = handwriting::bounds::from_corners(square_corners());
        HR_CHECK(ws.contains(1.0, 50.0, 0.5));     // 离左边界 1mm > 0.5mm 余量
        HR_CHECK(!ws.contains(0.2, 50.0, 0.5));    // 离左边界 0.2mm < 0.5mm 余量
        HR_CHECK(ws.contains(199.2, 50.0, 0.5));   // 离右边界 0.8mm > 0.5mm 余量，在界内
        HR_CHECK(!ws.contains(199.6, 50.0, 0.5));  // 离右边界 0.4mm < 0.5mm 余量，越界
        HR_CHECK(!ws.contains(100.0, 0.3, 0.5));   // 离下边界 0.3mm < 0.5mm 余量，越界
    }

    {
        // violation 文本：返回越界原因，界内返回空
        const Workspace ws = handwriting::bounds::from_corners(square_corners());
        HR_CHECK(ws.violation(100.0, 50.0, 0.0).empty());
        const std::string v = ws.violation(-3.0, 250.0, 0.0);
        HR_CHECK(!v.empty());
        HR_CHECK(v.find("X") != std::string::npos);
        HR_CHECK(v.find("Y") != std::string::npos);
    }

    {
        // 未标定工作区：contains 一律 false，violation 报"未标定"
        Workspace ws;
        ws.valid = false;
        HR_CHECK(!ws.contains(0.0, 0.0, 0.0));
        HR_CHECK(!ws.violation(0.0, 0.0, 0.0).empty());
    }

    {
        // 持久化往返
        const Workspace in = handwriting::bounds::from_corners(square_corners());
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "hr-bounds-test.json";
        handwriting::bounds::save(in, path);
        const Workspace out = handwriting::bounds::load(path);
        HR_CHECK(out.valid);
        HR_CHECK_NEAR(out.min_x, in.min_x);
        HR_CHECK_NEAR(out.max_x, in.max_x);
        HR_CHECK_NEAR(out.min_y, in.min_y);
        HR_CHECK_NEAR(out.max_y, in.max_y);
        std::filesystem::remove(path);
    }

    {
        // 读取不存在的文件 → invalid
        const Workspace out = handwriting::bounds::load(
            std::filesystem::temp_directory_path() / "hr-bounds-does-not-exist.json");
        HR_CHECK(!out.valid);
    }

    {
        // from_program_bounds：左上角对齐到原点
        const Workspace ws =
            handwriting::bounds::from_program_bounds(0.0, 0.0, 150.0, 213.0);
        HR_CHECK(ws.valid);
        HR_CHECK_NEAR(ws.min_x, 0.0);
        HR_CHECK_NEAR(ws.max_x, 150.0);
        HR_CHECK_NEAR(ws.min_y, 0.0);
        HR_CHECK_NEAR(ws.max_y, 213.0);
    }

    return hrtest::summary("bounds");
}
