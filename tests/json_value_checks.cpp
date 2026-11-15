// Strict JSON value, parser and serializer of the network layer (native/json_value.hpp): what it accepts, what it
// refuses (the text comes from the network), exact integers, Unicode escapes, limits, and serialization round trips.
#include "json_value.hpp"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

namespace {

using nickel::json::Limits;
using nickel::json::Parse;
using nickel::json::ParseError;
using nickel::json::Serialize;
using nickel::json::Type;
using nickel::json::Value;

int failed = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++failed;
    }
}

bool Accepts(const std::string& text) {
    Value value;
    return Parse(text, &value);
}

// Parses then serializes; empty when the text is refused.
std::string RoundTrip(const std::string& text) {
    Value value;
    if (!Parse(text, &value)) {
        return std::string();
    }
    return Serialize(value);
}

void Scalars() {
    Value value;
    Check(Parse("null", &value) && value.IsNull(), "null");
    Check(Parse(" true ", &value) && value.AsBool(), "true with spaces around");
    Check(Parse("false", &value) && value.IsBool() && !value.AsBool(true), "false");
    Check(Parse("\"hi\"", &value) && value.AsString() == "hi", "string");
    Check(Parse("-12.5e1", &value) && value.AsDouble() == -125.0, "number with fraction and exponent");
    Check(Parse("0", &value) && value.AsInt64(-1) == 0, "zero");
    Check(value.AsString().empty() && !value.AsBool(), "accessors of another type give the fallback");
}

void ExactIntegers() {
    Value value;
    int64_t exact = 0;
    Check(Parse("9007199254740993", &value) && value.GetInt64(&exact) && exact == 9007199254740993LL,
            "an integer above 2^53 keeps every digit");
    Check(Parse("-9223372036854775808", &value) && value.GetInt64(&exact) && exact == INT64_MIN, "int64 minimum");
    Check(Parse("9223372036854775808", &value) && !value.GetInt64(&exact) && value.AsDouble() > 9.2e18,
            "beyond int64: a double, not an exact integer");
    Check(Parse("1.5", &value) && !value.GetInt64(&exact), "a fraction is not an integer");
    Check(Parse("2e3", &value) && value.GetInt64(&exact) && exact == 2000, "a whole number written with an exponent");
    Check(Serialize(Value::MakeInt(9007199254740993LL)) == "9007199254740993", "exact integers serialize exactly");
    Check(Serialize(Value::MakeNumber(0.1)) == "0.1", "doubles serialize in their shortest form");
    Check(Serialize(Value::MakeNumber(std::numeric_limits<double>::infinity())) == "null",
            "infinity serializes as null");
}

void Strings() {
    Value value;
    Check(Parse("\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t\"", &value) && value.AsString() == "a\"b\\c/d\b\f\n\r\t",
            "every short escape");
    Check(Parse("\"\\u00e9\\u20AC\"", &value) && value.AsString() == "\xC3\xA9\xE2\x82\xAC", "\\u escapes to UTF-8");
    Check(Parse("\"\\ud83d\\ude00\"", &value) && value.AsString() == "\xF0\x9F\x98\x80", "a surrogate pair");
    Check(Parse("\"\xF0\x9F\x98\x80 caf\xC3\xA9\"", &value) && value.AsString() == "\xF0\x9F\x98\x80 caf\xC3\xA9",
            "raw UTF-8 is kept");
    Check(Parse("\"\\u0000\"", &value) && value.AsString() == std::string(1, '\0'), "an escaped NUL");
    Check(!Accepts("\"\\ud83d\""), "a lone high surrogate is refused");
    Check(!Accepts("\"\\ude00\""), "a lone low surrogate is refused");
    Check(!Accepts("\"\\ud83d\\u0041\""), "a high surrogate followed by another escape is refused");
    Check(!Accepts("\"\\u12\""), "a short \\u escape is refused");
    Check(!Accepts("\"\\x41\""), "an unknown escape is refused");
    Check(!Accepts("\"a\nb\""), "a raw control character is refused");
    Check(!Accepts("\"\xC3\""), "truncated UTF-8 is refused");
    Check(!Accepts("\"\xC0\xAF\""), "overlong UTF-8 is refused");
    Check(!Accepts("\"\xED\xA0\x80\""), "an encoded surrogate is refused");
    Check(!Accepts("\"\xF4\x90\x80\x80\""), "a code point above U+10FFFF is refused");
    Check(!Accepts("\"open"), "an unterminated string is refused");
}

void Containers() {
    Value value;
    const bool parsed = Parse("{\"name\":\"Fly\",\"stats\":{\"stars\":3,\"downloads\":120},\"tags\":[\"a\",\"b\"]}",
            &value);
    Check(parsed && value.IsObject() && value.size() == 3, "an object with three members");
    Check(value["name"].AsString() == "Fly", "member lookup");
    Check(value["stats"]["stars"].AsInt64() == 3, "nested member lookup");
    Check(value["tags"].size() == 2 && value["tags"].At(1).AsString() == "b", "array items");
    Check(value["missing"]["deeper"].IsNull() && value.Find("missing") == nullptr, "a missing member reads as null");
    Check(value.KeyAt(0) == "name" && value.KeyAt(2) == "tags" && value.KeyAt(3).empty(), "members keep their order");
    Check(value["tags"].At(9).IsNull(), "an item out of range reads as null");
    Check(Parse("[]", &value) && value.IsArray() && value.size() == 0, "empty array");
    Check(Parse("{ }", &value) && value.IsObject() && value.size() == 0, "empty object");
    Check(!Accepts("{\"a\":1,\"a\":2}"), "a duplicate key is refused");
}

void Grammar() {
    Check(!Accepts(""), "empty text is refused");
    Check(!Accepts("[1,]"), "a trailing comma in an array is refused");
    Check(!Accepts("{\"a\":1,}"), "a trailing comma in an object is refused");
    Check(!Accepts("01"), "a leading zero is refused");
    Check(!Accepts("1."), "a fraction without digits is refused");
    Check(!Accepts(".5"), "a number without an integer part is refused");
    Check(!Accepts("1e"), "an exponent without digits is refused");
    Check(!Accepts("+1"), "a plus sign is refused");
    Check(!Accepts("NaN") && !Accepts("Infinity"), "NaN and Infinity are refused");
    Check(!Accepts("1e400"), "a number that overflows a double is refused");
    Check(!Accepts("tru"), "a truncated literal is refused");
    Check(!Accepts("[1] [2]"), "two values are refused");
    Check(!Accepts("// c\n1"), "comments are refused");
    Check(!Accepts("{a:1}"), "an unquoted key is refused");
    Check(!Accepts("['a']"), "single quotes are refused");
    Check(!Accepts("\xEF\xBB\xBF{}"), "a byte order mark is refused");

    Value value = Value::MakeString("kept");
    ParseError error;
    Check(!Parse("[1, x]", &value, &error) && value.IsNull() && error.offset == 4 && !error.message.empty(),
            "a refused document leaves a null value, and says where it stopped");
}

void LimitsHold() {
    Limits limits;
    limits.maxDepth = 4;
    Value value;
    Check(Parse("[[[[1]]]]", &value, nullptr, limits), "nesting at the depth limit is accepted");
    Check(!Parse("[[[[[1]]]]]", &value, nullptr, limits), "nesting beyond the depth limit is refused");
    Check(!Parse("{\"a\":{\"b\":{\"c\":{\"d\":{}}}}}", &value, nullptr, limits), "the depth limit counts objects too");
    Check(!Accepts(std::string(100000, '[') + std::string(100000, ']')),
            "deep nesting is refused by the default limit (no stack overflow)");

    limits = Limits();
    limits.maxBytes = 8;
    Check(!Parse("\"123456789\"", &value, nullptr, limits), "a document larger than the size limit is refused");

    limits = Limits();
    limits.maxValues = 3;
    Check(Parse("[1,2]", &value, nullptr, limits), "values up to the limit (the array counts)");
    Check(!Parse("[1,2,3]", &value, nullptr, limits), "too many values are refused");
}

void Serialization() {
    const std::string document = "{\"a\":[1,-2.5,true,false,null],\"b\":{\"c\":\"x\\ny\"},\"d\":\"\"}";
    Check(RoundTrip(document) == document, "a compact document round-trips unchanged");
    Check(RoundTrip("{ \"a\" : [ 1 , 2 ] }") == "{\"a\":[1,2]}", "whitespace is dropped");
    Check(RoundTrip("\"\\u0001\\u001f\\u00e9\"") == "\"\\u0001\\u001f\xC3\xA9\"",
            "control characters are escaped, other characters stay UTF-8");

    Value built = Value::MakeObject();
    built.Set("name", Value::MakeString("Kill \"Aura\""));
    built.Set("stars", Value::MakeInt(7));
    Value tags = Value::MakeArray();
    tags.Push(Value::MakeString("combat"));
    built.Set("tags", std::move(tags));
    built.Set("stars", Value::MakeInt(8));
    Check(Serialize(built) == "{\"name\":\"Kill \\\"Aura\\\"\",\"stars\":8,\"tags\":[\"combat\"]}",
            "a built object serializes; Set replaces an existing member in place");
    Check(Serialize(Value::MakeString("a\xFF" "b")) == "\"a\\ufffdb\"", "invalid UTF-8 serializes as U+FFFD");

    Value reparsed;
    Check(Parse(Serialize(built), &reparsed) && reparsed["name"].AsString() == "Kill \"Aura\"",
            "the serializer's output parses back to the same value");
}

} // namespace

int main() {
    Scalars();
    ExactIntegers();
    Strings();
    Containers();
    Grammar();
    LimitsHold();
    Serialization();
    std::printf("%s (%d failed)\n", failed == 0 ? "all json checks passed" : "json checks failed", failed);
    return failed == 0 ? 0 : 1;
}
