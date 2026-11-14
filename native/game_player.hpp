// Player modules without a hook of their own.
// - Fast Use: hook on MC 0x65c0c40 (reader of the "no use before" field ClientInstance+0x328), result forced to 0.
// - Item Use Delay Fix: 6 "nop"s at MC 0x6586c08 (the "now + 200 ms" write after an attack); game bytes
// ff 15 92 93 41 09 restored on disable.
// - Auto Respawn (LevelTick): health <= 0 for Delay s -> RespawnPacket 0x2d (+0x3c = 2, +0x40 runtime id), retried
// every 1000 ms.
// - Fast Eat (LevelTick): ItemInUseComponent (0x10 bytes) holding food (vtable +0x98) -> remaining duration (+8)
// reduced by Speed (by 1 if smaller).
// - Anti AFK (LevelTick): every Delay s, a swing (type 4), a jump when on the ground and an alternating relative
// rotation of +/-25 degrees (mode 0, priority 0).
// - No Server Rotation (receive): MovePlayerPacket 0x13 with mode 2 targeting the player -> pitch / yaw (+0x44/+0x48)
// replaced by the current rotation.
// - Anti Debuff (receive): MobEffectPacket 0x1c adding / modifying (+0x3c in {1,2}) a selected effect (+0x40) ->
// packet cancelled (for every entity). Effects already active when enabled are not removed.
// - Auto Tool: ContinueDestroyBlock (hook #27) before the game call, once per break with progress >= Minimum, picks
// the fastest slot (Item Profiles "Custom Speed", else item->vtable[0x2c0]); LevelTick switches back when the
// player stops breaking (actor+0xb4a) and Switch Back is on.
#pragma once
#include "game_actions.hpp"
#include "game_rotation.hpp"
#include "game_hand.hpp"
#include "game_inventory.hpp"
#include <cctype>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace game {
enum{kRotAntiAfk=10};
struct PlayerProfile{std::string name;std::vector<std::string> items,blocks;float speed=8;};
struct PlayerShared{std::mutex guard;bool fastUse=false,respawn=false,fastEat=false,afk=false,noRotation=false,debuff=false,tool=false;
 float respawnDelay=.5f;int eatSpeed=4;float afkDelay=20;bool afkJump=true,afkSwing=true,afkRotate=false;std::set<int> effects;float toolMin=0;bool toolBack=true;std::vector<PlayerProfile> profiles;
 bool deathSeen=false,sent=false;int64_t deathAt=0,sentAt=0;int64_t afkAt=0;bool afkSense=false;int toolPrevious=-1;bool toolChosen=false;bool wasTool=false,wasAfk=false,wasRespawn=false;};
inline PlayerShared& Players(){static auto* s=new PlayerShared;return *s;}

// ---- Fast Use
using UseDelayFunction=uint64_t*(*)(void*,uint64_t*);inline UseDelayFunction useDelayOriginal=nullptr;
inline void(*volatile useDelayHook)(uint64_t*)=nullptr;   // Right Clicker (BlockBreakDelayEvent)
inline uint64_t* UseDelayDetour(void* self,uint64_t* out){uint64_t* r=useDelayOriginal(self,out);if(Players().fastUse&&out)*out=0;if(out)if(auto h=useDelayHook)h(out);return r;}
inline int InstallFastUseHook(void(*log)(const char*)){static int state=0;if(state)return state;const uint8_t bytes[10]={0x48,0x89,0xd0,0x48,0x8b,0x89,0x28,0x03,0x00,0x00};
 state=InstallOne(0x65c0c40,bytes,10,(void*)UseDelayDetour,(void**)&useDelayOriginal,"Fast Use",log);return state;}
// ---- Item Use Delay Fix
inline int PatchUseDelay(bool on,void(*log)(const char*)){static bool applied=false;if(on==applied)return 1;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size))return -1;
 uint8_t* at=(uint8_t*)(base+0x6586c08);const uint8_t call[6]={0xff,0x15,0x92,0x93,0x41,0x09},nops[6]={0x90,0x90,0x90,0x90,0x90,0x90};uint8_t now[6]{};for(int i=0;i<6;++i)if(!Read((uintptr_t)at+i,&now[i]))return -1;
 if(memcmp(now,on?call:nops,6)){if(log)log("Item Use Delay Fix: unexpected bytes at the patch site, nothing written");return -1;}
 DWORD old{};if(!VirtualProtect(at,6,PAGE_EXECUTE_READWRITE,&old))return -1;memcpy(at,on?nops:call,6);VirtualProtect(at,6,old,&old);FlushInstructionCache(GetCurrentProcess(),at,6);applied=on;
 if(log)log(on?"Item Use Delay Fix: patch applied":"Item Use Delay Fix: original bytes restored");return 1;}

// Current health (attribute 6, +0x7c); negative values are accepted (the test is <= 0).
inline bool PlayerHealth(const Context& c,float* health){uintptr_t element{};uint64_t begin{},end{},table{};
 if(!Element(c.registry,c.entity,"AttributesComponent",0x50,&element)||!Read(element,&begin)||!Read(element+8,&end)||!Read(element+0x18,&table)||end<begin||!table)return false;
 const uint64_t count=(end-begin)/4;if(!count||count>256)return false;for(uint64_t i=0;i<count;++i){uint32_t id{};if(!Read(begin+i*4,&id))return false;if(id==6)return Read(table+i*0x88+0x7c,health)&&std::isfinite(*health);}return false;}
// Stack speed against a block: Item Profiles (Custom Speed), then item->vtable[0x2c0].
inline std::string PlayerLower(std::string s){for(auto& ch:s)ch=(char)std::tolower((unsigned char)ch);return s;}
inline float PlayerSpeed(const Context& c,uint64_t stack,uint64_t block,const std::vector<PlayerProfile>& profiles){const uint64_t item=ActionItem(stack);if(!item)return 1.f;
 if(!profiles.empty()){std::string id;char text[160]{};if(Text(item+0xd8,text,sizeof(text)))id=text;std::string bare=id;const size_t colon=bare.rfind(':');if(colon!=std::string::npos)bare=bare.substr(colon+1);
  std::string blockName;XrayBlockName(block,&blockName);
  for(auto& p:profiles){bool match=p.items.empty()?(!p.name.empty()&&PlayerLower(bare).find(PlayerLower(p.name))!=std::string::npos):std::find(p.items.begin(),p.items.end(),id)!=p.items.end();if(!match)continue;
   if(p.blocks.empty()||std::find(p.blocks.begin(),p.blocks.end(),blockName)!=p.blocks.end())return p.speed;}}
 const uint64_t f=Virtual0(c,item,0x2c0);return f?reinterpret_cast<float(*)(uint64_t,uint64_t,uint64_t)>(f)(item,stack,block):1.f;}
inline int PlayerBestSlot(const Context& c,uintptr_t actor,uint64_t block,int current,const std::vector<PlayerProfile>& profiles){const uint64_t box=ActionContainer(actor);const int size=ActionSize(c,box),n=size<9?size:9;
 if(current<0||current>=n)return -1;int best=current;float score=PlayerSpeed(c,ActionStackAt(c,box,current),block,profiles);
 for(int i=0;i<n;++i){const float s=PlayerSpeed(c,ActionStackAt(c,box,i),block,profiles);if(s>score){score=s;best=i;}}return best;}
inline void AutoToolStop(PlayerShared& s,const Context* c){const int p=s.toolPrevious;s.toolPrevious=-1;s.toolChosen=false;if(p>=0&&s.toolBack&&c)if(int32_t* sel=ActionSelected(c->actor))if(*sel!=p)*sel=p;}

// ---- level tick
inline void PlayerTickWork(){auto& s=Players();std::lock_guard<std::mutex> l(s.guard);Context c{};const bool have=Local(&c);
 // Auto Respawn
 if(s.respawn!=s.wasRespawn){s.deathSeen=false;s.sent=false;s.wasRespawn=s.respawn;}
 if(s.respawn){float h=1;if(!have||!PlayerHealth(c,&h)||h>0){s.deathSeen=false;s.sent=false;}
  else{if(!s.deathSeen){s.deathAt=ActionNs();s.deathSeen=true;}uint64_t id{};
   if(ActionMs(s.deathAt)>s.respawnDelay*1000.0&&(!s.sent||ActionMs(s.sentAt)>1000.0)&&RuntimeId(c.actor,&id)){GamePacket p=CreatePacket(c,0x2d);
    if(p.object){*(uint64_t*)(p.object+0x30)=0;*(int32_t*)(p.object+0x38)=0;*(uint8_t*)(p.object+0x3c)=2;*(uint64_t*)(p.object+0x40)=id;SendPacket(c,p.object);s.sent=true;s.sentAt=ActionNs();}ReleasePacket(p);}}}
 if(!have)return;
 // Fast Eat
 if(s.fastEat){uintptr_t comp{};uint64_t p{},item{};if(Element(c.registry,c.entity,"ItemInUseComponent",0x10,&comp)&&Read(comp,&p)&&p&&Read(p,&item)&&item){const uint64_t f=Virtual0(c,item,0x98);
   if(f&&reinterpret_cast<bool(*)(uint64_t)>(f)(item)){int32_t* left=(int32_t*)(comp+8);int32_t v{};if(Read((uintptr_t)left,&v)){if(v>=s.eatSpeed)*left=v-s.eatSpeed;else if(v>=1)*left=v-1;}}}}
 // Anti AFK
 if(s.afk!=s.wasAfk){s.afkAt=ActionNs();s.afkSense=false;s.wasAfk=s.afk;}
 if(s.afk&&ActionMs(s.afkAt)>s.afkDelay*1000.0){s.afkAt=ActionNs();if(s.afkSwing)ActionSwing(c,c.actor,4);if(s.afkJump)JumpRequest(c.actor);
  if(s.afkRotate){RotRequest r;r.module=kRotAntiAfk;r.yaw=s.afkSense?25.f:-25.f;r.pitch=0;r.relative=true;r.mode=0;r.priority=0;RotSubmit(std::move(r));s.afkSense=!s.afkSense;}}
 // Auto Tool: switch back once the player stops breaking
 if(s.wasTool&&!s.tool){AutoToolStop(s,&c);}s.wasTool=s.tool;
 if(s.tool&&s.toolChosen){uint8_t breaking{};Read(c.actor+0xb4a,&breaking);if(!breaking)AutoToolStop(s,&c);}}
inline void PlayerTick(){static volatile long stopped_=0;if(!game::FeatureStopped(&stopped_))__try{PlayerTickWork();}__except(game::FeatureFault("PlayerTick",&stopped_,GetExceptionInformation())){game::FeatureFlush();}}
// ---- ContinueDestroyBlock (before the game call)
inline void AutoToolDestroyWork(uint64_t gm,const int32_t* pos){auto& s=Players();std::lock_guard<std::mutex> l(s.guard);if(!s.tool||s.toolChosen)return;float progress{};if(!Read(gm+0x24,&progress)||progress<s.toolMin)return;
 uint64_t player{};if(!Read(gm+8,&player)||!player)return;Context c{};if(!Client(&c))return;c.actor=player;const uint64_t region=ActorRegion(c);if(!region)return;int32_t* sel=ActionSelected(player);if(!sel)return;
 const int cur=*sel;const uint64_t block=RegionBlock(c,region,pos);const int best=PlayerBestSlot(c,player,block,cur,s.profiles);s.toolChosen=true;if(best>=0&&best!=cur){s.toolPrevious=cur;*sel=best;}}
inline void AutoToolDestroy(uint64_t gm,const int32_t* pos){static volatile long stopped_=0;if(!game::FeatureStopped(&stopped_))__try{AutoToolDestroyWork(gm,pos);}__except(game::FeatureFault("AutoToolDestroy",&stopped_,GetExceptionInformation())){game::FeatureFlush();}}
// ---- receive
inline bool PlayerReceiveWork(uintptr_t packet){auto& s=Players();bool noRot,debuff;std::set<int> effects;{std::lock_guard<std::mutex> l(s.guard);noRot=s.noRotation;debuff=s.debuff;if(debuff)effects=s.effects;}if(!noRot&&!debuff)return false;
 const int id=PacketIdOf(packet);
 if(id==0x13&&noRot){uint8_t mode{};uint64_t who{},mine{};Context c{};if(Read(packet+0x50,&mode)&&mode==2&&Local(&c)&&RuntimeId(c.actor,&mine)&&Read(packet+0x30,&who)&&who==mine){float p{},y{};if(RotRead(c.actor,&p,&y)){*(float*)(packet+0x44)=p;*(float*)(packet+0x48)=y;}}return false;}
 if(id==0x1c&&debuff){uint8_t ev{};int32_t effect{};if(Read(packet+0x3c,&ev)&&(ev==1||ev==2)&&Read(packet+0x40,&effect)&&effects.count(effect))return true;}
 return false;}
inline bool PlayerReceive(uintptr_t packet){__try{return PlayerReceiveWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
// ---- Auto Totem and Auto Armor (render frame); items moved through ItemStackRequest
struct InvModules{std::mutex guard;bool totem=false,spoof=true;int totemDelay=50;bool armor=false;int armorMode=0;float armorLow=50,armorHigh=100;
 double totemWait=0,armorWait=0;int64_t totemAt=0,armorAt=0;bool totemArmed=false,armorArmed=false,wasTotem=false,wasArmor=false;};
inline InvModules& InvMods(){static auto* m=new InvModules;return *m;}
inline bool InvNameIs(uint64_t stack,const char* want){std::string n;return ActionName(stack,&n)&&n==want;}
inline uint64_t EquipmentContainer(const Context& c,uintptr_t actor,int which){uintptr_t registry{},equipment{};uint32_t entity{};uint64_t box{};
 return ActorEntity(actor,&registry,&entity)&&Element(registry,entity,"ActorEquipmentComponent",0x10,&equipment)&&Read(equipment+which*8,&box)?box:0;}
inline bool TotemHeld(const Context& c,uintptr_t actor){const uint64_t hand=EquipmentContainer(c,actor,0);if(!hand||ActionSize(c,hand)<=1)return false;return InvNameIs(ActionStackAt(c,hand,1),"totem_of_undying");}
inline int TotemFind(const Context& c,uintptr_t actor){const uint64_t box=ActionContainer(actor);for(int i=ActionSize(c,box)-1;i>=0;--i)if(InvNameIs(ActionStackAt(c,box,i),"totem_of_undying"))return i;return -1;}
inline int TotemMove(const Context& c,uintptr_t actor){if(TotemHeld(c,actor))return 5;const int slot=TotemFind(c,actor);if(slot<0)return 5;uint8_t count{};Read(ActionStackAt(c,ActionContainer(actor),slot)+0x22,&count);return InvMoveStack(c,actor,0xc,slot,0x22,0,count);}
inline int ArmorSlotOf(const std::string& n){auto ends=[&](const char* e){const size_t l=strlen(e);return n.size()>l&&!n.compare(n.size()-l,l,e);};
 if(ends("_helmet")||ends("_cap"))return 0;if(ends("_chestplate")||ends("_tunic"))return 1;if(ends("_leggings"))return 2;if(ends("_boots"))return 3;return -1;}
inline float ArmorScore(const Context& c,uint64_t stack){const uint64_t item=ActionItem(stack);if(!item)return 0;const uint64_t f=Virtual0(c,item,0x1d0);float v=f?(float)reinterpret_cast<int(*)(uint64_t)>(f)(item):0;
 auto lvl=[&](int id){const int l=EnchantLevel(c,stack,(int16_t)id);return l>0?(float)l:0.f;};return v+lvl(0)+.5f*(lvl(1)+lvl(3)+lvl(4)+lvl(5));}
inline int ArmorDurability(const Context& c,uint64_t stack){const uint64_t item=ActionItem(stack);if(!item)return 0;const uint64_t top=Virtual0(c,item,0x120);if(!top)return 0;const int maximum=reinterpret_cast<short(*)(uint64_t)>(top)(item);
 uint64_t data{};const int damage=Read(stack+0x10,&data)&&data?NbtInt(data,"Damage"):0;return std::max(0,maximum-damage);}
inline bool ArmorChoose(const Context& c,uintptr_t actor,int* slot,int* piece){const uint64_t armor=EquipmentContainer(c,actor,1);if(!armor)return false;const int pieces=ActionSize(c,armor);
 const uint64_t box=ActionContainer(actor);int32_t* sel=ActionSelected(actor);const int n=ActionSize(c,box);float bestGain=-1;int bestDur=-1;bool found=false;
 for(int i=0;i<n;++i){if(sel&&i==*sel)continue;const uint64_t st=ActionStackAt(c,box,i);std::string name;if(!ActionName(st,&name))continue;const int a=ArmorSlotOf(name);if(a<0||a>=pieces)continue;
  const float sc=ArmorScore(c,st);const int du=ArmorDurability(c,st);const uint64_t worn=ActionStackAt(c,armor,a);const float ws=ActionItem(worn)?ArmorScore(c,worn):0;const int wd=ActionItem(worn)?ArmorDurability(c,worn):0;
  if(!(ws<=sc&&(ws!=sc||wd<du)))continue;const float gain=sc-ws;if(!found||gain>bestGain||(gain==bestGain&&du>bestDur)){found=true;bestGain=gain;bestDur=du;*slot=i;*piece=a;}}
 return found;}
inline int ArmorMove(const Context& c,uintptr_t actor){if(ActionCursorBusy(c,actor))return 2;int slot{},piece{};if(!ArmorChoose(c,actor,&slot,&piece))return 5;uint8_t count{};Read(ActionStackAt(c,ActionContainer(actor),slot)+0x22,&count);
 return InvMoveStack(c,actor,0xc,slot,6,piece,count);}
inline void InvModulesFrameWork(){auto& m=InvMods();Context c{};bool totem,spoof,armor;int mode;{std::lock_guard<std::mutex> l(m.guard);totem=m.totem;spoof=m.spoof;armor=m.armor;mode=m.armorMode;
  if(totem&&!m.wasTotem){m.totemWait=0;m.totemAt=ActionNs();m.totemArmed=true;}m.wasTotem=totem;if(armor&&!m.wasArmor){m.armorWait=0;m.armorAt=ActionNs();m.armorArmed=true;}m.wasArmor=armor;}
 if(!Local(&c))return;
 if(totem){bool due;{std::lock_guard<std::mutex> l(m.guard);due=m.totemArmed&&m.totemWait<ActionMs(m.totemAt);}
  if(due&&!TotemHeld(c,c.actor)){if(!spoof){InvCancel(2);if(TotemMove(c,c.actor)==0){std::lock_guard<std::mutex> l(m.guard);m.totemWait=m.totemDelay;m.totemAt=ActionNs();m.totemArmed=true;}}
   else if(TotemFind(c,c.actor)>=0)InvSubmit(2,[](uintptr_t actor){auto& mm=InvMods();{std::lock_guard<std::mutex> l(mm.guard);if(!mm.totem||!mm.spoof)return;}Context k{};if(!Local(&k)||k.actor!=actor)return;
     if(TotemMove(k,actor)==0){std::lock_guard<std::mutex> l(mm.guard);mm.totemWait=mm.totemDelay;mm.totemAt=ActionNs();mm.totemArmed=true;}});}}
 if(armor){if(mode==0&&!InvScreenOpen())return;bool due;{std::lock_guard<std::mutex> l(m.guard);due=m.armorArmed&&m.armorWait<ActionMs(m.armorAt);}if(!due)return;
  auto done=[](){auto& mm=InvMods();std::lock_guard<std::mutex> l(mm.guard);mm.armorWait=ActionRandom(mm.armorLow,mm.armorHigh);mm.armorAt=ActionNs();mm.armorArmed=true;};
  if(mode==0){InvCancel(3);if(ArmorMove(c,c.actor)==0)done();}
  else if(!ActionCursorBusy(c,c.actor)){int s{},p{};if(ArmorChoose(c,c.actor,&s,&p))InvSubmit(3,[done](uintptr_t actor){auto& mm=InvMods();{std::lock_guard<std::mutex> l(mm.guard);if(!mm.armor||mm.armorMode!=1)return;}Context k{};if(!Local(&k)||k.actor!=actor)return;if(ArmorMove(k,actor)==0)done();});}}}
inline void InvModulesFrame(){static volatile long stopped_=0;if(!game::FeatureStopped(&stopped_))__try{InvModulesFrameWork();}__except(game::FeatureFault("InvModulesFrame",&stopped_,GetExceptionInformation())){game::FeatureFlush();}}
}
