// Aim Assist (Combat): turns the aim towards a chosen target each GameUpdate through the rotation manager.
// - View: eye position interpolated by the frame fraction (game 0x2ceda40), rotation *(actor+0x250); without Silent
// and without Free Look, the camera angles replace the actor rotation.
// - Target: Single keeps the locked runtime id while it passes the filter and scores (FOV, or infinity with Increased
// FOV While Locked), else picks again; Switch always picks. Pick: lowest score among the actors accepted by the common
// filter (Players Only, Distance Range = Distance squared). Score = hypot(yaw delta, pitch delta) to the aim point
// (target velocity) below FOV, visible unless Through Walls; Switch sorts by distance squared, hurt time or the angle.
// - Aim point: box interpolated by the frame fraction, moved by velocity x Predict x 10; centre (y at 90% height)
// blended with the closest box point by Multipoint Horizontal / Vertical when Multi-Point is on.
// - Velocity is smoothed with 1 - exp(-20 dt) while the target stays the same.
// - Stop at Hitbox: no request while the view ray crosses the target box.
// - Step per axis (horizontal / vertical speed): Linear and Interpolate limit the step to 60 dt x speed x 6 and
// 1 - q^(60 dt) of the delta; Lock On uses 1 - exp(-6 x speed x lerp(28, 4, Smoothing) x dt). Never past the delta.
// - Mouse input: steps are snapped to the sensitivity unit (s x 0.6 + 0.15)^3 x 2880 / display width, the remainder
// carried to the next frame.
// - Request: relative {yaw, Vertical ? pitch : 0}, priority 1, mode 2 (Silent or Free Look), 1 (Lock On) or 0;
// it stays wanted while the target lives, Hold Click and Weapons Only hold.
// - Bypass: a PlayerAuthInputPacket (0x90) sent while the request is applied reports gamepad input (+0x98 = 3).
#pragma once
#include "game_combat.hpp"
#include "game_free_look.hpp"
#include "game_view.hpp"
#include <atomic>
#include <cmath>
#include <mutex>

namespace game {
constexpr int kRotAimAssist=13;
struct AimAssistConfig{int mode=0;float fov=40;bool distanceRange=true;float distance=6;int targetMode=0;bool increasedFov=false;int sortBy=0;
 int hSpeed=50,vSpeed=50;float smoothing=0.1f;bool multiPoint=false;int mpH=100,mpV=100,predict=0;
 bool stopAtHitbox=false,holdClick=true,playersOnly=true,vertical=false,weaponsOnly=false,throughWalls=true,silent=false;};
// Per-module state: locked target, last target, smoothed velocity and the sensitivity remainder.
struct AimAssistState{uint64_t lockId=0;bool lockValid=false;uint64_t lastId=0;bool lastValid=false;float vel[3]{};float rem[2]{};};
struct AimAssistShared{std::mutex guard;std::atomic<bool> enabled{false},bypass{false},resetPending{false};AimAssistConfig cfg;AimAssistState st;};   // st: game thread only
inline AimAssistShared& AimAssist(){static auto* s=new AimAssistShared;return *s;}
// Activation and deactivation both clear the state; it is applied on the next GameUpdate.
inline void AimAssistActivate(AimAssistShared& s){s.resetPending=true;}
inline void AimAssistReset(AimAssistShared& s){s.resetPending=true;RotRelease(kRotAimAssist);}
inline float AimPercent(int v){return (float)v/100.0f;}

struct AimView{float eye[3];float yaw,pitch,alpha;};
// Frame fraction of the game timer, clamped to [0, 1]; 1 without a timer.
inline float AimAlpha(const Context& c){uint64_t mc{},timer{};float a{};if(!Read(c.client+0x1d0,&mc)||!mc)return 1.0f;if(!Read(mc+0xe8,&timer)||!timer||!Read(timer+8,&a))return 1.0f;
 if(a>1.0f)return 1.0f;if(0.0f>a)return 0.0f;return a;}
// Box interpolated between the previous and current position (feet lowered by OffsetsComponent).
inline bool AimBox(uintptr_t actor,float alpha,float* b){uint64_t st{},bb{};float s[6],size[6];if(!Read(actor+0x240,&st)||!st||!Floats(st,s,6)||!Read(actor+0x248,&bb)||!bb||!Floats(bb,size,6))return false;
 const float r=1.0f-alpha;float y=s[4]*r+s[1]*alpha;uintptr_t registry{},off{};uint32_t entity{};float lift{};
 if(ActorEntity(actor,&registry,&entity)&&Element(registry,entity,"OffsetsComponent",0x44,&off)&&Read(off,&lift))y-=lift;
 const float x=s[3]*r+s[0]*alpha,z=s[5]*r+s[2]*alpha;const float hw=(size[3]-size[0])*0.5f,hd=(size[5]-size[2])*0.5f;
 b[0]=x-hw;b[1]=y;b[2]=z-hd;b[3]=hw+x+0.0f;b[4]=(size[4]-size[1])+y+0.0f;b[5]=hd+z+0.0f;return true;}
inline bool AimVelocity(uintptr_t actor,float* v){uint64_t st{};return Read(actor+0x240,&st)&&st&&Floats(st+0x18,v,3);}
// Living actor (ActorTypeComponent bit 0x100), as in the common filter.
inline bool AimAlive(uintptr_t actor){uintptr_t registry{},element{};uint32_t entity{};int32_t type{};
 return ActorEntity(actor,&registry,&entity)&&Element(registry,entity,"ActorTypeComponent",4,&element)&&Read(element,&type)&&(type&0x100);}
// Aim point: interpolated, predicted box; centre blended with the closest point to the eye.
inline bool AimPoint(const AimAssistConfig& k,const AimView& v,uintptr_t target,const float* vel,float* out){float b[6];if(!AimBox(target,v.alpha,b))return false;
 float h=0,w=0;if(k.multiPoint){h=AimPercent(k.mpH);w=AimPercent(k.mpV);}
 float p=AimPercent(k.predict);p=0.0f<=p?p:0.0f;p=p<=1.0f?p:1.0f;
 const float ox=vel[0]*p*10.0f,oy=vel[1]*p*10.0f,oz=vel[2]*p*10.0f;
 const float minX=b[0]+ox,minY=b[1]+oy,minZ=b[2]+oz,maxX=ox+b[3],maxY=oy+b[4],maxZ=oz+b[5];
 h=std::min(1.0f,std::max(0.0f,h));w=std::min(1.0f,std::max(0.0f,w));
 const float cx=std::min(maxX,std::max(minX,v.eye[0])),cy=std::min(maxY,std::max(minY,v.eye[1])),cz=std::min(maxZ,std::max(minZ,v.eye[2]));
 out[0]=(minX+maxX)*0.5f*(1.0f-h)+cx*h;out[1]=((maxY-minY)*0.9f+minY)*(1.0f-w)+cy*w;out[2]=(minZ+maxZ)*0.5f*(1.0f-h)+cz*h;return true;}
inline void AimAngles(const AimView& v,const float* p,float* yaw,float* pitch){const float dx=p[0]-v.eye[0],dy=p[1]-v.eye[1],dz=p[2]-v.eye[2];
 *pitch=atan2f(dy,hypotf(dx,dz))*-57.295780181884766f;*yaw=atan2f(-dx,dz)*57.295780181884766f;}
// Line of sight from the player position to the point (BlockSource::clip, region->vtable[0x198]); true without a region.
inline bool AimVisible(const Context& c,const float* point){const uint64_t region=ActorRegion(c);uint64_t st{};float from[3];if(!region||!Read(c.actor+0x240,&st)||!st||!Floats(st,from,3))return true;
 const uint64_t clipOf=Virtual0(c,region,0x198);if(!clipOf)return true;
 using Clip=void*(*)(uint64_t,void*,const float*,const float*,bool,bool,int,bool,bool,uint64_t,const std::function<bool()>*,bool);static const std::function<bool()> accept=[]{return true;};
 const float dx=point[0]-from[0],dy=point[1]-from[1],dz=point[2]-from[2];const float span=sqrtf(dx*dx+dy*dy+dz*dz);alignas(16) uint8_t res[0x200]{};
 reinterpret_cast<Clip>(clipOf)(region,res,from,point,false,true,(int)span+1,false,false,c.actor,&accept,false);
 uint64_t weak{};memcpy(&weak,res+0x40,8);if(weak&&InterlockedDecrement((volatile long*)(weak+0xc))==0){uint64_t t{},rel{};if(Read(weak,&t)&&Read(t+8,&rel)&&rel)reinterpret_cast<void(*)(uint64_t)>(rel)(weak);}
 int type{};memcpy(&type,res+0x18,4);return type!=0;}
inline float AimHurtTime(uintptr_t target){uintptr_t registry{},element{};uint32_t entity{};int32_t t{};
 return ActorEntity(target,&registry,&entity)&&Element(registry,entity,"MobHurtTimeComponent",4,&element)&&Read(element,&t)?(float)t:0.0f;}
// Score of a target (lower is better); FLT_MAX when outside the FOV or hidden.
inline float AimScore(const Context& c,const AimAssistConfig& k,const AimView& v,uintptr_t target,float fov){float vel[3],p[3],yaw{},pitch{};const float none=3.4028234663852886e+38f;
 if(!AimVelocity(target,vel)||!AimPoint(k,v,target,vel,p))return none;AimAngles(v,p,&yaw,&pitch);
 const float angle=hypotf(remainderf(yaw-v.yaw,360.0f),pitch-v.pitch);if(!(angle<fov))return none;
 if(!k.throughWalls&&!AimVisible(c,p))return none;if(k.targetMode!=1)return angle;
 if(k.sortBy==1)return AimHurtTime(target);
 if(k.sortBy==0){float a[3],b[3];if(!ActorPos(c.actor,a)||!ActorPos(target,b))return none;return (b[2]-a[2])*(b[2]-a[2])+(b[1]-a[1])*(b[1]-a[1])+(b[0]-a[0])*(b[0]-a[0]);}
 return angle;}
// Common filter plus the Distance Range limit (positions, squared).
inline bool AimAccept(const Context& c,const AimAssistConfig& k,uintptr_t target){if(!CombatTargetValid(c,target,k.playersOnly))return false;
 const float limit=k.distanceRange?k.distance*k.distance:3.4028234663852886e+38f;float a[3],b[3];if(!ActorPos(c.actor,a)||!ActorPos(target,b))return false;
 return (b[2]-a[2])*(b[2]-a[2])+(b[1]-a[1])*(b[1]-a[1])+(b[0]-a[0])*(b[0]-a[0])<=limit;}
// Best target over the actor view, walked from the end; the first lowest score wins.
inline uintptr_t AimPick(const Context& c,const AimAssistConfig& k,const AimView& v,float fov){uintptr_t owners{};uint64_t begin{},end{};
 if(!Storage(c.registry,Fnv("ActorOwnerComponent"),&owners)||!Read(owners+0x20,&begin)||!Read(owners+0x28,&end)||end<begin||(end-begin)%4||(end-begin)>4*65536)return 0;
 uintptr_t best=0;float bestScore=3.4028234663852886e+38f;
 for(uint64_t at=end;at>begin;){at-=4;uint32_t entity{};uintptr_t slot{};uint64_t actor{};if(!Read(at,&entity))break;if(entity>=0xfffc0000u)continue;if(!InStorage(owners,entity,8,&slot)||!Read(slot,&actor)||!actor)continue;
  if(!AimAccept(c,k,(uintptr_t)actor))continue;const float s=AimScore(c,k,v,(uintptr_t)actor,fov);if(s<bestScore){bestScore=s;best=(uintptr_t)actor;}}
 return best;}
// Slab test of the view ray against the box.
inline bool AimRayHits(const AimView& v,const float* b){const float yr=v.yaw*0.01745329238474369f,pr=0.01745329238474369f*v.pitch;const float cp=cosf(pr),sy=sinf(yr),sp=sinf(pr),cy=cosf(yr);
 const float dx=-sy*cp,dy=-sp,dz=cp*cy,eps=9.999999974752427e-07f,big=3.4028234663852886e+38f;float lo,hi;
 if(eps<=std::fabs(dx)){const float t0=(b[0]-v.eye[0])/dx,t1=(b[3]-v.eye[0])/dx;const float mx=t0<t1?t1:t0,mn=t1<t0?t1:t0;hi=mx<big?mx:big;lo=0.0f<mn?mn:0.0f;if(!(lo<=hi))return false;}
 else if(b[0]<=v.eye[0]&&v.eye[0]<=b[3]){lo=0.0f;hi=big;}else return false;
 if(eps<=std::fabs(sp)){const float t0=(b[1]-v.eye[1])/dy,t1=(b[4]-v.eye[1])/dy;const float mn=t1<t0?t1:t0,mx=t0<t1?t1:t0;hi=mx<hi?mx:hi;lo=lo<mn?mn:lo;if(!(lo<=hi))return false;}
 else if(!(b[1]<=v.eye[1]&&v.eye[1]<=b[4]))return false;
 if(eps<=std::fabs(dz)){const float t0=(b[2]-v.eye[2])/dz,t1=(b[5]-v.eye[2])/dz;float mn=t0<=t1?t0:t1;mn=mn<=lo?lo:mn;float mx=t1<=t0?t0:t1;mx=hi<=mx?hi:mx;return mn<=mx;}
 return b[2]<=v.eye[2]&&v.eye[2]<=b[5];}
// Linear / Interpolate step for one axis.
inline float AimStep(int mode,float d,float s,float axis,float t60){const float ad=std::fabs(d);float q;
 if(mode==0){const float r=6.0f/std::max(100.0f-s*100.0f,1.0f);q=1.0f<r?0.0f:1.0f-r;}
 else{const float kk=ad/axis*(s*6.0f);q=kk<0.0f?1.0f:1.0f-kk;q=1.0f<kk?0.0f:q;}
 const float step=std::min(t60*(s*6.0f),ad*(1.0f-powf(q,t60)));return copysignf(step,d);}
inline float AimLockK(float s){if(std::isfinite(s)){if(s!=1.0f){const float x=fmaf(s,-24.0f,28.0f);return 1.0f<s?(x<=4.0f?x:4.0f):(4.0f<=x?x:4.0f);}return 4.0f;}
 return std::isinf(s)?s*-24.0f:s+s;}
inline float AimExp(float k,float dt){return 1.0f-expf((dt<0.0f?0.0f:dt)*(k<0.0f?-0.0f:-k));}
// Input mode of the player (ExternalDataComponent in the entity context, virtual +0x28); -1 when unknown.
inline int AimInputMode(const Context& c){uint64_t owner{},buckets{},end{},nodes{};if(!Read(c.actor+8,&owner)||!owner||!Read(owner+0x30,&buckets)||!Read(owner+0x38,&end)||!Read(owner+0x50,&nodes)||end<=buckets||!nodes)return -1;
 const uint32_t key=0xa8a30f19u;uint64_t cell=buckets+(uint64_t)(((uint32_t)((end-buckets)>>3)-1u)&key)*8;
 for(int i=0;i<4096;++i){int64_t index{};if(!Read(cell,&index)||index<0||index>1048576)return -1;const uint64_t node=nodes+(uint64_t)index*0x30;uint32_t id{};if(!Read(node+8,&id))return -1;
  if(id==key){uint64_t value{},object{};if(!Read(node+0x10,&value)||!value||!Read(value,&object)||!object)return -1;const uint64_t f=Virtual0(c,object,0x28);return f?reinterpret_cast<int(*)(uint64_t)>(f)(object):-1;}cell=node;}
 return -1;}
// Mouse sensitivity (option layer of type 3 under option #0x18d, key 1; 0 when the key is missing); false without the layer.
inline bool AimSensitivity(const Context& c,float* sens){const uint64_t options=GameOptions(c);uint64_t node{},inner{},next{};int32_t kind{};if(!options||!Read(options+0xc78,&node))return false;
 for(int guard=0;;++guard){if(!node||guard>16||!Read(node+8,&inner)||!inner)return false;if(!Read(inner+0x238,&next))return false;if(!next)break;node=next;}
 if(!Read(inner+0x1d4,&kind)||kind!=3)return false;*sens=0.0f;
 uint64_t mask{},buckets{},end{},first{},at{};if(!Read(node+0x48,&mask)||!Read(node+0x30,&buckets)||!Read(node+0x20,&end)||!buckets)return true;
 uint64_t hash=0xcbf29ce484222325ull;const uint32_t key=1;for(int i=0;i<4;++i)hash=(hash^((key>>(8*i))&0xff))*0x100000001b3ull;const uint64_t slot=buckets+(hash&mask)*16;
 if(!Read(slot+8,&at)||at==end||!Read(slot,&first))return true;
 for(int guard=0;guard<64;++guard){uint32_t found{};if(!Read(at+0x10,&found))return true;if(found==key){float value{};if(at!=end&&Read(at+0x14,&value))*sens=value;return true;}if(at==first)return true;if(!Read(at+8,&at)||!at)return true;}
 return true;}
// Snaps the step to the mouse sensitivity unit and carries the remainder.
inline void AimQuantize(const Context& c,AimAssistState& st,float* out){float rem[2]={0.0f,0.0f};
 if(AimInputMode(c)==1){uint64_t gui{};float scale{},sens{};
  if(Read(c.client+0x668,&gui)&&gui&&AimSensitivity(c,&sens)&&Read(gui+0x40,&scale)&&0.0f<scale){const float g=sens*0.6f+0.15f;const float unit=g*g*g*2880.0f/scale;
   const float v0=out[0]+st.rem[0],v1=out[1]+st.rem[1];const float q0=truncf(v0/unit)*unit,q1=truncf(v1/unit)*unit;rem[0]=v0-q0;rem[1]=v1-q1;out[0]=q0;out[1]=q1;}}
 st.rem[0]=rem[0];st.rem[1]=rem[1];}
inline bool AimSilentLike(const AimAssistConfig& k){auto& fl=FreeLookState();return k.silent||(fl.active&&!fl.fault);}
inline bool AimGates(const Context& c,const AimAssistConfig& k){if(k.holdClick&&!(GetAsyncKeyState(VK_LBUTTON)&0x8000))return false;return !k.weaponsOnly||IsWeapon(c);}

// GameUpdate: picks the target and submits the relative rotation request.
inline void AimAssistWork(){auto& s=AimAssist();if(!s.enabled)return;AimAssistState& st=s.st;if(s.resetPending.exchange(false))st=AimAssistState{};
 AimAssistConfig k;{std::lock_guard<std::mutex> lock(s.guard);k=s.cfg;}Context c{};
 if(!Local(&c)||!CombatNoInterface()||!AimGates(c,k))return;
 {auto& m=Rotations();std::lock_guard<std::recursive_mutex> lock(m.guard);if(m.selValid&&m.selId!=kRotAimAssist)st.lockValid=false;}   // another module's rotation is applied
 AimView v{};v.alpha=AimAlpha(c);uint64_t rot{};float r[2];if(!Read(c.actor+0x250,&rot)||!rot||!Floats(rot,r,2))return;v.pitch=r[0];v.yaw=r[1];
 reinterpret_cast<float*(*)(float*,uint64_t,float)>(c.base+Rva(0x2ceda40))(v.eye,c.actor,v.alpha);if(!std::isfinite(v.eye[0])||!std::isfinite(v.eye[1])||!std::isfinite(v.eye[2]))return;
 if(!AimSilentLike(k)){float cp{},cy{};if(FreeLookCameraAngles(c,&cp,&cy)){v.pitch=cp;v.yaw=cy;}}
 uintptr_t target=0;
 if(k.targetMode==0){const float locked=k.increasedFov?INFINITY:k.fov;
  if(st.lockValid){uintptr_t t{};if(ActorByRuntimeId(c.registry,st.lockId,&t)&&t&&AimAccept(c,k,t)&&AimScore(c,k,v,t,locked)!=3.4028234663852886e+38f)target=t;}
  if(!target){target=AimPick(c,k,v,k.fov);uint64_t id{};st.lockValid=target&&RuntimeId(target,&id);if(st.lockValid)st.lockId=id;}}
 else{st.lockValid=false;target=AimPick(c,k,v,k.fov);}
 uint64_t id{};if(!target||!RuntimeId(target,&id)){st.lastValid=false;return;}
 const float dt=RotFrameDt();float vel[3];if(!AimVelocity(target,vel))return;
 if(!st.lastValid||st.lastId!=id){st.lastId=id;st.lastValid=true;for(int i=0;i<3;++i)st.vel[i]=vel[i];}   // new target: take its velocity
 else{const float f=AimExp(20.0f,dt);for(int i=0;i<3;++i)st.vel[i]=st.vel[i]*(1.0f-f)+vel[i]*f;}
 if(k.stopAtHitbox){float b[6];if(AimBox(target,v.alpha,b)&&AimRayHits(v,b))return;}
 float p[3],yawT{},pitchT{};if(!AimPoint(k,v,target,st.vel,p))return;AimAngles(v,p,&yawT,&pitchT);
 const float d[2]={remainderf(yawT-v.yaw,360.0f),pitchT-v.pitch};const float hs=AimPercent(k.hSpeed),vs=AimPercent(k.vSpeed),t60=dt*60.0f;float out[2]={0.0f,0.0f};
 if(k.mode==0||k.mode==1){out[0]=AimStep(k.mode,d[0],hs,180.0f,t60);out[1]=AimStep(k.mode,d[1],vs,90.0f,t60);}
 else if(k.mode==2){const float kk=AimLockK(k.smoothing);out[0]=AimExp(hs*kk*6.0f,dt)*d[0];out[1]=AimExp(vs*kk*6.0f,dt)*d[1];}
 for(int i=0;i<2;++i)out[i]=copysignf(std::min(std::fabs(d[i]),std::fabs(out[i])),out[i]);   // never past the target
 AimQuantize(c,st,out);
 RotRequest q;q.module=kRotAimAssist;q.relative=true;q.yaw=out[0];q.pitch=k.vertical?out[1]:0.0f;q.priority=1;q.mode=AimSilentLike(k)?2:(k.mode==2?1:0);
 q.predicate=[k,id](uintptr_t){Context c{};if(!AimAssist().enabled||!Local(&c))return false;uintptr_t t{};if(!ActorByRuntimeId(c.registry,id,&t)||!t||!AimAlive(t))return false;return AimGates(c,k);};   // still wanted
 RotSubmit(std::move(q));}
// PacketSend: Bypass reports gamepad input while the Aim Assist request is the applied one.
inline void AimAssistSend(uintptr_t packet){auto& s=AimAssist();if(!s.enabled||!s.bypass)return;uint64_t table{},get{};
 if(!Read(packet,&table)||!table||!Read(table+8,&get)||!get||reinterpret_cast<int(*)(uintptr_t)>(get)(packet)!=0x90)return;
 auto& m=Rotations();bool applied=false;{std::lock_guard<std::recursive_mutex> lock(m.guard);Context c{};
  if(m.stack.empty()&&Local(&c)){RotRequest* r=RotBest(m,c.actor,false);applied=r&&r->module==kRotAimAssist&&m.selValid&&m.selId==kRotAimAssist&&!r->fresh;}}
 if(applied)*(int32_t*)(packet+0x98)=3;}
}
