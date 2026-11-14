// Luau values written into the player's state (native/script_values.hpp): complete, finite, bounded vectors only.
#include <cmath>
#include <cstdio>
#include "lua.h"
#include "lualib.h"
#include "script_values.hpp"

namespace {
int failures=0;
void Check(bool ok,const char* what){if(!ok){++failures;std::printf("FAILED: %s\n",what);}}
// Pushes a table {x=, y=, z=} with the given fields (a missing field is skipped).
void PushXyz(lua_State* L,const double* x,const double* y,const double* z){lua_createtable(L,0,3);
 if(x){lua_pushnumber(L,*x);lua_setfield(L,-2,"x");}if(y){lua_pushnumber(L,*y);lua_setfield(L,-2,"y");}if(z){lua_pushnumber(L,*z);lua_setfield(L,-2,"z");}}
bool Read(lua_State* L,float out[3]){const bool ok=ReadVectorStrict(L,-1,out);lua_pop(L,1);return ok;}
}  // namespace

int main(){
 lua_State* L=luaL_newstate();float v[3]{};
 const double one=1,two=2,three=3,nan=NAN,inf=INFINITY,huge=1e9;
 PushXyz(L,&one,&two,&three);Check(Read(L,v)&&v[0]==1&&v[1]==2&&v[2]==3,"a complete table is read");
 lua_pushvector(L,4,5,6);Check(Read(L,v)&&v[0]==4&&v[2]==6,"a Luau vector is read");
 lua_createtable(L,3,0);for(int i=0;i<3;++i){lua_pushnumber(L,i+7);lua_rawseti(L,-2,i+1);}Check(Read(L,v)&&v[0]==7&&v[2]==9,"an array table is read");
 PushXyz(L,&one,&two,nullptr);Check(!Read(L,v),"a missing component is refused");
 PushXyz(L,&one,&nan,&three);Check(!Read(L,v),"NaN is refused");
 PushXyz(L,&inf,&two,&three);Check(!Read(L,v),"infinity is refused");
 PushXyz(L,&one,&two,&huge);Check(!Read(L,v),"an out-of-range value is refused");
 lua_pushstring(L,"1 2 3");Check(!Read(L,v),"a string is refused");
 lua_createtable(L,0,3);lua_pushstring(L,"a");lua_setfield(L,-2,"x");lua_pushnumber(L,2);lua_setfield(L,-2,"y");lua_pushnumber(L,3);lua_setfield(L,-2,"z");Check(!Read(L,v),"a non-numeric component is refused");
 lua_createtable(L,0,3);lua_pushstring(L,"5");lua_setfield(L,-2,"x");lua_pushnumber(L,2);lua_setfield(L,-2,"y");lua_pushnumber(L,3);lua_setfield(L,-2,"z");Check(!Read(L,v),"a numeric string is refused");
 const double far15k=15000;PushXyz(L,&far15k,&two,&three);Check(!Read(L,v),"a velocity beyond 1e4 is refused");
 PushXyz(L,&far15k,&two,&three);Check(ReadVectorStrict(L,-1,v,kPositionLimit)&&v[0]==15000,"a position at x = 15000 is accepted");lua_pop(L,1);
 const double edge=3.1e7;PushXyz(L,&edge,&two,&three);Check(!ReadVectorStrict(L,-1,v,kPositionLimit),"a position beyond the world border is refused");lua_pop(L,1);
 {float sum[3]{9000,0,0};const float add[3]{5000,-20000,1};AddVelocityClamped(sum,add);Check(sum[0]==10000&&sum[1]==-10000&&sum[2]==1,"added velocities stay within +-1e4");}
 lua_close(L);
 if(failures){std::printf("%d script value check(s) failed\n",failures);return 1;}
 std::printf("script value checks passed\n");return 0;
}
