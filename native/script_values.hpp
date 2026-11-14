#pragma once
// Validation of Luau values that end up in the game's memory (no game access: tests/script_values_checks.cpp).
#include <cmath>
#include "lua.h"

// Vectors written into the player's state: x, y and z must all be present, finite and within +-limit (kVectorLimit for
// velocities, kPositionLimit for positions: the world border is at +-3e7).
inline constexpr double kVectorLimit=1.0e4,kPositionLimit=3.0e7;
inline bool ReadVectorStrict(lua_State* L,int idx,float out[3],double limit=kVectorLimit){double v[3]{};
 if(lua_isvector(L,idx)){const float* f=lua_tovector(L,idx);for(int i=0;i<3;++i)v[i]=f[i];}
 else if(lua_istable(L,idx)){const char* k[]={"x","y","z"};for(int i=0;i<3;++i){lua_rawgetfield(L,idx,k[i]);if(lua_type(L,-1)!=LUA_TNUMBER){lua_pop(L,1);lua_rawgeti(L,idx,i+1);}
  const bool number=lua_type(L,-1)==LUA_TNUMBER;   // real numbers only, not numeric strings
 v[i]=number?lua_tonumber(L,-1):NAN;lua_pop(L,1);if(!number)return false;}}
 else return false;
 for(int i=0;i<3;++i){if(!std::isfinite(v[i])||std::fabs(v[i])>limit)return false;out[i]=(float)v[i];}return true;}
// player.addVelocity calls between two ticks add up; the sum stays within +-kVectorLimit.
inline void AddVelocityClamped(float sum[3],const float v[3]){for(int i=0;i<3;++i){const double s=(double)sum[i]+v[i];sum[i]=(float)(s>kVectorLimit?kVectorLimit:(s<-kVectorLimit?-kVectorLimit:s));}}
