// PvP Bot (Combat): fights the nearest player on its own (movement input, rotations, clicking, potting, pearling,
// refilling the hotbar). One state object per Mode (Classic / No Debuff), rebuilt when the module is enabled or the mode changes.
// - LevelTick (every game tick): target pick, snapshot, behaviour selection, behaviour tick, movement vector.
//   Target: Last Hit keeps the last attacked actor (valid only after an attack made since the module was enabled), else the
//   current target is kept, else the nearest accepted actor (common filter incl. Anti Bot / Teams, players only, position
//   distance) within Target Range; kept targets may drift out to 1.5 x Target Range. Positions are compared squared.
//   Snapshot (me / target): box centre (x, feet y, z), position (eye), velocity (actor+0x240: +0x00 / +0x18), box (actor+0x248),
//   MobHurtTimeComponent (0 when missing) and health (attribute 6, 0 when missing), reach = distance from the eye to
//   the closest point of the target box, flat = horizontal distance between box centres. A combo tracker counts hits dealt /
//   taken from the hurt-time increases (dealt / taken streaks reset by the other side or after 40 / 39 ticks) and a smoothed
//   target velocity (0.25 new + 0.75 old, no vertical part) gives the aim lead. A jump of the own position over 2.5 blocks
//   (teleport) makes the aim snap (speed 80) for 300 ms. Without a snapshot everything is reset and the rotation released.
//   Behaviours (priority; the running one stays while it reports itself active, otherwise a challenger needs a priority
//   above the running one + 0.1 and must start successfully):
//     Engage  0.8 (flat <= 6) / 0.3, always valid, movement mode 1 (forward 1 beyond 2.6, -0.5 below 2.0).
//     Chase   0.6 when Chase is on and flat > 6, movement mode 2 (straight at the target).
//     Heal    0.95 (No Debuff): health <= Heal Threshold, 600 ms after the last pot, a splash potion in the hotbar; Pot Mode
//             Down lowers it to 0.5 while being comboed (health > 4, Pot While Comboed keeps 0.95). Turns away from the target
//             (pitch 55 when three blocks behind have floor and two free cells, else 88). Back: waits for flat >= 5.5 or
//             900 / Pot Speed ms, then throws; Down throws at once. Movement mode 3 (away).
//     Side Pearl 0.97 / Pearl Out 0.96 (No Debuff): comboed (hits taken >= Combo Threshold, Pearl Out at least 4, hit within
//             15 ticks), Pearl Cooldown since the last pearl, 500 ms since the last attempt, pearl in the hotbar; Side Pearl also
//             needs flat <= 5, health > 10, 2000 ms since the last pot; Pearl Out needs health > 5. Side Pearl throws next to the
//             predicted target (perpendicular offsets +-1.5 / 2 / 2.5, random side); Pearl Out away from it (radii 16, 12, 9 at
//             0, -30, +30, -60, +60 degrees). Landing cell: floor solid, two replaceable cells above, searched from y+2 down to
//             y-3; the launch angle comes from a drag / gravity solver checked by a flight simulation (<= 1.5 from the point).
//   Item behaviours run one sequence (find the item, 50 ms, rotation within 6 degrees or 400 ms, use, switch back) while their
//   rotation request (priority 3, speed 50) is renewed every tick; they end when the sequence ends or after 1200 ms.
// - Aim (Engage / Chase): predicted target box, aim at the box centre, height of the eye clamped to [feet + 0.6, top - 0.2];
//   request priority 2, speed = turn speed / 5 (Turn Speed redrawn every 1500 ms), 80 right after a teleport. Visible = mode 1
//   (persistent, tag 0), Silent = mode 2.
// - GameUpdate clicker (set up each tick by Engage / Chase): at the CPS cadence (next = max(elapsed - 50, next) + interval) it
//   swings and, when the view ray (3 blocks) hits the target box (always with Silent) and the box is within 3 blocks, attacks;
//   between 3 and 4.5 blocks it only swings.
// - SprintIntent: the movement vector (world x / z) is turned into strafe / forward by the yaw and written as input bits
//   13..16 (forward, back, strafe), sprint (bit 8, flag bit 1) when forward is at least 0.7 of the length.
// - Jump Reset: a SetActorMotion for the player while the bot moves, on the ground, out of water / lava, standing in a cell
//   that is neither a ladder nor a cobweb (feet = position - OffsetsComponent[0], floored), with a horizontal push of at
//   least 0.1 against the look direction -> jump request.
// - Refill Hotbar (screen render, No Debuff, no item behaviour running): after a 50..120 ms delay following a refill, the first
//   empty hotbar slot receives the first splash potion stack of the inventory (slots 9+) through the inventory scheduler.
//   A fresh state starts its turn-speed timer (value 0 = instant turn) and its teleport timer already running, so the first
//   1.5 s / 0.3 s after enabling behave like that, as the game does.
// Not ported: Edge Safety and Combo Jump (both rely on a movement simulation).
#pragma once
#include "game_combat.hpp"
#include "game_actions.hpp"
#include "game_inventory.hpp"
#include "game_sticky_aim.hpp"
#include "game_trajectory.hpp"
#include "game_movement.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace game {
constexpr int kRotPvpBot=17;         // rotation manager module id
constexpr int kPvpBotAction=16;      // action planner module id
constexpr int kPvpBotInventory=16;   // inventory scheduler key

struct PvpBotConfig{bool noDebuff=false,lastHit=false;float range=24;bool playersOnly=true,chase=true,silent=false;float turnLow=140,turnHigh=220,cpsLow=12,cpsHigh=16;
 bool jumpReset=true,edgeSafety=true,comboJump=true;float healThreshold=10;bool potDown=false;float potSpeed=2;bool potComboed=false,refill=true,sidePearl=true,pearlOut=true;
 int comboThreshold=1;double pearlCooldownMs=15000;};

// ---- snapshot
struct PvpActor{float centre[3]{},pos[3]{},vel[3]{},box[6]{},health=0;int hurt=0;};
struct PvpSnap{PvpActor me,they;uint64_t id=0;int combo[4]{0,0,0x3fffffff,0x3fffffff};float reach=0,flat=0;};
inline double PvpMs(int64_t since){return (double)(ActionNs()-since)/1.0e6;}
// Health attribute (id 6, current value at +0x7c of its 0x88-byte entry); 0 and ok=false when missing.
inline float PvpHealth(uintptr_t actor,bool* ok){uintptr_t registry{},element{};uint32_t entity{};uint64_t begin{},end{},table{};*ok=false;
 if(!ActorEntity(actor,&registry,&entity)||!Element(registry,entity,"AttributesComponent",0x50,&element)||!Read(element,&begin)||!Read(element+8,&end)||!Read(element+0x18,&table)||end<begin||!table)return 0.f;
 const uint64_t count=(end-begin)/4;if(!count||count>256)return 0.f;
 for(uint64_t i=0;i<count;++i){uint32_t id{};if(!Read(begin+i*4,&id))return 0.f;if(id!=6)continue;float cur{};if(!Read(table+i*0x88+0x7c,&cur)||!std::isfinite(cur))return 0.f;*ok=true;return cur;}
 return 0.f;}
inline bool PvpReadActor(uintptr_t actor,PvpActor* o){uint64_t st{},bb{};float s[9],b[6];
 if(!Read(actor+0x240,&st)||!st||!Floats(st,s,9)||!Read(actor+0x248,&bb)||!bb||!Floats(bb,b,6))return false;
 o->centre[0]=(b[0]+b[3])*0.5f;o->centre[1]=b[1];o->centre[2]=(b[2]+b[5])*0.5f;
 for(int i=0;i<3;++i){o->pos[i]=s[i];o->vel[i]=s[6+i];}for(int i=0;i<6;++i)o->box[i]=b[i];
 bool ok=false;o->health=PvpHealth(actor,&ok);o->hurt=0;
 {uintptr_t registry{},element{};uint32_t entity{};int32_t hurt{};if(ActorEntity(actor,&registry,&entity)&&Element(registry,entity,"MobHurtTimeComponent",4,&element)&&Read(element,&hurt))o->hurt=hurt;}
 return true;}
inline bool PvpSnapshot(const Context& c,uintptr_t target,PvpSnap* s){uint64_t id{};if(!PvpReadActor(c.actor,&s->me)||!PvpReadActor(target,&s->they)||!RuntimeId(target,&id))return false;s->id=id;
 float d2=0;for(int i=0;i<3;++i){const float q=std::min(s->they.box[i+3],std::max(s->me.pos[i],s->they.box[i]))-s->me.pos[i];d2+=q*q;}
 s->reach=std::sqrt(d2);
 const float dx=s->they.centre[0]-s->me.centre[0],dz=s->they.centre[2]-s->me.centre[2];s->flat=std::sqrt(dz*dz+dx*dx);return true;}

// ---- trackers
struct PvpCombo{int a=0,b=0,c=0x3fffffff,d=0x3fffffff,prevThey=0,prevMe=0;};   // hits dealt, hits taken, ticks since dealt, ticks since taken
inline void PvpComboUpdate(PvpCombo& k,PvpSnap& s){const int oldThey=k.prevThey,oldMe=k.prevMe;k.prevThey=s.they.hurt;k.prevMe=s.me.hurt;
 int c=std::min(k.c,0x3ffffffe)+1;k.c=c;k.d=std::min(k.d,0x3ffffffe)+1;
 if(oldThey<s.they.hurt){++k.a;k.b=0;k.c=0;c=0;}
 if(oldMe<s.me.hurt){++k.b;k.a=0;k.d=0;}else if(c>0x28&&k.d>0x27){k.a=0;k.b=0;}
 s.combo[0]=k.a;s.combo[1]=k.b;s.combo[2]=k.c;s.combo[3]=k.d;}
inline bool PvpComboed(const PvpSnap& s){return s.combo[3]<0xf&&s.combo[1]>0;}   // hit within 15 ticks, streak taken
struct PvpAim{uint64_t id=0;float vel[3]{};bool valid=false;};
inline void PvpAimUpdate(PvpAim& a,const PvpSnap& s){float x=s.they.vel[0],z=s.they.vel[2],y=0.f;
 if(a.valid&&a.id==s.id){z=z*0.25f+a.vel[2]*0.75f;x=0.25f*x+a.vel[0]*0.75f;y=a.vel[1]*0.75f;}
 a.vel[0]=x;a.vel[1]=y;a.vel[2]=z;a.id=s.id;a.valid=true;}
struct PvpMove{float vec[2]{};bool active=false;};
// mode 0 off, 1 keep distance (forward 1 beyond 2.6, -0.5 below 2.0), 2 straight at the target, 3 away from it.
inline void PvpMoveUpdate(PvpMove& m,const PvpSnap& s,int mode){m.active=mode!=0;if(!mode){m.vec[0]=m.vec[1]=0;return;}
 float v[2]={0.f,1.f};if(s.flat>0.f){v[0]=(s.they.centre[0]-s.me.centre[0])/s.flat;v[1]=(s.they.centre[2]-s.me.centre[2])/s.flat;}
 if(mode==1){const float f=s.flat>2.6f?1.f:(s.flat<2.f?-0.5f:0.f);v[0]*=f;v[1]*=f;}else if(mode==3){v[0]=-v[0];v[1]=-v[1];}
 m.vec[0]=v[0];m.vec[1]=v[1];}
struct PvpClicker{uint64_t id=0;bool active=false;ClickModel model;int64_t startNs=0;bool started=false;double nextMs=0;int64_t turnNs=0;bool turnValid=false;float turnValue=0;int64_t retargetNs=0;bool retargetValid=false;};
struct PvpTracker{int64_t potNs=0;bool potValid=false;int64_t pearlNs=0;bool pearlValid=false;};   // last pot / last pearl

// ---- behaviours
struct PvpState;
struct PvpCtx{const Context* c;uintptr_t target;PvpSnap* snap;PvpAim* aim;const PvpBotConfig* cfg;PvpClicker* clicker;PvpTracker* tracker;PvpState* st;};
struct PvpBehavior{virtual ~PvpBehavior(){}virtual bool Eval(PvpCtx&,float*)=0;virtual bool Start(PvpCtx&){return true;}virtual bool Tick(PvpCtx&)=0;virtual int Mode()=0;virtual bool Active(){return false;}virtual void Cancel(){}};

struct PvpRefill{std::shared_ptr<std::atomic<bool>> moved=std::make_shared<std::atomic<bool>>(false);int64_t at=0;bool atValid=false;double delay=0;};
struct PvpState{std::vector<std::unique_ptr<PvpBehavior>> behaviours;PvpBehavior* cur=nullptr;PvpRefill refill;PvpClicker clicker;PvpMove move;PvpCombo combo;PvpAim aim;PvpTracker tracker;
 uint64_t curId=0;bool curValid=false,hasLast=false,built=false,noDebuff=false;float lastPos[3]{};uint32_t attackBase=0;};
struct PvpPending{std::string item;std::shared_ptr<std::atomic<bool>> done;};
struct PvpBotShared{std::mutex guard,state,pendingGuard;PvpState st;std::atomic<bool> enabled{false},resetPending{false};PvpBotConfig cfg;std::vector<PvpPending> pending;
 volatile long faultLevel=0,faultGame=0,faultScreen=0,faultSprint=0,faultReceive=0,faultFrame=0;};
inline PvpBotShared& PvpBot(){static auto* s=new PvpBotShared;return *s;}

inline void PvpRotate(float yaw,float pitch,bool silent,float speed,int priority){RotRequest r;r.module=kRotPvpBot;r.yaw=yaw;r.pitch=pitch;r.priority=priority;r.speed=speed;
 if(silent){r.mode=2;RotSubmit(std::move(r));}else{r.mode=1;RotSubmitPersistent(std::move(r),0);}}
inline int PvpHotbarCount(const Context& c,const char* name){const uint64_t box=ActionContainer(c.actor);const int n=ActionSize(c,box);int total=0;
 for(int i=0;i<(n>9?9:n);++i){const uint64_t stack=ActionStackAt(c,box,i);if(!ActionNameIs(stack,name))continue;uint8_t count{};if(Read(stack+0x22,&count))total+=count;}return total;}

// Item runner: arms with an item name and a target rotation; one planner sequence is submitted on the render thread.
struct PvpRunner{std::string name;float target[2]{};int64_t armNs=0;bool armValid=false;std::shared_ptr<std::atomic<bool>> done=std::make_shared<std::atomic<bool>>(false);bool started=false,active=false;
 void Reset(){armNs=ActionNs();armValid=true;done=std::make_shared<std::atomic<bool>>(false);started=false;active=false;}
 void Arm(const char* n,const float* t){name=n;target[0]=t[0];target[1]=t[1];done=std::make_shared<std::atomic<bool>>(false);armNs=ActionNs();armValid=true;started=false;active=true;}};
inline void PvpQueueSequence(const PvpRunner& r){auto& s=PvpBot();std::lock_guard<std::mutex> l(s.pendingGuard);s.pending.push_back({r.name,r.done});}
// Returns true when the item action is over.
inline bool PvpRunnerTick(PvpRunner& r,bool silent,float speed){if(!r.active)return true;
 PvpRotate(r.target[0],r.target[1],silent,speed*50.0f,3);
 if(!r.started){PvpQueueSequence(r);r.started=true;return false;}
 if(r.done->load()){r.active=false;return true;}
 if(r.armValid&&PvpMs(r.armNs)>1200.0){r.active=false;return true;}
 return false;}
inline void PvpSubmitSequence(const PvpPending& p){ActionSequence q;q.module=kPvpBotAction;q.silent=0;q.rotation=false;   // rotation flag off: the planner must not release another module's rotation id
 auto built=std::make_shared<int64_t>(ActionNs());
 q.steps.push_back(StepFindName(p.item));q.steps.push_back(StepWait(50.0));
 q.steps.push_back([built](ActionCtx&){return RotReadyFor(kRotPvpBot,6.0f)||ActionMs(*built)>400.0?1:0;});
 q.steps.push_back(StepUse(kRotPvpBot));q.steps.push_back(StepSwitchBack());
 auto done=p.done;q.done=[done](int){done->store(true);};ActionSubmit(std::move(q));}

// Engage / Chase tick: aim request and clicker arming.
inline void PvpAimTick(PvpCtx& x){auto& m=*x.clicker;const PvpSnap& s=*x.snap;const PvpBotConfig& k=*x.cfg;
 if(!(m.turnValid&&PvpMs(m.turnNs)<=1500.0)){m.turnValue=RandRange(k.turnLow,k.turnHigh);m.turnNs=ActionNs();m.turnValid=true;}
 float b[6];b[0]=s.they.box[0]+x.aim->vel[0];b[1]=s.they.box[1]+x.aim->vel[1];b[2]=s.they.box[2]+x.aim->vel[2];b[3]=s.they.box[3]+x.aim->vel[0];b[4]=s.they.box[4]+x.aim->vel[1];b[5]=s.they.box[5]+x.aim->vel[2];
 const float cz=(b[2]+b[5])*0.5f,cx=(b[0]+b[3])*0.5f,top=b[4]-0.2f,eye=s.me.pos[1],low=std::max(b[1]+0.6f,eye),ay=top<eye?top:low;
 const float dz=cz-s.me.pos[2],dx=cx-s.me.pos[0];
 const float pitch=atan2f(ay-eye,hypotf(dx,dz))*-57.295780181884766f,yaw=atan2f(-dx,dz)*57.295780181884766f;
 const bool recent=m.retargetValid&&(double)(CombatNs()-m.retargetNs)/1.0e6<=300.0;
 PvpRotate(yaw,pitch,k.silent,recent?80.0f:m.turnValue/5.0f,2);
 m.id=s.id;m.active=true;}
struct PvpEngage:PvpBehavior{bool Eval(PvpCtx& x,float* p)override{*p=x.snap->flat>6.0f?0.3f:0.8f;return true;}bool Tick(PvpCtx& x)override{PvpAimTick(x);return false;}int Mode()override{return 1;}};
struct PvpChase:PvpBehavior{bool Eval(PvpCtx& x,float* p)override{if(!x.cfg->chase||!(x.snap->flat>6.0f))return false;*p=0.6f;return true;}bool Tick(PvpCtx& x)override{PvpAimTick(x);return false;}int Mode()override{return 2;}};

// Heal: state 1 = backing off, 2 = throwing (the runner owns the rotation).
struct PvpHeal:PvpBehavior{uint8_t state=0;int64_t startNs=0;bool startValid=false;PvpRunner runner;
 bool Eval(PvpCtx& x,float* p)override{const auto& k=*x.cfg;const auto& s=*x.snap;auto& t=*x.tracker;bool ok=true;if(t.potValid)ok=PvpMs(t.potNs)>600.0;
  if(!ok||!(s.me.health<=k.healThreshold))return false;if(PvpHotbarCount(*x.c,"splash_potion")==0)return false;float v=0.95f;
  if(k.potDown&&PvpComboed(s)){v=s.me.health<=4.0f?0.95f:0.5f;if(k.potComboed)v=0.95f;}*p=v;return true;}
 bool Start(PvpCtx& x)override{state=x.cfg->potDown?2:1;startNs=ActionNs();startValid=true;runner.Reset();return true;}
 bool Tick(PvpCtx& x)override{const auto& k=*x.cfg;const auto& s=*x.snap;const Context& c=*x.c;
  const float dx=s.me.pos[0]-s.they.pos[0],dz=s.me.pos[2]-s.they.pos[2];const float yaw=atan2f(-dx,dz)*57.295780181884766f;float pitch=88.0f;
  if(const uint64_t region=ActorRegion(c)){const float r=yaw*0.01745329238474369f,sn=sinf(r),cs=cosf(r);const int fy=(int)floorf(s.me.centre[1]);bool all=true;
   for(int n=1;n<=3&&all;++n){const int32_t floor_[3]={(int32_t)floorf(s.me.centre[0]-sn*(float)n),fy-1,(int32_t)floorf(s.me.centre[2]+cs*(float)n)};
    const int32_t feet[3]={floor_[0],fy,floor_[2]},head[3]={floor_[0],fy+1,floor_[2]};
    all=BlockSolid(RegionBlock(c,region,floor_))&&BlockReplaceable(RegionBlock(c,region,feet))&&BlockReplaceable(RegionBlock(c,region,head));}
   if(all)pitch=55.0f;}
  const float target[2]={yaw,pitch};
  if(state==1){PvpRotate(yaw,pitch,k.silent,k.potSpeed*50.0f,3);
   bool go=s.flat>=5.5f;if(!go&&startValid&&900.0/(double)k.potSpeed<PvpMs(startNs))go=true;if(!go)return false;state=2;}
  if(!runner.active)runner.Arm("splash_potion",target);runner.target[0]=target[0];runner.target[1]=target[1];
  if(PvpRunnerTick(runner,k.silent,k.potSpeed)){state=0;x.tracker->potNs=ActionNs();x.tracker->potValid=true;return true;}return false;}
 int Mode()override{return 3;}bool Active()override{return state!=0;}void Cancel()override{state=0;runner.Reset();}};

// ---- pearl geometry
// Standing cell near a point: floor solid and two replaceable cells above, floors tried from y+2 down to y-3.
inline bool PvpLanding(const Context& c,uint64_t region,const float* at,float* out){const int x=(int)floorf(at[0]),y=(int)floorf(at[1]),z=(int)floorf(at[2]);
 for(int top=y+4;;--top){const int fl=top-2;const int32_t a[3]={x,fl,z},b[3]={x,top-1,z},d[3]={x,top,z};
  if(BlockSolid(RegionBlock(c,region,a))&&BlockReplaceable(RegionBlock(c,region,b))&&BlockReplaceable(RegionBlock(c,region,d))){out[0]=(float)x+0.5f;out[1]=(float)(top-1);out[2]=(float)z+0.5f;return true;}
  if(!(y-3<fl))break;}return false;}
// Launch velocity (length = speed) that reaches the offset d: searches the first tick count at which the required speed
// fits, refines inside that tick by bisection (20 steps).
inline bool PvpBallistic(const TrajectoryParams& p,const float* d,float* v){const float speed=p.speed;if(!(speed>0.f)||!std::isfinite(speed))return false;
 if(!(std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2])>9.999999747378752e-06f))return false;
 float g=p.order?p.gravity:0.f,k=p.order?p.air:1.f,t=0.f,h=0.f;
 for(int i=0;i<200;++i){const float h1=h+g,t1=t+k;const float vy=(d[1]+h1)/t1,vx=d[0]/t1,vz=d[2]/t1;
  if(std::sqrt(vx*vx+vy*vy+vz*vz)<=speed){float lo=0.f,hi=1.f;
   for(int j=0;j<20;++j){const float m=(lo+hi)*0.5f,tt=m*k+t,yy=(d[1]+m*g+h)/tt,xx=d[0]/tt,zz=d[2]/tt;if(std::sqrt(xx*xx+yy*yy+zz*zz)<=speed)hi=m;else lo=m;}
   t=t+k*hi;const float zz=d[2]/t,yy=(h+g*hi+d[1])/t,xx=d[0]/t,n=1.0f/std::sqrt(xx*xx+yy*yy+zz*zz);v[0]=speed*n*xx;v[1]=speed*n*yy;v[2]=zz*n*speed;return true;}
  k=p.air*k;g=g*p.air+p.gravity;t=t1;h=h1;}
 return false;}
// Yaw / pitch that land a pearl on the point: solver, then a flight simulation must end within 1.5 of the point.
inline bool PvpSolveThrow(const Context& c,const PvpSnap& s,const float* point,float* yaw,float* pitch){const TrajectoryParams p=TrajectoryBds(3);
 const float d[3]={point[0]-s.me.pos[0],point[1]-s.me.pos[1],point[2]-s.me.pos[2]};float v[3];if(!PvpBallistic(p,d,v))return false;
 const float len=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);if(!(len>9.999999974752427e-07f))return false;const float dir[3]={v[0]/len,v[1]/len,v[2]/len};
 const float vel[3]={dir[0]*p.speed,dir[1]*p.speed,dir[2]*p.speed};const uint64_t region=ActorRegion(c);if(!region)return false;
 std::vector<TrajectoryBody> bodies;TrajectoryBodies(c,c.actor,s.me.pos,bodies);TrajectoryPath path;TrajectorySimulate(c,c.actor,region,p,s.me.pos,vel,bodies,path);
 if(!path.hit||path.points.empty())return false;const auto& e=path.points.back();
 if(std::sqrt((point[2]-e[2])*(point[2]-e[2])+(point[1]-e[1])*(point[1]-e[1])+(point[0]-e[0])*(point[0]-e[0]))>1.5f)return false;
 *yaw=atan2f(-dir[0],dir[2])*57.295780181884766f;*pitch=atan2f(dir[1],hypotf(dir[0],dir[2]))*-57.295780181884766f;return true;}
inline bool PvpPearlReady(const PvpTracker& t,const PvpBotConfig& k){return !t.pearlValid||k.pearlCooldownMs<PvpMs(t.pearlNs);}

struct PvpPearl:PvpBehavior{bool out;int64_t selfNs=0;bool selfValid=false;PvpRunner runner;explicit PvpPearl(bool o):out(o){}
 bool Eval(PvpCtx& x,float* p)override{const auto& k=*x.cfg;const auto& s=*x.snap;auto& t=*x.tracker;const bool cool=PvpPearlReady(t,k),gap=!selfValid||500.0<PvpMs(selfNs);
  if(!out){const bool pot=!t.potValid||2000.0<PvpMs(t.potNs);if(s.combo[1]<k.comboThreshold||!PvpComboed(s))return false;if(!(pot&&gap&&cool&&k.sidePearl))return false;
   if(s.flat>5.0f||!(10.0f<s.me.health))return false;if(PvpHotbarCount(*x.c,"ender_pearl")==0)return false;*p=0.97f;return true;}
  const int need=std::max(4,k.comboThreshold);const bool comboed=s.combo[1]>=need&&PvpComboed(s);
  if(!(cool&&k.pearlOut))return false;if(!(gap&&comboed)||s.me.health<=5.0f)return false;if(PvpHotbarCount(*x.c,"ender_pearl")==0)return false;*p=0.96f;return true;}
 bool Start(PvpCtx& x)override{selfNs=ActionNs();selfValid=true;const Context& c=*x.c;const PvpSnap& s=*x.snap;const uint64_t region=ActorRegion(c);if(!region)return false;float yaw{},pitch{},spot[3];
  if(out){const float away=atan2f(-(s.me.centre[0]-s.they.centre[0]),s.me.centre[2]-s.they.centre[2])*57.295780181884766f;static const float radius[3]={16.f,12.f,9.f},turn[5]={0.f,-30.f,30.f,-60.f,60.f};
   for(int r=0;r<3;++r)for(int a=0;a<5;++a){const float rad=(away+turn[a])*0.01745329238474369f;const float at[3]={s.me.centre[0]-sinf(rad)*radius[r],s.me.centre[1],cosf(rad)*radius[r]+s.me.centre[2]};
    if(!PvpLanding(c,region,at,spot)||!PvpSolveThrow(c,s,spot,&yaw,&pitch))continue;const float t[2]={yaw,pitch};runner.Arm("ender_pearl",t);x.tracker->pearlNs=ActionNs();x.tracker->pearlValid=true;return true;}
   return false;}
  const float px=s.they.centre[0]+x.aim->vel[0]*10.0f,pz=s.they.centre[2]+x.aim->vel[2]*10.0f,py=x.aim->vel[1]*10.0f+s.they.centre[1];
  const float dx=px-s.me.centre[0],dz=pz-s.me.centre[2],l2=dx*dx+dz*dz;float nz=1.0f,nx=0.f;if(0.0f<l2){nz=dz/std::sqrt(l2);nx=dx/std::sqrt(l2);}
  const float side=(int64_t)ClickRng()()>=0?1.0f:-1.0f;static const float offset[6]={1.5f,2.0f,2.5f,-1.5f,-2.0f,-2.5f};
  for(int i=0;i<6;++i){const float o=offset[i]*side;const float at[3]={px-o*nz,py,nx*o+pz};if(!PvpLanding(c,region,at,spot)||!PvpSolveThrow(c,s,spot,&yaw,&pitch))continue;
   const float t[2]={yaw,pitch};runner.Arm("ender_pearl",t);x.tracker->pearlNs=ActionNs();x.tracker->pearlValid=true;return true;}
  return false;}
 bool Tick(PvpCtx& x)override{return PvpRunnerTick(runner,x.cfg->silent,1.0f);}
 int Mode()override{return 3;}bool Active()override{return runner.active;}void Cancel()override{runner.Reset();}};

inline void PvpBuild(PvpState& st,bool noDebuff){st=PvpState{};st.built=true;st.noDebuff=noDebuff;
 {auto& m=st.clicker;const int64_t now=CombatNs();m.startNs=now;m.started=true;m.nextMs=0;m.turnNs=now;m.turnValid=true;m.turnValue=0;m.retargetNs=now;m.retargetValid=true;}   // timers of a fresh state are already running{auto& e=Shared();std::lock_guard<std::mutex> l(e.guard);st.attackBase=e.attackSerial;}
 st.behaviours.push_back(std::make_unique<PvpEngage>());st.behaviours.push_back(std::make_unique<PvpChase>());
 if(noDebuff){st.behaviours.push_back(std::make_unique<PvpHeal>());st.behaviours.push_back(std::make_unique<PvpPearl>(false));st.behaviours.push_back(std::make_unique<PvpPearl>(true));}}

// ---- target
inline bool PvpAccept(const Context& c,const PvpBotConfig& k,uintptr_t a,float limit2){if(!a||a==c.actor||!CombatTargetValid(c,a,k.playersOnly))return false;float p[3],q[3];if(!ActorPos(c.actor,p)||!ActorPos(a,q))return false;
 return (q[2]-p[2])*(q[2]-p[2])+(q[1]-p[1])*(q[1]-p[1])+(q[0]-p[0])*(q[0]-p[0])<=limit2;}
inline uintptr_t PvpResolve(const Context& c,uint64_t id){uintptr_t a{};return ActorByRuntimeId(c.registry,id,&a)?a:0;}
inline uintptr_t PvpPick(const Context& c,const PvpBotConfig& k,const PvpState& st){const float wide=(1.5f*k.range)*(1.5f*k.range),near_=k.range*k.range;
 if(k.lastHit){uint64_t id{};uint32_t serial{};{auto& e=Shared();std::lock_guard<std::mutex> l(e.guard);id=e.attackTarget;serial=e.attackSerial;}
  if(serial!=st.attackBase){const uintptr_t a=PvpResolve(c,id);if(PvpAccept(c,k,a,wide))return a;}}
 if(st.curValid){const uintptr_t a=PvpResolve(c,st.curId);if(PvpAccept(c,k,a,wide))return a;}
 uintptr_t owners{};uint64_t begin{},end{};if(!Storage(c.registry,Fnv("ActorOwnerComponent"),&owners)||!Read(owners+0x20,&begin)||!Read(owners+0x28,&end)||end<begin||(end-begin)%4||(end-begin)>4*65536)return 0;
 uintptr_t best=0;float bestD=3.4028234663852886e+38f;float me[3];if(!ActorPos(c.actor,me))return 0;
 for(uint64_t at=end;at>begin;){at-=4;uint32_t entity{};uintptr_t slot{};uint64_t actor{};if(!Read(at,&entity))break;if(entity>=0xfffc0000u)continue;if(!InStorage(owners,entity,8,&slot)||!Read(slot,&actor)||!actor)continue;
  if(!PvpAccept(c,k,(uintptr_t)actor,near_))continue;float q[3];if(!ActorPos((uintptr_t)actor,q))continue;
  const float d=(q[2]-me[2])*(q[2]-me[2])+(q[1]-me[1])*(q[1]-me[1])+(q[0]-me[0])*(q[0]-me[0]);if(d<bestD){bestD=d;best=(uintptr_t)actor;}}
 return best;}

// Selection: the running behaviour stays while active, then by priority; the running one is kept against challengers
// that do not beat it by 0.1, a challenger must start.
inline PvpBehavior* PvpSelect(PvpState& st,PvpCtx& x){if(st.cur&&st.cur->Active())return st.cur;
 struct Entry{PvpBehavior* b;float p;};std::vector<Entry> list;for(auto& b:st.behaviours){float p{};if(b->Eval(x,&p))list.push_back({b.get(),p});}
 std::stable_sort(list.begin(),list.end(),[](const Entry& a,const Entry& b){return a.p>b.p;});
 bool found=false;float limit=0;for(auto& e:list)if(e.b==st.cur){found=true;limit=e.p+0.1f;break;}
 for(auto& e:list){if(e.b==st.cur||(found&&e.p<=limit))return st.cur;if(e.b->Start(x)){if(st.cur)st.cur->Cancel();return e.b;}}
 return found?st.cur:nullptr;}
inline void PvpReset(PvpState& st){for(auto& b:st.behaviours)b->Cancel();InvCancel(kPvpBotInventory);st.refill.atValid=false;st.cur=nullptr;st.curValid=false;st.hasLast=false;
 st.combo=PvpCombo{};st.aim=PvpAim{};{PvpClicker k;st.clicker=k;}st.move=PvpMove{};RotRelease(kRotPvpBot);}

// ---- LevelTick
inline void PvpBotLevelWork(){auto& s=PvpBot();if(!s.enabled)return;PvpBotConfig k;{std::lock_guard<std::mutex> l(s.guard);k=s.cfg;}
 Context c{};if(!Local(&c)||!StickyHasInput(c))return;std::lock_guard<std::mutex> lock(s.state);PvpState& st=s.st;
 if(s.resetPending.exchange(false)||!st.built||st.noDebuff!=k.noDebuff)PvpBuild(st,k.noDebuff);
 const uintptr_t target=PvpPick(c,k,st);uint64_t id=0;const bool valid=target&&RuntimeId(target,&id);
 if(valid!=st.curValid||(valid&&id!=st.curId)){st.combo=PvpCombo{};st.aim=PvpAim{};}
 PvpSnap snap{};if(!valid||!PvpSnapshot(c,target,&snap)){PvpReset(st);return;}
 st.curId=id;st.curValid=true;
 if(st.hasLast){const float dx=snap.me.centre[0]-st.lastPos[0],dy=snap.me.centre[1]-st.lastPos[1],dz=snap.me.centre[2]-st.lastPos[2];for(int i=0;i<3;++i)st.lastPos[i]=snap.me.centre[i];
  if(2.5f<std::sqrt(dz*dz+dy*dy+dx*dx)){st.clicker.retargetNs=CombatNs();st.clicker.retargetValid=true;}}
 else{for(int i=0;i<3;++i)st.lastPos[i]=snap.me.centre[i];st.hasLast=true;}
 st.clicker.active=false;PvpComboUpdate(st.combo,snap);PvpAimUpdate(st.aim,snap);
 PvpCtx x{&c,target,&snap,&st.aim,&k,&st.clicker,&st.tracker,&st};
 PvpBehavior* cur=PvpSelect(st,x);st.cur=cur;
 if(!cur){PvpMoveUpdate(st.move,snap,0);return;}
 const bool done=cur->Tick(x);PvpMoveUpdate(st.move,snap,cur->Mode());if(done){cur->Cancel();st.cur=nullptr;}}
inline void PvpBotLevelTick(){auto& s=PvpBot();if(s.faultLevel)return;__try{PvpBotLevelWork();}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&s.faultLevel,1);if(actionLog)actionLog("PvP Bot: fault in the tick, tick stopped until restart");}}

// ---- GameUpdate: clicker
inline void PvpBotGameWork(){auto& s=PvpBot();if(!s.enabled)return;PvpBotConfig k;{std::lock_guard<std::mutex> l(s.guard);k=s.cfg;}
 Context c{};if(!Local(&c)||!StickyHasInput(c)||!CombatNoInterface())return;std::lock_guard<std::mutex> lock(s.state);PvpState& st=s.st;if(!st.built)return;auto& m=st.clicker;if(!m.active)return;
 if(!m.started){m.startNs=CombatNs();m.started=true;m.nextMs=0;}
 const double elapsed=(double)(CombatNs()-m.startNs)/1.0e6;if(elapsed<m.nextMs)return;
 const uintptr_t target=m.id?PvpResolve(c,m.id):0;if(!target)return;PvpSnap snap{};if(!PvpSnapshot(c,target,&snap))return;
 bool aimed=true;
 if(!k.silent){float pitch{},yaw{};if(!RotRead(c.actor,&pitch,&yaw))return;const float pr=pitch*0.01745329238474369f,yr=yaw*0.01745329238474369f;
  const float to[3]={snap.me.pos[0]+(-sinf(yr))*cosf(pr)*3.0f,snap.me.pos[1]-sinf(pr)*3.0f,cosf(yr)*cosf(pr)*3.0f+snap.me.pos[2]};float t{};aimed=TrajectoryRay(snap.they.box,snap.me.pos,to,&t);}
 if(3.0f<snap.reach||!aimed){if(snap.reach>4.5f)return;ActionSwing(c,c.actor,4);overlay::RecordClick(0);}
 else{ActionSwing(c,c.actor,4);overlay::RecordClick(0);AttackActor(c,target);}
 double interval=0,hold=0;ClickNext(m.model,0,k.cpsLow,k.cpsHigh,&interval,&hold);double d=elapsed-50.0;if(d<=m.nextMs)d=m.nextMs;m.nextMs=d+interval;}
inline void PvpBotGameUpdate(){auto& s=PvpBot();if(s.faultGame)return;__try{PvpBotGameWork();}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&s.faultGame,1);if(actionLog)actionLog("PvP Bot: fault in the clicker, clicker stopped until restart");}}

// ---- SprintIntent: movement input
inline void PvpBotSprintWork(uint64_t input,uint64_t state,uint64_t movement){auto& s=PvpBot();if(!s.enabled||!input||!CombatNoInterface())return;Context c{};uint64_t mine{};
 if(!Local(&c)||!Read(c.actor+0x240,&mine)||mine!=state)return;std::lock_guard<std::mutex> lock(s.state);PvpState& st=s.st;if(!st.built||!st.move.active)return;
 float pitch{},yaw{};if(!RotRead(c.actor,&pitch,&yaw))return;const float r=yaw*0.01745329238474369f,sn=sinf(r),cs=cosf(r),wx=st.move.vec[0],wz=st.move.vec[1];
 const float out[2]={cs*wx+sn*wz,cs*wz-sn*wx};   // strafe, forward
 const float len2=out[0]*out[0]+out[1]*out[1];const bool sprint=0.0f<len2&&std::sqrt(len2)*0.699999988079071f<=out[1];uint32_t& bits=*(uint32_t*)input;
 bits=((out[1]<-0.30000001192092896f)?1u<<14:0u)|((0.30000001192092896f<out[1])?1u<<13:0u)|(bits&0xfffe1effu)|((0.30000001192092896f<out[0])?1u<<15:0u)|((out[0]<-0.30000001192092896f)?1u<<16:0u)|(sprint?1u<<8:0u);
 memcpy((void*)(input+4),out,8);memcpy((void*)(input+0x24),out,8);uint8_t& flags=*(uint8_t*)(input+0x60);flags=(uint8_t)((flags&0xfd)|(sprint?2:0));
 if(movement)memcpy((void*)movement,out,8);}
inline void PvpBotSprint(uint64_t input,uint64_t state,uint64_t movement){auto& s=PvpBot();if(s.faultSprint)return;__try{PvpBotSprintWork(input,state,movement);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&s.faultSprint,1);}}

// ---- PacketReceive: Jump Reset (the packet is never cancelled)
// Block the feet are in (position - OffsetsComponent[0], floored): Jump Reset does not fire on a ladder or in a cobweb.
inline bool PvpFeetFree(const Context& c){uint64_t st{};float p[3];uintptr_t off{};float lift=0;if(!Read(c.actor+0x240,&st)||!st||!Floats(st,p,3))return false;
 if(Element(c.registry,c.entity,"OffsetsComponent",0x44,&off))Read(off,&lift);const uint64_t region=ActorRegion(c);if(!region)return false;
 const int32_t cell[3]={(int32_t)floorf(p[0]),(int32_t)floorf(p[1]-lift),(int32_t)floorf(p[2])};const uint64_t block=RegionBlock(c,region,cell);std::string name;
 if(!block||!XrayBlockName(block,&name))return false;return name!="minecraft:ladder"&&name!="minecraft:web";}
inline void PvpBotReceiveWork(uintptr_t packet){auto& s=PvpBot();if(!s.enabled||PacketIdOf(packet)!=0x28)return;Context c{};uint64_t mine{},who{};
 if(!Local(&c)||!RuntimeId(c.actor,&mine)||!Read(packet+0x30,&who)||who!=mine)return;bool wanted=false;{std::lock_guard<std::mutex> l(s.guard);wanted=s.cfg.jumpReset;}if(!wanted)return;
 {std::lock_guard<std::mutex> lock(s.state);if(!s.st.built||!s.st.move.active)return;}
 float m[3];if(!Floats(packet+0x38,m,3)||m[2]*m[2]+m[0]*m[0]<0.009999999776482582f||!ActionOnGround(c.actor))return;if(LocalHas(c,0x78e89f39u)||LocalHas(c,0x832a2768u)||!PvpFeetFree(c))return;
 float pitch{},yaw{};if(!RotRead(c.actor,&pitch,&yaw))return;const float r=yaw*0.01745329238474369f;if(!(cosf(r)*m[2]<sinf(r)*m[0]))return;JumpRequest(c.actor);}
inline void PvpBotReceive(uintptr_t packet){auto& s=PvpBot();if(s.faultReceive)return;__try{PvpBotReceiveWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&s.faultReceive,1);}}

// ---- screen render: Refill Hotbar
inline bool PvpRefillFind(const Context& c,uintptr_t actor,int* src,int* dst){const uint64_t box=ActionContainer(actor);const int n=ActionSize(c,box);
 for(int i=0;i<(n>9?9:n);++i){if(ActionItem(ActionStackAt(c,box,i)))continue;
  for(int j=9;j<n;++j)if(ActionNameIs(ActionStackAt(c,box,j),"splash_potion")){*src=j;*dst=i;return true;}
  return false;}
 return false;}
inline void PvpBotScreenWork(){auto& s=PvpBot();if(!s.enabled)return;PvpBotConfig k;{std::lock_guard<std::mutex> l(s.guard);k=s.cfg;}Context c{};if(!Local(&c))return;
 std::lock_guard<std::mutex> lock(s.state);PvpState& st=s.st;if(!st.built||!st.noDebuff)return;auto& rf=st.refill;const bool idle=st.cur&&!st.cur->Active();
 if(rf.moved->exchange(false)){rf.delay=Rand01()*70.0+50.0;rf.at=ActionNs();rf.atValid=true;}
 if(idle&&k.refill){if(rf.atValid&&PvpMs(rf.at)<=rf.delay)return;if(!StickyHasInput(c)||ActionCursorBusy(c,c.actor))return;
  {auto& inv=Inventory();std::lock_guard<std::recursive_mutex> l(inv.guard);if(!inv.queue.empty())return;}
  int a{},b{};if(!PvpRefillFind(c,c.actor,&a,&b))return;auto moved=rf.moved;
  InvSubmit(kPvpBotInventory,[moved](uintptr_t actor){Context q{};if(!Local(&q)||q.actor!=actor)return;int x{},y{};if(!PvpRefillFind(q,actor,&x,&y))return;
   uint8_t count{};Read(ActionStackAt(q,ActionContainer(actor),x)+0x22,&count);if(InvMoveStack(q,actor,0xc,x,0xc,y,count)==0)moved->store(true);});}
 else InvCancel(kPvpBotInventory);}
inline void PvpBotScreen(const char*){auto& s=PvpBot();if(s.faultScreen)return;__try{PvpBotScreenWork();}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&s.faultScreen,1);if(actionLog)actionLog("PvP Bot: fault in the refill, refill stopped until restart");}}

// ---- action thread (before the planner): item sequences queued by the behaviours
inline void PvpBotFrameWork(){auto& s=PvpBot();std::vector<PvpPending> list;{std::lock_guard<std::mutex> l(s.pendingGuard);list.swap(s.pending);}if(list.empty())return;
 if(!s.enabled){for(auto& p:list)p.done->store(true);return;}for(auto& p:list)PvpSubmitSequence(p);}
inline void PvpBotFrame(){auto& s=PvpBot();if(s.faultFrame)return;__try{PvpBotFrameWork();}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&s.faultFrame,1);if(actionLog)actionLog("PvP Bot: fault queueing a sequence, bot sequences stopped until restart");}}

// ---- menu thread
inline void PvpBotActivate(){PvpBot().resetPending=true;}
inline void PvpBotDeactivate(){auto& s=PvpBot();s.resetPending=true;RotRelease(kRotPvpBot);InvCancel(kPvpBotInventory);}
}
