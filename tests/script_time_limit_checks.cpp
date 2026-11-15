// Time limit of a protected call into a Luau script (native/script_limits.hpp), run on a real Luau VM with the same
// interrupt rule as native/script_engine.hpp. A call that does not stop is reported by a watchdog.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <string>
#include "lua.h"
#include "lualib.h"
#include "luacode.h"
#include "script_limits.hpp"

namespace {
int failures=0;
void Check(bool ok,const char* what){if(!ok){++failures;std::printf("FAILED: %s\n",what);}}
double Now(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}

struct Call{double start=0,limit=.05;bool expired=false;};
void Interrupt(lua_State* L,int gc){
 if(gc>=0)return;
 Call* c=static_cast<Call*>(lua_callbacks(L)->userdata);
 if(c&&ScriptOverLimit(c->start,c->limit,c->expired,Now()))luaL_error(L,"script call took longer than 50 ms");}

struct Outcome{bool ok=false;std::string error;double seconds=0;};
// Compiles and runs `source` in a fresh VM under the 50 ms limit, like ScriptCall (start, pcall, reset).
Outcome Run(const char* source){
 Outcome out;Call call;
 lua_State* L=luaL_newstate();luaL_openlibs(L);
 lua_callbacks(L)->userdata=&call;lua_callbacks(L)->interrupt=Interrupt;
 size_t size=0;char* bytecode=luau_compile(source,std::strlen(source),nullptr,&size);
 const bool loaded=luau_load(L,"=check",bytecode,size,0)==0;std::free(bytecode);
 if(!loaded){out.error=lua_tostring(L,-1);lua_close(L);return out;}
 const double began=Now();
 call.start=began;call.expired=false;
 const int status=lua_pcall(L,0,0,0);
 call.start=0;call.expired=false;
 out.seconds=Now()-began;out.ok=status==LUA_OK;
 if(!out.ok){const char* text=lua_tostring(L,-1);out.error=text?text:"";}
 lua_close(L);return out;}

// Runs on a worker thread; a script still running after 5 s means the limit was bypassed.
Outcome RunWatched(const char* source,const char* what){
 auto task=std::async(std::launch::async,Run,source);
 if(task.wait_for(std::chrono::seconds(5))!=std::future_status::ready){
  std::printf("FAILED: %s (still running after 5 s: the time limit was bypassed)\n",what);std::fflush(stdout);std::_Exit(1);}
 return task.get();}
bool Stopped(const Outcome& o){return !o.ok&&o.error.find("longer than 50 ms")!=std::string::npos&&o.seconds<2;}
}  // namespace

int main(){
 Check(RunWatched("local x=0 for i=1,1000 do x+=i end","a short script").ok,"a short script finishes without error");
 Check(Stopped(RunWatched("while true do end","an endless loop")),"an endless loop is stopped");
 Check(Stopped(RunWatched("while true do pcall(function() while true do end end) end","pcall in a loop")),
       "an endless loop is stopped when the script catches the error with pcall");
 Check(Stopped(RunWatched("for i=1,3 do pcall(function() while true do end end) end while true do end","pcall then loop")),
       "the error caught by pcall does not reset the limit");
 Check(Stopped(RunWatched("local function f() while true do end end while true do xpcall(f,function(e) return e end) end","xpcall")),
       "an endless loop is stopped through xpcall with a handler");
 Check(Stopped(RunWatched("while true do pcall(error,'x') end","pcall of error")),"an endless loop of caught errors is stopped");
 // The limit belongs to one call: a later call starts with a fresh budget.
 {Call call;call.start=1;bool& e=call.expired;
  Check(!ScriptOverLimit(call.start,.05,e,1.01),"within the limit");
  Check(ScriptOverLimit(call.start,.05,e,1.06),"past the limit");
  Check(ScriptOverLimit(call.start,.05,e,1.07),"still stopped after the limit, in the same call");
  e=false;Check(!ScriptOverLimit(2,.05,e,2.01),"a new call starts within its limit");
  Check(!ScriptOverLimit(0,.05,e,99),"no call running: never stopped");}
 if(failures)return 1;
 std::printf("script time limit checks passed\n");return 0;}
