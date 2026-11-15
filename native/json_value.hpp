#pragma once
// A small strict JSON value, parser and serializer (RFC 8259), for the overlay's network layer (PUBLIC and Nickel AI).
// No game access, no Windows API: tests/json_value_checks.cpp.
//
// Strict on input, because the text comes from the network: one value, no comments, no trailing commas, no leading
// zeros, no control characters inside strings, valid UTF-8 only, \uXXXX escapes with paired surrogates, no duplicate
// object keys (two parsers could otherwise read the same document differently). Limits on size, nesting depth and
// number of values bound the memory and stack a hostile document can use.
//
// No exceptions cross the API: Parse returns false with an error, and the accessors return a fallback (a null value,
// an empty string, 0) when the value has another type. Numbers are kept as double, plus the exact int64 when the text
// was an integer that fits (ids and byte counts above 2^53 keep every digit).
//
// Not the menu's permissive reader (icons::Json in menu_icons.hpp), which reads trusted local files.
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nickel::json {

enum class Type : uint8_t {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
};

class Value {
public:
    Value() = default;

    static Value MakeBool(bool value) {
        Value result;
        result.type_ = Type::Bool;
        result.bool_ = value;
        return result;
    }

    static Value MakeNumber(double value) {
        Value result;
        result.type_ = Type::Number;
        result.number_ = value;
        // A whole double within +-2^53 is an exact integer too.
        if (std::isfinite(value) && std::floor(value) == value && std::fabs(value) <= 9007199254740992.0) {
            result.int_ = static_cast<int64_t>(value);
            result.exactInt_ = true;
        }
        return result;
    }

    static Value MakeInt(int64_t value) {
        Value result;
        result.type_ = Type::Number;
        result.number_ = static_cast<double>(value);
        result.int_ = value;
        result.exactInt_ = true;
        return result;
    }

    static Value MakeString(std::string value) {
        Value result;
        result.type_ = Type::String;
        result.string_ = std::move(value);
        return result;
    }

    static Value MakeArray() {
        Value result;
        result.type_ = Type::Array;
        return result;
    }

    static Value MakeObject() {
        Value result;
        result.type_ = Type::Object;
        return result;
    }

    Type type() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsBool() const { return type_ == Type::Bool; }
    bool IsNumber() const { return type_ == Type::Number; }
    bool IsString() const { return type_ == Type::String; }
    bool IsArray() const { return type_ == Type::Array; }
    bool IsObject() const { return type_ == Type::Object; }

    bool AsBool(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    double AsDouble(double fallback = 0) const { return type_ == Type::Number ? number_ : fallback; }

    // True with the exact value when the number is an integer that fits int64 (written without fraction or exponent,
    // or a whole double within +-2^53).
    bool GetInt64(int64_t* out) const {
        if (type_ != Type::Number || !exactInt_) {
            return false;
        }
        *out = int_;
        return true;
    }

    int64_t AsInt64(int64_t fallback = 0) const {
        int64_t value = 0;
        return GetInt64(&value) ? value : fallback;
    }

    const std::string& AsString() const {
        static const std::string kEmpty;
        return type_ == Type::String ? string_ : kEmpty;
    }

    // Array items or object members; 0 for other types.
    size_t size() const {
        if (type_ == Type::Array) {
            return items_.size();
        }
        return type_ == Type::Object ? keys_.size() : 0;
    }

    // Array item, or the value of the i-th object member (members keep their document order). Null when out of range.
    const Value& At(size_t index) const { return index < items_.size() ? items_[index] : Null(); }

    // Key of the i-th object member; empty when out of range.
    const std::string& KeyAt(size_t index) const {
        static const std::string kEmpty;
        return type_ == Type::Object && index < keys_.size() ? keys_[index] : kEmpty;
    }

    // Object member, or nullptr (also on another type). Linear search: API objects have few members.
    const Value* Find(std::string_view key) const {
        if (type_ != Type::Object) {
            return nullptr;
        }
        for (size_t i = 0; i < keys_.size(); ++i) {
            if (keys_[i] == key) {
                return &items_[i];
            }
        }
        return nullptr;
    }

    // Object member, or a null value when it is missing: response["stats"]["stars"].AsInt64().
    const Value& operator[](std::string_view key) const {
        const Value* found = Find(key);
        return found ? *found : Null();
    }

    // Appends to an array (a null value becomes an empty array first). Ignored on other types.
    void Push(Value value) {
        if (type_ == Type::Null) {
            type_ = Type::Array;
        }
        if (type_ == Type::Array) {
            items_.push_back(std::move(value));
        }
    }

    // Sets an object member, replacing an existing one (a null value becomes an empty object first).
    void Set(std::string key, Value value) {
        if (type_ == Type::Null) {
            type_ = Type::Object;
        }
        if (type_ != Type::Object) {
            return;
        }
        for (size_t i = 0; i < keys_.size(); ++i) {
            if (keys_[i] == key) {
                items_[i] = std::move(value);
                return;
            }
        }
        keys_.push_back(std::move(key));
        items_.push_back(std::move(value));
    }

private:
    friend class Parser;

    static const Value& Null() {
        static const Value kNull;
        return kNull;
    }

    Type type_ = Type::Null;
    bool bool_ = false;
    bool exactInt_ = false;
    double number_ = 0;
    int64_t int_ = 0;
    std::string string_;
    std::vector<Value> items_;       // array items, or object values in member order
    std::vector<std::string> keys_;  // object keys, same order as items_
};

struct Limits {
    size_t maxBytes = 4 * 1024 * 1024;
    int maxDepth = 64;
    size_t maxValues = 200000;  // every value counts, containers included
};

struct ParseError {
    size_t offset = 0;  // byte offset in the text where parsing stopped
    std::string message;
};

namespace detail {

// Length of the valid UTF-8 sequence starting at text[at], or 0 when it is invalid (overlong forms, surrogates and
// code points above U+10FFFF are invalid).
inline size_t Utf8SequenceLength(std::string_view text, size_t at) {
    const auto byte = [&](size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(at);
    if (lead < 0x80) {
        return 1;
    }
    size_t length = 0;
    uint32_t min = 0;
    uint32_t code = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        min = 0x80;
        code = lead & 0x1F;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        min = 0x800;
        code = lead & 0x0F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        min = 0x10000;
        code = lead & 0x07;
    } else {
        return 0;
    }
    if (at + length > text.size()) {
        return 0;
    }
    for (size_t i = 1; i < length; ++i) {
        const unsigned char next = byte(at + i);
        if ((next & 0xC0) != 0x80) {
            return 0;
        }
        code = (code << 6) | (next & 0x3F);
    }
    if (code < min || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        return 0;
    }
    return length;
}

inline void AppendUtf8(uint32_t code, std::string& out) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

} // namespace detail

class Parser {
public:
    Parser(std::string_view text, const Limits& limits) : text_(text), limits_(limits) {}

    bool Run(Value* out, ParseError* error) {
        *out = Value();
        bool ok = false;
        if (text_.size() > limits_.maxBytes) {
            Fail("document larger than the size limit");
        } else {
            SkipSpace();
            ok = ParseValue(*out, 0);
            SkipSpace();
            if (ok && pos_ != text_.size()) {
                ok = Fail("unexpected text after the value");
            }
        }
        if (!ok) {
            *out = Value();
            if (error) {
                error->offset = pos_;
                error->message = message_;
            }
        }
        return ok;
    }

private:
    bool Fail(const char* message) {
        if (message_.empty()) {
            message_ = message;
        }
        return false;
    }

    bool AtEnd() const { return pos_ >= text_.size(); }
    char Peek() const { return AtEnd() ? '\0' : text_[pos_]; }

    void SkipSpace() {
        while (!AtEnd() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool Literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) {
            return Fail("invalid literal");
        }
        pos_ += word.size();
        return true;
    }

    bool ParseValue(Value& out, int depth) {
        if (++values_ > limits_.maxValues) {
            return Fail("too many values");
        }
        switch (Peek()) {
            case 'n':
                return Literal("null");
            case 't':
                out = Value::MakeBool(true);
                return Literal("true");
            case 'f':
                out = Value::MakeBool(false);
                return Literal("false");
            case '"':
                out = Value::MakeString(std::string());
                return ParseString(out.string_);
            case '[':
                return ParseArray(out, depth + 1);
            case '{':
                return ParseObject(out, depth + 1);
            default:
                return ParseNumber(out);
        }
    }

    bool ParseArray(Value& out, int depth) {
        if (depth > limits_.maxDepth) {
            return Fail("nesting deeper than the limit");
        }
        out = Value::MakeArray();
        ++pos_;  // [
        SkipSpace();
        if (Peek() == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            out.items_.emplace_back();
            if (!ParseValue(out.items_.back(), depth)) {
                return false;
            }
            SkipSpace();
            if (Peek() == ',') {
                ++pos_;
                SkipSpace();
                continue;
            }
            if (Peek() == ']') {
                ++pos_;
                return true;
            }
            return Fail("expected ',' or ']'");
        }
    }

    bool ParseObject(Value& out, int depth) {
        if (depth > limits_.maxDepth) {
            return Fail("nesting deeper than the limit");
        }
        out = Value::MakeObject();
        ++pos_;  // {
        SkipSpace();
        if (Peek() == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            if (Peek() != '"') {
                return Fail("expected a string key");
            }
            out.keys_.emplace_back();
            if (!ParseString(out.keys_.back())) {
                return false;
            }
            SkipSpace();
            if (Peek() != ':') {
                return Fail("expected ':'");
            }
            ++pos_;
            SkipSpace();
            out.items_.emplace_back();
            if (!ParseValue(out.items_.back(), depth)) {
                return false;
            }
            SkipSpace();
            if (Peek() == ',') {
                ++pos_;
                SkipSpace();
                continue;
            }
            if (Peek() == '}') {
                ++pos_;
                return !HasDuplicateKey(out.keys_) || Fail("duplicate object key");
            }
            return Fail("expected ',' or '}'");
        }
    }

    static bool HasDuplicateKey(const std::vector<std::string>& keys) {
        if (keys.size() < 2) {
            return false;
        }
        std::unordered_set<std::string_view> seen;
        seen.reserve(keys.size());
        for (const std::string& key : keys) {
            if (!seen.insert(key).second) {
                return true;
            }
        }
        return false;
    }

    bool ReadHex4(uint32_t* code) {
        if (pos_ + 4 > text_.size()) {
            return Fail("truncated \\u escape");
        }
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return Fail("invalid \\u escape");
            }
        }
        *code = value;
        return true;
    }

    // After "\u": one code point, a surrogate pair written as two escapes, never a lone surrogate.
    bool ParseUnicodeEscape(std::string& out) {
        uint32_t code = 0;
        if (!ReadHex4(&code)) {
            return false;
        }
        if (code >= 0xDC00 && code <= 0xDFFF) {
            return Fail("lone low surrogate");
        }
        if (code >= 0xD800 && code <= 0xDBFF) {
            if (text_.substr(pos_, 2) != "\\u") {
                return Fail("high surrogate without its low surrogate");
            }
            pos_ += 2;
            uint32_t low = 0;
            if (!ReadHex4(&low)) {
                return false;
            }
            if (low < 0xDC00 || low > 0xDFFF) {
                return Fail("high surrogate without its low surrogate");
            }
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        }
        detail::AppendUtf8(code, out);
        return true;
    }

    bool ParseEscape(std::string& out) {
        ++pos_;  // backslash
        if (AtEnd()) {
            return Fail("truncated escape");
        }
        const char c = text_[pos_++];
        switch (c) {
            case '"':
            case '\\':
            case '/':
                out += c;
                return true;
            case 'b':
                out += '\b';
                return true;
            case 'f':
                out += '\f';
                return true;
            case 'n':
                out += '\n';
                return true;
            case 'r':
                out += '\r';
                return true;
            case 't':
                out += '\t';
                return true;
            case 'u':
                return ParseUnicodeEscape(out);
            default:
                --pos_;
                return Fail("invalid escape");
        }
    }

    bool ParseString(std::string& out) {
        ++pos_;  // opening quote
        for (;;) {
            if (AtEnd()) {
                return Fail("unterminated string");
            }
            const unsigned char c = static_cast<unsigned char>(text_[pos_]);
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c == '\\') {
                if (!ParseEscape(out)) {
                    return false;
                }
                continue;
            }
            if (c < 0x20) {
                return Fail("control character in a string");
            }
            const size_t length = detail::Utf8SequenceLength(text_, pos_);
            if (length == 0) {
                return Fail("invalid UTF-8");
            }
            out.append(text_.data() + pos_, length);
            pos_ += length;
        }
    }

    size_t SkipDigits() {
        const size_t start = pos_;
        while (!AtEnd() && text_[pos_] >= '0' && text_[pos_] <= '9') {
            ++pos_;
        }
        return pos_ - start;
    }

    // RFC 8259 grammar: -? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE] [+-]? [0-9]+)?
    bool ParseNumber(Value& out) {
        const size_t start = pos_;
        if (Peek() == '-') {
            ++pos_;
        }
        if (Peek() == '0') {
            ++pos_;
        } else if (Peek() >= '1' && Peek() <= '9') {
            SkipDigits();
        } else {
            pos_ = start;
            return Fail(AtEnd() ? "unexpected end of text" : "unexpected character");
        }
        bool integer = true;
        if (Peek() == '.') {
            ++pos_;
            integer = false;
            if (SkipDigits() == 0) {
                return Fail("digits expected after '.'");
            }
        }
        if (Peek() == 'e' || Peek() == 'E') {
            ++pos_;
            integer = false;
            if (Peek() == '+' || Peek() == '-') {
                ++pos_;
            }
            if (SkipDigits() == 0) {
                return Fail("digits expected in the exponent");
            }
        }
        const char* first = text_.data() + start;
        const char* last = text_.data() + pos_;
        double number = 0;
        const auto parsed = std::from_chars(first, last, number);
        if (parsed.ec != std::errc() || parsed.ptr != last || !std::isfinite(number)) {
            return Fail("number out of range");
        }
        out = Value::MakeNumber(number);
        int64_t exact = 0;
        if (integer) {
            const auto asInt = std::from_chars(first, last, exact);
            if (asInt.ec == std::errc() && asInt.ptr == last) {
                out.int_ = exact;
                out.exactInt_ = true;
            }
        }
        return true;
    }

    std::string_view text_;
    Limits limits_;
    size_t pos_ = 0;
    size_t values_ = 0;
    std::string message_;
};

// Parses one JSON document. On failure `out` is null and `error` (optional) says where and why.
inline bool Parse(std::string_view text, Value* out, ParseError* error = nullptr, const Limits& limits = {}) {
    Parser parser(text, limits);
    return parser.Run(out, error);
}

inline void SerializeString(std::string_view text, std::string& out) {
    static const char kHex[] = "0123456789abcdef";
    out += '"';
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\b') {
            out += "\\b";
        } else if (c == '\f') {
            out += "\\f";
        } else if (c < 0x20) {
            out += "\\u00";
            out += kHex[c >> 4];
            out += kHex[c & 0xF];
        } else if (c >= 0x80) {
            // Invalid UTF-8 built by the caller becomes U+FFFD, so the output is always valid JSON.
            const size_t length = detail::Utf8SequenceLength(text, i);
            if (length == 0) {
                out += "\\ufffd";
                ++i;
                continue;
            }
            out.append(text.data() + i, length);
            i += length;
            continue;
        } else {
            out += static_cast<char>(c);
        }
        ++i;
    }
    out += '"';
}

inline void SerializeTo(const Value& value, std::string& out) {
    switch (value.type()) {
        case Type::Null:
            out += "null";
            return;
        case Type::Bool:
            out += value.AsBool() ? "true" : "false";
            return;
        case Type::Number: {
            char buffer[32];
            std::to_chars_result written{};
            int64_t exact = 0;
            if (value.GetInt64(&exact)) {
                written = std::to_chars(buffer, buffer + sizeof(buffer), exact);
            } else if (std::isfinite(value.AsDouble())) {
                written = std::to_chars(buffer, buffer + sizeof(buffer), value.AsDouble());  // shortest round trip
            } else {
                out += "null";  // JSON has no NaN or infinity
                return;
            }
            out.append(buffer, written.ptr);
            return;
        }
        case Type::String:
            SerializeString(value.AsString(), out);
            return;
        case Type::Array:
            out += '[';
            for (size_t i = 0; i < value.size(); ++i) {
                if (i > 0) {
                    out += ',';
                }
                SerializeTo(value.At(i), out);
            }
            out += ']';
            return;
        case Type::Object:
            out += '{';
            for (size_t i = 0; i < value.size(); ++i) {
                if (i > 0) {
                    out += ',';
                }
                SerializeString(value.KeyAt(i), out);
                out += ':';
                SerializeTo(value.At(i), out);
            }
            out += '}';
            return;
    }
}

// Compact JSON text (no spaces), members in insertion order.
inline std::string Serialize(const Value& value) {
    std::string out;
    SerializeTo(value, out);
    return out;
}

} // namespace nickel::json
