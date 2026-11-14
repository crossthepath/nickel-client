// Camera and lighting Visuals: Zoom, Full Bright, Time Changer, No Camera Bob, Hurt Color (RVAs for 1.26.62.01).
// Zoom           hook #229  RVA 0x6f2ca80  std::optional<float>* f(self, out): the game runs first, then the field of
// view (radians) is replaced: target = fov x (1 - Zoom/100); with Animation, v += 0.15 x (target - v) per
// call. Sensitivity: option #0x18d (key 1, float at element+0x14) is multiplied by Sensitivity/100 on
// enable and RESTORED on disable (writes the game's options).
// Full Bright    hooks #237 RVA 0x5aed240 and #238 RVA 0x7100870: the game builds a 0x44-byte block (*return); byte
// +0x24 is forced to 1 and float +0x28 to at least Intensity/100. Vibrant visuals (option #0x2bf == 2):
// hook #218 RVA 0x666aa00, byte 1 and float Intensity/100 in the effect block *(rdx) when byte rdx+8 is 3.
// Time Changer   hook #247  RVA 0x293f460  float f(dimension, int time, float alpha): result replaced by Time
// (0 = noon, 0.25 = sunset, 0.5 = midnight, 0.75 = sunrise).
// No Camera Bob  hook #12   RVA 0x6f2ed10  the game function is skipped.
// Hurt Color     hook #3    RVA 0x6ebb280  returns false for every actor; the tint: hook at the start of RVA 0x6b230b0,
// game first, then the returned colour is replaced if the actor (*(r8+0x38)) is hurt (byte +0x238 bit 2
// and **(actor+0x448) > 0).
#pragma once
#include "game_hand.hpp"

namespace game {
struct ViewShared{
 volatile long zoom=0,zoomAnimation=1,fullBright=0,vibrant=0,timeChanger=0,noBob=0,hurtColor=0,fault=0,fovSeen=0,lightSeen=0,timeSeen=0,hurtSeen=0;
 float zoomAmount=.5f,brightness=1,time=0,hurt[4]{1,0,0,.6f},lastFov=0;volatile long timeTick=-1;   // last field of view and tick seen (Debug Overlay)
};
inline ViewShared& Camera(){static ViewShared* shared=new ViewShared;return *shared;}
using FovFunction=float*(*)(void*,float*,void*,void*);inline FovFunction fovOriginal=nullptr;
inline void FovWork(float* value){auto& shared=Camera();static float shown=0;static bool valid=false;{float seen{};uint8_t engaged{};if(Read((uintptr_t)value,&seen)&&Read((uintptr_t)value+4,&engaged)&&engaged==1)shared.lastFov=seen;}if(!shared.zoom){valid=false;return;}float fov{};uint8_t has{};if(!Read((uintptr_t)value,&fov)||!Read((uintptr_t)value+4,&has)||has!=1||!std::isfinite(fov))return;
 float target=fov*(1-std::clamp(shared.zoomAmount,0.f,1.f));if(shared.zoomAnimation){const float from=valid?shown:fov;float next=from+(target-from)*.15f;if((target-from)*(target-next)<0)next=target;target=next;}shown=target;valid=true;*value=target;InterlockedExchange(&shared.fovSeen,1);}
inline void FovGuarded(float* value){__try{FovWork(value);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Camera().fault,1);}}
inline float* FovDetour(void* self,float* out,void* c,void* d){float* result=fovOriginal(self,out,c,d);if(result&&!Camera().fault)FovGuarded(result);return result;}

using LightFunction=void**(*)(void*,void**,void*,void*);inline LightFunction lightOriginal[2]{};
inline void LightWork(void** result){auto& shared=Camera();if(!shared.fullBright||shared.vibrant)return;uint64_t block{};if(!Read((uintptr_t)result,&block)||!block)return;uint8_t night{};float scale{};if(!Read(block+0x24,&night)||!Read(block+0x28,&scale))return;
 float wanted=shared.brightness;if(night==1&&wanted<=scale)wanted=scale;*(float*)(block+0x28)=wanted;*(uint8_t*)(block+0x24)=1;InterlockedExchange(&shared.lightSeen,1);}
inline void LightGuarded(void** result){__try{LightWork(result);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Camera().fault,1);}}
// Xray (BaseLightTextureEvent): +0x24 = 1, +0x28 = 1.0 on the base light texture (not the Nether one).
inline volatile bool lightXray=false;
inline void LightXrayWork(void** result){uint64_t block{};if(Read((uintptr_t)result,&block)&&block){*(uint8_t*)(block+0x24)=1;*(float*)(block+0x28)=1.f;}}
inline void** LightDetour0(void* a,void** b,void* c,void* d){void** result=lightOriginal[0](a,b,c,d);if(result&&!Camera().fault)LightGuarded(result);
 if(result&&lightXray){__try{LightXrayWork(result);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Camera().fault,1);}}return result;}
inline void** LightDetour1(void* a,void** b,void* c,void* d){void** result=lightOriginal[1](a,b,c,d);if(result&&!Camera().fault)LightGuarded(result);return result;}
using VisionFunction=uint64_t(*)(void*,void*,void*,void*);inline VisionFunction visionOriginal=nullptr;
inline void VisionWork(void* second){auto& shared=Camera();if(!shared.fullBright||!shared.vibrant||!second)return;uint8_t kind{};uint64_t effect{};if(!Read((uintptr_t)second+8,&kind)||kind!=3||!Read((uintptr_t)second,&effect)||!effect)return;*(uint8_t*)effect=1;*(float*)(effect+4)=shared.brightness;InterlockedExchange(&shared.lightSeen,1);}
inline void VisionGuarded(void* second){__try{VisionWork(second);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Camera().fault,1);}}
inline bool (*effectHidden)(void*)=nullptr;   // game_filter.hpp filter (No Particle, No Render): true = effect is not inserted
inline uint64_t VisionDetour(void* a,void* b,void* c,void* d){if(effectHidden&&effectHidden(b))return 0;if(!Camera().fault)VisionGuarded(b);return visionOriginal(a,b,c,d);}

using TimeFunction=float(*)(void*,int,float);inline TimeFunction timeOriginal=nullptr;
inline float TimeDetour(void* self,int time,float alpha){const float value=timeOriginal(self,time,alpha);auto& shared=Camera();InterlockedExchange(&shared.timeTick,time);if(!shared.timeChanger)return value;InterlockedExchange(&shared.timeSeen,1);return shared.time;}
using BobFunction=void(*)(void*,void*,void*);inline BobFunction bobOriginal=nullptr;
inline void BobDetour(void* a,void* b,void* c){if(Camera().noBob)return;bobOriginal(a,b,c);}
using AddFunction=bool(*)(void*,void*,void*,bool);inline AddFunction addOriginal=nullptr;
inline int (*actorVerdict)(void*)=nullptr;   // game_filter.hpp filter: 1 = hidden (false), 2 = too far (true)
// Highlight ActorRendererAddEvent listener: true = "cancel and return false" (+0x59) so the entity leaves the
// batched lists and goes through individual render #0, where Chams/Walls/Outline mark it.
inline bool(*volatile actorAddHighlight)(void* actor)=nullptr;
inline bool AddDetour(void* a,void* b,void* c,bool d){if(actorVerdict){const int verdict=actorVerdict(b);if(verdict==2)return true;if(verdict==1)return false;}if(Camera().hurtColor)return false;
 if(auto highlight=actorAddHighlight)if(highlight(b))return false;return addOriginal(a,b,c,d);}
using HurtFunction=float*(*)(void*,float*,void*);inline HurtFunction hurtOriginal=nullptr;
inline void HurtWork(float* out,void* parameters){auto& shared=Camera();if(!shared.hurtColor||!out)return;uint64_t actor{},counter{};uint8_t flags{};int32_t ticks{};if(!Read((uintptr_t)parameters+0x38,&actor)||!actor||!Read(actor+0x238,&flags)||!(flags&2)||!Read(actor+0x448,&counter)||!counter||!Read(counter,&ticks)||ticks<=0)return;memcpy(out,shared.hurt,16);InterlockedExchange(&shared.hurtSeen,1);}
inline void HurtGuarded(float* out,void* parameters){__try{HurtWork(out,parameters);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Camera().fault,1);}}
// Extra subscriber for the actor overlay colour (none at the moment).
inline void(*volatile overlayListener)(float* out,void* parameters)=nullptr;
inline float* HurtDetour(void* material,float* out,void* parameters){float* result=hurtOriginal(material,out,parameters);if(auto listener=overlayListener)listener(result?result:out,parameters);if(!Camera().fault)HurtGuarded(result?result:out,parameters);return result;}

inline int InstallFovHook(void(*log)(const char*)){static int state=0;if(state)return state;const uint8_t bytes[16]={0x56,0x57,0x55,0x53,0x48,0x83,0xec,0x58,0x0f,0x29,0x74,0x24,0x40,0x48,0x89,0xd6};state=InstallOne(0x6f2ca80,bytes,16,(void*)FovDetour,(void**)&fovOriginal,"Zoom",log);return state;}
inline int InstallEffectHook(void(*log)(const char*)){static int state=0;if(state)return state;const uint8_t bytes[16]={0x55,0x56,0x57,0x53,0x48,0x81,0xec,0x08,0x01,0x00,0x00,0x48,0x8d,0xac,0x24,0x80};state=InstallOne(0x666aa00,bytes,16,(void*)VisionDetour,(void**)&visionOriginal,"Frame effects",log);return state;}
inline int InstallLightHooks(void(*log)(const char*)){static int state=0;if(state)return state;state=-1;
 const uint8_t a[16]={0x55,0x41,0x57,0x41,0x56,0x56,0x57,0x53,0x48,0x83,0xec,0x68,0x48,0x8d,0x6c,0x24},b[16]={0x55,0x41,0x56,0x56,0x57,0x53,0x48,0x83,0xec,0x50,0x48,0x8d,0x6c,0x24,0x50,0x0f};
 if(InstallOne(0x5aed240,a,16,(void*)LightDetour0,(void**)&lightOriginal[0],"Full Bright (light)",log)!=1)return state;if(InstallOne(0x7100870,b,16,(void*)LightDetour1,(void**)&lightOriginal[1],"Full Bright (nether light)",log)!=1)return state;
 InstallEffectHook(log);state=1;return state;}
inline int InstallTimeHook(void(*log)(const char*)){static int state=0;if(state)return state;const uint8_t bytes[16]={0x48,0x63,0xc2,0x48,0x69,0xc8,0xf1,0x19,0x76,0x05,0x48,0x89,0xca,0x48,0xc1,0xea};state=InstallOne(0x293f460,bytes,16,(void*)TimeDetour,(void**)&timeOriginal,"Time Changer",log);return state;}
inline int InstallBobHook(void(*log)(const char*)){static int state=0;if(state)return state;const uint8_t bytes[16]={0x41,0x57,0x41,0x56,0x56,0x57,0x53,0x48,0x81,0xec,0xe0,0x00,0x00,0x00,0x44,0x0f};state=InstallOne(0x6f2ed10,bytes,16,(void*)BobDetour,(void**)&bobOriginal,"No Camera Bob",log);return state;}
// Hook #3b alone (actor overlay colour, game RVA 0x6b230b0): used by Hurt Color.
inline int InstallOverlayHook(void(*log)(const char*)){static int state=0;if(state)return state;const uint8_t a[16]={0x41,0x57,0x41,0x56,0x41,0x54,0x56,0x57,0x55,0x53,0x48,0x83,0xec,0x30,0x0f,0x29};state=InstallOne(0x6b230b0,a,16,(void*)HurtDetour,(void**)&hurtOriginal,"Actor overlay colour",log);return state;}
inline int InstallHurtHooks(void(*log)(const char*)){static int state=0;if(state)return state;state=-1;
 const uint8_t a[16]={0x41,0x57,0x41,0x56,0x41,0x54,0x56,0x57,0x55,0x53,0x48,0x83,0xec,0x30,0x0f,0x29},b[10]={0x41,0x56,0x56,0x57,0x55,0x53,0x48,0x83,0xec,0x40};
 if(InstallOverlayHook(log)!=1)return state;if(!addOriginal&&InstallOne(0x6ebb280,b,10,(void*)AddDetour,(void**)&addOriginal,"Actor lists",log)!=1)return state;state=1;return state;}

// ---- Game options: options = client->vtable[0x560](client); inline pointer array at options+0x10; int at option+0x18
inline uint64_t GameOptions(const Context& context){const uint64_t get=Virtual(context,context.client,0x560);uint64_t options{};return get&&CallGetter(get,context.client,&options)?options:0;}
inline bool OptionValue(int index,int* value){Context context{};if(!Client(&context))return false;const uint64_t options=GameOptions(context);uint64_t option{};int32_t read{};if(!options||!Read(options+0x10+8ull*index,&option)||!option||!Read(option+0x18,&read))return false;*value=read;return true;}
// Zoom sensitivity: node = chain of option #0x18d (+8 then +0x238 while non-null; int type +0x1d4 == 3);
// FNV-1a 64 hash table on the u32 key: mask node+0x48, buckets node+0x30 (16 bytes), sentinel node+0x20,
// key element+0x10, float +0x14.
inline bool SensitivitySlot(float** slot){
 Context context{};if(!Client(&context))return false;const uint64_t options=GameOptions(context);uint64_t node{},inner{},next{};int32_t kind{};if(!options||!Read(options+0x10+8ull*0x18d,&node)||!node)return false;
 for(int guard=0;guard<16;++guard){if(!Read(node+8,&inner)||!inner)return false;if(!Read(inner+0x238,&next))return false;if(!next)break;node=next;}if(!Read(inner+0x1d4,&kind)||kind!=3)return false;
 uint64_t mask{},buckets{},end{},first{},last{};if(!Read(node+0x48,&mask)||!Read(node+0x30,&buckets)||!Read(node+0x20,&end)||!buckets)return false;uint64_t hash=0xcbf29ce484222325ull;const uint32_t key=1;for(int i=0;i<4;++i)hash=(hash^((key>>(8*i))&0xff))*0x100000001b3ull;
 if(!Read(buckets+(hash&mask)*16,&first)||!Read(buckets+(hash&mask)*16+8,&last)||!last||last==end)return false;uint64_t element=last;for(int guard=0;guard<64;++guard){uint32_t found{};if(!Read(element+0x10,&found))return false;if(found==key){*slot=(float*)(element+0x14);return true;}if(element==first)return false;if(!Read(element+8,&element)||!element)return false;}return false;
}
// Call when Zoom turns on (on = true) or off; keeps the previous value so it can be restored.
inline void ZoomSensitivity(bool on,float factor){
 static bool saved=false;static float original=0;__try{float* slot=nullptr;if(on&&!saved){if(SensitivitySlot(&slot)){original=*slot;saved=true;*slot=original*factor;}}else if(!on&&saved){saved=false;if(SensitivitySlot(&slot))*slot=original;}}__except(EXCEPTION_EXECUTE_HANDLER){saved=false;}
}
}
