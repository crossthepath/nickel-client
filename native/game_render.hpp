// Armor HUD: item icons drawn BY THE GAME inside its own UI rendering.
// This is the riskiest code in the overlay: a hook on the game's screen rendering, then calls into its item renderer.
// Game RVAs are for Minecraft 1.26.62.01:
// hook #246  0x4cb3430  screen render (rcx = screen, rdx = UI render context); we draw BEFORE the game
// 0x57e2560 / 0x57e29e0      actor render context constructor / destructor (0x300-byte zeroed buffer)
// 0x5757550, 0x5757a20       item render kind; flush flags
// 0x6e73e00 (11 arguments), 0x6e749c0 (13 arguments)  the two icon draw functions; 0x4c95550 flush; 0x54177b0
// vtables: container +0x38 slot item, +0xa0 size; item +0x120 max durability, +0x140 enchant glint, +0x3b8 icon image.
// Icons are drawn only on "hud_screen". All work runs under SEH; on the first fault the feature is disabled until the game
// restarts. Position comes from our HUD (render thread); durability text goes back there to be drawn by ImGui.
#pragma once
#include "game_events.hpp"
#include <atomic>
#include <cstring>
#include <string>

namespace game {
struct ArmorRequest{bool enabled,vertical,held,durability;float x,y,scale;};
struct ArmorResult{int count;char text[5][16];};
struct ArmorShared{SRWLOCK lock=SRWLOCK_INIT;ArmorRequest request{};ArmorResult result{};volatile long fault=0;volatile long frames=0;};
inline ArmorShared& Armor(){static ArmorShared* shared=new ArmorShared;return *shared;}
using ScreenFunction=void(*)(void*,void*);inline ScreenFunction screenOriginal=nullptr;
inline std::atomic<void(*)(void*)> namesScreenWork{};
inline std::atomic<void(*)(void*)> visualFiltersScreenWork{};
// Xray: chunk reloads run on the game's render thread (ScreenViewRender), never from Present
inline std::atomic<void(*)()> xrayScreenWork{};

// Integer from a CompoundTag (MSVC std::map at +8: head -> root at +8; node: left +0, right +0x10, nil +0x19, key
// std::string +0x20, value +0x40 with the integer at +0x48 and the type at +0x68; type 3 = int)
inline int NbtInt(uint64_t tag,const char* key){
 uint64_t head{},stack[64]{};int depth=0;if(!Read(tag+8,&head)||!head||!Read(head+8,&stack[0])||!stack[0])return 0;depth=1;
 for(int visited=0;depth>0&&visited<256;++visited){const uint64_t node=stack[--depth];uint8_t nil{};if(!node||node==head||!Read(node+0x19,&nil)||nil)continue;
  uint64_t size{},room{},data=node+0x20,left{},right{};if(!Read(node+0x30,&size)||!Read(node+0x38,&room))return 0;if(room>15&&!Read(node+0x20,&data))return 0;
  bool same=size==strlen(key);for(uint64_t i=0;same&&i<size;++i){char byte{};if(!Read(data+i,&byte)||byte!=key[i])same=false;}
  if(same){uint8_t type{};int32_t value{};return Read(node+0x68,&type)&&type==3&&Read(node+0x48,&value)?value:0;}
  if(Read(node,&left)&&Read(node+0x10,&right)&&depth<62){stack[depth++]=left;stack[depth++]=right;}}
 return 0;
}
inline bool ValidStack(uint64_t stack){uint64_t holder{},item{};return stack&&Read(stack+8,&holder)&&holder&&Read(holder,&item)&&item;}
inline uint64_t ItemOf(uint64_t stack){uint64_t holder{},item{};return Read(stack+8,&holder)&&holder&&Read(holder,&item)?item:0;}
inline uint64_t Virtual(const Context& context,uint64_t object,uint32_t offset){uint64_t table{},function{};return object&&Read(object,&table)&&table>=context.base&&table<context.base+context.size&&Read(table+offset,&function)&&InCode(context,function)?function:0;}
// Leather armour (dyeable): tag "minecraft:leather_tier" in [item+0x1f8, +0x200) (0x30 bytes, text at +8)
// or item name starting with "leather_" (item+0xd8)
inline bool Leather(uint64_t item){
 uint64_t begin{},end{};char text[40]{};if(Read(item+0x1f8,&begin)&&Read(item+0x200,&end)&&end>=begin&&(end-begin)<=0x30*64)for(uint64_t at=begin;at<end;at+=0x30)if(Text(at+8,text,sizeof(text))&&!strcmp(text,"minecraft:leather_tier"))return true;
 return Text(item+0xd8,text,sizeof(text))&&!strncmp(text,"leather_",8);
}
// Draws one icon. ui = UI context, ctx = actor render context built by us.
inline void RenderIcon(const Context& context,void* ui,uint8_t* ctx,uint64_t player,uint64_t stack,float x,float y,float scale,float alpha){
 const uint64_t item=ItemOf(stack);if(!item)return;const uint64_t image=Virtual(context,item,0x3b8),foil=Virtual(context,item,0x140);if(!image||!foil)return;
 const int frame=reinterpret_cast<int(*)(uint64_t,uint64_t,int,uint64_t,bool)>(image)(item,player,0,stack,true);
 const int kind=reinterpret_cast<int(*)(uint64_t)>(context.base+0x5757550)(stack);const bool leather=Leather(item);
 uint8_t flags=0;reinterpret_cast<void*(*)(uint8_t*,uint64_t)>(context.base+0x5757a20)(&flags,stack);
 uint64_t screen{},colour{},holder{},state{},level{};memcpy(&screen,ctx+0x28,8);if(!screen||!Read(screen+0x30,&colour)||!colour||!Read(screen+0x20,&holder)||!holder||!Read(holder+0x150,&state)||!state||!Read(state+0x40,&level)||!level)return;
 uint8_t saved[16];memcpy(saved,(void*)colour,16);const float before=*(float*)level;const float white[4]={1,1,1,1};memcpy((void*)colour,white,16);*(uint8_t*)(colour+16)=1;
 *(float*)level=leather?1000.f:alpha;*(uint8_t*)(state+0x31)=1;uint64_t renderer{};memcpy(&renderer,ctx+0x58,8);
 using Draw11=void(*)(uint64_t,uint8_t*,uint64_t,int,float,float,bool,float,float,float,int);using Draw13=void(*)(uint64_t,uint8_t*,int,uint64_t,float,float,float,float,float,int,bool,int,const float*);
 const Draw11 draw=reinterpret_cast<Draw11>(context.base+0x6e73e00);const auto flush=reinterpret_cast<void(*)(void*,uint8_t*,uint8_t*)>(context.base+0x4c95550);
 if(kind>=0&&kind<0xb&&((0x403u>>kind)&1)){uint64_t extra{};if(Read(screen+0xb0,&extra)&&extra){reinterpret_cast<void(*)(uint64_t)>(context.base+0x54177b0)(extra);
   reinterpret_cast<Draw13>(context.base+0x6e749c0)(renderer,ctx,kind,stack,x,y,1.f,alpha,scale,frame,true,0x62,white);*(uint8_t*)(extra+0x255)=0;}}
 else draw(renderer,ctx,stack,frame,x,y,false,alpha,1.f,scale,0x62);
 if(leather){*(float*)level=before;*(uint8_t*)(state+0x31)=1;}
 flush(ui,ctx,&flags);*(float*)level=before;*(uint8_t*)(state+0x31)=1;
 if(reinterpret_cast<bool(*)(uint64_t,uint64_t)>(foil)(item,stack)){draw(renderer,ctx,stack,frame,x,y,true,alpha,1.f,scale,0x62);flush(ui,ctx,&flags);}
 memcpy((void*)colour,saved,16);*(uint8_t*)(colour+16)=1;
}
// Name of the screen being rendered: *(screen+0x50) -> object at +8 -> std::string at +0x20
inline char currentScreen[32]{};
inline void ReadScreenName(void* view){uint64_t holder{},object{};currentScreen[0]=0;if(Read((uintptr_t)view+0x50,&holder)&&holder&&Read(holder+8,&object)&&object&&!Text(object+0x20,currentScreen,sizeof(currentScreen)))currentScreen[0]=0;}
// Last non-empty screen name, published for other threads (currentScreen is zeroed after every render and is only valid on the render thread).
// Seqlock: odd sequence = write in progress. ScreenSnapshot copies a consistent name into out[32]; false if no screen was seen yet.
inline std::atomic<uint64_t> screenPublished[4]{};inline std::atomic<uint32_t> screenSeq{0};
inline void ScreenPublish(const char* name){uint64_t words[4]{};std::memcpy(words,name,31);const uint32_t seq=screenSeq.load(std::memory_order_relaxed);screenSeq.store(seq+1,std::memory_order_relaxed);std::atomic_thread_fence(std::memory_order_release);
 for(int i=0;i<4;++i)screenPublished[i].store(words[i],std::memory_order_relaxed);screenSeq.store(seq+2,std::memory_order_release);}
inline bool ScreenSnapshot(char out[32]){for(int tries=0;tries<8;++tries){const uint32_t before=screenSeq.load(std::memory_order_acquire);if(before&1)continue;uint64_t words[4];for(int i=0;i<4;++i)words[i]=screenPublished[i].load(std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_acquire);if(screenSeq.load(std::memory_order_relaxed)!=before)continue;std::memcpy(out,words,32);out[31]=0;return out[0]!=0;}
 out[0]=0;return false;}
inline void ArmorWork(void* view,void* ui){
 auto& shared=Armor();AcquireSRWLockShared(&shared.lock);const ArmorRequest request=shared.request;ReleaseSRWLockShared(&shared.lock);if(!request.enabled)return;
 (void)view;if(strcmp(currentScreen,"hud_screen"))return;
 ArmorResult result{};uint64_t stacks[5]{};Context context{};
 if(Local(&context)){uintptr_t registry{},storage{},equipment{};uint32_t entity{};uint64_t hand{},armor{};
  if(ActorEntity(context.actor,&registry,&entity)&&Storage(registry,0xb06141a9u,&storage)&&InStorage(storage,entity,0x10,&equipment)&&Read(equipment,&hand)&&Read(equipment+8,&armor)){   // ActorEquipmentComponent: hand container, armour container
   if(request.held&&hand)if(const uint64_t get=Virtual(context,hand,0x38)){const uint64_t stack=reinterpret_cast<uint64_t(*)(uint64_t,int)>(get)(hand,0);if(ValidStack(stack))stacks[result.count++]=stack;}
   if(armor){const uint64_t get=Virtual(context,armor,0x38),size=Virtual(context,armor,0xa0);if(get&&size){const int slots=reinterpret_cast<int(*)(uint64_t)>(size)(armor);
     for(int i=0;i<slots&&i<4&&result.count<5;++i){const uint64_t stack=reinterpret_cast<uint64_t(*)(uint64_t,int)>(get)(armor,i);if(ValidStack(stack))stacks[result.count++]=stack;}}}}}
 if(request.durability)for(int i=0;i<result.count;++i){const uint64_t item=ItemOf(stacks[i]),top=Virtual(context,item,0x120);if(!top)continue;const int maximum=reinterpret_cast<short(*)(uint64_t)>(top)(item);if(maximum<=0)continue;
  uint64_t data{};const int damage=Read(stacks[i]+0x10,&data)&&data?NbtInt(data,"Damage"):0;wsprintfA(result.text[i],"%d",maximum-damage>0?maximum-damage:0);}
 AcquireSRWLockExclusive(&shared.lock);shared.result=result;ReleaseSRWLockExclusive(&shared.lock);InterlockedIncrement(&shared.frames);if(!result.count)return;
 // Pixel -> game UI unit ratio: *(client+0x668) + 0x40: [0],[1] size in pixels, [4],[5] size in UI units
 uint64_t gui{},game{},first{},second{},third{},screenContext{},clientOfUi{};float size[6];if(!Read(context.client+0x668,&gui)||!gui||!Floats(gui+0x40,size,6)||!(size[0]>0)||!(size[1]>0)||!(size[4]>0)||!(size[5]>0))return;
 if(!Read(context.base+0x128c0f90,&game)||!Read(game+8,&first)||!Read(first+0x20,&second)||!Read(second+0x48,&third)||!third||!Read((uintptr_t)ui+0x10,&screenContext)||!screenContext||!Read((uintptr_t)ui+8,&clientOfUi)||!clientOfUi)return;
 const float ratioX=size[4]/size[0],ratioY=size[5]/size[1],s=request.scale;alignas(16) uint8_t ctx[0x300];memset(ctx,0,sizeof(ctx));
 reinterpret_cast<void*(*)(uint8_t*,uint64_t,uint64_t,uint64_t)>(context.base+0x57e2560)(ctx,screenContext,clientOfUi,third);
 for(int i=0;i<result.count;++i)RenderIcon(context,ui,ctx,context.actor,stacks[i],(request.x+5*s+(request.vertical?0:i*22*s))*ratioX,(request.y+5*s+(request.vertical?i*22*s:0))*ratioY,s*20*ratioX*.0625f,1.f);
 reinterpret_cast<void(*)(uint8_t*)>(context.base+0x57e29e0)(ctx);
}
inline void ArmorGuarded(void* view,void* ui){__try{ArmorWork(view,ui);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Armor().fault,1);}}

// ---- Item Counter: counts the items picked in the "Items" setting across the inventory and off hand, then has
// the game draw their icons with a 22-unit step.
// inventory: *(*(player+0x5e0)+0xb8); vtable +0xa0 size, +0x38 slot; off hand: slot 1 of the first
// ActorEquipmentComponent container; stack count: byte +0x22; item name: std::string at item+0xd8.
// A picked item missing from the inventory is still shown at 50% opacity, using a synthetic stack:
// item registry = weak_ptr returned by level->vtable[0xc40] (level = *(player+0x200)), hash table at registry+0x90
// (list: head at +8, std::string key at node+0x18, item = **(node+0x40)), 0x98-byte stack built by game function
// RVA 0x1acc180, vtable RVA 0xeffae80, initialised by RVA 0x1a7ca40 (stack, item, 1, 0).
// Names are compared without their namespace ("minecraft:"). Synthetic stacks are never destroyed (one per name).
struct CounterRequest{bool enabled,vertical;float x,y,scale;int count;char names[16][48];};
struct CounterResult{int count;int amount[16];};
struct CounterShared{SRWLOCK lock=SRWLOCK_INIT;CounterRequest request{};CounterResult result{};volatile long fault=0;volatile long frames=0;};
inline CounterShared& Counter(){static CounterShared* shared=new CounterShared;return *shared;}
inline const char* Bare(const char* name){const char* colon=strrchr(name,':');return colon?colon+1:name;}
inline uint64_t ItemRegistry(const Context& context){
 uint64_t level{},out[2]{};if(!Read(context.actor+0x200,&level)||!level)return 0;const uint64_t get=Virtual(context,level,0xc40);if(!get)return 0;
 reinterpret_cast<void*(*)(uint64_t,uint64_t*)>(get)(level,out);const uint64_t registry=out[0],control=out[1];
 if(control&&InterlockedDecrement((volatile long*)(control+0xc))==0){uint64_t table{},drop{};if(Read(control,&table)&&Read(table+8,&drop)&&InCode(context,drop))reinterpret_cast<void(*)(uint64_t)>(drop)(control);return 0;}
 return registry;
}
inline uint64_t DisplayStack(const Context& context,const char* name){
 struct Made{char name[48];uint64_t registry,stack;};static Made made[16]{};static int next=0;const uint64_t registry=ItemRegistry(context);if(!registry)return 0;
 for(auto& entry:made)if(entry.registry==registry&&!strcmp(entry.name,name))return entry.stack;
 uint64_t head{},node{},item=0;if(!Read(registry+0x98,&head)||!head||!Read(head,&node))return 0;const char* wanted=Bare(name);
 for(int visited=0;node&&node!=head&&visited<16384;++visited){char key[64]{};uint64_t holder{},found{};if(Text(node+0x18,key,sizeof(key))&&!strcmp(Bare(key),wanted)&&Read(node+0x40,&holder)&&holder&&Read(holder,&found)&&found){item=found;break;}if(!Read(node,&node))return 0;}
 Made& slot=made[next++%16];strncpy_s(slot.name,name,_TRUNCATE);slot.registry=registry;slot.stack=0;if(!item)return 0;
 uint8_t* stack=(uint8_t*)_aligned_malloc(0x98,16);if(!stack)return 0;memset(stack,0,0x98);
 reinterpret_cast<void*(*)(uint8_t*)>(context.base+0x1acc180)(stack);*(uint64_t*)stack=context.base+0xeffae80;memset(stack+0x80,0,0x18);
 reinterpret_cast<void(*)(uint8_t*,uint64_t,int,int)>(context.base+0x1a7ca40)(stack,item,1,0);slot.stack=(uint64_t)stack;return slot.stack;
}
// Item registry names (or the blocks they place) for the settings-panel list widget: full name
// ("minecraft:diamond_sword") = std::string at node+0x18; block = *(item+0x178), name at block+0xe8.
// One game call (registry), under SEH. -1: no world or failure.
inline int RegistryNamesWork(char(*out)[64],int capacity,bool blocks){
 Context context{};if(!Local(&context))return -1;const uint64_t registry=ItemRegistry(context);uint64_t head{},node{};if(!registry||!Read(registry+0x98,&head)||!head||!Read(head,&node))return -1;int count=0;
 for(int visited=0;node&&node!=head&&visited<16384&&count<capacity;++visited){uint64_t holder{},item{},block{};
  if(Read(node+0x40,&holder)&&holder&&Read(holder,&item)&&item){if(!blocks){if(Text(node+0x18,out[count],64)&&out[count][0])++count;}else if(Read(item+0x178,&block)&&block&&Text(block+0xe8,out[count],64)&&out[count][0])++count;}
  if(!Read(node,&node))break;}
 return count;
}
inline int ReadRegistryNames(char(*out)[64],int capacity,bool blocks){__try{return RegistryNamesWork(out,capacity,blocks);}__except(EXCEPTION_EXECUTE_HANDLER){return -1;}}
inline void CounterWork(void* ui){
 auto& shared=Counter();AcquireSRWLockShared(&shared.lock);const CounterRequest request=shared.request;ReleaseSRWLockShared(&shared.lock);if(!request.enabled||!request.count)return;
 if(strcmp(currentScreen,"hud_screen"))return;Context context{};if(!Local(&context))return;
 uint64_t stacks[16]{};int amount[16]{};
 auto tally=[&](uint64_t stack){uint8_t quantity{};char name[64]{};if(!ValidStack(stack)||!Read(stack+0x22,&quantity)||!quantity||!Text(ItemOf(stack)+0xd8,name,sizeof(name)))return;const char* bare=Bare(name);
  for(int i=0;i<request.count;++i)if(!strcmp(Bare(request.names[i]),bare)){if(!stacks[i])stacks[i]=stack;amount[i]+=quantity;return;}};
 uint64_t supplies{},inventory{};if(Read(context.actor+0x5e0,&supplies)&&supplies&&Read(supplies+0xb8,&inventory)&&inventory){const uint64_t get=Virtual(context,inventory,0x38),size=Virtual(context,inventory,0xa0);
  if(get&&size){const int slots=reinterpret_cast<int(*)(uint64_t)>(size)(inventory);for(int i=0;i<slots&&i<128;++i)tally(reinterpret_cast<uint64_t(*)(uint64_t,int)>(get)(inventory,i));}}
 {uintptr_t registry{},storage{},equipment{};uint32_t entity{};uint64_t hand{};if(ActorEntity(context.actor,&registry,&entity)&&Storage(registry,0xb06141a9u,&storage)&&InStorage(storage,entity,0x10,&equipment)&&Read(equipment,&hand)&&hand){
   const uint64_t get=Virtual(context,hand,0x38),size=Virtual(context,hand,0xa0);if(get&&size&&reinterpret_cast<int(*)(uint64_t)>(size)(hand)>1)tally(reinterpret_cast<uint64_t(*)(uint64_t,int)>(get)(hand,1));}}
 CounterResult result{};uint64_t shown[16]{};for(int i=0;i<request.count;++i){uint64_t stack=stacks[i];if(!stack)stack=DisplayStack(context,request.names[i]);if(!stack)continue;shown[result.count]=stack;result.amount[result.count++]=amount[i];}
 AcquireSRWLockExclusive(&shared.lock);shared.result=result;ReleaseSRWLockExclusive(&shared.lock);InterlockedIncrement(&shared.frames);if(!result.count)return;
 uint64_t gui{},game{},first{},second{},third{},screenContext{},clientOfUi{};float size[6];if(!Read(context.client+0x668,&gui)||!gui||!Floats(gui+0x40,size,6)||!(size[0]>0)||!(size[1]>0)||!(size[4]>0)||!(size[5]>0))return;
 if(!Read(context.base+0x128c0f90,&game)||!Read(game+8,&first)||!Read(first+0x20,&second)||!Read(second+0x48,&third)||!third||!Read((uintptr_t)ui+0x10,&screenContext)||!screenContext||!Read((uintptr_t)ui+8,&clientOfUi)||!clientOfUi)return;
 const float ratioX=size[4]/size[0],ratioY=size[5]/size[1],s=request.scale;alignas(16) uint8_t ctx[0x300];memset(ctx,0,sizeof(ctx));
 reinterpret_cast<void*(*)(uint8_t*,uint64_t,uint64_t,uint64_t)>(context.base+0x57e2560)(ctx,screenContext,clientOfUi,third);
 for(int i=0;i<result.count;++i)RenderIcon(context,ui,ctx,context.actor,shown[i],(request.x+5*s+(request.vertical?0:i*22*s))*ratioX,(request.y+5*s+(request.vertical?i*22*s:0))*ratioY,s*20*ratioX*.0625f,result.amount[i]>0?1.f:.5f);
 reinterpret_cast<void(*)(uint8_t*)>(context.base+0x57e29e0)(ctx);
}
// ---- Effect Display icons: drawn by the game during screen rendering, one "textures/ui/<name>"
// resource-pack image per effect:
// tex = ui->vtable[0x100](ui, &TexturePtr (0x20 bytes), &ResourceLocation, 0)
// ui->vtable[0x40](ui, first word of TexturePtr, &position, &size, &uv (0,0), &uv size (1,1), 0)
// ui->vtable[0x50](ui, &white, 1.0 (xmm2), &HashedString "ui_textured_and_glcolor_sprite")
// then both TexturePtr shared pointers are released (control blocks at +8 and +0x18). ResourceLocation (0x38 bytes):
// u32 file system 0, std::string path at +8, FNV-1a 64 of the path at +0x28, at +0x30
// (0xcbf29ce484222325 * 0x100000001b3) ^ hash. HashedString (0x30): hash, std::string, 0.
// Our std::string has the same (MSVC) layout and stays owned by us.
// Position and size are in UI units.
struct IconRequest{bool enabled;float x,y,scale;int count;uint8_t ids[16];};
struct IconShared{SRWLOCK lock=SRWLOCK_INIT;IconRequest request{};volatile long fault=0;volatile long frames=0;};
inline IconShared& Icons(){static IconShared* shared=new IconShared;return *shared;}
inline uint64_t Fnv64(const std::string& text){uint64_t hash=0xcbf29ce484222325ull;for(unsigned char byte:text)hash=(hash^byte)*0x100000001b3ull;return hash;}
inline void DropShared(const Context& context,uint64_t control){
 if(!control||InterlockedDecrement((volatile long*)(control+8)))return;uint64_t table{},destroy{},drop{};if(!Read(control,&table)||!Read(table,&destroy)||!Read(table+8,&drop)||!InCode(context,destroy)||!InCode(context,drop))return;
 reinterpret_cast<void(*)(uint64_t)>(destroy)(control);if(!InterlockedDecrement((volatile long*)(control+0xc)))reinterpret_cast<void(*)(uint64_t)>(drop)(control);
}
inline void IconWork(void* ui){
 static const char* files[31]={"","speed_effect","slowness_effect","haste_effect","mining_fatigue_effect","strength_effect","","","jump_boost_effect","nausea_effect","regeneration_effect","resistance_effect","fire_resistance_effect","water_breathing_effect","invisibility_effect","blindness_effect","night_vision_effect","hunger_effect","weakness_effect","poison_effect","wither_effect","health_boost_effect","absorption_effect","","levitation_effect","poison_effect","conduit_power_effect","slow_falling_effect","bad_omen_effect","village_hero_effect","darkness_effect"};
 auto& shared=Icons();AcquireSRWLockShared(&shared.lock);const IconRequest request=shared.request;ReleaseSRWLockShared(&shared.lock);if(!request.enabled||!request.count||strcmp(currentScreen,"hud_screen"))return;
 Context context{};uint64_t gui{};float size[6];if(!Client(&context)||!Read(context.client+0x668,&gui)||!gui||!Floats(gui+0x40,size,6)||!(size[0]>0)||!(size[1]>0)||!(size[4]>0)||!(size[5]>0))return;
 const uint64_t self=(uint64_t)ui,getTexture=Virtual(context,self,0x100),drawImage=Virtual(context,self,0x40),flushImages=Virtual(context,self,0x50);if(!getTexture||!drawImage||!flushImages)return;
 struct Location{uint32_t system;uint32_t pad;std::string path;uint64_t hash,full;};struct Hashed{uint64_t hash;std::string text;uint64_t last;};static_assert(sizeof(Location)==0x38&&sizeof(Hashed)==0x30,"layout");
 static Hashed* material=nullptr;if(!material){material=new Hashed{};material->text="ui_textured_and_glcolor_sprite";material->hash=Fnv64(material->text);}
 const float ratioX=size[4]/size[0],ratioY=size[5]/size[1],s=request.scale,white[4]={1,1,1,1},zero[2]={0,0},one[2]={1,1};
 for(int i=0;i<request.count;++i){const int id=request.ids[i];if(id<=0||id>=31||!files[id][0])continue;
  Location location{};location.path=std::string("textures/ui/")+files[id];location.hash=Fnv64(location.path);location.full=(0xcbf29ce484222325ull*0x100000001b3ull)^location.hash;uint64_t texture[4]{};
  reinterpret_cast<void*(*)(uint64_t,uint64_t*,Location*,int)>(getTexture)(self,texture,&location,0);
  if(texture[0]){const float position[2]={(request.x+6*s)*ratioX,(request.y+7*s+i*30*s)*ratioY},extent[2]={24*s*ratioX,24*s*ratioY};
   reinterpret_cast<void(*)(uint64_t,uint64_t,const float*,const float*,const float*,const float*,uint8_t)>(drawImage)(self,texture[0],position,extent,zero,one,0);
   reinterpret_cast<void(*)(uint64_t,const float*,float,Hashed*)>(flushImages)(self,white,1.f,material);}
  DropShared(context,texture[1]);DropShared(context,texture[3]);}
 InterlockedIncrement(&shared.frames);
}
inline void IconGuarded(void* ui){__try{IconWork(ui);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Icons().fault,1);}}
// ---- World -> screen projector, built in the same hook before the game call:
// ci = *(ui+8); lr = ci->vtable[0x5c8](ci); camera = ci->vtable[0x5c0](ci); VP = Proj(camera+0x440) x View(camera+0x3c0)
// (column-major); origin = *(lr+0x480)+0x658. Two virtual game calls per frame, under SEH, only while a world-drawing
// module is enabled. `scaleY` = Proj[1][1], used for line thickness.
struct Projector{SRWLOCK lock=SRWLOCK_INIT;float m[16]{};float origin[3]{};float scaleY=1;uint64_t tick=0;volatile long wanted=0,fault=0,seen=0;};
inline Projector& View(){static Projector* shared=new Projector;return *shared;}
inline void ProjectorWork(void* ui){
 auto& view=View();if(!view.wanted||strcmp(currentScreen,"hud_screen"))return;Context context{};uint64_t client{},holder{};if(!Client(&context)||!Read((uintptr_t)ui+8,&client)||!client)return;
 const uint64_t rendererOf=Virtual(context,client,0x5c8),cameraOf=Virtual(context,client,0x5c0);if(!rendererOf||!cameraOf)return;const uint64_t renderer=reinterpret_cast<uint64_t(*)(uint64_t)>(rendererOf)(client);if(!renderer||!Read(renderer+0x480,&holder)||!holder)return;
 const uint64_t camera=reinterpret_cast<uint64_t(*)(uint64_t)>(cameraOf)(client);float projection[16],look[16],origin[3],out[16];if(!camera||!Floats(camera+0x440,projection,16)||!Floats(camera+0x3c0,look,16)||!Floats(holder+0x658,origin,3))return;
 for(int c=0;c<4;++c)for(int row=0;row<4;++row){float sum=0;for(int k=0;k<4;++k)sum+=projection[k*4+row]*look[c*4+k];out[c*4+row]=sum;}
 AcquireSRWLockExclusive(&view.lock);memcpy(view.m,out,sizeof(out));memcpy(view.origin,origin,sizeof(origin));view.scaleY=std::fabs(projection[5]);view.tick=GetTickCount64();ReleaseSRWLockExclusive(&view.lock);InterlockedExchange(&view.seen,1);
}
inline void ProjectorGuarded(void* ui){__try{ProjectorWork(ui);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&View().fault,1);}}
inline void CounterGuarded(void* ui){__try{CounterWork(ui);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Counter().fault,1);}}
inline void(*volatile screenUtility)()=nullptr;
inline void(*volatile combatScreen)(const char*)=nullptr;   // Combat modules (game_combat.hpp), called on ScreenViewRender
inline void(*volatile inventoryScreen)()=nullptr;   // Inventory scheduler, Replenish and the Utility action queue (game_inventory.hpp, game_actions.hpp)
inline void* volatile screenViewCurrent=nullptr;   // ScreenView being rendered (render thread only; Auto Refill Close Inventory)
// Combat screen subscribers run inside the rotation stack type 2 (screen), so their requests are tagged with it and
// survive the next GameUpdate; the action planner runs before, with an empty stack (RotReadyFor needs it).
// Game screens other than the in-game HUD (chat, pause, inventory, disconnect...): hash of the name and the time it was
// last drawn, so the Nickel menu can close itself when the game opens one of them (menu.cpp, MenuFollowScreens).
// Several screens can be drawn every frame (a server sidebar, the death screen over the HUD...), so each name keeps its
// own slot. Written by the render thread only.
constexpr int kScreenSlots=12;
inline std::atomic<uint64_t> otherScreenHash[kScreenSlots]{},otherScreenAt[kScreenSlots]{};
inline void(*volatile screenNameLog)(const char*)=nullptr;
inline uint64_t ScreenNameHash(const char* s){uint64_t h=1469598103934665603ull;for(;*s;++s){h^=(uint8_t)*s;h*=1099511628211ull;}return h;}
inline void NoteOtherScreen(const char* name){const uint64_t h=ScreenNameHash(name),now=GetTickCount64();int slot=-1,oldest=0;
 for(int i=0;i<kScreenSlots;++i){if(otherScreenHash[i].load()==h){slot=i;break;}if(otherScreenAt[i].load()<otherScreenAt[oldest].load())oldest=i;}
 if(slot<0){slot=oldest;otherScreenAt[slot].store(0);otherScreenHash[slot].store(h);if(auto log=screenNameLog){char line[64];wsprintfA(line,"Screen: %s",name);log(line);}}
 otherScreenAt[slot].store(now);}
inline void ScreenDetour(void* view,void* ui){screenViewCurrent=view;ReadScreenName(view);if(currentScreen[0]&&strcmp(currentScreen,"hud_screen")&&strcmp(currentScreen,"hud_crosshair_screen")&&strcmp(currentScreen,"debug_screen")&&strcmp(currentScreen,"toast_screen"))NoteOtherScreen(currentScreen);if(currentScreen[0])ScreenPublish(currentScreen);if(strcmp(currentScreen,"debug_screen")&&strcmp(currentScreen,"toast_screen")){if(auto u=screenUtility)u();auto rp=rotationPush;if(rp)rp(2);if(auto cs=combatScreen)cs(currentScreen);if(rp)if(auto pop=rotationPop)pop();if(auto inv=inventoryScreen)inv();}if(!Armor().fault)ArmorGuarded(view,ui);if(!Counter().fault)CounterGuarded(ui);if(!Icons().fault)IconGuarded(ui);if(!View().fault)ProjectorGuarded(ui);if(auto work=visualFiltersScreenWork.load(std::memory_order_acquire))work(ui);if(auto work=namesScreenWork.load(std::memory_order_acquire))work(ui);if(auto work=xrayScreenWork.load(std::memory_order_acquire))work();screenOriginal(view,ui);currentScreen[0]=0;}
// 1: hook installed; -1: refused. Only tried once.
inline int InstallScreenHook(void(*log)(const char*)){
 static int state=0;if(state)return state;state=-1;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size)){if(log)log("Armor HUD: not the supported Minecraft image, no hook installed");return state;}
 const uint8_t expected[16]={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0xb8,0x38,0x12,0x00};for(int i=0;i<16;++i){uint8_t byte{};if(!Read(base+0x4cb3430+i,&byte)||byte!=expected[i]){if(log)log("Armor HUD: unexpected bytes at the screen render function, no hook installed");return state;}}
 if(MH_CreateHook((void*)(base+0x4cb3430),(void*)ScreenDetour,(void**)&screenOriginal)!=MH_OK||MH_EnableHook((void*)(base+0x4cb3430))!=MH_OK){if(log)log("Armor HUD: MinHook could not install the screen hook");return state;}
 if(log)log("Armor HUD: screen render hook installed");state=1;return state;
}
}
