// Xray: hides every block that is not in the "blocks" list.
// - "blocks" list (hash 0xe262d6da) of full names; block -> legacy (block+0x68) -> legacy+0x90+0x50 = name string
// (std::string, data at +8, size +0x18, capacity +0x20); true if listed.
// - #5  BlockCacheLookup, game RVA 0x680a730: after the game, an unlisted rendered block is replaced by *(rcx).
// - #6  BlockLight, game RVA 0x68b0ac0: after the game, *(uint16*)rdx = 0x0f0f.
// - BaseLightTexture (#237, Full Bright): +0x24 = 1, +0x28 = 1.0.
// - #7/#8 BlockTessellate (shared Foliage hooks): unlisted block -> cancelled; tessellator+0x7a = 0.
// - #9 BlockActorRender (shared hook): if r9->vtable[0x48](): block = r8->vtable[0x10](r8, arg7), unlisted -> cancelled.
// - #10/#11 RenderChunkVisibility, game RVAs 0x791f170 (rcx+2) / 0x6b1c3e0 (rdx): all faces marked visible (0x5aa880).
// - ScreenViewRender: when the list changes, game RVA 0x7921b70(node[3], 1, 1) for every node of *(lr+0x30)
// (lr = client->vtable[0x5c8]()).
#pragma once
#include "game_filter.hpp"
#include "game_view.hpp"
#include <MinHook.h>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_set>

namespace game {
// verdict cache keyed by BlockLegacy: word = legacy (bits 0..47) | verdict (bit 48) | generation (bits 49..63); stale generations are ignored.
struct XrayShared{SRWLOCK lock=SRWLOCK_INIT;std::unordered_set<std::string> names;std::string text;volatile long enabled=0,fault=0,reload=0;std::atomic<uint64_t> cache[4096]{};std::atomic<uint32_t> gen{1};};
inline XrayShared& Xray(){static auto* s=new XrayShared;return *s;}
inline bool XrayCopyBytes(uintptr_t data,char* out,size_t size){__try{std::memcpy(out,(const void*)data,size);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
// Name of a BlockLegacy (same layout as in the header comment); false if unreadable.
inline bool XrayLegacyName(uint64_t legacy,std::string* out){
 const uintptr_t s=legacy+0x90+0x50;uint64_t size{},cap{},data{};
 if(!Read(s+0x18,&size)||!Read(s+0x20,&cap)||size>256)return false;if(cap>=16){if(!Read(s+8,&data)||!data)return false;}else data=s+8;
 char buffer[256];if(size&&!XrayCopyBytes((uintptr_t)data,buffer,(size_t)size))return false;out->assign(buffer,(size_t)size);return true;
}
inline bool XrayBlockName(uintptr_t block,std::string* out){
 uint64_t legacy{};if(!block||!Read(block+0x68,&legacy)||!legacy)return false;return XrayLegacyName(legacy,out);
}
// Verdict per BlockLegacy for the current name list (generation bumped when the list changes, on chunk reload and every 10 s); failed name reads are never cached.
inline bool XrayAllowed(uintptr_t block){
 auto& x=Xray();uint64_t legacy{};if(!block||!Read(block+0x68,&legacy)||!legacy)return true;const bool cacheable=(legacy>>48)==0;
 const uint64_t gen=x.gen.load(std::memory_order_acquire)&0x7fff;const size_t home=(size_t)(((legacy>>4)*0x9E3779B97F4A7C15ull)>>52);
 if(cacheable)for(size_t i=0;i<4;++i){const uint64_t w=x.cache[(home+i)&4095].load(std::memory_order_relaxed);if((w&0xffffffffffffull)==legacy&&(w>>49)==gen)return ((w>>48)&1)!=0;}
 static thread_local std::string name;if(!XrayLegacyName(legacy,&name))return true;
 AcquireSRWLockShared(&x.lock);const bool ok=x.names.count(name)!=0;ReleaseSRWLockShared(&x.lock);
 if(cacheable){size_t at=home;for(size_t i=0;i<4;++i){const size_t k=(home+i)&4095;const uint64_t w=x.cache[k].load(std::memory_order_relaxed);if((w>>49)!=gen||!w){at=k;break;}}
  x.cache[at].store(legacy|((uint64_t)ok<<48)|(gen<<49),std::memory_order_relaxed);}
 return ok;
}
inline bool XrayOn(){auto& x=Xray();return x.enabled&&!x.fault;}

// #5
using XrayLookupFn=uint64_t(*)(uint64_t,uint64_t,uint64_t,uint64_t);inline XrayLookupFn xrayLookupOriginal=nullptr;
inline uint64_t XrayLookupWork(uint64_t rcx,uint64_t result){if(!XrayOn()||XrayAllowed(result))return result;uint64_t air{};return Read(rcx,&air)?air:result;}
inline uint64_t XrayLookupDetour(uint64_t a,uint64_t b,uint64_t c,uint64_t d){const uint64_t r=xrayLookupOriginal(a,b,c,d);__try{return XrayLookupWork(a,r);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Xray().fault,1);return r;}}
// #6
using XrayLightFn=uint64_t(*)(uint64_t,uint64_t,uint64_t,uint64_t);inline XrayLightFn xrayLightOriginal=nullptr;
inline uint64_t XrayLightDetour(uint64_t a,uint64_t b,uint64_t c,uint64_t d){const uint64_t r=xrayLightOriginal(a,b,c,d);if(XrayOn()&&b){__try{*(uint16_t*)b=0x0f0f;}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Xray().fault,1);}}return r;}
// #10 / #11
inline void XrayVisible(uint8_t* p){
 *(uint32_t*)p=0;*(uint16_t*)(p+4)=0;*(uint16_t*)p=0x106;p[2]=1;p[0]=0x3e;*(uint32_t*)(p+1)|=0x1010204;p[1]=0x3d;p[5]|=3;p[3]|=6;p[4]|=6;   // not a game address: packed flag bits
 uint32_t f;std::memcpy(&f,p+2,4);const uint32_t before=f;f|=0x38100804u;std::memcpy(p+2,&f,4);p[3]=(uint8_t)(((before>>8)&0xff)|0x30);p[4]|=0x20;p[5]|=0x18;
}
using XrayVisFn=uint64_t(*)(uint64_t,uint64_t,uint64_t,uint64_t);inline XrayVisFn xrayVisOriginal[2]{};
inline uint64_t XrayVis0(uint64_t a,uint64_t b,uint64_t c,uint64_t d){const uint64_t r=xrayVisOriginal[0](a,b,c,d);if(XrayOn()&&a){__try{XrayVisible((uint8_t*)a+2);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Xray().fault,1);}}return r;}
inline uint64_t XrayVis1(uint64_t a,uint64_t b,uint64_t c,uint64_t d){const uint64_t r=xrayVisOriginal[1](a,b,c,d);if(XrayOn()&&b){__try{XrayVisible((uint8_t*)b);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Xray().fault,1);}}return r;}
// #7/#8
inline bool XrayTessellate(void* tessellator,void* block){if(!XrayOn())return false;bool hide=false;__try{hide=!XrayAllowed((uintptr_t)block);*((uint8_t*)tessellator+0x7a)=0;}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Xray().fault,1);}return hide;}
// #9
inline bool XrayBlockActorHidden(uint64_t context,uint64_t source,uint64_t position){
 if(!XrayOn()||!context||!source)return false;__try{uint64_t t{},fn{};if(!Read(context,&t)||!Read(t+0x48,&fn)||!fn||!reinterpret_cast<bool(*)(uint64_t)>(fn)(context))return false;
  uint64_t st{},get{};if(!Read(source,&st)||!Read(st+0x10,&get)||!get)return false;const uint64_t block=reinterpret_cast<uint64_t(*)(uint64_t,uint64_t)>(get)(source,position);return block&&!XrayAllowed(block);}
 __except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Xray().fault,1);return false;}}
// Rebuilds the chunk meshes.
inline void XrayReloadChunks(){Context c{};if(!Client(&c))return;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size))return;uint64_t table{},fn{},lr{},head{},node{};
 if(!Read(c.client,&table)||!Read(table+0x5c8,&fn)||!fn||!CallGetter(fn,c.client,&lr)||!lr||!Read(lr+0x30,&head)||!head||!Read(head,&node))return;
 for(int guard=0;guard<64&&node&&node!=head;++guard){uint64_t player{};if(Read(node+0x18,&player)&&player)reinterpret_cast<void(*)(uint64_t,int,int)>(base+Rva(0x7921b70))(player,1,1);if(!Read(node,&node))break;}}
// Screen rendering must run on the game thread: the rebuild is requested here and done on the next ScreenView.
inline void ConfigureXray(bool on,const std::string& text){auto& x=Xray();AcquireSRWLockExclusive(&x.lock);const bool changed=text!=x.text||(on!=(x.enabled!=0));if(text!=x.text){x.text=text;x.names.clear();x.gen.fetch_add(1,std::memory_order_release);size_t from=0;
  while(from<text.size()){size_t to=text.find(',',from);if(to==std::string::npos)to=text.size();if(to>from)x.names.insert(text.substr(from,to-from));from=to+1;}}
 ReleaseSRWLockExclusive(&x.lock);InterlockedExchange(&x.enabled,on&&!x.fault?1:0);lightXray=on;if(changed)InterlockedExchange(&x.reload,1);}
inline void XrayScreenTick(){auto& x=Xray();static ULONGLONG lastBump=0;const ULONGLONG now=GetTickCount64();if(now-lastBump>10000){lastBump=now;x.gen.fetch_add(1,std::memory_order_release);}   // block legacies can be freed and reused: refresh the verdicts regularly
 if(InterlockedExchange(&x.reload,0)){x.gen.fetch_add(1,std::memory_order_release);__try{XrayReloadChunks();}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&x.fault,1);}}}
inline int InstallXrayHooks(void(*log)(const char*)){
 static int status=0;if(status)return status;status=-1;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size))return status;
 if(InstallFoliageHooks(log)!=1||InstallBlockActorHook(log)!=1||InstallLightHooks(log)!=1){if(log)log("Xray: a shared hook is unavailable");return status;}
 struct Hook{uint32_t rva;uint8_t bytes[12];void* detour;void** original;};
 const Hook hooks[]={
  {0x680a730,{0x41,0x57,0x41,0x56,0x56,0x57,0x55,0x53,0x48,0x83,0xec,0x48},(void*)XrayLookupDetour,(void**)&xrayLookupOriginal},
  {0x68b0ac0,{0x41,0x57,0x41,0x56,0x41,0x54,0x56,0x57,0x55,0x53,0x48,0x83},(void*)XrayLightDetour,(void**)&xrayLightOriginal},
  {0x791f170,{0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x55,0x53},(void*)XrayVis0,(void**)&xrayVisOriginal[0]},
  {0x6b1c3e0,{0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53},(void*)XrayVis1,(void**)&xrayVisOriginal[1]}};
 for(const auto& h:hooks)if(!CodeIs(base+Rva(h.rva),h.bytes,12)){if(log)log("Xray: unexpected game code");return status;}
 size_t created=0;auto rollback=[&](const char* why){for(size_t i=0;i<created;++i)MH_RemoveHook((void*)(base+Rva(hooks[i].rva)));if(log)log(why);return status;};
 for(const auto& h:hooks){if(MH_CreateHook((void*)(base+Rva(h.rva)),h.detour,h.original)!=MH_OK)return rollback("Xray: could not create hooks");++created;}
 for(const auto& h:hooks)if(MH_QueueEnableHook((void*)(base+Rva(h.rva)))!=MH_OK)return rollback("Xray: could not queue hooks");
 if(MH_ApplyQueued()!=MH_OK)return rollback("Xray: could not enable hooks");
 tessellateXray=XrayTessellate;tessellateXrayOn=XrayOn;blockActorXray=XrayBlockActorHidden;xrayScreenWork.store(XrayScreenTick,std::memory_order_release);status=1;if(log)log("Xray: block cache, light, chunk visibility hooks installed; tessellate/block actor/light shared");return status;
}
}
