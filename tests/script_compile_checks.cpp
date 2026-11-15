// Compiling Luau source as the engine does (native/script_compile.hpp), with the real Luau compiler and no VM: what
// ScriptLibrary::Compile returns to the assistant's validate_script and what the folder scan shows for a held script.
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "script_compile.hpp"

namespace {

using namespace overlay;

int failed = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++failed;
    }
}

const char* const kValid = "local m = nickel.module(\"Hello\", \"Utility\")\n"
                           "m:onEnable(function() chat.print(\"hi\") end)\n";
const char* const kBroken = "local x = 1\n"
                            "local function f()\n"
                            "  return x\n";   // 'end' expected at <eof>, which is on line 4

void Valid() {
    Check(CompileScript(kValid).empty(), "valid source compiles without errors");
    Check(CompileScript("").empty(), "an empty source compiles");
    Check(CompileScript("\xef\xbb\xbf" "print(1)").empty(), "a leading UTF-8 byte order mark is dropped first");
    std::string error;
    const std::string code = CompileScriptBytecode(kValid, error);
    Check(error.empty() && !code.empty() && code[0] != 0, "valid source gives bytecode and no error");
}

void Broken() {
    const std::vector<CompileError> errors = CompileScript(kBroken);
    Check(errors.size() == 1, "broken source gives one error");
    if (errors.size() == 1) {
        Check(errors[0].line == 4, "the error is on the line of <eof>, after the last line");
        Check(errors[0].message.find("Expected 'end'") != std::string::npos, "the error has Luau's message");
        std::printf("     %d: %s\n", errors[0].line, errors[0].message.c_str());
    }
    const std::vector<CompileError> second = CompileScript("local a = \n\n\nlocal b = = 2\n");
    Check(second.size() == 1 && second[0].line == 4, "an error on line 4 is reported on line 4");

    std::string error;
    const std::string code = CompileScriptBytecode(kBroken, error);
    Check(!error.empty() && !code.empty() && code[0] == 0 && code.substr(1) == error,
          "failed bytecode starts with a 0 byte followed by the error, which is what ScriptLoad reports");
    Check(ParseCompileError(error).line == CompileScript(kBroken)[0].line, "the parsed line matches the raw error");
    Check(!CompileScript("\xef\xbb\xbf" "local = 1").empty(), "a byte order mark does not hide an error");
}

// luau_compile keeps no shared mutable state: two threads compiling at once get the same results as one.
void Threads() {
    bool results[2] = {true, true};
    std::vector<std::thread> threads;
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([t, &results] {
            for (int i = 0; i < 200; ++i) {
                const bool valid = CompileScript(kValid).empty();
                const std::vector<CompileError> broken = CompileScript(kBroken);
                if (!valid || broken.size() != 1 || broken[0].message.find("Expected 'end'") == std::string::npos) {
                    results[t] = false;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    Check(results[0] && results[1], "two threads compiling at once get the same results");
}

} // namespace

int main() {
    Valid();
    Broken();
    Threads();
    std::printf(failed ? "%d check(s) FAILED\n" : "all checks passed\n", failed);
    return failed ? 1 : 0;
}
