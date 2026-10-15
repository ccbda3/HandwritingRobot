#include "handwriting/json.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace handwriting::json {

namespace {

constexpr int kMaxDepth = 64;

[[nodiscard]] std::string describe(Type type) {
    switch (type) {
        case Type::Null:
            return "null";
        case Type::Bool:
            return "bool";
        case Type::Number:
            return "number";
        case Type::String:
            return "string";
        case Type::Array:
            return "array";
        case Type::Object:
            return "object";
    }
    return "unknown";
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    [[nodiscard]] Value run() {
        skip_whitespace();
        Value value = parse_value(0);
        skip_whitespace();
        if (position_ != text_.size()) {
            fail("末尾存在多余内容");
        }
        return value;
    }

private:
    std::string_view text_;
    std::size_t position_{0};

    [[noreturn]] void fail(const std::string& message) const {
        std::ostringstream oss;
        oss << "JSON 解析失败（位置 " << position_ << "）：" << message;
        throw ParseError(oss.str());
    }

    [[nodiscard]] bool eof() const noexcept { return position_ >= text_.size(); }
    [[nodiscard]] char peek() const { return eof() ? '\0' : text_[position_]; }

    void skip_whitespace() {
        while (!eof()) {
            const char c = text_[position_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++position_;
            } else {
                break;
            }
        }
    }

    void expect(char expected) {
        if (peek() != expected) {
            fail(std::string("期望 '") + expected + "'");
        }
        ++position_;
    }

    [[nodiscard]] Value parse_value(int depth) {
        if (depth > kMaxDepth) {
            fail("嵌套层级过深");
        }
        switch (peek()) {
            case '{':
                return parse_object(depth);
            case '[':
                return parse_array(depth);
            case '"':
                return Value(parse_string());
            case 't':
                consume_literal("true");
                return Value(true);
            case 'f':
                consume_literal("false");
                return Value(false);
            case 'n':
                consume_literal("null");
                return Value{};
            default:
                return parse_number();
        }
    }

    void consume_literal(std::string_view literal) {
        if (text_.compare(position_, literal.size(), literal) != 0) {
            fail("无法识别的字面量");
        }
        position_ += literal.size();
    }

    [[nodiscard]] Value parse_object(int depth) {
        expect('{');
        Object object;
        skip_whitespace();
        if (peek() == '}') {
            ++position_;
            return Value(std::move(object));
        }
        while (true) {
            skip_whitespace();
            if (peek() != '"') {
                fail("对象的键必须是字符串");
            }
            std::string key = parse_string();
            skip_whitespace();
            expect(':');
            skip_whitespace();
            object.emplace_back(std::move(key), parse_value(depth + 1));
            skip_whitespace();
            const char c = peek();
            if (c == ',') {
                ++position_;
                continue;
            }
            if (c == '}') {
                ++position_;
                break;
            }
            fail("对象成员之间需要 ',' 或 '}'");
        }
        return Value(std::move(object));
    }

    [[nodiscard]] Value parse_array(int depth) {
        expect('[');
        Array array;
        skip_whitespace();
        if (peek() == ']') {
            ++position_;
            return Value(std::move(array));
        }
        while (true) {
            skip_whitespace();
            array.push_back(parse_value(depth + 1));
            skip_whitespace();
            const char c = peek();
            if (c == ',') {
                ++position_;
                continue;
            }
            if (c == ']') {
                ++position_;
                break;
            }
            fail("数组元素之间需要 ',' 或 ']'");
        }
        return Value(std::move(array));
    }

    [[nodiscard]] std::string parse_string() {
        expect('"');
        std::string out;
        while (true) {
            if (eof()) {
                fail("字符串没有结束引号");
            }
            const char c = text_[position_++];
            if (c == '"') {
                break;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (eof()) {
                fail("转义序列不完整");
            }
            const char escaped = text_[position_++];
            switch (escaped) {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u':
                    append_unicode(out);
                    break;
                default:
                    fail("无法识别的转义字符");
            }
        }
        return out;
    }

    [[nodiscard]] unsigned read_hex4() {
        if (position_ + 4 > text_.size()) {
            fail("\\u 转义不完整");
        }
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[position_++];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                fail("\\u 后面必须是十六进制数字");
            }
        }
        return value;
    }

    void append_unicode(std::string& out) {
        unsigned code = read_hex4();
        // 代理对：高代理 0xD800~0xDBFF 后面必须跟低代理 0xDC00~0xDFFF
        if (code >= 0xD800 && code <= 0xDBFF && position_ + 1 < text_.size() &&
            text_[position_] == '\\' && text_[position_ + 1] == 'u') {
            position_ += 2;
            const unsigned low = read_hex4();
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        }
        append_utf8(out, code);
    }

    static void append_utf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    [[nodiscard]] Value parse_number() {
        const std::size_t start = position_;
        if (peek() == '-' || peek() == '+') {
            ++position_;
        }
        bool seen_digit = false;
        while (!eof() && std::isdigit(static_cast<unsigned char>(text_[position_]))) {
            ++position_;
            seen_digit = true;
        }
        if (peek() == '.') {
            ++position_;
            while (!eof() && std::isdigit(static_cast<unsigned char>(text_[position_]))) {
                ++position_;
                seen_digit = true;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            ++position_;
            if (peek() == '-' || peek() == '+') {
                ++position_;
            }
            while (!eof() && std::isdigit(static_cast<unsigned char>(text_[position_]))) {
                ++position_;
            }
        }
        if (!seen_digit) {
            fail("无法识别的值");
        }

        const std::string token(text_.substr(start, position_ - start));
        return Value(std::strtod(token.c_str(), nullptr));
    }
};

}  // namespace

bool Value::as_bool() const {
    if (type_ != Type::Bool) {
        throw ParseError("期望 bool，实际是 " + describe(type_));
    }
    return bool_;
}

double Value::as_number() const {
    if (type_ != Type::Number) {
        throw ParseError("期望 number，实际是 " + describe(type_));
    }
    return number_;
}

const std::string& Value::as_string() const {
    if (type_ != Type::String) {
        throw ParseError("期望 string，实际是 " + describe(type_));
    }
    return string_;
}

const Array& Value::as_array() const {
    if (type_ != Type::Array) {
        throw ParseError("期望 array，实际是 " + describe(type_));
    }
    return array_;
}

const Object& Value::as_object() const {
    if (type_ != Type::Object) {
        throw ParseError("期望 object，实际是 " + describe(type_));
    }
    return object_;
}

const Value& Value::at(std::size_t index) const {
    const Array& items = as_array();
    if (index >= items.size()) {
        throw ParseError("数组越界：请求下标 " + std::to_string(index) + "，长度 " +
                         std::to_string(items.size()));
    }
    return items[index];
}

const Value* Value::find(std::string_view key) const {
    if (type_ != Type::Object) {
        return nullptr;
    }
    for (const auto& member : object_) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

Value parse(std::string_view text) {
    return Parser(text).run();
}

Value parse_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw ParseError("无法打开文件：" + path.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string text = buffer.str();
    try {
        return parse(text);
    } catch (const ParseError& error) {
        throw ParseError(path.string() + "：" + error.what());
    }
}

}  // namespace handwriting::json
