// Custom Sky:
// - Sky.material.bin is replaced on load by native/game_materials.hpp (shaders without cbuffer); mode and phase are
// passed through the ALPHA of the two cubemap sky colours;
// - FrameSkyInsertEvent (hook #219, game RVA 0x6655de0: cancel +0x40, index +0x50, extra passes +0x54);
// - FancySkiesEvent (hook #32, game RVA 0x5afc2f0: forces the fancy skies option);
// - FogSetupEvent (fog hook at game RVA 0x5390350, shared with No Render: colour at +0x424).
#pragma once
#include "game_materials.hpp"
#include "game_events.hpp"
#include "game_filter.hpp"
#include <cmath>

namespace game {
struct SkyConfig{bool enabled=false;int mode=0;float speed=1;bool stars=true,clouds=true,overridePack=true;};
// Per mode: colour table and cycle duration in seconds.
struct SkyMode{float color[4];double seconds;};
inline const SkyMode skyModes[5]={{{.05f,.11f,.14f,1},100},{{.07f,.04f,.10f,1},240},{{.06f,.05f,.09f,1},300},{{.04f,.08f,.15f,1},90},{{.96f,.66f,.46f,1},240}};
struct SkyState{SRWLOCK lock=SRWLOCK_INIT;SkyConfig config;float saved[16]{};int64_t lastNs=0;bool timed=false;double phase=0;int loadsSeen=0,frames=0;bool waiting=false,active=false;volatile LONG fault=0,inserts=0;void(*log)(const char*)=nullptr;};
inline SkyState& Sky(){static SkyState* s=new SkyState;return *s;}
inline int64_t SkyNow(){LARGE_INTEGER f{},c{};QueryPerformanceFrequency(&f);QueryPerformanceCounter(&c);return (int64_t)((double)c.QuadPart*1e9/(double)f.QuadPart);}
inline bool SkyOverworld(){int dimension=-1;return ReadDimension(&dimension)&&dimension==0;}
struct SkyEvent{uint64_t* desc;int index;int extra;bool cancel;};

inline void SkyInsertWork(SkyEvent& ev){
 auto& s=Sky();const SkyConfig c=s.config;if(!c.enabled)return;const uint64_t obj=ev.desc[0];const uint8_t type=*(const uint8_t*)(ev.desc+1);
 if(type==3&&obj){s.active=false;return;}
 if(type==5&&obj){float* m=(float*)(obj+0x28);
  if(ev.index>0){const float* S=s.saved;float o[16];
   if(ev.index==1){const float t[16]={-S[0],-S[1],S[2],S[3],-S[4],-S[5],S[6],S[7],-S[8],-S[9],S[10],S[11],-S[12],-S[13],S[14],S[15]};memcpy(o,t,64);}
   else{const float a=(float)(ev.index-2)*1.5707964f,c=std::cos(a),n=std::sin(a);
    const float t[16]={n*-1.25f,0,c*1.25f,0,c,0,n,0,0,.0125f,0,0,S[12]+c,0,S[14]+n,S[15]};memcpy(o,t,64);}
   memcpy(m,o,64);return;}
  InterlockedExchange(&Materials().overridePack,c.overridePack?1:0);s.active=false;const int state=(int)Materials().skyState;
  if(state==0||(state==2&&!c.overridePack)||(state==3&&c.overridePack)){const int loads=(int)Materials().skyLoads;
   if(s.waiting&&s.loadsSeen==loads){const int before=s.frames;if(before!=0x550){s.frames=before+1;if(before>0x3b&&before+1!=0x550){s.active=true;ev.cancel=true;}}}
   else{s.waiting=true;s.loadsSeen=loads;s.frames=1;}return;}
  s.waiting=false;if(state!=1&&state!=2)return;s.active=true;
  Context context{};if(!Client(&context))return;const uint64_t get=Virtual0(context,context.client,0x5c8);uint64_t holder{},fog{};if(!get||!CallGetter(get,context.client,&holder)||!holder||!Read(holder+0x480,&fog)||!fog)return;
  if(*(const uint8_t*)(fog+0x968)||*(const uint8_t*)(fog+0x96b))return;
  const int mode=c.mode<0||c.mode>4?0:c.mode;float* zenith=(float*)(obj+0x8);float* horizon=(float*)(obj+0x18);memcpy(zenith,skyModes[mode].color,16);memcpy(horizon,skyModes[mode].color,16);
  const int64_t now=SkyNow();const int64_t dt=s.timed?now-s.lastNs:now;s.lastNs=now;s.timed=true;
  double phase=s.phase+((double)c.speed*((double)dt/1e6/1000.0))/skyModes[mode].seconds;phase-=std::floor(phase);s.phase=phase;
  const float alpha=((float)mode+.05f+(float)phase*.9f)*.125f;zenith[3]=alpha;horizon[3]=alpha;   // mode and phase encoded in the colour alpha
  m[13]*=0.0001953125f;memcpy(s.saved,m,64);ev.extra=5;InterlockedIncrement(&s.inserts);return;}
 if(type==3||type==5||type<1||type>7||!obj)return;
 if(type==1){if(!c.stars)ev.cancel=true;return;}
 if(type==6){if(!c.clouds)ev.cancel=true;return;}
 if(type==7){if(s.active&&*(const int32_t*)(obj+0x6c)!=2)ev.cancel=true;return;}
 if(s.active&&SkyOverworld())ev.cancel=true;}
inline void SkyInsertGuarded(SkyEvent& ev){__try{SkyInsertWork(ev);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Sky().fault,1);ev.cancel=false;ev.extra=0;}}
using SkyInsertFunction=void(*)(void*,uint64_t*);inline SkyInsertFunction skyInsertOriginal=nullptr;
// Hook #219 stub: fire the event, call the game if not cancelled, then one pass per requested extra pass.
inline void SkyInsertDetour(void* builder,uint64_t* desc){
 auto& s=Sky();if(!s.config.enabled||s.fault||!desc){skyInsertOriginal(builder,desc);return;}
 AcquireSRWLockExclusive(&s.lock);SkyEvent ev{desc,0,0,false};SkyInsertGuarded(ev);const int extra=ev.extra>0&&ev.extra<16?ev.extra:0;const bool cancel=ev.cancel;ReleaseSRWLockExclusive(&s.lock);
 if(cancel)return;uint64_t copy[2]={desc[0],desc[1]};skyInsertOriginal(builder,copy);
 for(int i=1;i<=extra;++i){AcquireSRWLockExclusive(&s.lock);SkyEvent more{desc,i,0,false};SkyInsertGuarded(more);ReleaseSRWLockExclusive(&s.lock);if(more.cancel)continue;uint64_t again[2]={desc[0],desc[1]};skyInsertOriginal(builder,again);}}
using FancySkiesFunction=bool(*)(void*);inline FancySkiesFunction fancySkiesOriginal=nullptr;
inline bool FancySkiesDetour(void* self){bool result=fancySkiesOriginal(self);auto& s=Sky();if(!s.config.enabled||s.fault)return result;
 const int state=(int)Materials().skyState;if(state==3&&!s.config.overridePack)return result;if(state==4)return result;if(!(s.waiting&&s.frames==0x550))result=true;return result;}
inline void SkyFogWork(void* object){auto& s=Sky();if(!s.config.enabled)return;const uint64_t fog=(uint64_t)object;
 const bool off=!(s.active&&SkyOverworld());if(*(const uint8_t*)(fog+0x968)||*(const uint8_t*)(fog+0x96b)||*(const uint8_t*)(fog+0x96a)||off)return;
 const int mode=s.config.mode<0||s.config.mode>4?0:s.config.mode;memcpy((void*)(fog+0x424),skyModes[mode].color,16);}
inline void SkyFogListener(void* fog){__try{SkyFogWork(fog);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Sky().fault,1);}}
inline void ConfigureSky(const SkyConfig& c){auto& s=Sky();AcquireSRWLockExclusive(&s.lock);s.config=c;if(!c.enabled)s.active=false;ReleaseSRWLockExclusive(&s.lock);InterlockedExchange(&Materials().overridePack,c.overridePack?1:0);}
inline int InstallSkyHooks(void(*log)(const char*)){
 static int status=0;if(status)return status;status=-1;Sky().log=log;
 static const uint8_t insertBytes[19]={0x55,0x56,0x57,0x53,0x48,0x81,0xec,0x08,0x01,0x00,0x00,0x48,0x8d,0xac,0x24,0x80,0x00,0x00,0x00};
 static const uint8_t fancyBytes[11]={0x48,0x83,0xec,0x38,0x48,0x8b,0x05,0xc5,0xdd,0x80,0x0c};
 if(InstallMaterialHook(log)!=1)return status;if(InstallFogHook(log)!=1)return status;fogListener=SkyFogListener;
 if(InstallOne(0x6655de0,insertBytes,19,(void*)SkyInsertDetour,(void**)&skyInsertOriginal,"Custom Sky (frame sky insert)",log)!=1)return status;
 if(InstallOne(0x5afc2f0,fancyBytes,11,(void*)FancySkiesDetour,(void**)&fancySkiesOriginal,"Custom Sky (fancy skies)",log)!=1)return status;
 status=1;return status;}
}
