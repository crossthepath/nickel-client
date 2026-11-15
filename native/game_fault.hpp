#pragma once
// Fault boundaries for code that runs from game hooks: fail closed for the feature, fail open for the game.
// A guarded site keeps a "stopped" flag; its first fault is logged once (exception code, module + offset) and the site
// is skipped for the rest of the world, while the game's own code keeps running. Joining a world (StartGame, or a new client
// level seen by the level tick hook) gives every stopped site one more chance, so a fault while a world is torn down does not disable a feature until restart. The flag is per call site, so one faulty
// subscriber never disables the others. These helpers hold no C++ objects, as __try requires.
//
//   static volatile long stopped_=0;if(!game::FeatureStopped(&stopped_))__try{Work();}
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
inline volatile long faultWorld=1;   // world counter; a site is stopped while its flag equals it
inline void FeatureNewWorld(){InterlockedIncrement(&faultWorld);}
inline bool FeatureStopped(volatile long* stopped){return *stopped==faultWorld;}
struct FaultRecord{const char* where;DWORD code;uintptr_t address;bool pending;};
inline thread_local FaultRecord faultRecord{};

// Exception filter: marks the site stopped and records the first fault for FeatureFlush. Nothing else (no lock, no
// formatting, little stack).
inline int FeatureFault(const char* where,volatile long* stopped,EXCEPTION_POINTERS* e){
 const long world=faultWorld;if(stopped&&InterlockedExchange(stopped,world)==world)return EXCEPTION_EXECUTE_HANDLER;   // already reported in this world
 if(e&&e->ExceptionRecord)faultRecord={where,e->ExceptionRecord->ExceptionCode,reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress),true};
 return EXCEPTION_EXECUTE_HANDLER;}

// Writes the recorded fault, if any (call from the __except block). Without a log yet, the record stays pending and the
// next flush on this thread writes it.
inline void FeatureFlush(){
 FaultRecord r=faultRecord;if(!r.pending)return;auto log=faultLog;if(!log)return;faultRecord.pending=false;
 HMODULE mod{};char name[MAX_PATH]="?";
 if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCSTR>(r.address),&mod))GetModuleFileNameA(mod,name,MAX_PATH);
 const char* base=strrchr(name,'\\');char line[400];
 snprintf(line,sizeof(line),"%s: fault %08lx at %s+0x%llx; stopped until the next world",r.where?r.where:"?",r.code,base?base+1:name,
  (unsigned long long)(mod?r.address-reinterpret_cast<uintptr_t>(mod):r.address));
 log(line);}

// Guarded calls through a subscriber pointer. A null pointer or a stopped site does nothing (bool: false, "not cancelled").
template<class... P,class... A>inline bool GuardBool(const char* where,volatile long* stopped,bool(*f)(P...),A... a){
 if(!f||FeatureStopped(stopped))return false;__try{return f(a...);}__except(FeatureFault(where,stopped,GetExceptionInformation())){FeatureFlush();return false;}}
template<class... P,class... A>inline void GuardVoid(const char* where,volatile long* stopped,void(*f)(P...),A... a){
 if(!f||FeatureStopped(stopped))return;__try{f(a...);}__except(FeatureFault(where,stopped,GetExceptionInformation())){FeatureFlush();}}

// Sites that stay stopped until restart and log their call path (Combat). Same two steps as FeatureFault /
// FeatureFlush: the filter only copies the exception and the top of the faulting thread's stack (no lock, no
// allocation, no formatting); FaultTraceFlush, called from the __except block after unwinding, writes the lines
// through faultLog: "<where>: fault <code> at <module>+0x<offset> (address 0x<data>); stopped until restart", then
// "<where>: stack +0x.. +0x.." (up to 8 return addresses into the faulting module).
//   __try{Work();}__except(game::FaultTraceFilter("Name",GetExceptionInformation())){fault=true;game::FaultTraceFlush();}
struct FaultTrace{const char* where;DWORD code;uintptr_t address,data;int words;bool pending;uint64_t stack[256];};
inline thread_local FaultTrace faultTrace{};
inline bool FaultReadWord(const uint64_t* p,uint64_t* out){__try{*out=*p;return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
inline int FaultTraceFilter(const char* where,EXCEPTION_POINTERS* e){
 if(!e||!e->ExceptionRecord)return EXCEPTION_EXECUTE_HANDLER;
 FaultTrace& t=faultTrace;const auto* r=e->ExceptionRecord;
 t.where=where;t.code=r->ExceptionCode;t.address=reinterpret_cast<uintptr_t>(r->ExceptionAddress);
 t.data=r->NumberParameters>1?r->ExceptionInformation[1]:0;t.words=0;
 if(e->ContextRecord){const uint64_t* sp=reinterpret_cast<const uint64_t*>(e->ContextRecord->Rsp);while(t.words<256&&FaultReadWord(sp+t.words,&t.stack[t.words]))++t.words;}
 t.pending=true;return EXCEPTION_EXECUTE_HANDLER;}
inline void FaultTraceFlush(){
 FaultTrace& t=faultTrace;if(!t.pending)return;t.pending=false;auto log=faultLog;if(!log)return;
 const char* where=t.where?t.where:"?";HMODULE mod{};char name[MAX_PATH]="?";
 if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCSTR>(t.address),&mod))GetModuleFileNameA(mod,name,MAX_PATH);
 const char* base=strrchr(name,'\\');char line[400];
 snprintf(line,sizeof(line),"%s: fault %08lx at %s+0x%llx (address 0x%llx); stopped until restart",where,t.code,base?base+1:name,
  (unsigned long long)(t.address-reinterpret_cast<uintptr_t>(mod)),(unsigned long long)t.data);log(line);
 MODULEINFO info{};if(!mod||!K32GetModuleInformation(GetCurrentProcess(),mod,&info,sizeof(info)))return;
 const uintptr_t lo=(uintptr_t)mod,hi=lo+info.SizeOfImage;char path[512];int n=snprintf(path,sizeof(path),"%s: stack",where),found=0;
 for(int i=0;i<t.words&&found<8&&n>0&&n<(int)sizeof(path);++i){const uint64_t v=t.stack[i];
  if(v>lo&&v<hi){n+=snprintf(path+n,sizeof(path)-n," +0x%llx",(unsigned long long)(v-lo));++found;}}
 if(found)log(path);}

}  // namespace game
