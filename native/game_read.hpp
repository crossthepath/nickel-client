// Read-only access to the local player's state in Minecraft Bedrock 1.26.62.01 (no writes and, apart from
// ReadPing, no game function calls). Offsets come from profiles/minecraft-1.26.62.01.json. Components are looked up
// by the 32-bit FNV-1a hash of their name, as in the game's EnTT registry. Every memory read is SEH-guarded: an invalid
// address yields "no data", never a crash. Only active in a Minecraft.Windows.exe whose image size matches the profile.
#pragma once
#include <Windows.h>
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "game_fault.hpp"

namespace game {
struct Player{float position[3];float previous[3];float velocity[3];bool onGround;};
struct Effect{uint32_t id;int32_t duration;int32_t amplifier;};
struct Server{bool singleplayer;char address[256];uint32_t port;};
struct Context{uintptr_t base;uint32_t size;uintptr_t client,registry,actor;uint32_t entity;};

template<class T>inline bool Read(uintptr_t address,T* value){__try{*value=*reinterpret_cast<const T*>(address);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
inline constexpr uint32_t Fnv(const char* text){uint32_t value=2166136261u;for(;*text;++text)value=(value^(uint8_t)*text)*16777619u;return value;}

// Hash table of the registry's component storages: buckets at +0x48/+0x50, 0x20-byte nodes at +0x68/+0x70.
inline bool Storage(uintptr_t registry,uint32_t type,uintptr_t* storage){
 uint64_t buckets{},end{},table{},stop{};if(!Read(registry+0x48,&buckets)||!Read(registry+0x50,&end)||!Read(registry+0x68,&table)||!Read(registry+0x70,&stop))return false;
 if(end<buckets||(end-buckets)%8)return false;const uint64_t count=(end-buckets)/8;if(!count||count>1048576||(count&(count-1)))return false;
 uint64_t cell=buckets+((count-1)&type)*8;
 for(int i=0;i<4096;++i){uint64_t index{};if(!Read(cell,&index)||index==~0ull||index>1048576)return false;const uint64_t node=table+index*0x20;uint32_t id{};if(!Read(node+8,&id))return false;
  if(id==type){uint64_t found{};if(node==stop||!Read(node+0x10,&found)||!found)return false;*storage=found;return true;}cell=node;}
 return false;
}
// Sparse set: pages of 2048 entries at +8/+0x10; the entry must carry the same entity version.
inline bool Packed(uintptr_t storage,uint32_t entity,uint32_t* packed){
 uint64_t sparse{},end{};if(!Read(storage+8,&sparse)||!Read(storage+0x10,&end)||end<sparse||(end-sparse)%8)return false;
 const uint64_t page=(entity&0x3ffff)>>11;if(page>=(end-sparse)/8)return false;uint64_t pointer{};if(!Read(sparse+page*8,&pointer)||!pointer)return false;
 uint32_t value{};if(!Read(pointer+(entity&0x7ff)*4,&value)||((value^(entity&0xfffc0000u))>0x3fffe))return false;*packed=value;return true;
}
// Address of a component element: pages of 128 elements at +0x50; version bits of `packed` are masked off.
inline bool Element(uintptr_t registry,uint32_t entity,const char* component,uint32_t stride,uintptr_t* element){
 uintptr_t storage{};uint32_t packed{};if(!Storage(registry,Fnv(component),&storage)||!Packed(storage,entity,&packed))return false;
 uint64_t pages{},page{};if(!Read(storage+0x50,&pages)||!Read(pages+((packed>>4)&0x3ff8),&page)||!page)return false;*element=page+(packed&0x7f)*stride;return true;
}
inline bool LocalActor(uintptr_t registry,uint32_t entity,uintptr_t* actor){
 uintptr_t local{},slot{};uint32_t packed{};if(!Storage(registry,Fnv("LocalPlayerComponent"),&local)||!Packed(local,entity,&packed))return false;
 uint64_t found{};if(!Element(registry,entity,"ActorOwnerComponent",8,&slot)||!Read(slot,&found)||!found)return false;*actor=found;return true;
}
inline bool Supported(uintptr_t* base,uint32_t* size){
 static int state=0;static uintptr_t image=0;static uint32_t imageSize=0;
 if(!state){state=-1;wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);auto file=wcsrchr(path,L'\\');
  if(!_wcsicmp(file?file+1:path,L"Minecraft.Windows.exe")){image=(uintptr_t)GetModuleHandleW(nullptr);auto dos=(const IMAGE_DOS_HEADER*)image;auto nt=(const IMAGE_NT_HEADERS64*)(image+dos->e_lfanew);imageSize=nt->OptionalHeader.SizeOfImage;if(imageSize==327405568u)state=1;}}
 *base=image;*size=imageSize;return state==1;
}
// Main client instance: *(base+0x128c0f90) -> +8 -> +0x20 -> +0x48, context at +0x30, tree at +0x9a0.
inline bool Client(Context* context){
 if(!Supported(&context->base,&context->size))return false;
 uint64_t game{},first{},second{},third{},head{},node{};
 if(!Read(context->base+0x128c0f90,&game)||!game||!Read(game+8,&first)||!Read(first+0x20,&second)||!Read(second+0x48,&third))return false;
 const uint64_t root=third+0x30;if(!Read(root+0x9a0,&head)||!Read(head+8,&node))return false;
 uint64_t candidate=head;bool found=false;
 for(int i=0;i<128;++i){uint8_t flag{};if(!Read(node+0x19,&flag))return false;if(flag){found=true;break;}candidate=node;if(!Read(node,&node))return false;}
 uint8_t sentinel{},secondary{};if(!found||!Read(candidate+0x19,&sentinel)||sentinel||!Read(candidate+0x20,&secondary)||secondary)return false;
 uint64_t client{};if(!Read(candidate+0x38,&client)||!client)return false;context->client=client;return true;
}
// Local player: weak reference at client+0x268 (registry owner) and entity at client+0x278.
inline bool Local(Context* context){
 if(!Client(context))return false;uint64_t owner{};uint32_t entity{};
 if(!Read(context->client+0x268,&owner)||!owner||!Read(context->client+0x278,&entity)||(entity&0x3ffff)==0x3ffff)return false;
 context->registry=owner+0x30;context->entity=entity;if(!LocalActor(context->registry,entity,&context->actor))return false;
 uint64_t table{};return Read(context->actor,&table)&&table>=context->base&&table<context->base+context->size;
}
inline bool ReadPlayer(Player* player){
 Context context{};if(!Local(&context))return false;uint64_t state{};if(!Read(context.actor+0x240,&state)||!state)return false;
 float data[9]{};for(int i=0;i<9;++i){if(!Read(state+i*4,&data[i])||!std::isfinite(data[i])||std::fabs(data[i])>=1e8f)return false;}
 for(int i=0;i<3;++i){player->position[i]=data[i];player->previous[i]=data[3+i];player->velocity[i]=data[6+i];}
 uintptr_t ground{};uint32_t packed{};player->onGround=Storage(context.registry,Fnv("OnGroundFlagComponent"),&ground)&&Packed(ground,context.entity,&packed);
 return true;
}
// Head yaw: first float of ActorHeadRotationComponent, else *(actor+0x250)+4.
inline bool ReadYaw(float* yaw){
 Context context{};if(!Local(&context))return false;uintptr_t element{};float value{};
 if(Element(context.registry,context.entity,"ActorHeadRotationComponent",8,&element)&&Read(element,&value)&&std::isfinite(value)){*yaw=value;return true;}
 uint64_t rotation{};if(!Read(context.actor+0x250,&rotation)||!rotation||!Read(rotation+4,&value)||!std::isfinite(value))return false;*yaw=value;return true;
}
// Health: AttributesComponent (0x50-byte element), sorted uint32 ids in [+0,+8), values every 0x88 bytes at +0x18;
// attribute 6 = health (+0x7c current value, +0x78 maximum).
inline bool ReadHealth(float* health,float* maximum){
 Context context{};if(!Local(&context))return false;uintptr_t element{};uint64_t begin{},end{},table{};
 if(!Element(context.registry,context.entity,"AttributesComponent",0x50,&element)||!Read(element,&begin)||!Read(element+8,&end)||!Read(element+0x18,&table)||end<begin||!table)return false;
 const uint64_t count=(end-begin)/4;if(!count||count>256)return false;
 for(uint64_t i=0;i<count;++i){uint32_t id{};if(!Read(begin+i*4,&id))return false;if(id!=6)continue;
  float current{},top{};if(!Read(table+i*0x88+0x7c,&current)||!Read(table+i*0x88+0x78,&top)||!std::isfinite(current)||current<0||current>4096)return false;*health=current;*maximum=top;return true;}
 return false;
}
// Active effects: MobEffectsComponent (0x18-byte element) = vector [+0,+8) of 0xA8-byte instances.
inline int ReadEffects(Effect* effects,int capacity){
 Context context{};if(!Local(&context))return -1;uintptr_t element{};uint64_t begin{},end{};
 if(!Element(context.registry,context.entity,"MobEffectsComponent",0x18,&element))return 0;
 if(!Read(element,&begin)||!Read(element+8,&end)||end<begin||(end-begin)%0xA8||(end-begin)>0xA8*64)return -1;
 int count=0;for(uint64_t at=begin;at<end&&count<capacity;at+=0xA8){Effect effect{};if(!Read(at,&effect.id)||!Read(at+4,&effect.duration)||!Read(at+0x20,&effect.amplifier))return -1;effects[count++]=effect;}
 return count;
}
// Server: u16 at client+0x6f8 (0 = singleplayer), MSVC std::string address at client+0x700, u32 port at client+0x784.
// The size is re-read after the copy; if it changed in the meantime, the read is rejected.
inline bool ReadServer(Server* server){
 Context context{};if(!Local(&context))return false;uint16_t flag{};uint64_t size{},capacity{},again{};
 if(!Read(context.client+0x6f8,&flag))return false;server->singleplayer=flag==0;server->address[0]=0;server->port=0;if(server->singleplayer)return true;
 if(!Read(context.client+0x710,&size)||!Read(context.client+0x718,&capacity)||size>capacity||size>255)return false;
 uint64_t data=context.client+0x700;if(capacity>15&&(!Read(context.client+0x700,&data)||!data))return false;
 for(uint64_t i=0;i<size;++i){char byte{};if(!Read(data+i,&byte))return false;server->address[i]=byte>=32&&byte<127?byte:'?';}server->address[size]=0;
 if(!Read(context.client+0x710,&again)||again!=size||!Read(context.client+0x784,&server->port))return false;return true;
}

// ---- Latency (Ping module). The only place this file CALLS game functions: no pure-read path is known.
// Chain (1.26.62.01 offsets):
// u16 client+0x6f8 non-zero (0 = singleplayer); X = client->vtable[0x910](client); byte X+0x28 zero; Y = *(X+0x20);
// vector [Y+0xe8, Y+0xf0) not empty; P = **(Y+0xe8); byte P+0x199 zero; Q = *(P+0x118);
// Q->vtable[0x10](Q, out); latency = int64 at out+8, accepted if >= 0.
// Guards: both vtables must lie in the game image and both functions in its code section (.text: RVA 0x1000,
// size 0xeec6600); calls run under SEH and the output is a zeroed 256-byte buffer. `trace` records how far the chain got.
struct PingTrace{int step;uint64_t getter,status;};   // step = last link reached; getter / status = RVAs of the two called functions
inline bool InCode(const Context& context,uint64_t address){return address>=context.base+0x1000&&address<context.base+0x1000+0xeec6600ull;}
inline bool CallGetter(uint64_t function,uint64_t self,uint64_t* result){__try{*result=reinterpret_cast<uint64_t(*)(uint64_t)>(function)(self);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
inline bool CallFill(uint64_t function,uint64_t self,void* out){__try{reinterpret_cast<void*(*)(uint64_t,void*)>(function)(self,out);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
// 1: latency read; 0: no latency (singleplayer or not yet connected, shown as 0 ms); -1: no player or a guard failed.
inline int ReadPing(int64_t* milliseconds,PingTrace* trace){
 Context context{};trace->step=0;trace->getter=trace->status=0;if(!Local(&context))return -1;uint16_t flag{};
 trace->step=1;if(!Read(context.client+0x6f8,&flag))return -1;if(!flag)return 0;
 uint64_t table{},getter{},x{},y{},begin{},end{},p{},q{},peerTable{},status{};uint8_t byte{};
 trace->step=2;if(!Read(context.client,&table)||table<context.base||table>=context.base+context.size||!Read(table+0x910,&getter)||!InCode(context,getter))return -1;trace->getter=getter-context.base;
 // In 1.26.62.01 this getter (RVA 0x232fe60) is just "mov rax,[rcx+0x1f0]; ret". While those eight bytes match,
 // the field is read directly instead of calling the function.
 uint64_t code{};const bool plain=Read(getter,&code)&&code==0xC3000001F0818B48ull;
 trace->step=3;if(plain?!Read(context.client+0x1f0,&x):!CallGetter(getter,context.client,&x))return -1;if(!x)return -1;
 trace->step=4;if(!Read(x+0x28,&byte))return -1;if(byte)return 0;if(!Read(x+0x20,&y)||!y)return 0;
 trace->step=5;if(!Read(y+0xe8,&begin)||!Read(y+0xf0,&end))return -1;if(begin==end||!begin)return 0;if(!Read(begin,&p))return -1;if(!p)return 0;
 trace->step=6;if(!Read(p+0x199,&byte))return -1;if(byte)return 0;if(!Read(p+0x118,&q))return -1;if(!q)return 0;
 trace->step=7;if(!Read(q,&peerTable)||peerTable<context.base||peerTable>=context.base+context.size||!Read(peerTable+0x10,&status)||!InCode(context,status))return -1;trace->status=status-context.base;
 trace->step=8;alignas(16) uint8_t out[256]{};if(!CallFill(status,q,out))return -1;int64_t value{};memcpy(&value,out+8,8);
 trace->step=9;if(value<0||value>600000)return 0;*milliseconds=value;return 1;
}
// Rotation manager (game_rotation.hpp): event stack (0 GameUpdate, 1 LevelTick, 2 screen render,
// 3 UpdatePlayerFromCamera), fed by the existing hooks; null while no rotation module is active.
inline void(*volatile rotationPush)(int)=nullptr;inline void(*volatile rotationPop)()=nullptr;
inline bool(*volatile rotationCamera)(uintptr_t)=nullptr;inline void(*volatile rotationSilent)(uintptr_t)=nullptr;
inline std::atomic<bool> menuPause{false};   // Nickel menu open: modules that click or turn the view are paused
inline void(*volatile rotationGameUpdate)()=nullptr;
inline void(*volatile combatGameUpdate)()=nullptr;
inline void(*volatile discordGameUpdate)()=nullptr;   // Discord Presence (game_discord.hpp)
inline void(*volatile lagGameUpdate)()=nullptr;
// Movement modules (game_movement.hpp): SprintIntent (input, state, flags) and packet receive.
inline void(*volatile sprintMovement)(uint64_t input,uint64_t state,uint64_t flags)=nullptr;inline void(*volatile movementReceive)(uintptr_t packet,int id)=nullptr;
// Item registry of the game's thread-local block (index at game RVA 0x128767c8, block = gs:[0x58][index], initialised when
// byte +0x120 is set, weak reference at +0xce8 / +0xcf0). Item descriptors assert when it is empty, which is the case on the
// threads the overlay runs work from; the registry seen on the level tick is lent there for the duration of the call.
struct ItemRegistryShared{std::atomic<uint64_t> object{0},control{0};};
inline ItemRegistryShared& ItemRegistry(){static auto* s=new ItemRegistryShared;return *s;}
inline uint64_t* ItemRegistrySlotAt(uintptr_t base){const uint32_t index=*reinterpret_cast<const uint32_t*>(base+0x128767c8);
 const uint64_t* table=reinterpret_cast<const uint64_t*>(__readgsqword(0x58));if(!table)return nullptr;uint8_t* block=reinterpret_cast<uint8_t*>(table[index]);
 return block&&block[0x120]?reinterpret_cast<uint64_t*>(block+0xce8):nullptr;}
// Lends the registry to this thread when its slot is empty; returns the slot to give back (nullptr: nothing lent).
inline uint64_t* ItemRegistryLendBegin(){uintptr_t base{};uint32_t size{};if(!Supported(&base,&size))return nullptr;uint64_t* slot=nullptr;
 __try{slot=ItemRegistrySlotAt(base);}__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
 auto& r=ItemRegistry();if(!slot||slot[1]||!r.control.load())return nullptr;slot[0]=r.object;slot[1]=r.control;return slot;}
inline void ItemRegistryLendEnd(uint64_t* slot){if(slot){slot[0]=0;slot[1]=0;}}
}
