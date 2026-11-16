#pragma once
// Compiling Luau source as the engine loads it: the engine's compile options in one place, used by the engine
// (script_engine.hpp: loading a script or a library, ScriptLibrary::Compile) and by tests/script_compile_checks.cpp.
// luau_compile only: no VM, no lock, no engine state, so any thread may call it. Needs Luau.Compiler.
#include <cstdlib>
#include <string>
#include <vector>

#include "luacode.h"
#include "script_library.hpp"

namespace overlay {

// The bytecode of `source` compiled with the engine's options (optimization level 1, debug level 1). On a compile
// error, `error` is Luau's message (":12: Expected 'end' ...") and the bytecode returned starts with a 0 byte; it must
// not be loaded then.
inline std::string CompileScriptBytecode(const std::string& source, std::string& error) {
    lua_CompileOptions options{};
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    size_t size = 0;
    char* bytecode = luau_compile(source.data(), source.size(), &options, &size);
    if (!bytecode) {
        error = "the compiler ran out of memory";
        return {};
    }
    std::string out(bytecode, size);
    std::free(bytecode);
    if (!out.empty() && out[0] == 0) {
        error = out.substr(1);
    }
    return out;
}

// The compile errors of `source` (empty: it compiles), after WithoutUtf8Bom as at load time. 0 or 1 entries today.
inline std::vector<CompileError> CompileScript(const std::string& source) {
    std::string error;
    CompileScriptBytecode(WithoutUtf8Bom(source), error);
    if (error.empty()) {
        return {};
    }
    return {ParseCompileError(error)};
}

} // namespace overlay
