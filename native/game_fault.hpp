#pragma once
// Fault boundaries for code that runs from game hooks: fail closed for the feature, fail open for the game.
// A guarded site keeps a "stopped" flag; its first fault is logged once (exception code, module + offset) and the site
// is skipped from then on, while the game's own code keeps running. The flag is per call site, so one faulty
// subscriber never disables the others. These helpers hold no C++ objects, as __try requires.
#include <Windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstring>

namespace game {

inline void(*volatile faultLog)(const char*)=nullptr;   // set by the overlay (WriteLog)

// Exception filter: logs the first fault of `where`, sets *stopped, handles the exception.
inline int FeatureFault(const char* where,volatile long* stopped,EXCEPTION_POINTERS* e){
 if(stopped&&InterlockedExchange(stopped,1))return EXCEPTION_EXECUTE_HANDLER;   // already reported
 auto log=faultLog;if(!log||!e||!e->ExceptionRecord)return EXCEPTION_EXECUTE_HANDLER;
 const auto* r=e->ExceptionRecord;const uintptr_t at=reinterpret_cast<uintptr_t>(r->ExceptionAddress);
 HMODULE mod{};char name[MAX_PATH]="?";
 if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCSTR>(at),&mod))GetModuleFileNameA(mod,name,MAX_PATH);
 const char* base=strrchr(name,'\\');char line[400];
 snprintf(line,sizeof(line),"%s: fault %08lx at %s+0x%llx; stopped until restart",where?where:"?",r->ExceptionCode,base?base+1:name,
  (unsigned long long)(mod?at-reinterpret_cast<uintptr_t>(mod):at));
 log(line);return EXCEPTION_EXECUTE_HANDLER;}

// Guarded calls through a subscriber pointer. A null pointer or a stopped site does nothing (bool: false, "not cancelled").
template<class... P,class... A>inline bool GuardBool(const char* where,volatile long* stopped,bool(*f)(P...),A... a){
 if(!f||*stopped)return false;__try{return f(a...);}__except(FeatureFault(where,stopped,GetExceptionInformation())){return false;}}
template<class... P,class... A>inline void GuardVoid(const char* where,volatile long* stopped,void(*f)(P...),A... a){
 if(!f||*stopped)return;__try{f(a...);}__except(FeatureFault(where,stopped,GetExceptionInformation())){}}

}  // namespace game
