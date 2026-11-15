// Combat modules: shared click/attack building blocks, Auto Clicker, Right Clicker, Click Assist, Kill Aura and
// Trigger Bot.
// - Click RNG: std::mt19937_64 seeded on first use; a fresh std::normal_distribution per draw.
// - Cadence model: Normal draws the CPS from the range; Extra / Extra+ draw a log-normal interval around the current
// CPS with AR(1) drift, bursts and fatigue, plus a hold duration in Extra+. Scheduling compensates for lateness.
// - Attack: the hit point is the eye (feet + 1.62) clamped into the target's box, sent as an
// ItemUseOnActorInventoryTransaction (vtable RVA 0xf15a0f0, type 3, action 1, runtime id, held slot, player position,
// hit point) through the same virtual (+0x718) as item transactions.
// Targets carrying component 0xacb47b38 are skipped.
// - Weapon: item name ending in "_sword" or "_axe", or an item marked Weapon in Item Profiles.
// The "valid player" guard means the in-game HUD screen (hud_screen) is the active screen; it is the only case where
// the clickers act without the Inventory option.
#pragma once
#include "game_actions.hpp"
#include "game_events.hpp"
#include "game_rotation.hpp"
#include "game_visual_entities.hpp"
#include "game_utility_packets.hpp"
#include "menu.hpp"
#include <random>
#include <mutex>
#include <string>
#include <vector>

namespace game {
inline std::mt19937_64& ClickRng(){static std::mt19937_64* rng=new std::mt19937_64{std::random_device{}()};return *rng;}
inline double Rand01(){return (double)(ClickRng()()>>11)*1.1102230246251565e-16;}
inline float Rand24(){return (float)(((uint32_t)(ClickRng()()>>32))>>8)*5.9604644775390625e-08f;}
inline float RandRange(float a,float b){return (b-a)*Rand24()+a;}
inline double Normal01(){std::normal_distribution<double> n(0.0,1.0);return n(ClickRng());}
inline int64_t CombatNs(){LARGE_INTEGER f{},c{};QueryPerformanceFrequency(&f);QueryPerformanceCounter(&c);return (int64_t)(c.QuadPart/f.QuadPart)*1000000000LL+(int64_t)(c.QuadPart%f.QuadPart)*1000000000LL/f.QuadPart;}

// ---- cadence model
struct ClickModel{double total=0,ar=0,arMean=0,nextShift=0,offset=0;int burst=0,fatigueLeft=0;bool init=false;};
inline void ClickModelInit(ClickModel& m,int mode){
 m.init=true;m.ar=Rand01()*1.2+-0.6;m.arMean=Rand01()+-0.5;m.nextShift=Rand01()*60000.0+30000.0;
 if(mode==2&&Rand01()<0.7)m.burst=(int)(((uint32_t)(ClickRng()()>>32))>>30)+2;}
inline double ClickFatigue(ClickModel& m,int mode){
 double a=0.72,b=0.16000000000000003;
 if(m.fatigueLeft<1){
  if(mode==2){
   if(Rand01()<0.025)return Rand01()*0.17000000000000004+0.38;
   const double p=(1.0-std::exp(m.total/-12000.0))*0.4*0.045+0.012;
   if(Rand01()<p)return Rand01()*1.2+1.8;}
  const double r=Rand01();uint64_t u=ClickRng()();
  if(0.04<=r){if(0.03<=(double)(u>>11)*1.1102230246251565e-16)return 1.0;return Rand01()*0.44999999999999996+1.35;}
  uint64_t hi=0,lo=_umul128(u,3,&hi);while(lo==0){u=ClickRng()();lo=_umul128(u,3,&hi);}
  m.fatigueLeft=(int)hi+1;}
 else m.fatigueLeft=m.fatigueLeft-1;
 return Rand01()*b+a;}
inline void ClickModelUpdate(ClickModel& m,double interval){   // AR(1) drift of the interval
 if(m.nextShift<=m.total){m.arMean=Rand01()+-0.5;m.nextShift=Rand01()*60000.0+30000.0+m.nextShift;}
 const double rho=std::exp(interval/-2500.0);double s=1.0-rho*rho;s=s<0.0?std::sqrt(-s)*0.0:std::sqrt(s);s*=0.45;
 const double v=Normal01()*s+rho*(m.ar-m.arMean)+m.arMean;m.ar=1.0<v?1.0:std::max(-1.0,v);}
// Next click interval (ms) and hold duration (ms, Extra+ only).
inline void ClickNext(ClickModel& m,int mode,float low,float high,double* interval,double* hold){
 if(mode==0){float cps=low;if(low<high)cps=low+(high-low)*Rand24();*interval=(double)(1000.0f/cps);*hold=0;return;}
 if(!m.init)ClickModelInit(m,mode);
 const double lo=low,hi=high<=low?low:high,mid=(lo+hi)*0.5,half=(hi-lo)*0.5;double v=m.ar;const int burst=m.burst;
 if(mode==2){const double d4=std::exp(m.total/-1500.0)*0.5;const double d5=v+(1.0-std::exp(m.total/-12000.0))*-0.4+d4;v=(burst>0&&d5<0.85)?0.85:d5;}
 const double c=1.0<v?1.0:std::max(-1.0,v);if(burst>0)m.burst=burst-1;
 const double base=1000.0/(mid+half*c),sigma=mode==2?0.14:0.1;double z,A,B;
 if(mode==2){const double r=Rand01();if(0.12<=r){if(0.17<=r)z=Normal01();else{z=Normal01();z=(Rand01()*0.6000000000000001+1.4)*z;}}else z=Rand01()*3.0+-1.5;A=0.35;B=3.2;}
 else{z=Normal01();A=0.55;B=2.0;}
 const double logn=std::exp(-0.5*sigma*sigma+z*sigma),fatigue=ClickFatigue(m,mode);
 const double d12=0.12*base,d14=0.1*m.offset,corr=d12<d14?d12:std::max(-d12,d14);
 const double minI=std::max(10.0,(1000.0/hi)*A),maxI=std::max(minI,(1000.0/lo)*B);
 double t=fatigue*logn*base-corr;t=maxI<t?maxI:std::max(minI,t);
 const double lim=4.0*base,off=(m.offset+t)-base;m.offset=lim<off?lim:std::max(-lim,off);
 m.total+=t;ClickModelUpdate(m,t);*interval=t;*hold=0;
 if(mode==2){double h=std::min(t,99.0);h=((145.0-h)*0.4)/100.0+0.3;const double n=Normal01();const double d=1.0-(n*0.05+h);*hold=(0.7<d?0.7:std::max(0.2,d))*t;}}
// Forced button duration (ns); 0 without a hold duration (a single pass).
inline int64_t ClickDuration(double holdMs,double delayMs,bool holdSetting){
 if(!(0.0<holdMs))return 0;const double minRel=std::max(20.0,0.3*delayMs);const double d2=holdSetting?delayMs-holdMs:holdMs;
 const double d3=std::max(0.0,delayMs-minRel),d1=std::max(0.0,d2);return (int64_t)((d3<d2?d3:d1)*1000000.0);}

// ---- target and attack
inline bool CrosshairHit(const Context& c,uint64_t* hit,int32_t* kind){uint64_t level{};if(!Read(c.actor+0x200,&level)||!level)return false;const uint64_t getter=Virtual0(c,level,0xa78);
 return getter&&CallGetter(getter,level,hit)&&*hit&&Read(*hit+0x18,kind);}
inline uintptr_t CrosshairActor(const Context& c){uint64_t hit{},holder{},actor{};int32_t kind{};uint32_t entity{};uintptr_t owners{},slot{};
 if(!CrosshairHit(c,&hit,&kind)||kind!=1||!Read(hit+0x38,&holder)||!holder||!Read(hit+0x48,&entity))return 0;
 if(!Storage(holder+0x30,Fnv("ActorOwnerComponent"),&owners)||!InStorage(owners,entity,8,&slot)||!Read(slot,&actor))return 0;return (uintptr_t)actor;}
inline bool ActorSkipAttack(uintptr_t actor){uintptr_t registry{},storage{},slot{};uint32_t entity{};   // skip targets carrying component 0xacb47b38
 uint32_t packed{};(void)slot;return ActorEntity(actor,&registry,&entity)&&Storage(registry,0xacb47b38u,&storage)&&Packed(storage,entity,&packed);}   // presence only (empty component)
inline bool ActorFeet(uintptr_t actor,float* p){uint64_t st{};float lift=0;uintptr_t registry{},off{};uint32_t entity{};if(!Read(actor+0x240,&st)||!st||!Floats(st,p,3))return false;
 if(ActorEntity(actor,&registry,&entity)&&Element(registry,entity,"OffsetsComponent",0x44,&off))Floats(off,&lift,1);p[1]-=lift;return true;}
inline bool AttackActor(const Context& c,uintptr_t target){   // swing, then attack transaction
 float feet[3],own[3],size[6];uint64_t bb{},st{},id{};if(!ActorFeet(target,feet)||!ActorFeet(c.actor,own)||!Read(target+0x248,&bb)||!bb||!Floats(bb,size,6))return false;
 const float hw=(size[3]-size[0])*0.5f,hd=(size[5]-size[2])*0.5f;const float lo[3]={feet[0]-hw,feet[1],feet[2]-hd},hi[3]={feet[0]+hw,(size[4]-size[1])+feet[1],feet[2]+hd};
 const float eye[3]={own[0]+0.0f,own[1]+1.6200100183486938f,own[2]};float point[3];for(int i=0;i<3;++i)point[i]=std::min(hi[i],std::max(eye[i],lo[i]));
 if(!RuntimeId(target,&id)||!Read(c.actor+0x240,&st)||!st)return false;float pos[3];if(!Floats(st,pos,3))return false;
 int32_t* sel=ActionSelected(c.actor);if(!sel)return false;const uint64_t stack=ActionHeld(c,c.actor);
 uint8_t* t=(uint8_t*)::operator new(0xf8);memset(t,0,0xf8);uint8_t* inv=t+0x10;*(float*)inv=1.0f;
 uint64_t* head=(uint64_t*)::operator new(0x38);head[0]=head[1]=(uint64_t)head;*(uint64_t*)(inv+8)=(uint64_t)head;*(uint64_t*)(inv+0x10)=0;
 uint64_t* buckets=(uint64_t*)::operator new(0x80);for(int i=0;i<16;++i)buckets[i]=(uint64_t)head;
 *(uint64_t*)(inv+0x18)=(uint64_t)buckets;*(uint64_t*)(inv+0x20)=(uint64_t)(buckets+16);*(uint64_t*)(inv+0x28)=(uint64_t)(buckets+16);*(uint64_t*)(inv+0x30)=7;*(uint64_t*)(inv+0x38)=8;
 DescriptorBuild(c,(uint64_t)(t+0x80),stack);*(uint64_t*)t=c.base+Rva(0xf15a0f0);*(int32_t*)(t+8)=3;
 *(uint64_t*)(t+0x68)=id;*(int32_t*)(t+0x70)=1;*(int32_t*)(t+0x74)=*sel;memcpy(t+0xe0,pos,12);memcpy(t+0xec,point,12);
 SendUseTransaction(c,c.actor,t);return true;}

// ---- weapon check (Weapons Only)
struct CombatShared{std::mutex guard;std::vector<std::string> weaponItems;std::string screen;};
inline CombatShared& Combat(){static auto* s=new CombatShared;return *s;}
inline std::string BareItemName(std::string n){const size_t at=n.find(':');if(at!=std::string::npos)n=n.substr(at+1);for(auto& ch:n)ch=(char)tolower((unsigned char)ch);return n;}
inline bool IsWeapon(const Context& c){std::string name;if(!ActionName(ActionHeld(c,c.actor),&name))return false;name=BareItemName(name);
 auto ends=[&](const char* s){const size_t n=strlen(s);return name.size()>=n&&name.compare(name.size()-n,n,s)==0;};if(ends("_sword")||ends("_axe"))return true;
 auto& s=Combat();std::lock_guard<std::mutex> lock(s.guard);for(auto& w:s.weaponItems)if(BareItemName(w)==name)return true;return false;}

// ---- Auto Clicker (ScreenViewRenderEvent, priority 2)
struct AutoClickerShared{std::mutex guard;bool enabled=false,hold=true,breakBlocks=true,weaponsOnly=false,inventory=false,jitter=false;
 float low=15,high=18,jitterStrength=0.5f;int mode=0,randomization=0;
 bool init=false,active=false;double delay=0,interval=0,holdMs=0;int64_t last=0;ClickModel model;};
inline AutoClickerShared& AutoClicker(){static auto* s=new AutoClickerShared;return *s;}
constexpr int kRotAutoClicker=11;
inline void AutoClickerReset(AutoClickerShared& s){s.init=false;s.delay=0;s.last=CombatNs();s.active=true;s.interval=s.holdMs=0;s.model=ClickModel{};overlay::ClearButton(0);}
inline void AutoClickerSchedule(AutoClickerShared& s){double interval=0,hold=0;ClickNext(s.model,s.randomization,s.low,s.high,&interval,&hold);const int64_t now=CombatNs();   // next delay, compensating for lateness
 double comp=0;if(s.randomization!=0&&s.init){comp=interval*0.5;const double over=(double)(now-s.last)/1e6-s.delay;comp=comp<over?comp:std::max(0.0,over);}
 s.delay=interval-comp;s.interval=interval;s.holdMs=hold;s.last=now;s.active=true;s.init=true;}
inline void AutoClickerTrace(const char* why){static const char* last=nullptr;if(why==last)return;last=why;if(actionLog){char t[160];snprintf(t,sizeof(t),"Auto Clicker: %s",why);actionLog(t);}}
inline void AutoClickerWork(const char* screen){auto& s=AutoClicker();std::lock_guard<std::mutex> lock(s.guard);if(!s.enabled)return;if(menuPause){AutoClickerReset(s);return;}Context c{};
 if(!Local(&c)){AutoClickerTrace("no local player");AutoClickerReset(s);return;}
 // Screen rendering visits every layer (hud_screen, hud_crosshair_screen...): act once per frame, on hud_screen when
 // no UI is open, or on inventory_screen with the Inventory option; any other screen suspends clicking.
 static uint64_t lastUi=0;const bool hud=strncmp(screen,"hud",3)==0;if(!hud)lastUi=GetTickCount64();const bool uiOpen=GetTickCount64()-lastUi<150;
 const bool playing=hud&&!uiOpen;
 if(hud&&(uiOpen||strcmp(screen,"hud_screen")))return;
 if(!hud&&!(s.inventory&&strcmp(screen,"inventory_screen")==0)){AutoClickerReset(s);return;}
 if(s.hold&&!(GetAsyncKeyState(VK_LBUTTON)&0x8000)){AutoClickerReset(s);return;}
 if(s.weaponsOnly&&!IsWeapon(c)){AutoClickerReset(s);return;}
 if(s.breakBlocks){uint64_t hit{};int32_t kind=-1;if(CrosshairHit(c,&hit,&kind)&&kind==0){AutoClickerReset(s);return;}}
 if(!s.init){AutoClickerSchedule(s);return;}
 if(!s.active)return;if((double)(CombatNs()-s.last)/1e6<=s.delay)return;
 if(s.jitter){const float j=s.jitterStrength;RotRequest r;r.module=kRotAutoClicker;r.yaw=RandRange(-j,j);r.pitch=RandRange(-j,j);r.relative=true;r.mode=0;r.priority=0;Rotations().enabled[kRotAutoClicker]=true;RotSubmit(std::move(r));}
 AutoClickerSchedule(s);
 if(s.mode==1&&playing){ActionSwing(c,c.actor,4);overlay::RecordClick(0);const uintptr_t target=CrosshairActor(c);if(target&&!ActorSkipAttack(target))AttackActor(c,target);return;}
 overlay::ForceButton(0,!s.hold,ClickDuration(s.holdMs,s.delay,s.hold));}
// ---- Right Clicker (GameUpdateEvent; BlockBreakDelayEvent, priority 2). Forces the right button like Auto Clicker;
// Delay in ms (CPS = 1000 / Delay for both bounds); Start Delay counts from activation; Blocks Only and item
// whitelist / blacklist. "Valid player" guard: no UI open (last non-HUD screen render more than 150 ms ago).
inline std::atomic<uint64_t> combatUiTick{0};
inline bool CombatNoInterface(){return !menuPause&&GetTickCount64()-combatUiTick.load()>150;}
struct RightClickerShared{std::mutex guard;bool enabled=false,onlyHolding=true,blocksOnly=true,useWhite=false,useBlack=false;int delayMs=100,startDelay=0,randomization=0;
 std::vector<std::string> white,black;bool clicked=false,init=false;int64_t last=0,start=0;double delay=0,interval=0,holdMs=0;ClickModel model;};
inline RightClickerShared& RightClicker(){static auto* s=new RightClickerShared;return *s;}
inline void RightClickerReset(RightClickerShared& s){s.init=false;s.model=ClickModel{};s.delay=s.interval=s.holdMs=0;s.last=0;s.start=0;}
inline double RightClickerDelay(const RightClickerShared& s){return s.randomization==0?(double)s.delayMs:s.delay;}
inline void RightClickerSchedule(RightClickerShared& s,double overshoot){double interval=0,hold=0;const float cps=1000.0f/(float)std::max(1,s.delayMs);
 ClickNext(s.model,s.randomization,cps,cps,&interval,&hold);double comp=0;if(overshoot<s.delay){comp=0.5*interval<overshoot?0.5*interval:std::max(0.0,overshoot);}
 s.delay=interval-comp;s.interval=interval;s.holdMs=hold;}
inline bool InList(const std::vector<std::string>& list,const std::string& name){const std::string n=BareItemName(name);for(auto& e:list)if(BareItemName(e)==n)return true;return false;}
inline void RightClickerWork(){auto& s=RightClicker();std::lock_guard<std::mutex> lock(s.guard);s.clicked=false;if(!s.enabled)return;Context c{};
 if(!Local(&c)||!CombatNoInterface()){RightClickerReset(s);return;}
 if(s.onlyHolding&&!(GetAsyncKeyState(VK_RBUTTON)&0x8000)){RightClickerReset(s);return;}   // Start Delay restarts on each press
 const int64_t now=CombatNs();if(!s.init){s.init=true;s.start=now;RightClickerSchedule(s,0);}
 if((double)(now-s.start)/1e6<(double)s.startDelay)return;
 const double elapsed=(double)(now-s.last)/1e6,cur=RightClickerDelay(s);if(elapsed<=cur)return;
 s.last=now;const uint64_t stack=ActionHeld(c,c.actor);std::string name;const bool item=stack&&ActionItem(stack)&&ActionName(stack,&name);bool allowed=true;
 if(s.blocksOnly){if(!item)return;if(!BlockStack(stack))allowed=false;}
 if(allowed&&s.useWhite){if(!item)return;allowed=InList(s.white,name);}
 if(allowed&&s.useBlack&&item)allowed=!InList(s.black,name);
 if(!allowed){return;}
 RightClickerSchedule(s,elapsed-cur);s.clicked=true;overlay::ForceButton(1,!s.onlyHolding,ClickDuration(s.holdMs,RightClickerDelay(s),s.onlyHolding));}
inline void RightClickerUseDelay(uint64_t* next){auto& s=RightClicker();std::lock_guard<std::mutex> lock(s.guard);if(!s.enabled||!s.clicked)return;   // BlockBreakDelayEvent: use delay override
 const int64_t ms=(int64_t)std::lround(RightClickerDelay(s)),now=CombatNs();if(ms*1000000<(int64_t)*next-now)*next=(uint64_t)(now+ms*1000000);}
// ---- Click Assist (GameUpdate, priority 2): on each new left press, with probability Chance (integer roll 0..99),
// adds Extra Clicks, each a forced one-pass release, spaced by a delay drawn from the Delay range.
// With Blocks off, no assist while the crosshair is on a block.
struct ClickAssistShared{std::mutex guard;bool enabled=false,blocks=false;int chance=100,extra=1;float low=30,high=60;bool held=false,active=false;int remaining=0;float delay=0;int64_t last=0;};
inline ClickAssistShared& ClickAssist(){static auto* s=new ClickAssistShared;return *s;}
inline void ClickAssistReset(ClickAssistShared& s){s.held=false;s.remaining=0;s.delay=0;s.last=CombatNs();s.active=true;overlay::ClearButton(0);}
inline int Percent100(){uint64_t hi=0,lo=_umul128(ClickRng()(),100,&hi);while((lo&0xfffffffffffffff0ull)==0)lo=_umul128(ClickRng()(),100,&hi);return (int)hi;}
inline void ClickAssistWork(){auto& s=ClickAssist();std::lock_guard<std::mutex> lock(s.guard);if(!s.enabled)return;Context c{};
 if(!Local(&c)||!CombatNoInterface()){if(s.held||s.remaining>0)overlay::ClearButton(0);s.held=false;s.remaining=0;return;}
 const bool held=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0,was=s.held;s.held=held;
 if(!held&&was){s.remaining=0;overlay::ClearButton(0);}
 if(held&&!was&&s.remaining==0){uint64_t hit{};int32_t kind=-1;const bool onBlock=CrosshairHit(c,&hit,&kind)&&kind==0;
  if((s.blocks||!onBlock)&&Percent100()<s.chance){s.remaining=s.extra;s.delay=s.low<s.high?s.low+(s.high-s.low)*Rand24():s.low;s.last=CombatNs();s.active=true;}}   // arm the extra clicks
 if(s.remaining!=0&&s.active&&(double)s.delay<(double)(CombatNs()-s.last)/1e6){overlay::ForceButton(0,false,0);--s.remaining;
  s.delay=s.low<s.high?s.low+(s.high-s.low)*Rand24():s.low;s.last=CombatNs();s.active=true;}}
// ---- Kill Aura (GameUpdate, PacketSend; the suffix shows the mode name). State: last time, scheduled flag,
// delay (ms), teleport side, target, and teleported / arrived flags.
// - Target: walk all actors (ActorOwnerComponent) through the common filter (local player excluded, alive,
// Players Only, Anti Bot / Teams). In range when the distance from the player to the nearest point of the target
// box is <= range and, outside Teleport, hypot(remainderf(yaw), pitch) to that point is <= Max Angle. Pick range:
// Distance (Default), max(Aim Range, Distance) (Silent), Teleport Range (Teleport). The closest centre wins.
// - Cadence: delay = 1000 / APS drawn from the range, on the first pass and after each attack.
// - Attack: swing (4), then the attack transaction; Multi Targets hits every target within Distance.
// - Default attacks without turning. Silent requests a rotation (mode 2 = body turned, camera untouched,
// priority 2) towards the nearest box point and attacks in the "reached" callback if the target was in range
// and the delay elapsed.
// - Teleport: free side around the box centre (Box Size; order side+1, side^2, side-1, side; feet and head
// replaceable), position = (x, box bottom + eye height, z), written like a teleport (position, previous position,
// zero velocity, shifted box); attacks on the GameUpdate after a PlayerAuthInputPacket (0x90) is sent.
inline bool CombatTargetValid(const Context& c,uintptr_t target,bool playersOnly,bool skipMarked=true,bool bots=true,bool teams=true);
// Per-pass target filter inputs (roster frame, team rules, own identity): identical for every actor of one pass, so they are read lazily once and reused.
struct CombatTargetFilter{bool botsBuilt=false,teamsBuilt=false;std::shared_ptr<const VisualRosterFrame> roster;nickel::visual::TeamRules rules;VisualIdentity self{};};
struct CombatTargets{std::mutex guard;nickel::visual::TeamRules teams;};
inline CombatTargets& CombatTargetRules(){static auto* s=new CombatTargets;return *s;}
inline bool CombatTargetValid(const Context& c,uintptr_t target,bool playersOnly,bool skipMarked,CombatTargetFilter& filter,bool bots=true,bool teams=true);
constexpr int kRotKillAura=12;
struct KillAuraConfig{int mode=0;float low=8,high=12,distance=3,aimRange=3,tpRange=8,boxSize=1,maxAngle=90;bool multi=false,mouseDown=false,weaponsOnly=false,playersOnly=false;};
struct KillAuraShared{std::mutex guard;std::atomic<bool> enabled{false};KillAuraConfig cfg;std::atomic<int64_t> last{0};std::atomic<bool> scheduled{false};std::atomic<float> delay{0};
 int side=0;std::atomic<bool> pending{false},arrived{false};uint64_t pendingTarget=0;};
inline KillAuraShared& KillAura(){static auto* s=new KillAuraShared;return *s;}
inline void KillAuraActivate(KillAuraShared& s){s.last=CombatNs();s.scheduled=true;s.delay=0;s.side=0;s.pending=false;s.arrived=false;}
inline void KillAuraSchedule(const KillAuraConfig& k){auto& s=KillAura();const float aps=k.low<k.high?k.low+(k.high-k.low)*Rand24():k.low;
 s.delay=1000.0f/std::max(0.1f,aps);s.last=CombatNs();s.scheduled=true;}
inline bool ActorBox(uintptr_t actor,float* lo,float* hi){float feet[3],size[6];uint64_t bb{};if(!ActorFeet(actor,feet)||!Read(actor+0x248,&bb)||!bb||!Floats(bb,size,6))return false;
 const float hw=(size[3]-size[0])*0.5f,hd=(size[5]-size[2])*0.5f;lo[0]=feet[0]-hw;lo[1]=feet[1];lo[2]=feet[2]-hd;hi[0]=feet[0]+hw;hi[1]=(size[4]-size[1])+feet[1];hi[2]=feet[2]+hd;return true;}
inline bool ActorPos(uintptr_t actor,float* p){uint64_t st{};return Read(actor+0x240,&st)&&st&&Floats(st,p,3);}
inline bool AimAtBox(const Context& c,uintptr_t target,float* yaw,float* pitch){float lo[3],hi[3],own[3];if(!ActorBox(target,lo,hi)||!ActorFeet(c.actor,own))return false;   // yaw / pitch to the nearest point of the target box
 const float eye[3]={own[0],own[1]+1.6200100183486938f,own[2]};float d[3];for(int i=0;i<3;++i)d[i]=std::min(hi[i],std::max(eye[i],lo[i]))-eye[i];
 *pitch=atan2f(d[1],std::hypot(d[0],d[2]))*-57.295780181884766f;*yaw=atan2f(-d[0],d[2])*57.295780181884766f;return true;}
inline bool KillAuraScore(const Context& c,const KillAuraConfig& k,uintptr_t target,float range,float* score){float yaw{},pitch{},lo[3],hi[3],p[3],t[3];   // range and angle test
 if(!AimAtBox(c,target,&yaw,&pitch)||!ActorBox(target,lo,hi)||!ActorPos(c.actor,p))return false;
 float sq=0;for(int i=0;i<3;++i){const float q=std::min(hi[i],std::max(p[i],lo[i]))-p[i];sq+=q*q;}if(sq>range*range)return false;
 if(k.mode!=2){float cp{},cy{};if(!RotRead(c.actor,&cp,&cy))return false;if(std::hypot(remainderf(yaw-cy,360.f),pitch-cp)>k.maxAngle)return false;}
 if(!ActorPos(target,t))return false;if(score)*score=(t[0]-p[0])*(t[0]-p[0])+(t[1]-p[1])*(t[1]-p[1])+(t[2]-p[2])*(t[2]-p[2]);return true;}
inline float KillAuraPickRange(const KillAuraConfig& k){return k.mode==2?k.tpRange:(k.mode==1?std::max(k.aimRange,k.distance):k.distance);}
template<class F> inline void ForEachActor(const Context& c,F&& f){uintptr_t owners{};uint64_t begin{},end{};if(!Storage(c.registry,Fnv("ActorOwnerComponent"),&owners)||!Read(owners+0x20,&begin)||!Read(owners+0x28,&end)||end<begin||(end-begin)%4||(end-begin)>4*65536)return;
 for(uint64_t at=begin;at<end;at+=4){uint32_t entity{};uintptr_t slot{};uint64_t actor{};if(!Read(at,&entity))break;if(entity>=0xfffc0000u)continue;if(!InStorage(owners,entity,8,&slot)||!Read(slot,&actor)||!actor)continue;if(!f((uintptr_t)actor))break;}}
inline void KillAuraAttack(const Context& c,const KillAuraConfig& k,uintptr_t target){
 if(!k.multi){ActionSwing(c,c.actor,4);overlay::RecordClick(0);AttackActor(c,target);KillAuraSchedule(k);return;}
 bool any=false;CombatTargetFilter filter;ForEachActor(c,[&](uintptr_t a){if(CombatTargetValid(c,a,k.playersOnly,true,filter)&&KillAuraScore(c,k,a,k.distance,nullptr)){if(!any){ActionSwing(c,c.actor,4);overlay::RecordClick(0);}any=true;AttackActor(c,a);}return true;});
 if(any)KillAuraSchedule(k);}
inline uintptr_t KillAuraResolve(const Context& c,uint64_t id){uintptr_t a{};return id&&ActorByRuntimeId(c.registry,id,&a)?a:0;}
inline bool KillAuraGates(const Context& c,const KillAuraConfig& k){if(k.mouseDown&&!(GetAsyncKeyState(VK_LBUTTON)&0x8000))return false;if(k.weaponsOnly&&!IsWeapon(c))return false;return true;}
inline void KillAuraTeleport(const Context& c,const KillAuraConfig& k,uintptr_t target){auto& s=KillAura();float lo[3],hi[3];uint64_t id{};
 if(!ActorBox(target,lo,hi)||!RuntimeId(target,&id))return;const uint64_t region=ActorRegion(c);if(!region)return;
 static const float dirs[4][2]={{-1,-1},{1,-1},{1,1},{-1,1}};const float cx=(lo[0]+hi[0])*0.5f,cz=(lo[2]+hi[2])*0.5f;const int by=(int)floorf(lo[1]);
 const int cur=s.side;const int order[4]={(cur+1)&3,(cur&3)^2,(cur-1)&3,cur&3};int chosen=-1;float px=0,pz=0;
 for(int side:order){const float x=cx+dirs[side][0]*k.boxSize,z=cz+dirs[side][1]*k.boxSize;const int32_t feet[3]={(int32_t)floorf(x),by,(int32_t)floorf(z)},head[3]={feet[0],by+1,feet[2]};
  if(BlockReplaceable(RegionBlock(c,region,feet))&&BlockReplaceable(RegionBlock(c,region,head))){chosen=side;px=x;pz=z;break;}}
 if(chosen<0)return;float eyeH=0;{uintptr_t reg{},off{};uint32_t ent{};if(ActorEntity(c.actor,&reg,&ent)&&Element(reg,ent,"OffsetsComponent",0x44,&off))Floats(off,&eyeH,1);}
 const float to[3]={px,lo[1]+eyeH,pz};uint64_t st{},bb{};float old[3],box[6];if(!Read(c.actor+0x240,&st)||!st||!Floats(st,old,3)||!Read(c.actor+0x248,&bb)||!bb||!Floats(bb,box,6))return;   // teleport: position, previous position, zero velocity, shifted box
 float* sv=(float*)st;for(int i=0;i<3;++i){sv[i]=to[i];sv[3+i]=to[i];sv[6+i]=0;}float* b=(float*)bb;for(int i=0;i<3;++i){b[i]+=to[i]-old[i];b[3+i]+=to[i]-old[i];}
 s.side=chosen;s.pending=true;s.pendingTarget=id;s.arrived=false;}
inline void KillAuraWork(){auto& s=KillAura();if(!s.enabled)return;KillAuraConfig k;{std::lock_guard<std::mutex> lock(s.guard);k=s.cfg;}Context c{};
 if(!Local(&c)||!CombatNoInterface())return;if(!KillAuraGates(c,k))return;
 if(s.pending){if(!s.arrived)return;s.pending=false;s.arrived=false;const uintptr_t t=KillAuraResolve(c,s.pendingTarget);
  if(!t||!CombatTargetValid(c,t,k.playersOnly)||!KillAuraScore(c,k,t,k.distance,nullptr))return;KillAuraAttack(c,k,t);return;}
 uintptr_t best=0;float bestScore=3.4028234663852886e+38f;const float range=KillAuraPickRange(k);
 CombatTargetFilter filter;ForEachActor(c,[&](uintptr_t a){float sc{};if(CombatTargetValid(c,a,k.playersOnly,true,filter)&&KillAuraScore(c,k,a,range,&sc)&&sc<bestScore){bestScore=sc;best=a;}return true;});
 if(!best)return;if(s.delay.load()==0.0f)KillAuraSchedule(k);
 const bool ready=s.scheduled&&(double)s.delay.load()<(double)(CombatNs()-s.last.load())/1e6;
 if(k.mode==1){const bool attack=ready&&KillAuraScore(c,k,best,k.distance,nullptr);float yaw{},pitch{};if(!AimAtBox(c,best,&yaw,&pitch))return;uint64_t id{};if(!RuntimeId(best,&id))return;
  RotRequest r;r.module=kRotKillAura;r.yaw=yaw;r.pitch=pitch;r.mode=2;r.priority=2;
  r.predicate=[k,id](uintptr_t){Context c{};if(!KillAura().enabled||!Local(&c)||!KillAuraGates(c,k))return false;const uintptr_t t=KillAuraResolve(c,id);   // rotation still wanted
   return t&&CombatTargetValid(c,t,k.playersOnly)&&KillAuraScore(c,k,t,KillAuraPickRange(k),nullptr);};
  if(attack)r.reached=[k,id](uintptr_t){Context c{};if(!Local(&c))return;const uintptr_t t=KillAuraResolve(c,id);if(t&&KillAuraScore(c,k,t,k.distance,nullptr))KillAuraAttack(c,k,t);};   // rotation reached: attack
  RotSubmit(std::move(r));return;}
 if(!ready)return;if(k.mode==2){KillAuraTeleport(c,k,best);return;}KillAuraAttack(c,k,best);}
inline void KillAuraSend(uintptr_t packet){auto& s=KillAura();if(!s.pending)return;uint64_t table{},get{};   // attack after the teleport packet is sent
 if(Read(packet,&table)&&table&&Read(table+8,&get)&&get&&reinterpret_cast<int(*)(uintptr_t)>(get)(packet)==0x90)s.arrived=true;}
inline void AimAssistWork();inline void AimAssistSend(uintptr_t packet);inline bool CriticalsSend(uintptr_t packet,bool nested);   // game_aim_assist.hpp, game_criticals.hpp
inline void CrystalOptimizerSend(uintptr_t packet);inline void AntiCrystalSend(uintptr_t packet);inline void CrystalOptimizerReceive(uintptr_t packet);inline void CrystalOptimizerRender();   // game_crystal.hpp
inline void PvpBotGameUpdate();inline void PvpBotScreen(const char* screen);inline void PvpBotReceive(uintptr_t packet);   // game_pvp_bot.hpp
inline void AutoMaceSend(uintptr_t packet);inline void AutoMaceScreen();   // game_auto_mace.hpp
inline void CombatSend(uintptr_t packet){AutoMaceSend(packet);static std::atomic<bool> fault{false},critFault{false},crystalFault{false};if(!crystalFault){__try{CrystalOptimizerSend(packet);AntiCrystalSend(packet);}__except(EXCEPTION_EXECUTE_HANDLER){crystalFault=true;}}if(!fault){__try{KillAuraSend(packet);AimAssistSend(packet);}__except(EXCEPTION_EXECUTE_HANDLER){fault=true;}}
 if(!critFault){__try{CriticalsSend(packet,sendDepth.load()>1);}__except(EXCEPTION_EXECUTE_HANDLER){critFault=true;}}}
// Hit Select: skips an attack while the target is still immune, i.e. when Tick Delay plus the target's remaining hurt
// time (MobHurtTimeComponent) exceeds 10 ticks.
struct HitSelectShared{std::atomic<bool> enabled{false};std::atomic<int> delay{9};};
inline HitSelectShared& HitSelect(){static auto* s=new HitSelectShared;return *s;}
inline bool HitSelectCancel(uintptr_t,uintptr_t target){auto& s=HitSelect();if(!s.enabled)return false;uintptr_t registry{},element{};uint32_t entity{};int32_t hurt{};
 if(!ActorEntity(target,&registry,&entity)||!Element(registry,entity,"MobHurtTimeComponent",4,&element)||!Read(element,&hurt))return false;
 return s.delay.load()+hurt>10;}
// Misplace: pulls the position in incoming MoveActorAbsolute (0x12, position at +0x3c) and MovePlayer (0x13, +0x38)
// packets towards the local player by up to Offset Factor blocks, never closer than 0.5 blocks horizontally.
struct MisplaceShared{std::atomic<bool> enabled{false},playersOnly{true};std::atomic<float> offset{1.0f};};
inline MisplaceShared& Misplace(){static auto* s=new MisplaceShared;return *s;}
inline void MisplaceReceive(uintptr_t packet){auto& s=Misplace();if(!s.enabled)return;uint64_t table{},get{};
 if(!Read(packet,&table)||!table||!Read(table+8,&get)||!get)return;const int id=reinterpret_cast<int(*)(uintptr_t)>(get)(packet);if(id!=0x12&&id!=0x13)return;
 float* pos=reinterpret_cast<float*>(packet+(id==0x13?0x38:0x3c));uint64_t runtime{};if(!Read(packet+0x30,&runtime))return;
 Context c{};uint64_t own{};uintptr_t actor{};if(!Local(&c)||(RuntimeId(c.actor,&own)&&own==runtime)||!ActorByRuntimeId(c.registry,runtime,&actor)||!actor)return;
 if(s.playersOnly){uintptr_t registry{};uint32_t entity{};if(!ActorEntity(actor,&registry,&entity)||!Has(registry,entity,"PlayerComponent"))return;}
 float local[3];if(!ActorPos(c.actor,local))return;const float dx=local[0]-pos[0],dz=local[2]-pos[2],d=std::sqrt(dx*dx+dz*dz);
 if(d>0.5f){const float step=std::min(s.offset.load(),d-0.5f);pos[0]+=dx/d*step;pos[2]+=dz/d*step;}}
inline void CombatReceive(uintptr_t packet){static std::atomic<bool> fault{false};PvpBotReceive(packet);if(fault)return;__try{MisplaceReceive(packet);CrystalOptimizerReceive(packet);}__except(EXCEPTION_EXECUTE_HANDLER){fault=true;}}
inline void StickyAimTick();   // game_sticky_aim.hpp
inline void BacktrackTick();   // game_backtrack.hpp
inline void CriticalsJumpWork();
inline void CombatGameUpdate(){StickyAimTick();CriticalsJumpWork();BacktrackTick();PvpBotGameUpdate();static std::atomic<bool> fault{false};if(fault)return;__try{RightClickerWork();ClickAssistWork();KillAuraWork();AimAssistWork();}__except(FaultTraceFilter("Combat (Right Clicker, Click Assist, Kill Aura, Aim Assist)",GetExceptionInformation())){fault=true;FaultTraceFlush();}}
// ---- common target filter. Flags: 0x100 = skip targets carrying component 0xacb47b38; always requires a living
// actor (ActorTypeComponent, bit 0x100); 0x1 = players only (PlayerComponent); 0x1000000 / 0x10000 = Anti Bot / Teams
// (the filter shared with Visuals). Bit 32 of the last argument = maximum squared distance (unused by Trigger Bot).
inline bool CombatTargetValid(const Context& c,uintptr_t target,bool playersOnly,bool skipMarked,bool bots,bool teams){CombatTargetFilter filter;return CombatTargetValid(c,target,playersOnly,skipMarked,filter,bots,teams);}
inline bool CombatTargetValid(const Context& c,uintptr_t target,bool playersOnly,bool skipMarked,CombatTargetFilter& filter,bool bots,bool teams){
 uintptr_t registry{},element{};uint32_t entity{};if(!target||!ActorEntity(target,&registry,&entity))return false;
 if(skipMarked&&ActorSkipAttack(target))return false;
 int32_t type{};if(!Element(registry,entity,"ActorTypeComponent",4,&element)||!Read(element,&type)||!(type&0x100))return false;
 const bool player=Has(registry,entity,"PlayerComponent");if(playersOnly&&!player)return false;
 if(bots){if(!filter.botsBuilt){filter.botsBuilt=true;filter.roster=SnapshotVisualRoster(c);}if(IsVisualBot(c,entity,player,filter.roster))return false;}
 if(teams&&player){if(!filter.teamsBuilt){filter.teamsBuilt=true;{auto& t=CombatTargetRules();std::lock_guard<std::mutex> lock(t.guard);filter.rules=t.teams;}if(filter.rules.enabled)ReadVisualIdentity(c.registry,c.entity,c.actor,&filter.self);}
  if(filter.rules.enabled){VisualIdentity other{};ReadVisualIdentity(registry,entity,target,&other);
   if(nickel::visual::teammate(filter.rules,filter.self.name,filter.self.display,other.name,other.display))return false;}}
 return true;}
// ---- Trigger Bot (ScreenViewRenderEvent): on the game screen, attacks the entity under the crosshair if it passes
// the filter (Players Only), at most Attacks Per Second (drawn from the range at each attack, delay = 1000 / APS ms).
// Hold Click: only while the attack button is held; Weapons Only: weapon in hand.
// Attacks like Auto Clicker's packet mode: swing (4), click counted by the CPS HUD, then the transaction.
struct TriggerBotShared{std::mutex guard;bool enabled=false,hold=false,weaponsOnly=false,playersOnly=true;float low=8,high=12;double delay=0;bool scheduled=false;int64_t last=0;};
inline TriggerBotShared& TriggerBot(){static auto* s=new TriggerBotShared;return *s;}
inline void TriggerBotWork(const char* screen){auto& s=TriggerBot();std::lock_guard<std::mutex> lock(s.guard);if(!s.enabled||strcmp(screen,"hud_screen")||menuPause)return;Context c{};
 if(!Local(&c))return;if(s.hold&&!(GetAsyncKeyState(VK_LBUTTON)&0x8000))return;if(s.weaponsOnly&&!IsWeapon(c))return;
 const uintptr_t target=CrosshairActor(c);if(!target||!CombatTargetValid(c,target,s.playersOnly))return;
 const int64_t now=CombatNs();if(s.scheduled&&(double)(now-s.last)/1e6<=s.delay)return;
 const float aps=s.low<s.high?s.low+(s.high-s.low)*Rand24():s.low;s.delay=1000.0/(double)std::max(0.1f,aps);s.scheduled=true;s.last=now;
 ActionSwing(c,c.actor,4);overlay::RecordClick(0);AttackActor(c,target);}
inline void CombatScreen(const char* screen){AutoMaceScreen();if(strncmp(screen,"hud",3))combatUiTick=GetTickCount64();PvpBotScreen(screen);{static std::atomic<bool> crystalFault{false};if(!crystalFault){__try{CrystalOptimizerRender();}__except(EXCEPTION_EXECUTE_HANDLER){crystalFault=true;}}}static std::atomic<bool> fault{false};if(fault)return;__try{AutoClickerWork(screen);TriggerBotWork(screen);}__except(EXCEPTION_EXECUTE_HANDLER){fault=true;}}
}
