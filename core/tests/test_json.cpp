// JSON 解析器测试。

#include <string>

#include "check.hpp"
#include "handwriting/json.hpp"

using handwriting::json::ParseError;
using handwriting::json::Value;

namespace {

bool throws_parse_error(const std::string& text) {
    try {
        (void)handwriting::json::parse(text);
        return false;
    } catch (const ParseError&) {
        return true;
    }
}

}  // namespace

int main() {
    // ---- 基本类型 ----
    HR_CHECK(handwriting::json::parse("null").is_null());
    HR_CHECK(handwriting::json::parse("true").as_bool());
    HR_CHECK(!handwriting::json::parse("false").as_bool());
    HR_CHECK_NEAR(handwriting::json::parse("42").as_number(), 42.0);
    HR_CHECK_NEAR(handwriting::json::parse("-3.5e2").as_number(), -350.0);
    HR_CHECK(handwriting::json::parse("\"hi\"").as_string() == "hi");

    // ---- 数组与嵌套 ----
    const Value array = handwriting::json::parse("[1, 2, [3, 4]]");
    HR_CHECK(array.is_array());
    HR_CHECK(array.as_array().size() == 3);
    HR_CHECK_NEAR(array.at(0).as_number(), 1.0);
    HR_CHECK_NEAR(array.at(2).at(1).as_number(), 4.0);

    HR_CHECK(handwriting::json::parse("[]").as_array().empty());

    // ---- 对象 ----
    const Value object = handwriting::json::parse(R"({"a": 1, "b": {"c": "x"}})");
    HR_CHECK(object.is_object());
    HR_CHECK(object.find("a") != nullptr);
    HR_CHECK_NEAR(object.find("a")->as_number(), 1.0);
    HR_CHECK(object.find("b")->find("c")->as_string() == "x");
    HR_CHECK(object.find("missing") == nullptr);

    // ---- 空白容错 ----
    const Value spaced = handwriting::json::parse("  {\n\t\"k\" : [ 1 , 2 ]\n}  ");
    HR_CHECK(spaced.find("k")->as_array().size() == 2);

    // ---- 字符串转义 ----
    HR_CHECK(handwriting::json::parse(R"("a\"b")").as_string() == "a\"b");
    HR_CHECK(handwriting::json::parse(R"("a\\b")").as_string() == "a\\b");
    HR_CHECK(handwriting::json::parse(R"("a\nb")").as_string() == "a\nb");
    HR_CHECK(handwriting::json::parse(R"("\u4E2D")").as_string() == "中");
    // 代理对：U+1F600 应编码成 4 字节 UTF-8
    HR_CHECK(handwriting::json::parse(R"("\uD83D\uDE00")").as_string().size() == 4);

    // ---- 错误处理 ----
    HR_CHECK(throws_parse_error("{"));
    HR_CHECK(throws_parse_error("[1,]"));
    HR_CHECK(throws_parse_error("{\"a\" 1}"));
    HR_CHECK(throws_parse_error("tru"));
    HR_CHECK(throws_parse_error(""));
    HR_CHECK(throws_parse_error("{} extra"));
    HR_CHECK(throws_parse_error("\"unterminated"));

    // ---- 类型不匹配时抛出，而不是返回垃圾值 ----
    bool threw = false;
    try {
        (void)handwriting::json::parse("1").as_string();
    } catch (const ParseError&) {
        threw = true;
    }
    HR_CHECK(threw);

    threw = false;
    try {
        (void)handwriting::json::parse("[1]").at(5);
    } catch (const ParseError&) {
        threw = true;
    }
    HR_CHECK(threw);

    // ---- 真实字形数据的结构 ----
    const Value glyph = handwriting::json::parse(
        R"({"strokes": ["M 0 0"], "medians": [[[428, 824], [503, 781]], [[300, 300]]], "radStrokes": [0]})");
    const Value* medians = glyph.find("medians");
    HR_CHECK(medians != nullptr && medians->is_array());
    HR_CHECK(medians->as_array().size() == 2);
    HR_CHECK_NEAR(medians->at(0).at(0).at(0).as_number(), 428.0);
    HR_CHECK_NEAR(medians->at(1).at(0).at(1).as_number(), 300.0);

    return hrtest::summary("json");
}
