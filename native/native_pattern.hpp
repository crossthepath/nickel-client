// Byte patterns ("signatures") for the scripts' native library and Nickel AI's find_signature: "48 8B 05 ?? ?? ?? ??
// 48 85 C0" where "??" (or "?") matches any byte. No game code here: tests/native_pattern_checks.cpp runs it as is.
#ifndef NICKEL_NATIVE_PATTERN_HPP
#define NICKEL_NATIVE_PATTERN_HPP

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nickel::native {

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<bool> any; // true: this position matches any byte
};

// Parses a pattern; false (and why) when it is malformed, empty, longer than 256 bytes or only wildcards.
inline bool ParsePattern(std::string_view text, Pattern* out, std::string* why) {
    out->bytes.clear();
    out->any.clear();
    size_t i = 0;
    bool concrete = false;
    auto hex = [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; };
    auto value = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : (std::tolower(c) - 'a' + 10); };
    while (i < text.size()) {
        if (text[i] == ' ' || text[i] == '\t') {
            ++i;
            continue;
        }
        if (text[i] == '?') {
            i += (i + 1 < text.size() && text[i + 1] == '?') ? 2 : 1;
            out->bytes.push_back(0);
            out->any.push_back(true);
        } else if (i + 1 < text.size() && hex(text[i]) && hex(text[i + 1])) {
            out->bytes.push_back(static_cast<uint8_t>(value(text[i]) * 16 + value(text[i + 1])));
            out->any.push_back(false);
            concrete = true;
            i += 2;
        } else {
            *why = "a pattern is bytes in hex (\"48 8B\") and \"??\" for any byte";
            return false;
        }
        if (out->bytes.size() > 256) {
            *why = "a pattern is at most 256 bytes";
            return false;
        }
    }
    if (out->bytes.empty() || !concrete) {
        *why = "a pattern needs at least one byte that is not \"??\"";
        return false;
    }
    return true;
}

// Offsets in [data, data + size) where the pattern matches, at most `limit`.
inline std::vector<size_t> FindPattern(const uint8_t* data, size_t size, const Pattern& p, size_t limit = 16) {
    std::vector<size_t> found;
    const size_t n = p.bytes.size();
    if (!data || n == 0 || size < n) {
        return found;
    }
    size_t first = 0; // the first concrete byte, to skip quickly
    while (first < n && p.any[first]) {
        ++first;
    }
    for (size_t at = 0; at + n <= size && found.size() < limit; ++at) {
        if (data[at + first] != p.bytes[first]) {
            continue;
        }
        bool match = true;
        for (size_t k = 0; k < n && match; ++k) {
            match = p.any[k] || data[at + k] == p.bytes[k];
        }
        if (match) {
            found.push_back(at);
        }
    }
    return found;
}

} // namespace nickel::native

#endif // NICKEL_NATIVE_PATTERN_HPP
