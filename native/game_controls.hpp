// Custom Paperdoll / Scoreboard / Chat / Item Name: these modules draw nothing; they hide or move a native UI
// control ("hud_player", "sidebar", "chat_panel", "popup_item_text").
// Hook #245: game function at RVA 0x4ca6610 renders a control (7 arguments, rdx = control, returns int; returning 0
// without calling the game skips the draw). Control offsets (1.26.62.01): float2 position +0x10, flags +0x18 (bit 0
// cleared after a write), std::string name +0x20, offset from parent +0x40, size +0x48, weak parent +0x88, children
// (vector of shared pointers, 16 bytes each) +0x98.
// Writes into game memory are guarded by SEH. The written position is the top-left corner of our HUD element; the
// parent is read without locking its weak pointer; Chat only applies on the "hud_screen" screen.
#pragma once
#include "game_render.hpp"

namespace game {
struct ControlSlot{const char* name;bool hudOnly;bool enabled,hide;float x,y;float width,height,nativeX,nativeY;bool moved;long seen;};
struct ControlShared{SRWLOCK lock=SRWLOCK_INIT;ControlSlot slots[6]={{"hud_player",false},{"sidebar",false},{"chat_panel",true},{"popup_item_text",false},{"mob_effects_renderer",false},{"cursor",false}};uint64_t cursorTick=0;volatile long fault=0;};
inline ControlShared& Controls(){static ControlShared* shared=new ControlShared;return *shared;}
using ControlFunction=int(*)(void*,void*,void*,void*,void*,void*,uint8_t);inline ControlFunction controlOriginal=nullptr;
// place a control, then its children (child position = its +0x40 offset + parent position)
inline void PlaceControl(uint64_t control,float x,float y,int depth){
 float position[2]={x,y};memcpy((void*)(control+0x10),position,8);*(uint8_t*)(control+0x18)&=0xfe;if(depth>12)return;
 uint64_t begin=*(uint64_t*)(control+0x98),end=*(uint64_t*)(control+0xa0);if(end<begin||end-begin>16*512)return;
 for(uint64_t at=begin;at<end;at+=16){const uint64_t child=*(uint64_t*)at;if(!child)continue;float offset[2];memcpy(offset,(void*)(child+0x40),8);PlaceControl(child,offset[0]+x,offset[1]+y,depth+1);}
}
// 0: let the game draw; 1: skip drawing (Hide)
inline int ControlWork(void* control){
 {auto& all=Controls();bool active=false;for(int i=0;i<6;++i)active=active||all.slots[i].enabled||all.slots[i].moved;if(!active)return 0;}   // nothing enabled and nothing left to put back: skip the name read (racy flag reads are harmless)
 char name[32]{};if(!control||!Text((uintptr_t)control+0x20,name,sizeof(name)))return 0;auto& shared=Controls();int index=-1;for(int i=0;i<6;++i)if(!strcmp(name,shared.slots[i].name))index=i;if(index<0)return 0;if(index==5)shared.cursorTick=GetTickCount64();   // native crosshair: time of the last render (used by Custom Crosshair)
 AcquireSRWLockShared(&shared.lock);const ControlSlot slot=shared.slots[index];ReleaseSRWLockShared(&shared.lock);const uint64_t self=(uint64_t)control;
 if(!slot.enabled){
  if(slot.moved){uint64_t parent=*(uint64_t*)(self+0x88);float base[2]={0,0},offset[2];if(parent)memcpy(base,(void*)(parent+0x10),8);memcpy(offset,(void*)(self+0x40),8);PlaceControl(self,base[0]+offset[0],base[1]+offset[1],0);
   AcquireSRWLockExclusive(&shared.lock);shared.slots[index].moved=false;ReleaseSRWLockExclusive(&shared.lock);}
  return 0;}
 if(slot.hudOnly&&strcmp(currentScreen,"hud_screen"))return 0;
 if(slot.hide)return 1;
 Context context{};uint64_t gui{};float size[6],own[2];if(!Client(&context)||!Read(context.client+0x668,&gui)||!gui||!Floats(gui+0x40,size,6)||!(size[0]>0)||!(size[1]>0)||!(size[4]>0)||!(size[5]>0))return 0;
 const float ratioX=size[4]/size[0],ratioY=size[5]/size[1];float where[2];memcpy(own,(void*)(self+0x48),8);memcpy(where,(void*)(self+0x10),8);
 if(slot.x>-10000)PlaceControl(self,slot.x*ratioX,slot.y*ratioY,0);
 AcquireSRWLockExclusive(&shared.lock);auto& live=shared.slots[index];live.width=own[0]/ratioX;live.height=own[1]/ratioY;if(!slot.moved){live.nativeX=where[0]/ratioX;live.nativeY=where[1]/ratioY;}if(slot.x>-10000)live.moved=true;++live.seen;ReleaseSRWLockExclusive(&shared.lock);return 0;
}
inline uint64_t ControlSeenTick(int){return Controls().cursorTick;}
inline int ControlGuarded(void* control){__try{return ControlWork(control);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Controls().fault,1);return 0;}}
inline int ControlDetour(void* a,void* control,void* c,void* d,void* e,void* f,uint8_t g){if(!Controls().fault&&ControlGuarded(control))return 0;return controlOriginal(a,control,c,d,e,f,g);}
// ---- Custom Item Name "Format" setting: hook #244, game function at RVA 0x4d55810,
// bool f(rcx, std::string* text, r8, r9) = held-item text above the hotbar. The game runs first; if it returns true
// and the text is not empty, every "{name}" in the format is replaced by that text and the result written back.
// The game string is rewritten IN PLACE within its existing capacity (its allocator cannot be used), so a longer
// result is truncated.
struct NameFormat{SRWLOCK lock=SRWLOCK_INIT;bool enabled=false;char format[256]{};volatile long fault=0,seen=0,clipped=0;};
inline NameFormat& Format(){static NameFormat* shared=new NameFormat;return *shared;}
using PopupFunction=bool(*)(void*,void*,void*,void*);inline PopupFunction popupOriginal=nullptr;
inline void PopupWork(uint64_t text){
 auto& shared=Format();AcquireSRWLockShared(&shared.lock);const bool enabled=shared.enabled;const std::string format=shared.format;ReleaseSRWLockShared(&shared.lock);if(!enabled||!text)return;
 const uint64_t size=*(uint64_t*)(text+0x10),room=*(uint64_t*)(text+0x18);if(!size||size>room||room<15||room>4096)return;char* data=room>15?*(char**)text:(char*)text;if(!data)return;
 const std::string name(data,size);std::string out=format;if(out.size()>=6)for(size_t at=out.find("{name}");at!=std::string::npos;at=out.find("{name}",at+name.size()))out.replace(at,6,name);
 if(out.size()>room){out.resize(room);InterlockedExchange(&shared.clipped,1);}memcpy(data,out.data(),out.size());data[out.size()]=0;*(uint64_t*)(text+0x10)=out.size();InterlockedIncrement(&shared.seen);
}
inline void PopupGuarded(void* text){__try{PopupWork((uint64_t)text);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Format().fault,1);}}
inline bool PopupDetour(void* a,void* text,void* c,void* d){const bool result=popupOriginal(a,text,c,d);if(result&&!Format().fault)PopupGuarded(text);return result;}
inline int InstallPopupHook(void(*log)(const char*)){
 static int state=0;if(state)return state;state=-1;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size))return state;
 const uint8_t expected[16]={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0xd8};for(int i=0;i<16;++i){uint8_t byte{};if(!Read(base+0x4d55810+i,&byte)||byte!=expected[i]){if(log)log("Item Name: unexpected bytes at the item text function, no hook installed");return state;}}
 if(MH_CreateHook((void*)(base+0x4d55810),(void*)PopupDetour,(void**)&popupOriginal)!=MH_OK||MH_EnableHook((void*)(base+0x4d55810))!=MH_OK){if(log)log("Item Name: MinHook could not install the item text hook");return state;}
 if(log)log("Item Name: item text hook installed");state=1;return state;
}
inline int InstallControlHook(void(*log)(const char*)){
 static int state=0;if(state)return state;state=-1;if(InstallScreenHook(log)!=1)return state;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size))return state;
 const uint8_t expected[16]={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0x78};for(int i=0;i<16;++i){uint8_t byte{};if(!Read(base+0x4ca6610+i,&byte)||byte!=expected[i]){if(log)log("Native controls: unexpected bytes at the control render function, no hook installed");return state;}}
 if(MH_CreateHook((void*)(base+0x4ca6610),(void*)ControlDetour,(void**)&controlOriginal)!=MH_OK||MH_EnableHook((void*)(base+0x4ca6610))!=MH_OK){if(log)log("Native controls: MinHook could not install the control hook");return state;}
 if(log)log("Native controls: control render hook installed");state=1;return state;
}
}
