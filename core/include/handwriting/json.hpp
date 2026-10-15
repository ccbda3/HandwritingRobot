// 极简 JSON 解析器。
//
// 为什么自己写而不是引第三方：核心库目前零依赖，这带来三个实际好处 ——
// 构建快、无需包管理、不会因为某个库的许可或 ABI 变化而卡住。
// 代价是这个解析器只覆盖我们真正需要的 JSON 子集。
//
// 支持：object / array / string（含转义与 \u）/ number / true / false / null
// 不支持：注释、尾随逗号、NaN / Infinity 字面量、超大整数（一律按 double 处理）
//
// 对象用有序 vector 而非 map：字形数据的字段很少，线性查找更快，
// 而且保留插入顺序便于调试输出。
#pragma once

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace handwriting::json {

class Value;

using Array = std::vector<Value>;
using Member = std::pair<std::string, Value>;
using Object = std::vector<Member>;

class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string& message) : std::runtime_error(message) {}
};

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() = default;
    explicit Value(bool value) : type_(Type::Bool), bool_(value) {}
    explicit Value(double value) : type_(Type::Number), number_(value) {}
    explicit Value(std::string value) : type_(Type::String), string_(std::move(value)) {}
    explicit Value(Array value) : type_(Type::Array), array_(std::move(value)) {}
    explicit Value(Object value) : type_(Type::Object), object_(std::move(value)) {}

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool is_null() const noexcept { return type_ == Type::Null; }
    [[nodiscard]] bool is_bool() const noexcept { return type_ == Type::Bool; }
    [[nodiscard]] bool is_number() const noexcept { return type_ == Type::Number; }
    [[nodiscard]] bool is_string() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool is_array() const noexcept { return type_ == Type::Array; }
    [[nodiscard]] bool is_object() const noexcept { return type_ == Type::Object; }

    [[nodiscard]] bool as_bool() const;
    [[nodiscard]] double as_number() const;
    [[nodiscard]] const std::string& as_string() const;
    [[nodiscard]] const Array& as_array() const;
    [[nodiscard]] const Object& as_object() const;

    // 取数组元素，越界或类型不符时抛出带路径信息的错误
    [[nodiscard]] const Value& at(std::size_t index) const;

    // 按键查成员；不存在或不是 object 时返回 nullptr
    [[nodiscard]] const Value* find(std::string_view key) const;

private:
    Type type_{Type::Null};
    bool bool_{false};
    double number_{0.0};
    std::string string_;
    Array array_;
    Object object_;
};

// 解析文本。失败时抛 ParseError，消息里带出错位置。
[[nodiscard]] Value parse(std::string_view text);

// 读文件并解析。文件不存在或无法读取时抛 ParseError。
[[nodiscard]] Value parse_file(const std::filesystem::path& path);

}  // namespace handwriting::json
