// Extended script API, included by script_engine.hpp just before the API is registered (same anonymous namespace
// as menu.cpp):
// - mod:rotate(yaw, pitch, {mode, priority, smooth}) / mod:aimAt(target, options) through the rotation manager
// (modes camera 0, visible 1, silent 2, packet 3; priorities ambient 0, aim 1, combat 2, action 3, view 4;
// default silent / aim);
// - world.raycast(from, to, liquids) (BlockSource::clip, region->vtable[0x198]), world.findBlocks(name, radius
// 1..12, at most 256), world.placeBlock(pos) (range 5, placed like Auto Block Reset, arm swing);
// - mod:useItem({slot, ghost, disable, waitForAim, aimTolerance, switchDelay, restoreDelay, done}) via the action queue;
// - player.useItemOn(pos, face) (arm 3, block transaction, click point near the face centre with slight jitter),
// player.releaseItem() (gamemode->vtable[0x80]);
// - inventory: item, size, held, offhand, armor, find, hotbarFind, swap, equip, toOffhand (direct ItemStackRequest), drop
// (Inventory::dropSlot: the whole stack, or one item at a time for a smaller count);
// - container (the chest-like window shown, as Chest Stealer: model *(player+0x5c8), enum 7 / 0x3a / 0x1e): isOpen,
// type, size, item, items, take / put (whole stacks, to the first free slot: inventory 9.. then 0..8, or the first
// empty container slot), close (ContainerClosePacket);
// - player.simulate (game_simulate.hpp), projectile.trace / projectile.aim (game_trajectory.hpp, the ballistic solver
// of Pearl Chaser), path.* and the pathEnd event (Path Finder, game_movement_e.hpp);
// - events jump (cancel, boost), hurt / death / respawn (from health tracking), teleport (MovePlayer 0x13 mode 2,
// pearl thrown within 5 s), move (SprintIntent: forward, strafe, jump, sneak, sprint rewritten).
// - world.block(pos) {name, runtimeId, isAir, solid, liquid (BlockLegacy byte +0x29 bits 2 / 1), position, states (the
// block's "states" compound as a table)}, world.isLoaded(pos) (chunk present, height inside the world);
// - player.attack(id) (swing, attack transaction), player.interact(id) (swing, game mode vtable 0x70 at the entity's
// position), startBreak(pos, face) / continueBreak(pos, face) (game mode vtable 8 / 0x18, both return the result and
// the "done" flag) and stopBreak(pos) (vtable 0x20);
// Differences: take / put move whole stacks only (a smaller count answers InvalidAmount); container.type names follow
// the protocol's container types; player.simulate is Nickel's own model of the player physics (see game_simulate.hpp).
int scriptRotId(const ScriptModule& m){return 16+(m.index&15);}   // rotation manager module id (16..31)
bool ScriptRotOptions(lua_State* L,int idx,game::RotRequest& r){if(!lua_istable(L,idx))return true;
 lua_rawgetfield(L,idx,"mode");if(lua_isstring(L,-1)){std::string v=lua_tostring(L,-1);std::transform(v.begin(),v.end(),v.begin(),[](unsigned char c){return (char)std::tolower(c);});
  if(v=="camera")r.mode=0;else if(v=="visible")r.mode=1;else if(v=="silent")r.mode=2;else if(v=="packet")r.mode=3;else{lua_pop(L,1);luaL_error(L,"unknown value \"%s\"",v.c_str());}}lua_pop(L,1);
 lua_rawgetfield(L,idx,"priority");if(lua_isstring(L,-1)){std::string v=lua_tostring(L,-1);std::transform(v.begin(),v.end(),v.begin(),[](unsigned char c){return (char)std::tolower(c);});
  if(v=="ambient")r.priority=0;else if(v=="aim")r.priority=1;else if(v=="combat")r.priority=2;else if(v=="action")r.priority=3;else if(v=="view")r.priority=4;else{lua_pop(L,1);luaL_error(L,"unknown value \"%s\"",v.c_str());}}lua_pop(L,1);
 lua_rawgetfield(L,idx,"smooth");if(lua_isnumber(L,-1))r.speed=(float)lua_tonumber(L,-1);lua_pop(L,1);return true;}
bool scriptWantsRotations=false;
void ScriptSubmitRotation(const ScriptModule& m,float yaw,float pitch,lua_State* L,int optionsIdx){game::RotRequest r;r.module=scriptRotId(m);r.yaw=yaw;r.pitch=std::clamp(pitch,-90.f,90.f);r.mode=2;r.priority=1;
 ScriptRotOptions(L,optionsIdx,r);const int index=m.index;r.predicate=[index](uintptr_t){return index>=0&&modules[index].enabled;};game::Rotations().enabled[r.module]=true;scriptWantsRotations=true;game::RotSubmit(std::move(r));}
int ModRotate21(lua_State* L){Script* s=ScriptOf(L);if(s->loading)luaL_error(L,"modules can rotate once the script has loaded");ScriptModule& m=ModuleOf(L,1);ScriptSubmitRotation(m,(float)luaL_checknumber(L,2),(float)luaL_checknumber(L,3),L,4);return 0;}
int ModAimAt21(lua_State* L){ScriptModule& m=ModuleOf(L,1);float target[3],eye[3];if(!LocalEye(eye)){lua_pushboolean(L,0);return 1;}
 if(lua_isnumber(L,2)){game::Context c{};uintptr_t actor{};uint64_t bb{};float box[6]{};if(!game::Local(&c)||!game::ActorByRuntimeId(c.registry,(uint64_t)lua_tonumber(L,2),&actor)||!game::Read(actor+0x248,&bb)||!bb||!game::Floats(bb,box,6)){lua_pushboolean(L,0);return 1;}
  float predict=0;if(lua_istable(L,3)){lua_rawgetfield(L,3,"predict");if(lua_isnumber(L,-1))predict=std::clamp((float)lua_tonumber(L,-1),0.f,1.f);lua_pop(L,1);}
  if(predict>0){uint64_t st{};float v[3]{};if(game::Read(actor+0x240,&st)&&st&&game::Floats(st+0x18,v,3))for(int i=0;i<3;++i){box[i]+=v[i]*predict;box[i+3]+=v[i]*predict;}}
  for(int i=0;i<3;++i)target[i]=std::clamp(eye[i],box[i],box[i+3]);target[1]=std::clamp(eye[1],box[1]+(box[4]-box[1])*.2f,box[4]-(box[4]-box[1])*.1f);}
 else if(!ReadVector(L,2,target)){lua_pushboolean(L,0);return 1;}
 const float dx=target[0]-eye[0],dy=target[1]-eye[1],dz=target[2]-eye[2];ScriptSubmitRotation(m,-std::atan2(dx,dz)*57.2958f,-std::atan2(dy,std::sqrt(dx*dx+dz*dz))*57.2958f,L,3);lua_pushboolean(L,1);return 1;}
// ---- world
const char* scriptFaces[6]={"Down","Up","North","South","West","East"};
int WorldRaycast(lua_State* L){game::Context c{};if(!game::Local(&c))return 0;const uint64_t region=game::ActorRegion(c);if(!region)luaL_error(L,"the world is not available");float from[3],to[3];
 if(!ReadVector(L,1,from))luaL_error(L,"expected a vector");if(!ReadVector(L,2,to))luaL_error(L,"expected a vector");const bool liquids=lua_toboolean(L,3)!=0;
 const uint64_t clipOf=game::Virtual0(c,region,0x198);if(!clipOf){lua_pushnil(L);return 1;}using Clip=void*(*)(uint64_t,void*,const float*,const float*,bool,bool,int,bool,bool,uint64_t,const std::function<bool()>*,bool);
 static const std::function<bool()> accept=[]{return true;};const float span=std::sqrt((to[0]-from[0])*(to[0]-from[0])+(to[1]-from[1])*(to[1]-from[1])+(to[2]-from[2])*(to[2]-from[2]));alignas(16) uint8_t res[0x200]{};
 reinterpret_cast<Clip>(clipOf)(region,res,from,to,liquids,true,(int)span+1,false,false,c.actor,&accept,false);
 uint64_t weak{};memcpy(&weak,res+0x40,8);if(weak&&InterlockedDecrement((volatile long*)(weak+0xc))==0){uint64_t t{},rel{};if(game::Read(weak,&t)&&game::Read(t+8,&rel)&&rel)reinterpret_cast<void(*)(uint64_t)>(rel)(weak);}
 int type{};memcpy(&type,res+0x18,4);float hit[3];memcpy(hit,res+0x2c,12);lua_createtable(L,0,5);
 if(type==0){int b[3];memcpy(b,res+0x20,12);uint8_t face=res[0x1c];lua_pushstring(L,"block");lua_setfield(L,-2,"type");const float bv[3]={(float)b[0],(float)b[1],(float)b[2]};PushVector(L,bv);lua_setfield(L,-2,"block");
  lua_pushstring(L,face<6?scriptFaces[face]:"Up");lua_setfield(L,-2,"face");PushVector(L,hit);lua_setfield(L,-2,"position");}
 else if(type==1||type==2){lua_pushstring(L,"entity");lua_setfield(L,-2,"type");PushVector(L,hit);lua_setfield(L,-2,"position");}
 else if(type==3){lua_pushstring(L,"none");lua_setfield(L,-2,"type");}else{PushVector(L,hit);lua_setfield(L,-2,"position");}return 1;}
int WorldFindBlocks(lua_State* L){game::Context c{};if(!game::Local(&c))return 0;const uint64_t region=game::ActorRegion(c);if(!region)luaL_error(L,"the world is not available");
 std::string want=luaL_checkstring(L,1);std::transform(want.begin(),want.end(),want.begin(),[](unsigned char ch){return (char)std::tolower(ch);});const int r=std::clamp((int)luaL_optinteger(L,2,8),1,12);
 uint64_t st{};float p[3]{},lift=0;if(!game::Read(c.actor+0x240,&st)||!st||!game::Floats(st,p,3)){lua_newtable(L);return 1;}uintptr_t off{};if(game::Element(c.registry,c.entity,"OffsetsComponent",0x44,&off))game::Floats(off,&lift,1);
 const int fx=(int)std::floor(p[0]),fy=(int)std::floor(p[1]-lift),fz=(int)std::floor(p[2]);int16_t minY{},maxY{};game::Read(region+0x3a,&minY);game::Read(region+0x38,&maxY);
 lua_newtable(L);int n=0;std::unordered_map<uint64_t,bool> cache;
 for(int dx=-r;dx<=r&&n<256;++dx)for(int dy=-r;dy<=r&&n<256;++dy){const int y=fy+dy;if(y<minY||y>=maxY)continue;for(int dz=-r;dz<=r&&n<256;++dz){const int32_t pos[3]={fx+dx,y,fz+dz};const uint64_t b=game::RegionBlock(c,region,pos);if(!b)continue;
   auto f=cache.find(b);bool hit;if(f!=cache.end())hit=f->second;else{uint64_t legacy{};char full[96]{},bare[96]{};hit=false;if(game::Read(b+0x68,&legacy)&&legacy){game::Text(legacy+0xe8,full,sizeof(full));game::Text(legacy+0x98,bare,sizeof(bare));
     std::string a=full,bb=bare;std::transform(a.begin(),a.end(),a.begin(),[](unsigned char ch){return (char)std::tolower(ch);});std::transform(bb.begin(),bb.end(),bb.begin(),[](unsigned char ch){return (char)std::tolower(ch);});hit=a==want||bb==want;}cache[b]=hit;}
   if(hit){const float v[3]={(float)pos[0],(float)pos[1],(float)pos[2]};PushVector(L,v);lua_rawseti(L,-2,++n);}}}
 return 1;}
int WorldPlaceBlock(lua_State* L){game::Context c{};if(!game::Local(&c))return 0;float v[3];if(!ReadVector(L,1,v))luaL_error(L,"expected a vector");const int32_t pos[3]={(int32_t)std::floor(v[0]),(int32_t)std::floor(v[1]),(int32_t)std::floor(v[2])};
 game::AbrPlace place{};if(!game::AbrFind(c,c.actor,pos,5.f,&place)){lua_pushboolean(L,0);return 1;}game::ActionSwing(c,c.actor,1);lua_pushboolean(L,game::AbrPlaceBlock(c,c.actor,place,5.f));return 1;}
// ---- items
int ModUseItem21(lua_State* L){Script* s=ScriptOf(L);if(s->loading)luaL_error(L,"modules can use items once the script has loaded");ScriptModule& m=ModuleOf(L,1);int slot=-1;bool ghost=false,disable=false,wait=false;double tol=6,sw=0,rs=0;int doneRef=0;
 if(lua_istable(L,2)){lua_rawgetfield(L,2,"slot");if(lua_isnumber(L,-1)){slot=(int)lua_tonumber(L,-1);if(slot<1||slot>9){lua_pop(L,1);luaL_error(L,"hotbar slots are numbered 1 to 9");}}lua_pop(L,1);
  ghost=OptBool(L,2,"ghost",false);disable=OptBool(L,2,"disable",false);wait=OptBool(L,2,"waitForAim",false);
  auto num=[&](const char* k,double d,double lo,double hi){lua_rawgetfield(L,2,k);double v=lua_isnumber(L,-1)?lua_tonumber(L,-1):d;lua_pop(L,1);return std::clamp(v,lo,hi);};tol=num("aimTolerance",6,.01,180);sw=num("switchDelay",0,0,5000);rs=num("restoreDelay",0,0,5000);
  lua_rawgetfield(L,2,"done");if(lua_isfunction(L,-1))doneRef=lua_ref(L,-1);lua_pop(L,1);}
 const int rot=scriptRotId(m);const int index=m.index;game::ActionSequence q;q.module=100+(index&0xff);q.silent=ghost?1:0;
 if(slot>0){const int sl=slot-1;q.steps.push_back([sl](game::ActionCtx& x){game::ActionSwitch(x,sl);return 1;});if(sw>0)q.steps.push_back(game::StepWait(sw));}
 if(wait){auto start=std::make_shared<int64_t>(0);q.steps.push_back([rot,tol,start](game::ActionCtx&){if(!*start)*start=game::ActionNs();if(game::RotReadyFor(rot,(float)tol))return 1;return game::ActionMs(*start)>400.0?2:0;});}
 q.steps.push_back(game::StepUse());if(slot>0){if(rs>0)q.steps.push_back(game::StepWait(rs));q.steps.push_back(game::StepSwitchBack());}
 Script* owner=s;q.done=[owner,doneRef,disable,index](int result){std::lock_guard<std::recursive_mutex> lock(scriptGuard);if(disable&&index>=0&&modules[index].enabled){modules[index].enabled=false;Save(modules[index]);}
  if(doneRef){bool alive=false;for(auto& sp:scripts)if(sp.get()==owner&&owner->state==1&&owner->L)alive=true;if(alive){lua_State* Ls=owner->L;lua_getref(Ls,doneRef);lua_pushboolean(Ls,result==0);ScriptCall(*owner,Ls,1,0);lua_unref(Ls,doneRef);}}};
 game::ActionSubmit(std::move(q));return 0;}
int PlayerUseItemOn(lua_State* L){game::Context c{};if(!game::Local(&c))return 0;float v[3];if(!ReadVector(L,1,v))luaL_error(L,"expected a vector");int face=1;
 if(lua_isstring(L,2)){std::string f=lua_tostring(L,2);for(int i=0;i<6;++i)if(_stricmp(f.c_str(),scriptFaces[i])==0)face=i;}
 const int32_t pos[3]={(int32_t)std::floor(v[0]),(int32_t)std::floor(v[1]),(int32_t)std::floor(v[2])};int32_t* sel=game::ActionSelected(c.actor);if(!sel)return 0;const uint64_t stack=game::ActionHeld(c,c.actor);float me[3]{};if(!game::ActionPosition(c.actor,me))return 0;
 game::ActionSwing(c,c.actor,3);static const float n[6][3]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};const float jitter=(float)(rand()%1000)/1000.f*.1f-.05f;float rel[3];for(int k=0;k<3;++k)rel[k]=.5f+.5f*n[face][k]+(n[face][k]==0?jitter:0);
 const uint64_t region=game::ActorRegion(c);int32_t runtime{};if(region)if(const uint64_t b=game::RegionBlock(c,region,pos))game::Read(b+0x114,&runtime);
 uint8_t* t=game::NewUseTransaction(c,stack);*(int32_t*)(t+0x68)=0;memcpy(t+0x70,pos,12);*(int32_t*)(t+0x7c)=runtime;*(int32_t*)(t+0x84)=*sel;*(uint8_t*)(t+0x88)=0;*(uint8_t*)(t+0x80)=(uint8_t)face;memcpy(t+0xf0,me,12);memcpy(t+0xfc,rel,12);game::SendUseTransaction(c,c.actor,t);return 0;}
int PlayerReleaseItem(lua_State* L){(void)L;game::Context c{};uint64_t gm{};if(!game::Local(&c)||!game::Read(c.actor+0xac8,&gm)||!gm)return 0;const uint64_t f=game::Virtual0(c,gm,0x80);if(f)reinterpret_cast<void(*)(uint64_t)>(f)(gm);return 0;}
// ---- inventory
void PushItem(lua_State* L,const game::Context& c,uint64_t stack){std::string id;if(!stack||!game::ActionItem(stack)||!game::ActionName(stack,&id)){lua_pushnil(L);return;}uint8_t count{};game::Read(stack+0x22,&count);lua_createtable(L,0,5);
 std::string full=id.find(':')==std::string::npos?"minecraft:"+id:id;lua_pushstring(L,full.c_str());lua_setfield(L,-2,"id");lua_pushstring(L,id.c_str());lua_setfield(L,-2,"name");lua_pushnumber(L,count);lua_setfield(L,-2,"count");
 const uint64_t item=game::ActionItem(stack);const uint64_t top=game::Virtual0(c,item,0x120);if(top){const int maximum=reinterpret_cast<short(*)(uint64_t)>(top)(item);if(maximum>0){uint64_t data{};const int damage=game::Read(stack+0x10,&data)&&data?game::NbtInt(data,"Damage"):0;
   lua_pushnumber(L,std::max(0,maximum-damage));lua_setfield(L,-2,"durability");lua_pushnumber(L,maximum);lua_setfield(L,-2,"maxDurability");}}}
bool InvContext(lua_State* L,game::Context* c,uint64_t* box){if(!game::Local(c)||!(*box=game::ActionContainer(c->actor)))luaL_error(L,"the inventory is not available");return true;}
int InvItem(lua_State* L){game::Context c{};uint64_t box{};InvContext(L,&c,&box);const int n=game::ActionSize(c,box);const int slot=(int)luaL_checkinteger(L,1);if(slot<1||slot>n)luaL_error(L,"slots are numbered 1 to %d",n);PushItem(L,c,game::ActionStackAt(c,box,slot-1));return 1;}
int InvSize(lua_State* L){game::Context c{};uint64_t box{};InvContext(L,&c,&box);lua_pushnumber(L,game::ActionSize(c,box));return 1;}
int InvHeld(lua_State* L){game::Context c{};uint64_t box{};InvContext(L,&c,&box);PushItem(L,c,game::ActionHeld(c,c.actor));return 1;}
int InvOffhand(lua_State* L){game::Context c{};uint64_t box{};InvContext(L,&c,&box);const uint64_t hand=game::EquipmentContainer(c,c.actor,0);if(!hand||game::ActionSize(c,hand)<=1){lua_pushnil(L);return 1;}PushItem(L,c,game::ActionStackAt(c,hand,1));return 1;}
int InvArmor(lua_State* L){game::Context c{};uint64_t box{};InvContext(L,&c,&box);const int i=(int)luaL_checkinteger(L,1);const uint64_t armor=game::EquipmentContainer(c,c.actor,1);if(!armor||i<1||i>game::ActionSize(c,armor)){lua_pushnil(L);return 1;}PushItem(L,c,game::ActionStackAt(c,armor,i-1));return 1;}
int InvFindIn(lua_State* L,int limit){game::Context c{};uint64_t box{};InvContext(L,&c,&box);std::string want=luaL_checkstring(L,1);std::transform(want.begin(),want.end(),want.begin(),[](unsigned char ch){return (char)std::tolower(ch);});
 int n=game::ActionSize(c,box);if(limit)n=std::min(n,limit);for(int i=0;i<n;++i){std::string id;if(!game::ActionName(game::ActionStackAt(c,box,i),&id))continue;std::string low=id,full;std::transform(low.begin(),low.end(),low.begin(),[](unsigned char ch){return (char)std::tolower(ch);});
  full=low.find(':')==std::string::npos?"minecraft:"+low:low;const size_t colon=low.rfind(':');const std::string bare=colon==std::string::npos?low:low.substr(colon+1);if(full==want||bare==want||low==want){lua_pushnumber(L,i+1);return 1;}}lua_pushnil(L);return 1;}
int InvFind(lua_State* L){return InvFindIn(L,0);}int InvHotbarFind(lua_State* L){return InvFindIn(L,9);}
const char* scriptInvResults[7]={"Submitted","Unavailable","Busy","InvalidContainer","InvalidSlot","InvalidAmount","IncompatibleItems"};
int InvResult(lua_State* L,int r){lua_pushboolean(L,r==0);lua_pushstring(L,r>=0&&r<7?scriptInvResults[r]:"Unavailable");return 2;}
int InvMove(lua_State* L,int from,uint8_t dstC,int dst){game::Context c{};uint64_t box{};InvContext(L,&c,&box);const uint64_t stack=game::ActionStackAt(c,box,from);uint8_t count{};if(!stack||!game::ActionItem(stack)||!game::Read(stack+0x22,&count)||!count)return InvResult(L,5);
 return InvResult(L,game::InvMoveStack(c,c.actor,0xc,from,dstC,dst,count));}
int InvSwap(lua_State* L){game::Context c{};uint64_t box{};InvContext(L,&c,&box);const int n=game::ActionSize(c,box);const int a=(int)luaL_checkinteger(L,1),b=(int)luaL_checkinteger(L,2);if(a<1||a>n||b<1||b>n)luaL_error(L,"slots are numbered 1 to %d",n);return InvMove(L,a-1,0xc,b-1);}
int InvEquip(lua_State* L){game::Context c{};uint64_t box{};InvContext(L,&c,&box);const int n=game::ActionSize(c,box);const int a=(int)luaL_checkinteger(L,1);if(a<1||a>n)luaL_error(L,"slots are numbered 1 to %d",n);std::string name;
 const int piece=game::ActionName(game::ActionStackAt(c,box,a-1),&name)?game::ArmorSlotOf(name):-1;if(piece<0){lua_pushboolean(L,0);lua_pushstring(L,"NotArmor");return 2;}return InvMove(L,a-1,6,piece);}
int InvDrop(lua_State* L){game::Context c{};uint64_t box{};InvContext(L,&c,&box);const int n=game::ActionSize(c,box);const int a=(int)luaL_checkinteger(L,1);if(a<1||a>n)luaL_error(L,"slots are numbered 1 to %d",n);
 const uint64_t stack=game::ActionStackAt(c,box,a-1);uint8_t have{};if(!stack||!game::ActionItem(stack)||!game::Read(stack+0x22,&have)||!have)return InvResult(L,4);if(game::ActionCursorBusy(c,c.actor))return InvResult(L,2);
 const int count=std::clamp((int)luaL_optinteger(L,2,have),1,(int)have);auto dropSlot=reinterpret_cast<void(*)(uint64_t,int,bool,bool,bool)>(c.base+0x2af140);   // Inventory::dropSlot(slot, client only, all, randomly)
 if(count>=have)dropSlot(box,a-1,false,true,false);else for(int i=0;i<count;++i)dropSlot(box,a-1,false,false,false);return InvResult(L,0);}
int InvToOffhand(lua_State* L){game::Context c{};uint64_t box{};InvContext(L,&c,&box);const int n=game::ActionSize(c,box);const int a=(int)luaL_checkinteger(L,1);if(a<1||a>n)luaL_error(L,"slots are numbered 1 to %d",n);return InvMove(L,a-1,0x22,0);}
// ---- world.block / world.isLoaded, player attack / interact / break
// MSVC map<string, Tag> walked in order: head *(compound+8), nodes {left +0, parent +8, right +0x10, nil +0x19, key +0x20,
// tag +0x40 (vtable 0x28 = type id; value at +8)}.
void PushNbtCompound(lua_State* L,const game::Context& c,uint64_t compound){lua_newtable(L);uint64_t head{},node{};if(!compound||!game::Read(compound+8,&head)||!head||!game::Read(head,&node))return;
 for(int guard=0;guard<128&&node&&node!=head;++guard){uint8_t nil{};if(!game::Read(node+0x19,&nil)||nil)break;std::string key;
  if(game::SusString(node+0x20,&key)){const uint64_t tag=node+0x40;const uint64_t idOf=game::Virtual(c,tag,0x28);const uint8_t type=idOf?reinterpret_cast<uint8_t(*)(uint64_t)>(idOf)(tag):0;bool pushed=true;
   switch(type){case 1:{uint8_t v{};game::Read(tag+8,&v);lua_pushnumber(L,v);break;}case 2:{int16_t v{};game::Read(tag+8,&v);lua_pushnumber(L,v);break;}case 3:{int32_t v{};game::Read(tag+8,&v);lua_pushnumber(L,v);break;}
    case 4:{int64_t v{};game::Read(tag+8,&v);lua_pushnumber(L,(double)v);break;}case 5:{float v{};game::Read(tag+8,&v);lua_pushnumber(L,v);break;}case 6:{double v{};game::Read(tag+8,&v);lua_pushnumber(L,v);break;}
    case 8:{std::string v;if(game::SusString(tag+8,&v))lua_pushstring(L,v.c_str());else pushed=false;break;}default:pushed=false;}
   if(pushed)lua_setfield(L,-2,key.c_str());}
  uint64_t right{},next{};if(!game::Read(node+0x10,&right))break;uint8_t rnil=1;if(right)game::Read(right+0x19,&rnil);
  if(right&&!rnil){next=right;for(int g=0;g<64;++g){uint64_t left{};uint8_t lnil=1;if(!game::Read(next,&left)||!left||!game::Read(left+0x19,&lnil)||lnil)break;next=left;}node=next;}
  else{uint64_t cur=node,parent{};for(int g=0;g<64;++g){if(!game::Read(cur+8,&parent)||!parent)return;uint64_t pr{};game::Read(parent+0x10,&pr);if(pr!=cur)break;cur=parent;}node=parent;}}}
int WorldBlock(lua_State* L){game::Context c{};if(!game::Local(&c))return 0;const uint64_t region=game::ActorRegion(c);if(!region)return 0;float v[3];if(!ReadVector(L,1,v))luaL_error(L,"expected a vector");
 const int32_t pos[3]={(int32_t)std::floor(v[0]),(int32_t)std::floor(v[1]),(int32_t)std::floor(v[2])};const uint64_t block=game::RegionBlock(c,region,pos);if(!block)return 0;std::string name;
 uint64_t legacy{};char full[96]{};if(game::Read(block+0x68,&legacy)&&legacy&&game::Text(legacy+0xe8,full,sizeof(full)))name=full;uint8_t f0{},f1{};game::BlockFlags(block,&f0,&f1);int32_t runtime{};game::Read(block+0x114,&runtime);
 lua_createtable(L,0,7);lua_pushstring(L,name.c_str());lua_setfield(L,-2,"name");lua_pushnumber(L,(double)(uint32_t)runtime);lua_setfield(L,-2,"runtimeId");
 lua_pushboolean(L,name=="minecraft:air"||name=="air");lua_setfield(L,-2,"isAir");lua_pushboolean(L,(f1&4)!=0);lua_setfield(L,-2,"solid");lua_pushboolean(L,(f1&2)!=0);lua_setfield(L,-2,"liquid");
 const float bp[3]={(float)pos[0],(float)pos[1],(float)pos[2]};PushVector(L,bp);lua_setfield(L,-2,"position");PushNbtCompound(L,c,game::SusTag(c,block+0xf0,"states",10));lua_setfield(L,-2,"states");return 1;}
int WorldIsLoaded(lua_State* L){game::Context c{};if(!game::Local(&c))return 0;const uint64_t region=game::ActorRegion(c);if(!region)luaL_error(L,"the world is not available");float v[3];if(!ReadVector(L,1,v))luaL_error(L,"expected a vector");
 const int x=(int)std::floor(v[0]),y=(int)std::floor(v[1]),z=(int)std::floor(v[2]);int16_t minY{},maxY{};game::Read(region+0x3a,&minY);game::Read(region+0x38,&maxY);
 const uint64_t chunkAt=game::Virtual(c,region,0x148);lua_pushboolean(L,y>=minY&&y<maxY&&chunkAt&&game::PfChunkRaw(chunkAt,region,x>>4,z>>4));return 1;}
uintptr_t ScriptEntity(lua_State* L,const game::Context& c,int idx){const double id=luaL_checknumber(L,idx);uintptr_t actor{};return id>=0&&game::ActorByRuntimeId(c.registry,(uint64_t)id,&actor)?actor:0;}
int PlayerAttack(lua_State* L){game::Context c{};if(!game::Local(&c))return 0;const uintptr_t target=ScriptEntity(L,c,1);lua_pushboolean(L,target&&target!=c.actor&&game::AttackActor(c,target));return 1;}
int PlayerInteract(lua_State* L){game::Context c{};uint64_t gm{};if(!game::Local(&c)||!game::Read(c.actor+0xac8,&gm)||!gm)return 0;const uintptr_t target=ScriptEntity(L,c,1);if(!target||target==c.actor){lua_pushboolean(L,0);return 1;}
 float at[3]{};game::ActorPos(target,at);game::ActionSwing(c,c.actor,3);const uint64_t f=game::Virtual0(c,gm,0x70);lua_pushboolean(L,f&&reinterpret_cast<bool(*)(uint64_t,uintptr_t,const float*,int)>(f)(gm,target,at,0));return 1;}
bool ScriptBreakArgs(lua_State* L,game::Context* c,uint64_t* gm,int32_t* pos,uint8_t* face){if(!game::Local(c)||!game::Read(c->actor+0xac8,gm)||!*gm)return false;float v[3];if(!ReadVector(L,1,v))luaL_error(L,"expected a vector");
 for(int i=0;i<3;++i)pos[i]=(int32_t)std::floor(v[i]);*face=1;if(lua_isstring(L,2)){const char* f=lua_tostring(L,2);bool known=false;for(int i=0;i<6;++i)if(_stricmp(f,scriptFaces[i])==0){*face=(uint8_t)i;known=true;}if(!known)luaL_error(L,"unknown value \"%s\"",f);}return true;}
int PlayerStartBreak(lua_State* L){game::Context c{};uint64_t gm{};int32_t pos[3];uint8_t face{};if(!ScriptBreakArgs(L,&c,&gm,pos,&face))return 0;const uint64_t f=game::Virtual0(c,gm,8);if(!f)return 0;bool done=false;
 const bool ok=reinterpret_cast<bool(*)(uint64_t,int32_t*,uint8_t,bool*)>(f)(gm,pos,face,&done);game::ActionSwing(c,c.actor,4);lua_pushboolean(L,ok);lua_pushboolean(L,done);return 2;}
int PlayerContinueBreak(lua_State* L){game::Context c{};uint64_t gm{};int32_t pos[3];uint8_t face{};if(!ScriptBreakArgs(L,&c,&gm,pos,&face))return 0;const uint64_t f=game::Virtual0(c,gm,0x18);if(!f)return 0;bool done=false;float eye[3]{};LocalEye(eye);
 const bool ok=reinterpret_cast<bool(*)(uint64_t,int32_t*,uint8_t,float*,bool*)>(f)(gm,pos,face,eye,&done);game::ActionSwing(c,c.actor,4);lua_pushboolean(L,ok);lua_pushboolean(L,done);return 2;}
int PlayerStopBreak(lua_State* L){game::Context c{};uint64_t gm{};if(!game::Local(&c)||!game::Read(c.actor+0xac8,&gm)||!gm)return 0;float v[3];if(!ReadVector(L,1,v))luaL_error(L,"expected a vector");int32_t pos[3]={(int32_t)std::floor(v[0]),(int32_t)std::floor(v[1]),(int32_t)std::floor(v[2])};
 const uint64_t f=game::Virtual0(c,gm,0x20);if(f)reinterpret_cast<void(*)(uint64_t,int32_t*)>(f)(gm,pos);return 0;}
// ---- container (chest-like window shown)
const char* ScriptContainerType(int t){static const char* names[]={"Container","Workbench","Furnace","Enchantment","BrewingStand","Anvil","Dispenser","Dropper","Hopper","Cauldron","MinecartChest","MinecartHopper","Horse","Beacon",
  "StructureEditor","Trade","CommandBlock","Jukebox","Armor","Hand","CompoundCreator","ElementConstructor","MaterialReducer","LabTable","Loom","Lectern","Grindstone","BlastFurnace","Smoker","Stonecutter","Cartography","Hud","JigsawEditor","SmithingTable","ChestBoat","DecoratedPot","Crafter"};
 if(t==-9)return "None";if(t==-1)return "Inventory";if(t>=0&&t<(int)(sizeof(names)/sizeof(names[0])))return names[t];return "Unknown";}
struct ScriptBox{game::Context c{};uint8_t window=0;int8_t type=0;int e=0;uint64_t box=0;};
bool ScriptOpenBox(ScriptBox* b){if(!game::Local(&b->c))return false;uint64_t model{},mgr{};uint8_t w{},t{};if(!game::Read(b->c.actor+0x5c8,&model)||!model||!game::Read(model+0x50,&w)||!game::Read(model+0x51,&t))return false;
 if(!game::PbOpenEnum(b->c,b->c.actor,w,t,&b->e))return false;if(!game::Read(b->c.actor+0xae0,&mgr)||!mgr||!(b->box=game::PbContainer(b->c,mgr,(uint8_t)b->e)))return false;b->window=w;b->type=(int8_t)t;return true;}
int ContainerIsOpen(lua_State* L){ScriptBox b;lua_pushboolean(L,ScriptOpenBox(&b));return 1;}
int ContainerType(lua_State* L){ScriptBox b;lua_pushstring(L,ScriptOpenBox(&b)?ScriptContainerType(b.type):"None");return 1;}
int ContainerSize(lua_State* L){ScriptBox b;lua_pushnumber(L,ScriptOpenBox(&b)?game::ActionSize(b.c,b.box):0);return 1;}
int ContainerItem(lua_State* L){ScriptBox b;if(!ScriptOpenBox(&b))luaL_error(L,"no container is open");const int n=game::ActionSize(b.c,b.box);const int slot=(int)luaL_checkinteger(L,1);if(slot<1||slot>n)luaL_error(L,"container slots are numbered 1 to %d",n);
 PushItem(L,b.c,game::ActionStackAt(b.c,b.box,slot-1));return 1;}
int ContainerItems(lua_State* L){ScriptBox b;if(!ScriptOpenBox(&b))luaL_error(L,"no container is open");const int n=game::ActionSize(b.c,b.box);lua_createtable(L,n,0);
 for(int i=0;i<n;++i){PushItem(L,b.c,game::ActionStackAt(b.c,b.box,i));lua_rawseti(L,-2,i+1);}return 1;}
int ContainerPair(lua_State* L,const char* why){lua_pushboolean(L,0);lua_pushstring(L,why);return 2;}
int ContainerTake(lua_State* L){ScriptBox b;if(!ScriptOpenBox(&b))luaL_error(L,"no container is open");const int n=game::ActionSize(b.c,b.box);const int slot=(int)luaL_checkinteger(L,1);if(slot<1||slot>n)luaL_error(L,"container slots are numbered 1 to %d",n);
 const uint64_t stack=game::ActionStackAt(b.c,b.box,slot-1);const int have=game::PbCount(stack);if(!game::StackItem(stack)||!have)return ContainerPair(L,"Empty");if(game::ActionCursorBusy(b.c,b.c.actor))return ContainerPair(L,"Busy");
 const int free=game::PbFreeSlot(b.c,game::ActionContainer(b.c.actor));if(free<0)return ContainerPair(L,"InventoryFull");const int count=std::clamp((int)luaL_optinteger(L,2,have),1,have);if(count!=have)return InvResult(L,5);
 return InvResult(L,game::PbTransfer(b.c,b.c.actor,1,(uint8_t)b.e,slot-1,0xc,free,count));}
int ContainerPut(lua_State* L){ScriptBox b;if(!ScriptOpenBox(&b))luaL_error(L,"no container is open");const uint64_t inv=game::ActionContainer(b.c.actor);const int n=game::ActionSize(b.c,inv);const int slot=(int)luaL_checkinteger(L,1);
 if(slot<1||slot>n)luaL_error(L,"inventory slots are numbered 1 to %d",n);const uint64_t stack=game::ActionStackAt(b.c,inv,slot-1);const int have=game::PbCount(stack);if(!game::StackItem(stack)||!have)return ContainerPair(L,"Empty");
 if(game::ActionCursorBusy(b.c,b.c.actor))return ContainerPair(L,"Busy");int free=-1;for(int i=0,m=game::ActionSize(b.c,b.box);i<m;++i)if(!game::StackItem(game::ActionStackAt(b.c,b.box,i))){free=i;break;}if(free<0)return ContainerPair(L,"ContainerFull");
 const int count=std::clamp((int)luaL_optinteger(L,2,have),1,have);if(count!=have)return InvResult(L,5);return InvResult(L,game::PbTransfer(b.c,b.c.actor,1,0xc,slot-1,(uint8_t)b.e,free,count));}
int ContainerClose(lua_State* L){(void)L;ScriptBox b;if(!ScriptOpenBox(&b))return 0;const uint8_t w=b.window,t=(uint8_t)b.type;game::InvSend(b.c,0x2f,[&](uint64_t p){*(uint8_t*)(p+0x30)=w;*(uint8_t*)(p+0x31)=t;*(uint8_t*)(p+0x32)=0;});return 0;}
// ---- path (Path Finder)
int PathGoto(lua_State* L){float v[3];if(!ReadVector(L,1,v))luaL_error(L,"expected a vector");game::PfGoto((int)std::floor(v[0]),(int)std::floor(v[1]),(int)std::floor(v[2]),std::max(0,(int)luaL_optinteger(L,2,0)));return 0;}
int PathFollow(lua_State* L){const char* name=luaL_checkstring(L,1);game::PfFollow(name,lua_isnumber(L,2)?(int)lua_tonumber(L,2):0);return 0;}
int PathMine(lua_State* L){luaL_checktype(L,1,LUA_TTABLE);std::vector<std::string> names;const int n=lua_objlen(L,1);for(int i=1;i<=n;++i){lua_rawgeti(L,1,i);if(lua_isstring(L,-1))names.push_back(lua_tostring(L,-1));lua_pop(L,1);}
 if(names.empty())luaL_error(L,"path.mine needs at least one block name");game::PfMine(names,std::max(0,(int)luaL_optinteger(L,2,0)));return 0;}
int PathWander(lua_State* L){game::PfWander(lua_isnumber(L,1)?std::clamp((int)lua_tonumber(L,1),1,4096):0);return 0;}
int PathStop(lua_State*){game::PfStop();return 0;}int PathPause(lua_State*){game::PfPause();return 0;}int PathResume(lua_State*){game::PfResume();return 0;}
int PathIsActive(lua_State* L){lua_pushboolean(L,game::PfIsActive());return 1;}
int PathStatus(lua_State* L){std::string state,process,detail;int remaining=0;game::PfStatus(&state,&process,&detail,&remaining);lua_createtable(L,0,4);lua_pushstring(L,state.c_str());lua_setfield(L,-2,"state");
 lua_pushstring(L,process.c_str());lua_setfield(L,-2,"process");lua_pushstring(L,detail.c_str());lua_setfield(L,-2,"detail");lua_pushnumber(L,remaining);lua_setfield(L,-2,"remaining");return 1;}
bool scriptPathWas=false;
void ScriptsPathWatch(){if(!ScriptsWant("pathEnd")){scriptPathWas=false;return;}const bool now=game::PfIsActive();if(scriptPathWas&&!now)Fire("pathEnd");scriptPathWas=now;}
// ---- projectile
int ScriptProjectileKind(lua_State* L,int idx){size_t len=0;const char* name=luaL_checklstring(L,idx,&len);int kind{};if(!game::TrajectoryKind(name,&kind)){const char* bare=!strncmp(name,"minecraft:",10)?name+10:name;luaL_error(L,"\"%s\" is not a projectile item",bare);}return kind;}
int ProjectileTrace(lua_State* L){const int kind=ScriptProjectileKind(L,1);const float yaw=(float)luaL_checknumber(L,2),pitch=(float)luaL_checknumber(L,3);game::Context c{};if(!game::Local(&c))return 0;
 float start[3]{};if(!game::PcStartRaw(c,start))luaL_error(L,"the player has no position");game::TrajectoryPath path;if(!game::TrajectoryTrace(c,kind,start,yaw,pitch,&path)||path.points.empty())return 0;
 lua_createtable(L,0,3);const auto& hit=path.points.back();const float hv[3]={hit[0],hit[1],hit[2]};PushVector(L,hv);lua_setfield(L,-2,"position");lua_pushnumber(L,(double)(path.points.size()-1));lua_setfield(L,-2,"ticks");
 uint64_t rid{};if(path.actor&&game::RuntimeId(path.actor,&rid)){lua_pushnumber(L,(double)rid);lua_setfield(L,-2,"entity");}return 1;}
int ProjectileAim(lua_State* L){const int kind=ScriptProjectileKind(L,1);float target[3];if(!ReadVector(L,2,target))luaL_error(L,"expected a vector");const float tolerance=(float)luaL_optnumber(L,3,1.5);
 game::Context c{};if(!game::Local(&c))return 0;float start[3]{};if(!game::PcStartRaw(c,start))return 0;const uint64_t region=game::ActorRegion(c);if(!region)return 0;const game::TrajectoryParams p=game::TrajectoryBds(kind);
 const float d[3]={target[0]-start[0],target[1]-start[1],target[2]-start[2]};float v[3]{};if(!game::PcSolve(p,d,v))return 0;const float n=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);if(n<=1e-6f)return 0;
 const float dir[3]={v[0]/n,v[1]/n,v[2]/n};const float vel[3]={p.speed*dir[0],p.speed*dir[1],p.speed*dir[2]};float miss{};if(!game::PcFlight(c,region,p,start,vel,target,&miss)||miss>tolerance)return 0;
 lua_pushnumber(L,std::atan2(-dir[0],dir[2])*57.2958f);lua_pushnumber(L,-std::atan2(dir[1],std::sqrt(dir[0]*dir[0]+dir[2]*dir[2]))*57.2958f);return 2;}
// ---- player.simulate
std::atomic<float> scriptInputStrafe{0},scriptInputForward{0};std::atomic<uint8_t> scriptInputFlags{0};   // last SprintIntent input (bit 4 jump, bit 0 sneak, bit 1 sprint)
int PlayerSimulate(lua_State* L){game::Context c{};if(!game::Local(&c)||!game::ActorRegion(c))return 0;game::SimInput in;float pitch=0,yaw=0;game::RotRead(c.actor,&pitch,&yaw);in.yaw=yaw;in.pitch=pitch;
 const uint8_t flags=scriptInputFlags.load();in.strafe=scriptInputStrafe.load();in.forward=scriptInputForward.load();in.jump=flags&0x10;in.sneak=flags&1;in.sprint=flags&2;
 if(lua_istable(L,1)){auto num=[&](const char* k,float& out,float lo,float hi){lua_rawgetfield(L,1,k);if(lua_isnumber(L,-1))out=std::clamp((float)lua_tonumber(L,-1),lo,hi);lua_pop(L,1);};
  num("strafe",in.strafe,-1,1);num("forward",in.forward,-1,1);num("pitch",in.pitch,-90,90);lua_rawgetfield(L,1,"yaw");if(lua_isnumber(L,-1))in.yaw=(float)lua_tonumber(L,-1);lua_pop(L,1);
  in.jump=OptBool(L,1,"jump",in.jump);in.sneak=OptBool(L,1,"sneak",in.sneak);in.sprint=OptBool(L,1,"sprint",in.sprint);}
 const int ticks=std::clamp((int)luaL_optinteger(L,2,1),1,60);std::vector<game::SimTick> out;if(!game::SimulateGuarded(in,ticks,out))return 0;
 static const char* outcomes[7]={"Normal","Teleport","Unreliable","UnloadedChunk","ImmobileOrNotReady","Mounted","InvalidInput"};lua_createtable(L,(int)out.size(),0);
 for(size_t i=0;i<out.size();++i){const auto& k=out[i];lua_createtable(L,0,6);PushVector(L,k.pos);lua_setfield(L,-2,"position");PushVector(L,k.vel);lua_setfield(L,-2,"velocity");lua_pushboolean(L,k.onGround);lua_setfield(L,-2,"onGround");
  lua_pushboolean(L,k.landed);lua_setfield(L,-2,"landed");lua_pushboolean(L,k.collided);lua_setfield(L,-2,"collided");lua_pushstring(L,outcomes[std::clamp(k.outcome,0,6)]);lua_setfield(L,-2,"outcome");lua_rawseti(L,-2,(int)i+1);}
 return 1;}
// ---- events jump / move / hurt / death / respawn / teleport
std::atomic<bool> scriptJumpCancel{false};std::atomic<float> scriptJumpBoost{0.f};
void ScriptsJumpPre(uint64_t arg7,uint64_t arg8){(void)arg7;(void)arg8;if(!ScriptsWant("jump"))return;scriptJumpCancel=false;scriptJumpBoost=0.f;float boost=0;
 const bool cancelled=Dispatch("jump",[](lua_State* L){NewEvent(L);lua_pushnumber(L,0);lua_setfield(L,-2,"boost");return 1;},[&](lua_State* L,int ev){lua_rawgetfield(L,ev,"boost");if(lua_isnumber(L,-1))boost=(float)lua_tonumber(L,-1);lua_pop(L,1);});
 scriptJumpCancel=cancelled;scriptJumpBoost=std::isfinite(boost)?boost:0.f;}
float scriptLastHealth=-1;
void ScriptsHealthWatch(){if(!ScriptsWant("hurt")&&!ScriptsWant("death")&&!ScriptsWant("respawn")){scriptLastHealth=-1;return;}game::Context c{};float h{};if(!game::Local(&c)||!game::PlayerHealth(c,&h))return;const float prev=scriptLastHealth;scriptLastHealth=h;if(prev<0)return;
 if(h>0&&h<prev)Dispatch("hurt",[&](lua_State* L){NewEvent(L);lua_pushnumber(L,prev-h);lua_setfield(L,-2,"amount");lua_pushnumber(L,h);lua_setfield(L,-2,"health");return 1;},[](lua_State*,int){});
 if(prev>0&&h<=0)Fire("death");if(prev<=0&&h>0)Fire("respawn");}
std::atomic<int64_t> scriptPearlAt{0};
void ScriptsTeleport(uintptr_t packet){if(!ScriptsWant("teleport"))return;uint8_t mode{};uint64_t who{},mine{};game::Context c{};if(!game::Read(packet+0x50,&mode)||mode!=2||!game::Local(&c)||!game::RuntimeId(c.actor,&mine)||!game::Read(packet+0x30,&who)||who!=mine)return;
 float p[3];if(!game::Floats(packet+0x38,p,3))return;const int64_t at=scriptPearlAt.exchange(0);const bool pearl=at&&game::ActionMs(at)<=5000.0;
 Dispatch("teleport",[&](lua_State* L){NewEvent(L);PushVector(L,p);lua_setfield(L,-2,"position");lua_pushboolean(L,pearl);lua_setfield(L,-2,"pearl");return 1;},[](lua_State*,int){});}
void ScriptsPearlSend(uintptr_t packet){if(game::PacketId(packet)!=0x1e)return;game::Context c{};if(!game::Local(&c))return;if(game::ActionNameHas(game::ActionHeld(c,c.actor),"ender_pearl"))scriptPearlAt=game::ActionNs();}
// SprintIntent (#19): input state at *(ev+0x40): +0x24 strafe, +0x28 forward, byte +0x60 (bit 4 jump, bit 0 sneak,
// bit 1 sprint).
void ScriptsMove(uint64_t input,uint64_t state,uint64_t moveVec){if(input){game::Context c{};uint64_t mine{};if(game::Local(&c)&&game::Read(c.actor+0x240,&mine)&&mine==state){scriptInputStrafe=*(float*)(input+0x24);scriptInputForward=*(float*)(input+0x28);scriptInputFlags=*(uint8_t*)(input+0x60);}}   // player.simulate default input
 if(!ScriptsWant("move")||!input)return;{game::Context c{};uint64_t mine{};if(!game::Local(&c)||!game::Read(c.actor+0x240,&mine)||mine!=state)return;}float strafe=*(float*)(input+0x24),forward=*(float*)(input+0x28);uint8_t flags=*(uint8_t*)(input+0x60);
 bool jump=flags&0x10,sneak=flags&1,sprint=flags&2;float s2=strafe,f2=forward;bool j2=jump,k2=sneak,r2=sprint;
 Dispatch("move",[&](lua_State* L){NewEvent(L);lua_pushnumber(L,forward);lua_setfield(L,-2,"forward");lua_pushnumber(L,strafe);lua_setfield(L,-2,"strafe");lua_pushboolean(L,jump);lua_setfield(L,-2,"jump");
   lua_pushboolean(L,sneak);lua_setfield(L,-2,"sneak");lua_pushboolean(L,sprint);lua_setfield(L,-2,"sprint");return 1;},
  [&](lua_State* L,int ev){auto num=[&](const char* k,float& out){lua_rawgetfield(L,ev,k);if(lua_isnumber(L,-1)){const float v=(float)lua_tonumber(L,-1);if(std::isfinite(v))out=std::clamp(v,-1.f,1.f);}lua_pop(L,1);};
   auto flag=[&](const char* k,bool& out){lua_rawgetfield(L,ev,k);if(lua_isboolean(L,-1))out=lua_toboolean(L,-1)!=0;lua_pop(L,1);};num("forward",f2);num("strafe",s2);flag("jump",j2);flag("sneak",k2);flag("sprint",r2);});
 if(s2==strafe&&f2==forward&&j2==jump&&k2==sneak&&r2==sprint)return;uint32_t& bits=*(uint32_t*)input;bits&=0xfbfe1e7e;if(f2>0)bits|=1u<<13;else if(f2<0)bits|=1u<<14;if(s2>0)bits|=1u<<15;else if(s2<0)bits|=1u<<16;
 if(j2)bits|=(1u<<7)|(1u<<26);if(k2)bits|=1;if(r2)bits|=1u<<8;*(float*)(input+0x24)=s2;*(float*)(input+0x28)=f2;*(uint8_t*)(input+0x60)=(uint8_t)((flags&~0x13)|(j2?0x10:0)|(k2?1:0)|(r2?2:0));
 if(moveVec){*(float*)moveVec=s2;*(float*)(moveVec+4)=f2;}}
bool ScriptsJumpCancel(){return scriptJumpCancel.exchange(false);}
void ScriptsJumpPost(uint64_t arg8){const float b=scriptJumpBoost.exchange(0.f);if(b!=0.f&&arg8)*(float*)(arg8+0x1c)+=b;}
void ScriptsMoveGuarded(uint64_t input,uint64_t state,uint64_t movement){static volatile long stopped_=0;if(!stopped_)__try{ScriptsMove(input,state,movement);}__except(game::FeatureFault("ScriptsMoveGuarded",&stopped_,GetExceptionInformation())){game::FeatureFlush();}}
