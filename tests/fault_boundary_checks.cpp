// Fault boundaries (native/game_fault.hpp): a faulting subscriber is logged once, skipped afterwards, and does not stop
// the other subscribers or the caller. Runs without the game (Windows, MSVC structured exceptions).
#include <cstdio>
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
int failures=0;
void Check(bool ok,const char* what){if(!ok){++failures;std::printf("FAILED: %s\n",what);}}
}  // namespace

int main(){
 game::faultLog=Log;
 volatile long stopped=0,healthyStopped=0;
 // first call faults: handled, reported once, the site stops
 Check(!game::GuardBool("Test subscriber",&stopped,Faulty,(uintptr_t)1),"a faulting subscriber returns \"not cancelled\"");
 Check(stopped==1,"the faulting site is stopped");
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
 Check(voidStopped==1&&calls==2&&logged.size()==2&&logged[1].find("e0000001")!=std::string::npos,"void subscriber: logged once, stopped");
 // a restore helper keeps running after its first fault: reported once, called every time
 volatile long reported=0;for(int i=0;i<3;++i){__try{++calls;RaiseException(0xE0000002,0,0,nullptr);}__except(game::FeatureFault("Restore helper",&reported,GetExceptionInformation())){game::FeatureFlush();}}
 Check(calls==5&&logged.size()==3&&logged[2].find("Restore helper: fault e0000002")==0,"restore helper: runs every time, logged once");
 if(failures){std::printf("%d fault boundary check(s) failed\n",failures);return 1;}
 std::printf("fault boundary checks passed\n");return 0;
}
