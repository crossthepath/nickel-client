// Byte patterns of the scripts' native library and Nickel AI's find_signature (native/native_pattern.hpp).
#include "native_pattern.hpp"
#include <cstdio>

namespace {
int failed = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failed += ok ? 0 : 1;
}
} // namespace

int main() {
    using namespace nickel::native;
    Pattern p;
    std::string why;
    Check(ParsePattern("48 8B 05 ?? ?? ? 48", &p, &why) && p.bytes.size() == 7 && p.any[3] && p.any[5] && !p.any[6]
                  && p.bytes[1] == 0x8b,
          "parse: hex bytes and ?? / ? wildcards");
    Check(ParsePattern("488b05", &p, &why) && p.bytes.size() == 3, "parse: spaces are optional");
    Check(!ParsePattern("?? ??", &p, &why) && !why.empty(), "parse: only wildcards is refused");
    Check(!ParsePattern("48 zz", &p, &why), "parse: a bad byte is refused");
    Check(!ParsePattern("", &p, &why), "parse: empty is refused");
    const uint8_t code[] = {0x90, 0x48, 0x8b, 0x05, 0x11, 0x22, 0x48, 0x8b, 0x05, 0x33, 0x44, 0xc3};
    ParsePattern("48 8B 05 ?? ??", &p, &why);
    const auto found = FindPattern(code, sizeof(code), p);
    Check(found.size() == 2 && found[0] == 1 && found[1] == 6, "find: every match, in order");
    Check(FindPattern(code, sizeof(code), p, 1).size() == 1, "find: the limit holds");
    ParsePattern("?? 8B 05 33", &p, &why);
    Check(FindPattern(code, sizeof(code), p) == std::vector<size_t>{6}, "find: a leading wildcard");
    ParsePattern("C3 00", &p, &why);
    Check(FindPattern(code, sizeof(code), p).empty(), "find: no match past the end");
    std::printf("%s (%d failed)\n", failed == 0 ? "all native pattern checks passed" : "native pattern checks failed", failed);
    return failed == 0 ? 0 : 1;
}
