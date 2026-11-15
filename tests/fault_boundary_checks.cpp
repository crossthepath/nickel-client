// Fault boundaries (native/game_fault.hpp): a faulting subscriber is logged once, skipped afterwards, and does not stop
// the other subscribers or the caller. Runs without the game (Windows, MSVC structured exceptions).
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include "game_fault.hpp"

namespace {
std::vector<std::string> logged;
void Log(const char* line){logged.push_back(line);}
volatile int calls=0,healthyCalls=0;   // volatile: the count is stored before the fault
bool Faulty(uintptr_t){++calls;volatile int* nowhere=nullptr;return *nowhere==1;}   // access violation
bool Healthy(uintptr_t packet){++healthyCalls;return packet==42;}
void FaultyVoid(int,int){++calls;RaiseException(0xE0000001,0,0,nullptr);}
// Trace sites (FaultTraceFilter / FaultTraceFlush): the fault happens while the log's own lock is held, as when a feature
// faults inside the log or the heap. The log must be written after unwinding, once the lock is released.
std::timed_mutex logLock;bool logBlocked=false;
void LockedLog(const char* line){if(!logLock.try_lock_for(std::chrono::seconds(2))){logBlocked=true;return;}logged.push_back(line);logLock.unlock();}
void FaultHoldingLogLock(){std::lock_guard<std::timed_mutex> hold(logLock);volatile int* nowhere=nullptr;*nowhere=1;}
volatile int traceHandled=0;
void TraceSite(){__try{FaultHoldingLogLock();}__except(game::FaultTraceFilter("Trace site",GetExceptionInformation())){traceHandled=1;game::FaultTraceFlush();}}
int failures=0;
void Check(bool ok,const char* what){if(!ok){++failures;std::printf("FAILED: %s\n",what);}}
}  // namespace

int main(){
 game::faultLog=Log;
 volatile long stopped=0,healthyStopped=0;
 // first call faults: handled, reported once, the site stops
 Check(!game::GuardBool("Test subscriber",&stopped,Faulty,(uintptr_t)1),"a faulting subscriber returns \"not cancelled\"");
 Check(game::FeatureStopped(&stopped),"the faulting site is stopped");
 Check(logged.size()==1&&logged[0].find("Test subscriber: fault c0000005")==0,"one log line with the site and the exception code");
 // later calls skip the stopped site without calling it again or logging again
 for(int i=0;i<5;++i)Check(!game::GuardBool("Test subscriber",&stopped,Faulty,(uintptr_t)1),"a stopped site returns \"not cancelled\"");
 Check(calls==1,"a stopped site is not called again");
 Check(logged.size()==1,"no repeated log line");
 // another site keeps working and its result passes through
 Check(game::GuardBool("Healthy subscriber",&healthyStopped,Healthy,(uintptr_t)42),"a healthy subscriber still cancels");
 Check(!game::GuardBool("Healthy subscriber",&healthyStopped,Healthy,(uintptr_t)7),"a healthy subscriber still lets packets through");
 Check(healthyCalls==2&&healthyStopped==0,"the healthy site is unaffected");
 // a null subscriber is a no-op
 bool(*none)(uintptr_t)=nullptr;volatile long noneStopped=0;
 Check(!game::GuardBool("None",&noneStopped,none,(uintptr_t)1),"a null subscriber does nothing");
 // void subscribers and software exceptions
 volatile long voidStopped=0;game::GuardVoid("Void subscriber",&voidStopped,FaultyVoid,1,2);game::GuardVoid("Void subscriber",&voidStopped,FaultyVoid,1,2);
 Check(game::FeatureStopped(&voidStopped)&&calls==2&&logged.size()==2&&logged[1].find("e0000001")!=std::string::npos,"void subscriber: logged once, stopped");
 // a restore helper keeps running after its first fault: reported once, called every time
 volatile long reported=0;for(int i=0;i<3;++i){__try{++calls;RaiseException(0xE0000002,0,0,nullptr);}__except(game::FeatureFault("Restore helper",&reported,GetExceptionInformation())){game::FeatureFlush();}}
 Check(calls==5&&logged.size()==3&&logged[2].find("Restore helper: fault e0000002")==0,"restore helper: runs every time, logged once");
 // a fault before the log is set is written by the next flush instead of being lost
 game::faultLog=nullptr;volatile long early=0;game::GuardVoid("Early subscriber",&early,FaultyVoid,1,2);
 game::faultLog=Log;game::FeatureFlush();
 Check(logged.size()==4&&logged[3].find("Early subscriber: fault e0000001")==0,"a fault recorded before the log exists is written later");
 // a new world gives a stopped site one more chance; its next fault is reported again
 game::FeatureNewWorld();
 Check(!game::FeatureStopped(&stopped),"a stopped site runs again in the next world");
 game::GuardBool("Test subscriber",&stopped,Faulty,(uintptr_t)1);
 Check(calls==7&&game::FeatureStopped(&stopped)&&logged.size()==5&&logged[4].find("Test subscriber: fault c0000005")==0,"the site faults again: stopped and logged once more");
 // a trace site logs its fault and call path only after unwinding (never inside the exception filter)
 logged.clear();game::faultLog=LockedLog;TraceSite();game::faultLog=Log;
 Check(traceHandled==1,"trace site: the fault is handled");
 Check(!logBlocked,"trace site: nothing is logged while the faulting code still holds the log's lock");
 Check(!logged.empty()&&logged[0].find("Trace site: fault c0000005 at ")==0&&logged[0].find("; stopped until restart")!=std::string::npos,
       "trace site: one line with the site, the exception code and \"stopped until restart\"");
 Check(logged.size()==2&&logged[1].find("Trace site: stack +0x")==0,"trace site: a second line with the call path");
 logged.clear();game::FaultTraceFlush();Check(logged.empty(),"trace site: a flush without a new fault writes nothing");
 if(failures){std::printf("%d fault boundary check(s) failed\n",failures);return 1;}
 std::printf("fault boundary checks passed\n");return 0;
}
