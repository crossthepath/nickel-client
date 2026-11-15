// Game version gate (native/game_read.hpp): the overlay accepts any Minecraft 1.26.62 installation by its file version.
// GameVersion reads VS_FIXEDFILEINFO from a module's version resource; checked on kernel32.dll (always loaded, always
// versioned) and on this test executable (no version resource). SupportedVersion accepts 1.26.62.x only.
#include "game_read.hpp"
#include <cstdio>

int main(){
 int failed=0;auto check=[&](bool ok,const char* name){std::printf("%s %s\n",ok?"ok  ":"FAIL",name);if(!ok)++failed;};
 uint16_t v[4]{};
 check(game::GameVersion(GetModuleHandleW(L"kernel32.dll"),v)&&v[0]>=6,"the version of a versioned module is read");
 check(!game::SupportedVersion(v),"a Windows module is not the supported game");
 uint16_t none[4]{};check(!game::GameVersion(GetModuleHandleW(nullptr),none),"a module without a version resource has no version");
 const uint16_t store[4]={1,26,62,1},other[4]={1,26,62,7},newer[4]={1,26,63,0},older[4]={1,26,61,2},major[4]={2,26,62,1};
 check(game::SupportedVersion(store),"1.26.62.1 is supported");
 check(game::SupportedVersion(other),"another 1.26.62 revision is supported");
 check(!game::SupportedVersion(newer)&&!game::SupportedVersion(older)&&!game::SupportedVersion(major),"other versions are refused");
 std::printf("%s\n",failed?"game version checks FAILED":"game version checks passed");return failed?1:0;}
