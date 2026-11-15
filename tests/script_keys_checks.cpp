// client.isKeyDown for scripts (native/script_keys.hpp): a key is reported only while Minecraft is the foreground window.
#include <cstdio>
#include <windows.h>
#include "script_keys.hpp"

namespace {
int failures=0;
void Check(bool ok,const char* what){if(!ok){++failures;std::printf("FAILED: %s\n",what);}}
int held=0;   // the key the fake keyboard reports as held
SHORT WINAPI FakeState(int code){return code==held?(SHORT)0x8000:(SHORT)0;}
}  // namespace

int main(){
 const HWND game=reinterpret_cast<HWND>(0x1000),other=reinterpret_cast<HWND>(0x2000);
 held='A';
 Check(ScriptKeyDown('A',game,game,FakeState),"a held key is reported while Minecraft is in the foreground");
 Check(!ScriptKeyDown('B',game,game,FakeState),"a released key is not reported");
 Check(!ScriptKeyDown(0,game,game,FakeState),"an unknown key name (code 0) is not reported");
 Check(!ScriptKeyDown('A',other,game,FakeState),"a key typed in another application is not reported");
 Check(!ScriptKeyDown('A',nullptr,game,FakeState),"no foreground window: nothing is reported");
 Check(!ScriptKeyDown('A',game,nullptr,FakeState),"the game window is not known yet: nothing is reported");
 if(failures)return 1;
 std::printf("script key checks passed\n");return 0;}
