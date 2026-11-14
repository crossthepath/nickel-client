#pragma once
// Fault boundaries for code that runs from game hooks: fail closed for the feature, fail open for the game.
// A guarded site keeps a "stopped" flag; its first fault is logged once (exception code, module + offset) and the site
// is skipped from then on, while the game's own code keeps running. The flag is per call site, so one faulty
// subscriber never disables the others. These helpers hold no C++ objects, as __try requires.
//
//   static volatile long stopped_=0;if(!stopped_)__try{Work();}
//   __except(game::FeatureFault("Name",&stopped_,GetExceptionInformation())){game::FeatureFlush();}
//
// The filter only records the fault: it runs before unwinding, while locks taken by the faulting code may still be
// held (the log itself takes one) and possibly on an exhausted stack. FeatureFlush writes the line from the handler,
// after unwinding. Restore helpers that undo a change made earlier in the same frame must keep running: they pass a
// flag that is only used to log once and do not test it.
#include <Windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstring>

namespace game {

inline void(*volatile faultLog)(const char*)=nullptr;   // set by the overlay (WriteLog)
struct FaultRecord{const char* where;DWORD code;uintptr_t address;bool pending;};
inline thread_local FaultRecord faultRecord{};

// Exception filter: marks the site stopped and records the first fault for FeatureFlush. Nothing else (no lock, no
// formatting, little stack).
inline int FeatureFault(const char* where,volatile long* stopped,EXCEPTION_POINTERS* e){
 if(stopped&&InterlockedExchange(stopped,1))return EXCEPTION_EXECUTE_HANDLER;   // already reported
 if(e&&e->ExceptionRecord)faultRecord={where,e->ExceptionRecord->ExceptionCode,reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress),true};
 return EXCEPTION_EXECUTE_HANDLER;}

// Writes the recorded fault, if any (call from the __except block). Without a log yet, the record stays pending and the
// next flush on this thread writes it.
inline void FeatureFlush(){
 FaultRecord r=faultRecord;if(!r.pending)return;auto log=faultLog;if(!log)return;faultRecord.pending=false;
 HMODULE mod{};char name[MAX_PATH]="?";
 if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCSTR>(r.address),&mod))GetModuleFileNameA(mod,name,MAX_PATH);
 const char* base=strrchr(name,'\\');char line[400];
 snprintf(line,sizeof(line),"%s: fault %08lx at %s+0x%llx; stopped until restart",r.where?r.where:"?",r.code,base?base+1:name,
  (unsigned long long)(mod?r.address-reinterpret_cast<uintptr_t>(mod):r.address));
 log(line);}

// Guarded calls through a subscriber pointer. A null pointer or a stopped site does nothing (bool: false, "not cancelled").
template<class... P,class... A>inline bool GuardBool(const char* where,volatile long* stopped,bool(*f)(P...),A... a){
 if(!f||*stopped)return false;__try{return f(a...);}__except(FeatureFault(where,stopped,GetExceptionInformation())){FeatureFlush();return false;}}
template<class... P,class... A>inline void GuardVoid(const char* where,volatile long* stopped,void(*f)(P...),A... a){
 if(!f||*stopped)return;__try{f(a...);}__except(FeatureFault(where,stopped,GetExceptionInformation())){FeatureFlush();}}

}  // namespace game
