// Movement modules, group C: Glide, Jetpack, Boat Fly, Elytra Fly, Phase, Null Movement.
// All of them act on the local player. Velocity is the StateVector reached through *(actor+0x240): +0x18 x, +0x1c y,
// +0x20 z (blocks per tick). Movement keys are the bitset of MoveInputComponent at +0x10 (bit 0 sneak, 7 jump, 13
// forward, 14 back, 15 left, 16 right); rotation is *(actor+0x250): pitch, yaw in degrees.
// - Glide (GameUpdate): not on the ground (OnGroundFlagComponent): vy = max(vy, -Fall Speed), the climb is untouched.
// - Jetpack (LevelTick, held key): the velocity becomes Speed x the look direction (-sin yaw cos pitch, -sin pitch,
//   cos yaw cos pitch), with no ground, water or flight condition. The menu passes "key held" (or enabled when no key).
// - Boat Fly (LevelTick, DismountButton hook RVA 0x6584450): the ridden entity comes from PassengerComponent
//   (0x98e40c0e, first int = vehicle entity) resolved through ActorOwnerComponent; it is kept when All Rideables is
//   set or its ActorTypeComponent (a plain int) is 0x5a (boat) or 0xda (chest boat). Horizontal: keys normalised, turned
//   by player yaw + 90 degrees, times Horizontal Speed (boats ignore left / right, which turn the boat). Vertical: jump
//   +Vertical Speed, sneak -Vertical Speed, Shift held with no screen open and external input -Vertical Speed, else
//   Normal 0 / Glide -Glide Speed. The vehicle's StateVector receives the result. A boat also damps the steering input
//   of BoatMovementComponent (0x9671ba31, 0x14 bytes per entity): +8 = ((left - right) + +8) * 0.9. The dismount
//   button is cancelled (game function not called) while the vehicle is airborne, or on the ground without Shift.
// - Elytra Fly (LevelTick, SprintIntent, PlayerAuthInput send, ScreenViewRender): ActorDataFlagComponent bit 32 =
//   gliding. LevelTick while gliding: Control sets the velocity to the key direction (optionally with pitch) x
//   Horizontal Speed, vertical = (jump - sneak) x Vertical Speed else -Fall Speed; Boost adds Boost Acceleration along
//   the look direction while forward is held; every mode clamps the horizontal speed to Max Speed. Firework Auto uses a
//   firework_rocket (hotbar sequence through the action planner, silent switch optional; or a move from the inventory
//   through the inventory scheduler) when horizontal speed is below Firework Speed and Firework Delay has passed.
//   SprintIntent: Auto Takeoff / Auto Redeploy press jump for one tick (every other tick) when the chest slot holds a
//   usable elytra. Bounce: every PlayerAuthInput clears bit 33 (StopGliding) and sets bit 32 (StartGliding) when the
//   player does not glide.
// - Phase (FetchCollisionShapes hook RVA 0x360a1f0): after the game filled the collision shape list for the local
//   player (24 bytes per shape: min xyz, max xyz; a parallel list of the same stride at +0x70 loses the same indexes),
//   shapes are removed so the player walks through walls: All / Horizontal drop every shape reaching above the feet
//   (maxY > minY + 0.001); All / Vertical drop shapes below the feet while sneaking, or above the head while jumping.
// - Null Movement (SprintIntent, PlayerAuthInput send, GameUpdate): when both keys of an axis are held the winner (last
//   or first pressed) keeps its direction and the other key is cleared from the input word (input+0), the 0x90 input
//   bits and the move vectors. Suspended while Freecam is active; state is zeroed when it is not applicable.
#pragma once
#include "game_movement.hpp"
#include "game_movement3.hpp"
#include "game_movement_b.hpp"
#include "game_free_camera.hpp"
#include "game_sticky_aim.hpp"
#include "game_combat.hpp"
#include "game_player.hpp"
#include "game_inventory.hpp"
#include "game_hand.hpp"
#include <MinHook.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace game {
constexpr int kElytraAction=22;   // action planner module id and inventory scheduler key of Elytra Fly

struct MoveCShared{
 // Glide, Jetpack
 std::atomic<bool> glide{false},jetpack{false};std::atomic<float> glideFall{0.1f},jetSpeed{0.5f};
 // Boat Fly
 std::atomic<bool> boat{false},boatGlide{false},boatAll{false};std::atomic<float> boatH{0.5f},boatV{0.3f},boatGlideSpeed{0.03f};
 // Phase (0 All, 1 Horizontal, 2 Vertical)
 std::atomic<bool> phase{false};std::atomic<int> phaseMode{0};
 // Null Movement (0 Last Input, 1 First Input)
 std::atomic<bool> nullOn{false},nullForward{true},nullStrafe{true};std::atomic<int> nullMode{0};
 // Elytra Fly (mode 0 Control, 1 Boost, 2 Bounce)
 std::atomic<bool> elytra{false},pitchControl{false},antiKick{true},fireworkAuto{false},fromInventory{false},silent{true},takeoff{true},redeploy{true};
 std::atomic<int> elytraMode{0},fireworkDelay{1000},redeployDelay{2};
 std::atomic<float> elytraH{1.0f},elytraV{0.5f},elytraFall{0.0f},boost{0.05f},maxSpeed{2.5f},fireworkSpeed{1.0f};
 // Null Movement state (16 bytes in the module): held flags and winners, guarded by `lock`.
 std::mutex lock;uint8_t nullState[16]{};
 // Elytra state: last rocket (ns), "ready" flag (always 1 in the module), request from the tick to the screen thread, glide flag
 // of the previous SprintIntent, redeploy and Anti-Kick counters.
 std::atomic<int64_t> lastRocket{0};std::atomic<bool> inventoryRequest{false},wasGliding{false},sequenceBusy{false},cancelSequence{false},wasElytra{false},wasNull{false};
 std::atomic<int> redeployTicks{0},antiKickTicks{0};
};
inline MoveCShared& MoveC(){static auto* s=new MoveCShared;return *s;}

// ---- shared helpers
inline bool MoveCBit(uintptr_t input,int bit){uint32_t w{};return Read(input+0x10,&w)&&((w>>bit)&1);}
inline uintptr_t MoveCFlagsElement(const Context& c){uintptr_t f{};return Element(c.registry,c.entity,"ActorDataFlagComponent",0x18,&f)?f:0;}
inline bool MoveCGliding(const Context& c){const uintptr_t f=MoveCFlagsElement(c);uint64_t v{};return f&&Read(f,&v)&&((v>>32)&1);}
inline bool MoveCClientLevel(const Context& c){uint64_t level{};return Read(c.actor+0x200,&level)&&level&&(uint64_t)currentTickLevel==level;}   // LevelTick of the client level only
inline bool MoveCShiftHeld(){return (GetAsyncKeyState(VK_LSHIFT)&0x8000)||(GetAsyncKeyState(VK_RSHIFT)&0x8000);}
inline float MoveCSignedFill(float other,bool negative){const float v=other!=0.f?std::fabs(other):1.f;return negative?-v:v;}   // |other|, or 1 when other is 0, with the sign flipped for the losing side

// ---- Glide: GameUpdate (every frame)
inline void MoveCGlideWork(){auto& m=MoveC();if(!m.glide)return;Context c{};if(!Local(&c)||ActionOnGround(c.actor))return;uint64_t sv{};float vy{};
 if(!Read(c.actor+0x240,&sv)||!sv||!Read(sv+0x1c,&vy))return;const float limit=-m.glideFall.load();if(vy<limit)*(float*)(sv+0x1c)=limit;}

// ---- Jetpack: LevelTick
inline void MoveCJetpackWork(){auto& m=MoveC();if(!m.jetpack)return;Context c{};if(!Local(&c)||!MoveCClientLevel(c))return;float pitch{},yaw{};uint64_t sv{};if(!RotRead(c.actor,&pitch,&yaw)||!Read(c.actor+0x240,&sv)||!sv)return;
 const float k=0.01745329238474369f,p=pitch*k,y=yaw*k,s=m.jetSpeed.load();
 *(float*)(sv+0x18)=s*(-std::sin(y))*std::cos(p);*(float*)(sv+0x1c)=s*(-std::sin(p));*(float*)(sv+0x20)=std::cos(y)*std::cos(p)*s;}

// ---- Boat Fly
// Vehicle entity of the local player (PassengerComponent), then its actor; 0 when none.
inline uintptr_t MoveCVehicle(const Context& c,uint32_t* entity){uintptr_t el{};uint32_t id{};if(!Element(c.registry,c.entity,"PassengerComponent",16,&el)||!Read(el,&id)||(id&0x3ffff)==0x3ffff)return 0;
 uintptr_t slot{};uint64_t actor{};if(!Element(c.registry,id,"ActorOwnerComponent",8,&slot)||!Read(slot,&actor)||!actor)return 0;if(entity)*entity=id;return (uintptr_t)actor;}
// The vehicle retained by the module (boat 0x5a, chest boat 0xda, or any with All Rideables).
inline uintptr_t MoveCBoatVehicle(const Context& c,uint32_t* entity,bool* isBoat){uint32_t id{};const uintptr_t v=MoveCVehicle(c,&id);if(!v)return 0;
 uintptr_t el{};int32_t type{};const bool known=Element(c.registry,id,"ActorTypeComponent",4,&el)&&Read(el,&type);const bool boat=known&&(type==0x5a||type==0xda);
 if(!MoveC().boatAll&&!boat)return 0;if(entity)*entity=id;if(isBoat)*isBoat=boat;return v;}
inline void MoveCBoatWork(){auto& m=MoveC();if(!m.boat)return;Context c{};if(!Local(&c)||!MoveCClientLevel(c))return;uint32_t vid{};bool boat=false;const uintptr_t vehicle=MoveCBoatVehicle(c,&vid,&boat);
 const uintptr_t input=Move3Input(c);if(!vehicle||!input)return;
 // type recomputed as in the module: a boat is a retained vehicle of type 0x5a / 0xda
 uint32_t bits{};if(!Read(input+0x10,&bits))return;const float H=m.boatH.load();
 const float f=(float)((bits>>13)&1)-(float)((bits>>14)&1);float s=0.f;if(!boat)s=(float)((bits>>16)&1)-(float)((bits>>15)&1);
 float vx=0,vz=0;const float n2=f*f+s*s;
 if(n2!=0.f){float pitch{},yaw{};if(!RotRead(c.actor,&pitch,&yaw))return;const float n=std::sqrt(n2),r=(yaw+90.f)*0.01745329238474369f,co=std::cos(r),si=std::sin(r),a=f/n,b=s/n;
  vx=H*(co*a-si*b);vz=H*(co*b+si*a);}
 const float V=m.boatV.load();float vy;
 if(bits&0x80)vy=V;
 else if(bits&1)vy=-V;
 else{const bool down=StickyHasInput(c)&&CombatNoInterface()&&MoveCShiftHeld();
  if(down)vy=-V;else vy=m.boatGlide?-m.boatGlideSpeed.load():0.f;}
 uint64_t sv{};if(!Read(vehicle+0x240,&sv)||!sv)return;*(float*)(sv+0x18)=vx;*(float*)(sv+0x1c)=vy;*(float*)(sv+0x20)=vz;
 if(boat){uintptr_t comp{};if(Element(c.registry,vid,"BoatMovementComponent",0x14,&comp)){float steer{};if(Read(comp+8,&steer)){const float turn=(float)((bits>>16)&1)-(float)((bits>>15)&1);*(float*)(comp+8)=(turn+steer)*0.8999999761581421f;}}}}
// Dismount hook: true = cancel (the game function is not called).
inline bool MoveCDismountWork(){auto& m=MoveC();if(!m.boat)return false;Context c{};if(!Local(&c))return false;uint32_t vid{};bool boat=false;const uintptr_t vehicle=MoveCBoatVehicle(c,&vid,&boat);if(!vehicle)return false;
 bool lenient=ActionOnGround(vehicle);if(!lenient){uintptr_t comp{};uint8_t first{};lenient=Element(c.registry,vid,"BoatMovementComponent",0x14,&comp)&&Read(comp,&first)&&first==0;}
 if(lenient&&MoveCShiftHeld())return false;return true;}
inline bool MoveCDismountGuarded(){__try{return MoveCDismountWork();}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
using MoveCDismountFunction=uint64_t(*)(uint64_t);inline MoveCDismountFunction moveCDismountOriginal=nullptr;
inline uint64_t MoveCDismountDetour(uint64_t self){if(MoveCDismountGuarded())return 0;return moveCDismountOriginal(self);}
inline int InstallBoatDismountHook(void(*log)(const char*)){static int state=0;if(state)return state;
 const uint8_t bytes[16]={0x48,0x83,0xec,0x28,0x48,0x8b,0x01,0x48,0x8b,0x80,0xf0,0x00,0x00,0x00,0xff,0x15};
 state=InstallOne(0x6584450,bytes,16,(void*)MoveCDismountDetour,(void**)&moveCDismountOriginal,"Boat Fly (dismount button)",log);return state;}

// ---- Phase: collision shape list
inline void MoveCRemoveShape(uint64_t list,uint64_t index){   // same index in both lists; the first list's end is at +0x60, the parallel list's at +0x78
 for(int pair=0;pair<2;++pair){const uint64_t begin=pair?list+0x70:list+0x58,endAt=pair?list+0x78:list+0x60;uint64_t b{},e{};if(!Read(begin,&b)||!Read(endAt,&e)||!b||e<b)return;
  const uint64_t count=(e-b)/0x18;if(index>=count)return;std::memmove((void*)(b+index*0x18),(void*)(b+(index+1)*0x18),(size_t)((count-index-1)*0x18));*(uint64_t*)endAt=e-0x18;}}
inline uint64_t MoveCShapeCount(uint64_t list){uint64_t b{},e{};return Read(list+0x58,&b)&&Read(list+0x60,&e)&&e>=b?(e-b)/0x18:0;}
inline void MoveCPhaseWork(uint64_t who,uint64_t boxAddress,uint64_t list){auto& m=MoveC();if(!m.phase||!who||!boxAddress||!list)return;Context c{};uint32_t mine{};int32_t asked{};
 if(!Local(&c)||!Read(c.actor+0x18,&mine)||!Read(who,&asked)||(int32_t)mine!=asked)return;float bb[6];if(!Floats(boxAddress,bb,6))return;const int mode=m.phaseMode.load();const float eps=0.0010000000474974513f;
 auto shapeFloat=[&](uint64_t index,int offset){float v{};uint64_t b{};return Read(list+0x58,&b)&&Read(b+index*0x18+offset,&v)?v:0.f;};
 if(mode==0||mode==1){for(uint64_t i=MoveCShapeCount(list);i>0;--i)if(bb[1]+eps<shapeFloat(i-1,0x10))MoveCRemoveShape(list,i-1);}
 if(mode!=1){const uintptr_t input=Move3Input(c);uint32_t bits{};if(!input||!Read(input+0x10,&bits))return;
  if(!(bits&1)){if(bits&0x80){const float e2=-eps;for(uint64_t i=MoveCShapeCount(list);i>0;--i)if(bb[4]+e2<shapeFloat(i-1,0x10))MoveCRemoveShape(list,i-1);}}
  else{const float limit=bb[1]+eps;for(uint64_t i=MoveCShapeCount(list);i>0;--i){const float minY=shapeFloat(i-1,4);if(minY<limit)MoveCRemoveShape(list,i-1);}}}}
inline void MoveCPhaseGuarded(uint64_t who,uint64_t box,uint64_t list){__try{MoveCPhaseWork(who,box,list);}__except(EXCEPTION_EXECUTE_HANDLER){}}
// Phase runs in the shared collision shapes detour of game_movement_b.hpp (Jesus), after the game.
inline int InstallPhaseHook(void(*log)(const char*)){shapesPhase=MoveCPhaseGuarded;return InstallJesusHook(log);}

// ---- Elytra Fly
// "Can glide": airborne, chest slot holds an elytra with more than 1 durability left, no water or lava, not flying, not riding.
inline bool MoveCCanGlide(const Context& c){if(ActionOnGround(c.actor))return false;const uint64_t armor=EquipmentContainer(c,c.actor,1);if(!armor||ActionSize(c,armor)<=1)return false;
 const uint64_t chest=ActionStackAt(c,armor,1);if(!chest||!ActionNameIs(chest,"elytra"))return false;
 const uint64_t item=ActionItem(chest);const uint64_t top=item?Virtual0(c,item,0x120):0;const int maximum=top?reinterpret_cast<short(*)(uint64_t)>(top)(item):0;if(maximum>0&&ArmorDurability(c,chest)<=1)return false;
 if(LocalHas(c,0x78e89f39u)||LocalHas(c,0x832a2768u)||LocalFlying(c))return false;
 const uintptr_t f=MoveCFlagsElement(c);if(!f)return true;uint64_t v{};return Read(f,&v)&&!((v>>2)&1);}
inline void MoveCElytraEdge(bool on){auto& m=MoveC();if(m.wasElytra.exchange(on)==on)return;   // enabled or disabled: cancel the sequences, drop the inventory request, reset the flags
 m.cancelSequence=true;m.inventoryRequest=false;m.wasGliding=false;m.redeployTicks=0;m.antiKickTicks=0;InvCancel(kElytraAction);}
inline void MoveCElytraFirework(const Context& c,float horizontal){auto& m=MoveC();if(m.fireworkSpeed.load()<=horizontal)return;   // too fast already
 if((double)(ActionNs()-m.lastRocket.load())/1000000.0<=(double)m.fireworkDelay.load())return;
 // [a rocket still attached to the player is not detected here, see the integration notes]
 if(m.sequenceBusy)return;
 if(!ActionHasItem(c,c.actor,"firework_rocket")){m.inventoryRequest=m.fromInventory.load();return;}
 ActionSequence q;q.module=kElytraAction;q.silent=m.silent?1:0;
 q.conditions.push_back([](uintptr_t,int64_t start){const double e=ActionMs(start);return e<500.0||e==500.0;});
 q.steps.push_back(StepFindName("firework_rocket"));
 q.steps.push_back([](ActionCtx& x){Context k{};return Local(&k)&&k.actor==x.actor&&MoveCGliding(k)?1:2;});   // still gliding -> next, else cancel
 q.steps.push_back(StepUse());q.steps.push_back(StepSwitchBack());
 q.done=[](int){MoveC().sequenceBusy=false;};m.sequenceBusy=true;ActionSubmit(std::move(q));
 m.lastRocket=ActionNs();}
inline void MoveCElytraWork(){auto& m=MoveC();if(m.cancelSequence.exchange(false)){ActionCancel(kElytraAction);}
 if(!m.elytra)return;Context c{};if(!Local(&c)||!MoveCClientLevel(c)||!MoveCGliding(c))return;const uintptr_t input=Move3Input(c);float pitch{},yaw{};uint64_t sv{};
 if(!input||!RotRead(c.actor,&pitch,&yaw)||!Read(c.actor+0x240,&sv)||!sv)return;const int mode=m.elytraMode.load();uint32_t bits{};if(!Read(input+0x10,&bits))return;auto key=[&](int b){return (float)((bits>>b)&1);};
 float* v=(float*)(sv+0x18);const float k=0.01745329238474369f;
 if(mode==0){const bool pc=m.pitchControl.load();const float f=key(13)-key(14),s=key(16)-key(15),y=yaw*k;
  float dirX,dirY,dirZ;
  { // direction: key axes rotated by the yaw (and the pitch with Pitch Control), normalised
   float forwardX,forwardY,forwardZ;if(!pc){forwardX=-std::sin(y);forwardY=0.f;forwardZ=std::cos(y);}else{const float p=pitch*k,cp=std::cos(p);forwardX=-std::sin(y)*cp;forwardY=-std::sin(p);forwardZ=std::cos(y)*cp;}
   const float ex=f*forwardX-std::cos(y)*s,ey=s*0.f+forwardY*f,ez=f*forwardZ-std::sin(y)*s;const float n2=ez*ez+ey*ey+ex*ex;dirX=dirY=dirZ=0.f;
   if(0.f<n2){const float n=std::sqrt(n2);dirX=ex/n;dirY=ey/n;dirZ=ez/n;}}
  const bool moving=dirZ!=0.f||dirY!=0.f||dirX!=0.f;const float H=m.elytraH.load();v[0]=dirX*H;v[1]=dirY*H;v[2]=dirZ*H;
  const float vertical=key(7)-key(0);
  if(vertical!=0.f)v[1]=m.elytraV.load()*vertical;
  else if(!(pc&&moving))v[1]=-m.elytraFall.load();
  else{   // Pitch Control while moving with no vertical key: the module's Anti-Kick counter (its reset condition is never met while moving)
   const bool idle=!moving&&vertical==0.f;int n=m.antiKickTicks.load()+1;if(!idle)n=0;m.antiKickTicks=n;
   if(idle&&m.antiKick&&n%40==0)v[1]+=-0.03999999910593033f;}}
 else if(mode==1&&((bits>>13)&1)){const float B=m.boost.load(),y=yaw*k,p=pitch*k,cp=std::cos(p);v[0]=v[0]-std::sin(y)*cp*B;v[1]=v[1]-std::sin(p)*B;v[2]=std::cos(y)*cp*B+v[2];}
 const float limit=m.maxSpeed.load();float h=std::hypot(v[0],v[2]);if(limit<h&&0.f<h){v[0]*=limit/h;v[2]*=limit/h;}
 if(!m.fireworkAuto)return;h=std::hypot(v[0],v[2]);MoveCElytraFirework(c,h);}
inline void MoveCElytraTick(){__try{MoveCElytraWork();}__except(EXCEPTION_EXECUTE_HANDLER){}}
// SprintIntent: take off / redeploy. flags = pointer to the intent flag bitset (bit 32 = gliding), state = StateVector, movement = arg 12.
inline void MoveCElytraSprintWork(uint64_t input,uint64_t state,uint64_t flags,uint64_t movement){auto& m=MoveC();if(!m.elytra)return;Context c{};if(!Local(&c)||!MoveCCanGlide(c)){m.wasGliding=false;m.redeployTicks=0;return;}
 uint64_t bits{};if(!flags||!Read(flags,&bits))return;
 if((bits>>32)&1){m.wasGliding=true;m.redeployTicks=0;return;}
 if(m.elytraMode.load()==2)return;float vy{};if(!state||!Read(state+0x1c,&vy))return;const bool take=m.takeoff&&vy<0.f;
 if(!(m.redeploy&&m.wasGliding)){if(!take)return;}
 else{const int before=m.redeployTicks.fetch_add(1);if(!take&&before<m.redeployDelay.load())return;}
 // Take off with a jump press (key word bits 7 / 25 / 26 and input flag 0x10, as Path Finder jumps), every other tick so
 // each one is a new press. The reference wrote byte +0x23 of the movement argument on every falling tick: on this game
 // version that kept the elytra from opening at all, even with the jump key.
 (void)movement;static bool pulsed=false;pulsed=!pulsed;if(!pulsed||!input)return;
 uint32_t& keys=*(uint32_t*)input;keys|=(1u<<25)|0x4000080u;*(uint8_t*)(input+0x60)|=0x10;}
inline void MoveCElytraSprint(uint64_t input,uint64_t state,uint64_t flags,uint64_t movement){__try{MoveCElytraSprintWork(input,state,flags,movement);}__except(EXCEPTION_EXECUTE_HANDLER){}}
inline void MoveCSetPacketBit(uintptr_t packet,int bit,bool on){uint64_t w{};if(!Read(packet+0x88,&w))return;if(on)w|=1ull<<bit;else w&=~(1ull<<bit);*(uint64_t*)(packet+0x88)=w;}
inline void MoveCElytraSendWork(uintptr_t packet){auto& m=MoveC();if(!m.elytra||m.elytraMode.load()!=2||PacketIdOf(packet)!=0x90)return;Context c{};if(!Local(&c)||!MoveCCanGlide(c))return;
 MoveCSetPacketBit(packet,33,false);if(!MoveCGliding(c))MoveCSetPacketBit(packet,32,true);}
// ScreenViewRender: a pending "rocket from the inventory" request is handed to the inventory scheduler.
inline void MoveCElytraInventory(uintptr_t actor){auto& m=MoveC();if(!m.elytra||!m.fromInventory)return;Context c{};if(!Local(&c)||c.actor!=actor||ActionHasItem(c,actor,"firework_rocket"))return;
 const uint64_t box=ActionContainer(actor);const int size=ActionSize(c,box);int src=-1;for(int i=9;i<size;++i)if(ActionNameIs(ActionStackAt(c,box,i),"firework_rocket")){src=i;break;}if(src<0)return;
 const int hotbar=size<9?size:9;int dst=-1;for(int i=0;i<hotbar;++i)if(!ActionItem(ActionStackAt(c,box,i))){dst=i;break;}
 if(dst<0){int32_t* selected=ActionSelected(actor);for(int i=hotbar-1;i>=0;--i)if(!selected||i!=*selected){dst=i;break;}}
 if(dst<0||ActionCursorBusy(c,actor))return;uint8_t count{};Read(ActionStackAt(c,box,src)+0x22,&count);InvMoveStack(c,actor,0xc,src,0xc,dst,count);}
inline void MoveCScreenWork(){auto& m=MoveC();if(m.inventoryRequest.exchange(false))InvSubmit(kElytraAction,MoveCElytraInventory);}
inline void MoveCScreen(){__try{MoveCScreenWork();}__except(EXCEPTION_EXECUTE_HANDLER){}}

// ---- Null Movement
inline void MoveCNullZero(){auto& m=MoveC();std::lock_guard<std::mutex> l(m.lock);memset(m.nullState,0,sizeof(m.nullState));}
inline void MoveCNullEdge(bool on){auto& m=MoveC();if(m.wasNull.exchange(on)!=on)MoveCNullZero();}
inline bool MoveCNullGuard(const Context& c){return StickyHasInput(c)&&!FreeCameraActive();}
// Winner of one axis: 0 none; the axis must be enabled and both keys held. First Input inverts the stored winner.
inline int MoveCNullPick(const uint8_t* axis,bool enabled,int mode){if(!enabled||!axis[0]||!axis[1])return 0;const int winner=*(const int*)(axis+4);if(mode==1){if(winner==1)return 2;return winner==2?1:0;}return winner;}
inline void MoveCNullChoose(int* forward,int* side){auto& m=MoveC();*forward=MoveCNullPick(m.nullState,m.nullForward,m.nullMode.load());*side=MoveCNullPick(m.nullState+8,m.nullStrafe,m.nullMode.load());}
inline void MoveCNullSprintWork(uint64_t input,uint64_t state,uint64_t flags,uint64_t movement){(void)state;(void)flags;auto& m=MoveC();if(!m.nullOn||!input)return;Context c{};
 if(!Local(&c)||!MoveCNullGuard(c)){MoveCNullZero();return;}uint32_t h{};if(!Read(input+0x10,&h))return;int a=0,l=0;
 {std::lock_guard<std::mutex> lk(m.lock);uint8_t* s=m.nullState;const bool fwd=(h>>13)&1,back=(h>>14)&1,left=(h>>15)&1,right=(h>>16)&1;
  if(fwd&&!s[0])*(int*)(s+4)=1;if(back&&!s[1])*(int*)(s+4)=2;s[0]=fwd;s[1]=back;
  if(left&&!s[8])*(int*)(s+0xc)=1;if(right&&!s[9])*(int*)(s+0xc)=2;s[8]=left;s[9]=right;MoveCNullChoose(&a,&l);}
 if(!a&&!l)return;uint32_t& word=*(uint32_t*)input;if(a)word&=(uint32_t)((a!=1)*0x2000)-0x4001u;
 float x=*(float*)(input+0x24),z=*(float*)(input+0x28);
 if(l){word&=(uint32_t)((l!=1)*0x8000)-0x10001u;x=*(float*)(input+0x24);z=*(float*)(input+0x28);if(x==0.f)*(float*)(input+0x24)=MoveCSignedFill(z,l!=1);}
 if(a&&z==0.f)*(float*)(input+0x28)=MoveCSignedFill(x,a!=1);
 if(movement)memcpy((void*)movement,(const void*)(input+0x24),8);}
inline void MoveCNullSprint(uint64_t input,uint64_t state,uint64_t flags,uint64_t movement){__try{MoveCNullSprintWork(input,state,flags,movement);}__except(EXCEPTION_EXECUTE_HANDLER){}}
inline bool MoveCPacketBit(uintptr_t packet,int bit){uint64_t w{};return Read(packet+0x88,&w)&&((w>>bit)&1);}
inline void MoveCNullSendWork(uintptr_t packet){auto& m=MoveC();if(!m.nullOn||PacketIdOf(packet)!=0x90)return;Context c{};if(!Local(&c)||!MoveCNullGuard(c))return;int a=0,l=0;{std::lock_guard<std::mutex> lk(m.lock);MoveCNullChoose(&a,&l);}
 const int forward=(MoveCPacketBit(packet,10)&&MoveCPacketBit(packet,11))?a:0;const int side=(MoveCPacketBit(packet,12)&&MoveCPacketBit(packet,13))?l:0;
 if(forward==1){MoveCSetPacketBit(packet,11,false);MoveCSetPacketBit(packet,52,false);MoveCSetPacketBit(packet,51,false);}   // forward wins: Down, DownRight, DownLeft cleared
 else if(forward==2){MoveCSetPacketBit(packet,10,false);MoveCSetPacketBit(packet,15,false);MoveCSetPacketBit(packet,14,false);}
 float x{},z{};bool lateral=false;
 if(side==1){MoveCSetPacketBit(packet,13,false);MoveCSetPacketBit(packet,52,false);MoveCSetPacketBit(packet,15,false);lateral=true;}
 else if(side==2){MoveCSetPacketBit(packet,12,false);MoveCSetPacketBit(packet,51,false);MoveCSetPacketBit(packet,14,false);lateral=true;}
 x=*(float*)(packet+0x64);z=*(float*)(packet+0x68);
 if(lateral&&x==0.f)*(float*)(packet+0x64)=MoveCSignedFill(z,side!=1);
 if(forward&&z==0.f)*(float*)(packet+0x68)=MoveCSignedFill(x,forward!=1);
 const float x2=*(float*)(packet+0x5c),z2=*(float*)(packet+0x60);
 if(x2!=0.f||z2!=0.f){if(lateral&&x2==0.f)*(float*)(packet+0x5c)=MoveCSignedFill(z2,side!=1);if(forward&&z2==0.f)*(float*)(packet+0x60)=MoveCSignedFill(x2,forward!=1);}}
inline void MoveCNullGameUpdateWork(){auto& m=MoveC();if(!m.nullOn)return;Context c{};if(!Local(&c)||!MoveCNullGuard(c))MoveCNullZero();}

// ---- entry points
inline void MoveCGameUpdate(){__try{MoveCGlideWork();MoveCNullGameUpdateWork();}__except(EXCEPTION_EXECUTE_HANDLER){}}   // GameUpdate #30 subscribers
inline void MoveCTick(){__try{MoveCJetpackWork();MoveCBoatWork();}__except(EXCEPTION_EXECUTE_HANDLER){}MoveCElytraTick();}   // UtilityTick (LevelTick)
inline bool MoveCSend(uintptr_t packet){__try{MoveCElytraSendWork(packet);MoveCNullSendWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){}return false;}   // PlayerAuthInput subscribers
inline void MoveCSprint(uint64_t input,uint64_t state,uint64_t flags,uint64_t movement){MoveCElytraSprint(input,state,flags,movement);MoveCNullSprint(input,state,flags,movement);}   // SprintIntent #19
}
