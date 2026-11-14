// Luau scripting engine (Scripting tab). Included by menu.cpp inside its anonymous namespace, after Save / Place /
// Label / ModuleById (no #include here: lua.h, lualib.h and luacode.h are included at the top of menu.cpp).
// One Luau VM per .luau file in %APPDATA%\Nickel\scripts: reloaded about a second after saving, unloaded when the file
// disappears; *.d.luau files are never run; libraries lib/<name>.luau are loaded through require.
// Limits: 50 ms per call (1 s at load), 64 MB, 256 timers; 5 errors within 10 s stop the script until it is saved again.
// No file or network access (Luau standard libraries only, read-only environment, luaL_sandbox / luaL_sandboxthread).
// Script modules are appended to `modules` (category given by the script; "Visual" -> Visuals tab) and are configured,
// bound and saved like built-in modules. An unloaded script module stays in the array with no category (hidden) so
// pointers into `modules` remain valid; it is reused on reload if the name matches.
// Threads: all Luau code runs under `scriptGuard`; update / render2d / render3d / key / timers run on the render thread,
// tick / packetSend / packetReceive / chat / attack on the game thread (shared hooks).
// The 2.1 API lives in script_api21.hpp (inventory, container, path, projectile, player.simulate, the 2.1 events).
struct ScriptHandler{std::string event;int ref;};
struct ScriptModule{int index=-1;std::string name;int enableRef=0,disableRef=0;std::vector<ScriptHandler> handlers;bool was=false;int activation=0;bool hud=false;float hudW=0,hudH=0;bool frame=false;float fx=0,fy=0,fs=1,fw=0,fh=0;
 bool rotating=false;float yaw=0,pitch=0;};
struct ScriptCommand{std::string name,description;int specRef=0;};
struct ScriptTimer{int id;double due,every;int ref;};
struct Script{
 std::wstring file;std::string name;uint64_t stamp=0;lua_State* L=nullptr;int threadRef=0;
 int state=0;   // 0 Stopped, 1 Running, 2 Stopped after errors, 3 Failed to load
 std::vector<ScriptModule> mods;std::vector<ScriptCommand> commands;std::deque<std::string> errors;std::deque<double> errorTimes;
 size_t memory=0;double frameMs=0,frameSum=0,frameStart=0;std::vector<ScriptTimer> timers;int nextTimer=1;bool loading=false;double callStart=0,limit=.05;
 bool userStopped=false;std::map<std::string,int> required;std::vector<std::string> requiring;
};
std::recursive_mutex scriptGuard;std::vector<std::unique_ptr<Script>> scripts;int scriptSelected=-1;char scriptSearch[64]{};double scriptScanAt=0;
std::wstring ScriptFolder(){wchar_t base[MAX_PATH]{};if(!GetEnvironmentVariableW(L"APPDATA",base,MAX_PATH))return L"";return std::wstring(base)+L"\\Nickel\\scripts";}
double ScriptNow(){LARGE_INTEGER f{},c{};QueryPerformanceFrequency(&f);QueryPerformanceCounter(&c);return (double)c.QuadPart/(double)f.QuadPart;}
Script* ScriptOf(lua_State* L){return (Script*)lua_callbacks(L)->userdata;}
const char* ScriptStateName(int state){static const char* n[]={"Stopped","Running","Stopped after errors","Failed to load"};return n[std::clamp(state,0,3)];}

// ---------------------------------------------------------------- keys (documented names <-> virtual-key codes)
struct ScriptKey{const char* name;int code;};
const ScriptKey scriptKeys[]={{"Space",VK_SPACE},{"Enter",VK_RETURN},{"Escape",VK_ESCAPE},{"Tab",VK_TAB},{"Shift",VK_SHIFT},{"LeftShift",VK_LSHIFT},{"RightShift",VK_RSHIFT},{"LeftControl",VK_LCONTROL},{"RightControl",VK_RCONTROL},
 {"LeftAlt",VK_LMENU},{"RightAlt",VK_RMENU},{"Up",VK_UP},{"Down",VK_DOWN},{"Left",VK_LEFT},{"Right",VK_RIGHT},{"MouseLeft",VK_LBUTTON},{"MouseRight",VK_RBUTTON},{"MouseMiddle",VK_MBUTTON},{"MouseX1",VK_XBUTTON1},{"MouseX2",VK_XBUTTON2}};
int ScriptKeyCode(std::string name){std::string low=name;std::transform(low.begin(),low.end(),low.begin(),[](unsigned char c){return (char)std::tolower(c);});
 for(auto& k:scriptKeys){std::string n=k.name;std::transform(n.begin(),n.end(),n.begin(),[](unsigned char c){return (char)std::tolower(c);});if(n==low)return k.code;}
 if(low.size()==1&&low[0]>='a'&&low[0]<='z')return 'A'+(low[0]-'a');if(low.size()==2&&low[0]=='d'&&low[1]>='0'&&low[1]<='9')return '0'+(low[1]-'0');
 if(low.size()>=2&&low[0]=='f'){int n=atoi(low.c_str()+1);if(n>=1&&n<=12)return VK_F1+n-1;}if(low.rfind("numpad",0)==0&&low.size()==7&&low[6]>='0'&&low[6]<='9')return VK_NUMPAD0+(low[6]-'0');return 0;}
std::string ScriptKeyName(int code){for(auto& k:scriptKeys)if(k.code==code)return k.name;if(code>='A'&&code<='Z')return std::string(1,(char)code);if(code>='0'&&code<='9')return std::string("D")+(char)code;
 if(code>=VK_F1&&code<=VK_F12)return "F"+std::to_string(code-VK_F1+1);if(code>=VK_NUMPAD0&&code<=VK_NUMPAD9)return "Numpad"+std::to_string(code-VK_NUMPAD0);return code?"Key"+std::to_string(code):"None";}

// ---------------------------------------------------------------- errors, protected calls, limits
void ScriptUnload(Script& s);
void ScriptError(Script& s,const std::string& message){
 const double now=ScriptNow();s.errors.push_back(message);while(s.errors.size()>8)s.errors.pop_front();s.errorTimes.push_back(now);while(!s.errorTimes.empty()&&now-s.errorTimes.front()>10)s.errorTimes.pop_front();
 if(WriteLog){std::string line="Script "+s.name+": "+message;WriteLog(line.c_str());}
 if(s.state==1&&s.errorTimes.size()>=5){s.state=2;for(auto& m:s.mods)if(m.index>=0&&modules[m.index].enabled){modules[m.index].enabled=false;Save(modules[m.index]);}}
}
// Stack: function then arguments; returns true if the call finished without error (results left on the stack).
bool ScriptCall(Script& s,lua_State* L,int args,int results){
 s.callStart=ScriptNow();const int status=lua_pcall(L,args,results,0);const double spent=ScriptNow()-s.callStart;if(!s.loading)s.frameSum+=spent;s.callStart=0;
 if(status!=LUA_OK){const char* text=lua_tostring(L,-1);ScriptError(s,text?text:"error");lua_pop(L,1);return false;}return true;}
void ScriptInterrupt(lua_State* L,int gc){if(gc>=0)return;Script* s=ScriptOf(L);if(s&&s->callStart>0&&ScriptNow()-s->callStart>s->limit){s->callStart=ScriptNow()+1e9;luaL_error(L,s->loading?"script took longer than one second to load":"script call took longer than 50 ms");}}
void* ScriptAlloc(void* ud,void* ptr,size_t osize,size_t nsize){Script* s=(Script*)ud;if(nsize==0){free(ptr);s->memory-=std::min(s->memory,osize);return nullptr;}
 if(nsize>osize&&s->memory+(nsize-osize)>64ull*1024*1024)return nullptr;void* p=realloc(ptr,nsize);if(p)s->memory=s->memory-std::min(s->memory,osize)+nsize;return p;}
// Calls a referenced function with an optional argument already pushed.
bool ScriptInvoke(Script& s,int ref,int args=0){lua_State* L=s.L;lua_getref(L,ref);if(args)lua_insert(L,-1-args);return ScriptCall(s,L,args,0);}

// ---------------------------------------------------------------- values
void PushColor(lua_State* L,const std::string& text){const ImVec4 c=ParseColor(text);lua_createtable(L,4,0);const float v[4]={c.x,c.y,c.z,c.w};for(int i=0;i<4;++i){lua_pushnumber(L,v[i]);lua_rawseti(L,-2,i+1);}}
ImVec4 ReadColor(lua_State* L,int idx,ImVec4 fallback=ImVec4(1,1,1,1)){if(!lua_istable(L,idx))return fallback;float v[4]={fallback.x,fallback.y,fallback.z,fallback.w};
 for(int i=0;i<4;++i){lua_rawgeti(L,idx,i+1);if(lua_isnumber(L,-1))v[i]=(float)std::clamp(lua_tonumber(L,-1),0.0,1.0);lua_pop(L,1);}
 const char* keys[]={"r","g","b","a"};for(int i=0;i<4;++i){lua_rawgetfield(L,idx,keys[i]);if(lua_isnumber(L,-1))v[i]=(float)std::clamp(lua_tonumber(L,-1),0.0,1.0);lua_pop(L,1);}return ImVec4(v[0],v[1],v[2],v[3]);}
bool ReadVector(lua_State* L,int idx,float out[3]){if(lua_isvector(L,idx)){const float* v=lua_tovector(L,idx);out[0]=v[0];out[1]=v[1];out[2]=v[2];return true;}
 if(lua_istable(L,idx)){const char* k[]={"x","y","z"};for(int i=0;i<3;++i){lua_rawgetfield(L,idx,k[i]);if(!lua_isnumber(L,-1)){lua_pop(L,1);lua_rawgeti(L,idx,i+1);}out[i]=(float)lua_tonumber(L,-1);lua_pop(L,1);}return true;}return false;}
#include "script_values.hpp"   // ReadVectorStrict (tested without the game)
int OptInt(lua_State* L,int idx,const char* key,int fallback){if(!lua_istable(L,idx))return fallback;lua_rawgetfield(L,idx,key);const int v=lua_isnumber(L,-1)?(int)lua_tonumber(L,-1):fallback;lua_pop(L,1);return v;}
double OptNum(lua_State* L,int idx,const char* key,double fallback){if(!lua_istable(L,idx))return fallback;lua_rawgetfield(L,idx,key);const double v=lua_isnumber(L,-1)?lua_tonumber(L,-1):fallback;lua_pop(L,1);return v;}
bool OptBool(lua_State* L,int idx,const char* key,bool fallback){if(!lua_istable(L,idx))return fallback;lua_rawgetfield(L,idx,key);const bool v=lua_isboolean(L,-1)?lua_toboolean(L,-1)!=0:fallback;lua_pop(L,1);return v;}
std::string OptStr(lua_State* L,int idx,const char* key,const char* fallback){if(!lua_istable(L,idx))return fallback;lua_rawgetfield(L,idx,key);std::string v=lua_isstring(L,-1)?lua_tostring(L,-1):fallback;lua_pop(L,1);return v;}
std::string Slug(const std::string& name){std::string out;for(char c:name){if(std::isalnum((unsigned char)c))out+=(char)std::tolower((unsigned char)c);else if(!out.empty()&&out.back()!='_')out+='_';}while(!out.empty()&&out.back()=='_')out.pop_back();return out.empty()?"x":out;}

// ---------------------------------------------------------------- Setting
Setting* SettingFrom(lua_State* L,int idx,Module** owner=nullptr){luaL_checktype(L,idx,LUA_TTABLE);lua_rawgetfield(L,idx,"__m");lua_rawgetfield(L,idx,"__s");const int m=(int)lua_tonumber(L,-2),s=(int)lua_tonumber(L,-1);lua_pop(L,2);
 if(m<0||m>=(int)modules.size()||s<0||s>=(int)modules[m].settings.size())luaL_error(L,"setting no longer exists");if(owner)*owner=&modules[m];return &modules[m].settings[s];}
void PushSettingValue(lua_State* L,const Setting& st){
 if(st.kind=="bool")lua_pushboolean(L,st.number!=0);else if(st.kind=="number")lua_pushnumber(L,st.number);else if(st.kind=="color")PushColor(L,st.text);
 else if(st.kind=="key"){const size_t bar=st.text.find('|');const int code=bar==std::string::npos?0:atoi(st.text.c_str()+bar+1);lua_pushstring(L,ScriptKeyName(code).c_str());}else lua_pushstring(L,st.text.c_str());}
int SettingGet(lua_State* L){PushSettingValue(L,*SettingFrom(L,1));return 1;}
int SettingName(lua_State* L){lua_pushstring(L,SettingFrom(L,1)->name.c_str());return 1;}
int SettingType(lua_State* L){Setting* st=SettingFrom(L,1);lua_rawgetfield(L,1,"__t");if(lua_isstring(L,-1))return 1;lua_pop(L,1);lua_pushstring(L,st->kind.c_str());return 1;}
int SettingSet(lua_State* L){Module* m=nullptr;Setting* st=SettingFrom(L,1,&m);
 if(st->kind=="bool")st->number=lua_toboolean(L,2)?1:0;
 else if(st->kind=="number"||st->kind=="range"){double v=luaL_checknumber(L,2);if(st->step>=1)v=std::round(v);st->number=std::clamp(v,st->minimum,st->maximum);if(st->kind=="range"&&lua_isnumber(L,3))st->text=std::to_string(std::clamp(lua_tonumber(L,3),st->minimum,st->maximum));}
 else if(st->kind=="color")st->text=FormatColor(ReadColor(L,2));
 else if(st->kind=="enum"){std::string v=luaL_checkstring(L,2);if(std::find(st->options.begin(),st->options.end(),v)==st->options.end())luaL_error(L,"'%s' is not an option of %s",v.c_str(),st->name.c_str());st->text=v;}
 else if(st->kind=="key"){std::string v=luaL_checkstring(L,2);const int code=ScriptKeyCode(v);st->text=code?ScriptKeyName(code)+"|"+std::to_string(code):"";}
 else st->text=luaL_checkstring(L,2);
 Save(*m,st);return 0;}
void PushSetting(lua_State* L,int module,int setting,const char* type=nullptr){lua_createtable(L,0,3);lua_pushnumber(L,module);lua_setfield(L,-2,"__m");lua_pushnumber(L,setting);lua_setfield(L,-2,"__s");if(type){lua_pushstring(L,type);lua_setfield(L,-2,"__t");}
 luaL_getmetatable(L,"NickelSetting");lua_setmetatable(L,-2);}

// ---------------------------------------------------------------- script module
ScriptModule& ModuleOf(lua_State* L,int idx){luaL_checktype(L,idx,LUA_TTABLE);lua_rawgetfield(L,idx,"__id");const int id=(int)lua_tonumber(L,-1);lua_pop(L,1);Script* s=ScriptOf(L);if(id<0||id>=(int)s->mods.size())luaL_error(L,"invalid module");return s->mods[id];}
void RequireLoading(lua_State* L,const char* what){if(!ScriptOf(L)->loading)luaL_error(L,"%s can only be declared while the script loads",what);}
int AddSetting(lua_State* L,const char* kind,const char* type){
 RequireLoading(L,"settings");ScriptModule& sm=ModuleOf(L,1);Module& m=modules[sm.index];std::string name=luaL_checkstring(L,2);if(name.size()>48)name.resize(48);
 Setting st{};st.id=Slug(name);st.name=name;st.kind=kind;st.minimum=0;st.maximum=1;st.step=.01;st.number=0;
 if(!strcmp(type,"boolean")){st.step=1;st.number=lua_toboolean(L,3)?1:0;}
 else if(!strcmp(type,"number")||!strcmp(type,"integer")){const bool whole=!strcmp(type,"integer");st.number=luaL_checknumber(L,3);st.minimum=luaL_checknumber(L,4);st.maximum=luaL_checknumber(L,5);st.step=luaL_optnumber(L,6,whole?1:.1);
  if(whole)st.step=std::max(1.0,std::round(st.step));if(st.maximum<st.minimum)std::swap(st.minimum,st.maximum);st.number=std::clamp(st.number,st.minimum,st.maximum);}
 else if(!strcmp(type,"choice")){luaL_checktype(L,3,LUA_TTABLE);for(int i=1;;++i){lua_rawgeti(L,3,i);if(lua_isnil(L,-1)){lua_pop(L,1);break;}st.options.push_back(lua_tostring(L,-1)?lua_tostring(L,-1):"");lua_pop(L,1);}if(st.options.empty())luaL_error(L,"choice needs at least one option");st.text=st.options[0];st.maximum=1;}
 else if(!strcmp(type,"color"))st.text=FormatColor(ReadColor(L,3));
 else if(!strcmp(type,"text"))st.text=luaL_optstring(L,3,"");
 else if(!strcmp(type,"key")){const char* k=luaL_optstring(L,3,"");const int code=ScriptKeyCode(k);st.text=code?ScriptKeyName(code)+"|"+std::to_string(code):"";}
 int index=-1;for(int i=0;i<(int)m.settings.size();++i)if(m.settings[i].id==st.id)index=i;
 if(index<0){if(m.settings.size()>=128)luaL_error(L,"too many settings");m.settings.push_back(st);index=(int)m.settings.size()-1;}
 else{Setting& old=m.settings[index];const bool keep=old.kind==st.kind&&(st.kind!="enum"||std::find(st.options.begin(),st.options.end(),old.text)!=st.options.end());Setting fresh=st;if(keep){fresh.number=st.kind=="number"?std::clamp(old.number,st.minimum,st.maximum):old.number;fresh.text=old.text;}old=fresh;}
 // Saved value (module-preferences.ini), as ReadPreferences does for built-in modules.
 {Setting& t=m.settings[index];static wchar_t text[8192];text[0]=0;GetPrivateProfileStringW(Wide(m.id).c_str(),Wide(t.id).c_str(),L"",text,8192,configPath.c_str());
  if(text[0]){if(t.kind=="number"||t.kind=="bool"){wchar_t* end{};double v=wcstod(text,&end);if(end&&!*end&&std::isfinite(v)&&v>=t.minimum&&v<=t.maximum)t.number=v;}else{auto v=Utf8(text);if(t.kind=="color"?(v.size()==9&&v[0]=='#'):(t.kind!="enum"||std::find(t.options.begin(),t.options.end(),v)!=t.options.end()))t.text=v;}}}
 PushSetting(L,sm.index,index,type);return 1;}
int ModBoolean(lua_State* L){return AddSetting(L,"bool","boolean");}
int ModNumber(lua_State* L){return AddSetting(L,"number","number");}
int ModInteger(lua_State* L){return AddSetting(L,"number","integer");}
int ModChoice(lua_State* L){return AddSetting(L,"enum","choice");}
int ModColor(lua_State* L){return AddSetting(L,"color","color");}
int ModText(lua_State* L){return AddSetting(L,"text","text");}
int ModKey(lua_State* L){return AddSetting(L,"key","key");}
int ModOnEnable(lua_State* L){ScriptModule& m=ModuleOf(L,1);luaL_checktype(L,2,LUA_TFUNCTION);if(m.enableRef)lua_unref(L,m.enableRef);m.enableRef=lua_ref(L,2);return 0;}
int ModOnDisable(lua_State* L){ScriptModule& m=ModuleOf(L,1);luaL_checktype(L,2,LUA_TFUNCTION);if(m.disableRef)lua_unref(L,m.disableRef);m.disableRef=lua_ref(L,2);return 0;}
const char* scriptEvents[]={"update","tick","render2d","render3d","attack","packetSend","packetReceive","chat","key","move","jump","hurt","death","respawn","join","leave","dimension","screen","containerOpen","containerClose","pathEnd","teleport","hit"};
int ModOn(lua_State* L){ScriptModule& m=ModuleOf(L,1);std::string event=luaL_checkstring(L,2);luaL_checktype(L,3,LUA_TFUNCTION);bool known=false;for(auto e:scriptEvents)if(event==e)known=true;if(!known)luaL_error(L,"unknown event '%s'",event.c_str());
 if(m.handlers.size()>=64)luaL_error(L,"too many event handlers");m.handlers.push_back({event,lua_ref(L,3)});return 0;}
int ModIsEnabled(lua_State* L){ScriptModule& m=ModuleOf(L,1);lua_pushboolean(L,modules[m.index].enabled);return 1;}
int ModSetEnabled(lua_State* L){ScriptModule& m=ModuleOf(L,1);const bool on=lua_toboolean(L,2)!=0;if(modules[m.index].enabled!=on){modules[m.index].enabled=on;Save(modules[m.index]);}return 0;}
int ModHud(lua_State* L){RequireLoading(L,"HUD elements");ScriptModule& m=ModuleOf(L,1);m.hud=true;m.hudW=(float)std::clamp(luaL_checknumber(L,2),1.0,4000.0);m.hudH=(float)std::clamp(luaL_checknumber(L,3),1.0,4000.0);return 0;}
int ModHudFrame(lua_State* L){ScriptModule& m=ModuleOf(L,1);if(!m.hud||!m.frame){for(int i=0;i<5;++i)lua_pushnil(L);return 5;}lua_pushnumber(L,m.fx);lua_pushnumber(L,m.fy);lua_pushnumber(L,m.fs);lua_pushnumber(L,m.fw);lua_pushnumber(L,m.fh);return 5;}
// Rotation: Camera mode only here (Silent/Visible/Packet modes go through the rotation manager in script_api21.hpp).
int ModRotate(lua_State* L){ScriptModule& m=ModuleOf(L,1);m.yaw=(float)luaL_checknumber(L,2);m.pitch=(float)std::clamp(luaL_checknumber(L,3),-90.0,90.0);m.rotating=true;return 0;}
bool LocalEye(float eye[3]);bool EntityPosition(uint64_t id,float out[3]);
int ModAimAt(lua_State* L){ScriptModule& m=ModuleOf(L,1);float target[3],eye[3];if(lua_isnumber(L,2)){if(!EntityPosition((uint64_t)lua_tonumber(L,2),target)){lua_pushboolean(L,0);return 1;}target[1]+=1.4f;}
 else if(!ReadVector(L,2,target)){lua_pushboolean(L,0);return 1;}if(!LocalEye(eye)){lua_pushboolean(L,0);return 1;}
 const float dx=target[0]-eye[0],dy=target[1]-eye[1],dz=target[2]-eye[2];m.yaw=std::atan2(dz,dx)*57.29578f-90;m.pitch=-std::atan2(dy,std::sqrt(dx*dx+dz*dz))*57.29578f;m.rotating=true;lua_pushboolean(L,1);return 1;}
int ModUseItem(lua_State* L){ScriptModule& m=ModuleOf(L,1);if(lua_istable(L,2)){lua_rawgetfield(L,2,"done");if(lua_isfunction(L,-1)){lua_pushboolean(L,0);lua_call(L,1,0);}else lua_pop(L,1);if(OptBool(L,2,"disable",false)&&modules[m.index].enabled){modules[m.index].enabled=false;Save(modules[m.index]);}}return 0;}

void ScriptHideModules(Script& s){for(auto& m:s.mods)if(m.index>=0){Module& mod=modules[m.index];if(mod.enabled&&s.state!=1){}mod.category.clear();}}
int ScriptModuleDecl(lua_State* L){
 RequireLoading(L,"modules");Script* s=ScriptOf(L);luaL_checktype(L,1,LUA_TTABLE);std::string name=OptStr(L,1,"name","");std::string category=OptStr(L,1,"category","");if(name.empty())luaL_error(L,"a module needs a name");if(name.size()>48)luaL_error(L,"module names are at most 48 characters");
 static const char* cats[][2]={{"Combat","Combat"},{"Movement","Movement"},{"Player","Player"},{"Visual","Visuals"},{"Utility","Utility"},{"HUD","HUD"}};const char* menuCategory=nullptr;for(auto& c:cats)if(category==c[0])menuCategory=c[1];
 if(!menuCategory)luaL_error(L,"category must be Combat, Movement, Player, Visual, Utility or HUD");
 for(auto& m:s->mods)if(m.name==name)luaL_error(L,"this script already has a module named '%s'",name.c_str());
 const std::string id="script."+Slug(s->name)+"."+Slug(name);int index=-1;for(int i=0;i<(int)modules.size();++i)if(modules[i].id==id)index=i;
 if(index<0){if(modules.size()>=modules.capacity())luaL_error(L,"too many script modules");Module fresh{};fresh.id=id;modules.push_back(fresh);index=(int)modules.size()-1;
  modules[index].enabled=GetPrivateProfileIntW(Wide(id).c_str(),L"enabled",0,configPath.c_str())==1;
  wchar_t bind[65]{};GetPrivateProfileStringW(Wide(id).c_str(),L"keybind",L"",bind,65,configPath.c_str());const int code=GetPrivateProfileIntW(Wide(id).c_str(),L"keycode",0,configPath.c_str());
  if(bind[0]&&code>0){keybinds[id]=Utf8(bind);keycodes[id]=code;}else{const std::string key=OptStr(L,1,"key","");const int vk=ScriptKeyCode(key);if(vk){keybinds[id]=ScriptKeyName(vk);keycodes[id]=vk;}}}
 Module& m=modules[index];m.name=name;m.category=menuCategory;m.description=OptStr(L,1,"description","");m.settings.clear();
 ScriptModule sm{};sm.index=index;sm.name=name;const std::string act=OptStr(L,1,"activation","Toggle");sm.activation=act=="Hold"?1:act=="OneShot"?2:0;if(act!="Toggle"&&act!="Hold"&&act!="OneShot")luaL_error(L,"activation must be Toggle, Hold or OneShot");
 sm.was=false;s->mods.push_back(sm);
 lua_createtable(L,0,1);lua_pushnumber(L,(double)s->mods.size()-1);lua_setfield(L,-2,"__id");luaL_getmetatable(L,"NickelModule");lua_setmetatable(L,-2);return 1;}

// ---------------------------------------------------------------- commands
int ScriptCommandDecl(lua_State* L){RequireLoading(L,"commands");Script* s=ScriptOf(L);luaL_checktype(L,1,LUA_TTABLE);ScriptCommand c{};c.name=OptStr(L,1,"name","");c.description=OptStr(L,1,"description","");
 if(c.name.empty())luaL_error(L,"a command needs a name");for(char ch:c.name)if(!std::isalnum((unsigned char)ch)&&ch!='_'&&ch!='-')luaL_error(L,"command names use letters, digits, _ and -");if(c.description.size()>80)luaL_error(L,"descriptions are at most 80 characters");
 lua_pushvalue(L,1);c.specRef=lua_ref(L,-1);lua_pop(L,1);s->commands.push_back(c);return 0;}

// ---------------------------------------------------------------- built-in modules
int BuiltinFrom(lua_State* L,int idx){luaL_checktype(L,idx,LUA_TTABLE);lua_rawgetfield(L,idx,"__m");const int m=(int)lua_tonumber(L,-1);lua_pop(L,1);if(m<0||m>=(int)modules.size())luaL_error(L,"module no longer exists");return m;}
int BuiltinName(lua_State* L){lua_pushstring(L,modules[BuiltinFrom(L,1)].name.c_str());return 1;}
int BuiltinCategory(lua_State* L){const std::string c=modules[BuiltinFrom(L,1)].category;lua_pushstring(L,c=="Visuals"?"Visual":c.c_str());return 1;}
int BuiltinIsEnabled(lua_State* L){lua_pushboolean(L,modules[BuiltinFrom(L,1)].enabled);return 1;}
int BuiltinSetEnabled(lua_State* L){Module& m=modules[BuiltinFrom(L,1)];const bool on=lua_toboolean(L,2)!=0;if(m.enabled!=on){m.enabled=on;Save(m);}return 0;}
int BuiltinToggle(lua_State* L){Module& m=modules[BuiltinFrom(L,1)];m.enabled=!m.enabled;Save(m);return 0;}
int BuiltinSettings(lua_State* L){Module& m=modules[BuiltinFrom(L,1)];lua_createtable(L,(int)m.settings.size(),0);int n=0;for(auto& st:m.settings)if(st.kind!="hidden"){lua_pushstring(L,st.name.c_str());lua_rawseti(L,-2,++n);}return 1;}
int BuiltinSetting(lua_State* L){const int mi=BuiltinFrom(L,1);std::string name=luaL_checkstring(L,2);auto& m=modules[mi];for(int i=0;i<(int)m.settings.size();++i)if(m.settings[i].kind!="hidden"&&(m.settings[i].name==name||m.settings[i].id==name)){PushSetting(L,mi,i);return 1;}lua_pushnil(L);return 1;}
int ModulesList(lua_State* L){lua_createtable(L,(int)modules.size(),0);int n=0;for(auto& m:modules)if(!m.category.empty()){lua_pushstring(L,m.name.c_str());lua_rawseti(L,-2,++n);}return 1;}
int ModulesGet(lua_State* L){std::string name=luaL_checkstring(L,1);for(int i=0;i<(int)modules.size();++i)if(!modules[i].category.empty()&&modules[i].name==name){lua_createtable(L,0,1);lua_pushnumber(L,i);lua_setfield(L,-2,"__m");luaL_getmetatable(L,"NickelBuiltin");lua_setmetatable(L,-2);return 1;}lua_pushnil(L);return 1;}

// ---------------------------------------------------------------- client, chat, storage
std::mutex scriptChatGuard;std::vector<std::pair<int,std::string>> scriptChatQueue;   // 0 print, 1 send, 2 command; flushed on the game thread
int ChatPrint(lua_State* L){std::lock_guard<std::mutex> lock(scriptChatGuard);if(scriptChatQueue.size()<256)scriptChatQueue.push_back({0,luaL_checkstring(L,1)});return 0;}
int ChatSend(lua_State* L){std::string m=luaL_checkstring(L,1);if(m.size()>512)m.resize(512);std::lock_guard<std::mutex> lock(scriptChatGuard);if(scriptChatQueue.size()<256)scriptChatQueue.push_back({1,m});return 0;}
int ChatCommand(lua_State* L){std::string m=luaL_checkstring(L,1);std::lock_guard<std::mutex> lock(scriptChatGuard);if(scriptChatQueue.size()<256)scriptChatQueue.push_back({2,m});return 0;}
int ClientNotify(lua_State* L){std::string title=luaL_checkstring(L,1),text=luaL_optstring(L,2,"");if(notices.size()<16)notices.push_back({8,title,text});return 0;}
int ClientTime(lua_State* L){lua_pushnumber(L,ScriptNow()*1000);return 1;}
volatile long scriptTicks=0;
int ClientTick(lua_State* L){lua_pushnumber(L,(double)scriptTicks);return 1;}
int AddTimer(lua_State* L,bool repeat){Script* s=ScriptOf(L);const double ms=std::max(0.0,luaL_checknumber(L,1));luaL_checktype(L,2,LUA_TFUNCTION);if(s->timers.size()>=256)luaL_error(L,"a script can have at most 256 timers");
 ScriptTimer t{s->nextTimer++,ScriptNow()+ms/1000,repeat?std::max(ms,1.0)/1000:0,lua_ref(L,2)};s->timers.push_back(t);lua_pushnumber(L,t.id);return 1;}
int ClientAfter(lua_State* L){return AddTimer(L,false);}
int ClientEvery(lua_State* L){return AddTimer(L,true);}
int ClientCancel(lua_State* L){Script* s=ScriptOf(L);const int id=(int)luaL_checknumber(L,1);for(size_t i=0;i<s->timers.size();++i)if(s->timers[i].id==id){lua_unref(L,s->timers[i].ref);s->timers.erase(s->timers.begin()+i);lua_pushboolean(L,1);return 1;}lua_pushboolean(L,0);return 1;}
int ClientIsKeyDown(lua_State* L){const int code=ScriptKeyCode(luaL_checkstring(L,1));lua_pushboolean(L,code&&(GetAsyncKeyState(code)&0x8000)!=0);return 1;}
int ClientServer(lua_State* L){game::Server server{};lua_createtable(L,0,3);if(game::ReadServer(&server)){lua_pushboolean(L,server.singleplayer);lua_setfield(L,-2,"local");if(!server.singleplayer){lua_pushstring(L,server.address);lua_setfield(L,-2,"address");lua_pushnumber(L,server.port);lua_setfield(L,-2,"port");}}else{lua_pushboolean(L,1);lua_setfield(L,-2,"local");}return 1;}
int ClientTransport(lua_State* L){lua_pushstring(L,game::TransportName(game::CurrentTransport()));return 1;}   // "RakNet", "NetherNet" or "None"
int ClientPing(lua_State* L){int64_t ms=0;game::PingTrace trace{};if(game::ReadPing(&ms,&trace)==1)lua_pushnumber(L,(double)ms);else lua_pushnil(L);return 1;}
// storage: one simplified JSON file per script (%APPDATA%\Nickel\scripts\.storage\<name>.txt), written by the client.
std::wstring StorageFile(const Script& s){return ScriptFolder()+L"\\.storage\\"+Wide(Slug(s.name))+L".txt";}
void Serialize(lua_State* L,int idx,std::string& out,int depth){
 if(depth>16)luaL_error(L,"storage values are nested at most 16 levels");const int t=lua_type(L,idx);
 if(t==LUA_TNIL)out+="n";else if(t==LUA_TBOOLEAN)out+=lua_toboolean(L,idx)?"t":"f";else if(t==LUA_TNUMBER){char b[40];snprintf(b,sizeof(b),"d%.17g;",lua_tonumber(L,idx));out+=b;}
 else if(t==LUA_TSTRING){size_t n;const char* s=lua_tolstring(L,idx,&n);out+="s"+std::to_string(n)+":";out.append(s,n);}
 else if(t==LUA_TTABLE){out+="{";const int abs=idx<0?lua_gettop(L)+idx+1:idx;lua_pushnil(L);while(lua_next(L,abs)){Serialize(L,-2,out,depth+1);Serialize(L,-1,out,depth+1);lua_pop(L,1);}out+="}";}
 else luaL_error(L,"storage keeps nil, booleans, numbers, strings and tables of those");}
bool Deserialize(lua_State* L,const std::string& in,size_t& at,int depth){if(at>=in.size()||depth>17)return false;const char c=in[at++];
 if(c=='n')lua_pushnil(L);else if(c=='t')lua_pushboolean(L,1);else if(c=='f')lua_pushboolean(L,0);
 else if(c=='d'){size_t end=in.find(';',at);if(end==std::string::npos)return false;lua_pushnumber(L,atof(in.substr(at,end-at).c_str()));at=end+1;}
 else if(c=='s'){size_t colon=in.find(':',at);if(colon==std::string::npos)return false;const size_t n=(size_t)atoll(in.substr(at,colon-at).c_str());if(colon+1+n>in.size())return false;lua_pushlstring(L,in.data()+colon+1,n);at=colon+1+n;}
 else if(c=='{'){lua_newtable(L);while(at<in.size()&&in[at]!='}'){if(!Deserialize(L,in,at,depth+1)){lua_pop(L,1);return false;}if(!Deserialize(L,in,at,depth+1)){lua_pop(L,2);return false;}if(lua_isnil(L,-2)){lua_pop(L,2);continue;}lua_rawset(L,-3);}if(at>=in.size())return false;++at;}
 else return false;return true;}
std::string ReadAll(const std::wstring& path){FILE* f=nullptr;if(_wfopen_s(&f,path.c_str(),L"rb")||!f)return {};std::string data;char buf[8192];size_t n;while((n=fread(buf,1,sizeof(buf),f))>0)data.append(buf,n);fclose(f);return data;}
int StorageGet(lua_State* L){Script* s=ScriptOf(L);const std::string key=luaL_checkstring(L,1);const std::string all=ReadAll(StorageFile(*s));size_t at=0;
 if(!all.empty()&&Deserialize(L,all,at,0)&&lua_istable(L,-1)){lua_rawgetfield(L,-1,key.c_str());lua_remove(L,-2);return 1;}lua_settop(L,1);lua_pushnil(L);return 1;}
int StorageSet(lua_State* L){Script* s=ScriptOf(L);const std::string key=luaL_checkstring(L,1);if(lua_isvector(L,2))luaL_error(L,"vectors aren't stored; save { x, y, z } tables");
 const std::string all=ReadAll(StorageFile(*s));size_t at=0;if(all.empty()||!Deserialize(L,all,at,0)||!lua_istable(L,-1)){lua_settop(L,2);lua_newtable(L);}
 lua_pushvalue(L,2);lua_setfield(L,-2,key.c_str());std::string out;Serialize(L,-1,out,0);lua_pop(L,1);
 CreateDirectoryW((ScriptFolder()+L"\\.storage").c_str(),nullptr);FILE* f=nullptr;if(!_wfopen_s(&f,StorageFile(*s).c_str(),L"wb")&&f){fwrite(out.data(),1,out.size(),f);fclose(f);}return 0;}

// ---------------------------------------------------------------- player / world (reads, render or game thread)
void PushVector(lua_State* L,const float v[3]){lua_pushvector(L,v[0],v[1],v[2]);}
bool LocalEye(float eye[3]){game::Player p{};if(!game::ReadPlayer(&p))return false;eye[0]=p.position[0];eye[1]=p.position[1];eye[2]=p.position[2];return true;}   // player position is at eye height (state +0x240)
bool LocalRotation(float* yaw,float* pitch){game::Context c{};uint64_t rot{};if(!game::Local(&c)||!game::Read(c.actor+0x250,&rot)||!rot)return false;return game::Read(rot,pitch)&&game::Read(rot+4,yaw);}
bool EntityPosition(uint64_t id,float out[3]){game::Context c{};uintptr_t actor{};uint64_t state{};return game::Local(&c)&&game::ActorByRuntimeId(c.registry,id,&actor)&&game::Read(actor+0x240,&state)&&state&&game::Floats(state,out,3);}
int PlayerInWorld(lua_State* L){game::Context c{};lua_pushboolean(L,game::Local(&c));return 1;}
int PlayerPosition(lua_State* L){game::Context c{};uintptr_t off{};float eye[3],drop=1.62f;if(!LocalEye(eye)){lua_pushnil(L);return 1;}if(game::Local(&c)&&game::Element(c.registry,c.entity,"OffsetsComponent",0x44,&off))game::Floats(off,&drop,1);eye[1]-=drop;PushVector(L,eye);return 1;}
int PlayerEye(lua_State* L){float eye[3];if(!LocalEye(eye)){lua_pushnil(L);return 1;}PushVector(L,eye);return 1;}
int PlayerVelocity(lua_State* L){game::Player p{};if(!game::ReadPlayer(&p)){lua_pushnil(L);return 1;}PushVector(L,p.velocity);return 1;}
int PlayerRotation(lua_State* L){float yaw=0,pitch=0;LocalRotation(&yaw,&pitch);lua_pushnumber(L,yaw);lua_pushnumber(L,pitch);return 2;}
int PlayerAnglesTo(lua_State* L){float t[3],eye[3];if(!ReadVector(L,1,t)||!LocalEye(eye)){lua_pushnil(L);lua_pushnil(L);return 2;}const float dx=t[0]-eye[0],dy=t[1]-eye[1],dz=t[2]-eye[2];
 lua_pushnumber(L,std::atan2(dz,dx)*57.29578f-90);lua_pushnumber(L,-std::atan2(dy,std::sqrt(dx*dx+dz*dz))*57.29578f);return 2;}
int PlayerHealth(lua_State* L){float h{},m{};if(game::ReadHealth(&h,&m))lua_pushnumber(L,h);else lua_pushnil(L);return 1;}
int PlayerName(lua_State* L){uint64_t id{};char name[96]{};if(game::LocalIdentity(&id,name,sizeof(name)))lua_pushstring(L,name);else lua_pushstring(L,"");return 1;}
int PlayerRuntimeId(lua_State* L){uint64_t id{};if(game::LocalIdentity(&id,nullptr,0))lua_pushnumber(L,(double)id);else lua_pushnil(L);return 1;}
int PlayerUniqueId(lua_State* L){game::Context c{};uintptr_t e{};uint64_t v{};if(game::Local(&c)&&game::Element(c.registry,c.entity,"ActorUniqueIDComponent",8,&e)&&game::Read(e,&v))lua_pushnumber(L,(double)(int64_t)v);else lua_pushnil(L);return 1;}
int PlayerOnGround(lua_State* L){game::Player p{};lua_pushboolean(L,game::ReadPlayer(&p)&&p.onGround);return 1;}
int PlayerFlags(lua_State* L,int bit){game::DebugInfo d{};lua_pushboolean(L,game::ReadDebug(&d,false)&&((bit==1&&d.sneak)||(bit==3&&d.sprint)));return 1;}
int PlayerSneaking(lua_State* L){return PlayerFlags(L,1);}
int PlayerSprinting(lua_State* L){return PlayerFlags(L,3);}
int PlayerGameMode(lua_State* L){game::DebugInfo d{};static const char* n[]={"Survival","Creative","Adventure","Spectator"};const bool ok=game::ReadDebug(&d,false);lua_pushstring(L,ok&&d.gameMode>=0&&d.gameMode<4?n[d.gameMode]:ok&&d.gameMode==5?"Default":"Undefined");return 1;}
int PlayerFall(lua_State* L){game::Context c{};uintptr_t storage{},element{};float fall=0;if(game::Local(&c)&&game::Storage(c.registry,0xce6b34f6u,&storage)&&game::InStorage(storage,c.entity,4,&element))game::Read(element,&fall);lua_pushnumber(L,fall);return 1;}
int PlayerHas(lua_State* L,uint32_t component){game::Context c{};uintptr_t storage{};uint32_t packed{};lua_pushboolean(L,game::Local(&c)&&game::Storage(c.registry,component,&storage)&&game::Packed(storage,c.entity,&packed));return 1;}
int PlayerInWater(lua_State* L){return PlayerHas(L,0x78e89f39u);}
int PlayerInLava(lua_State* L){return PlayerHas(L,game::Fnv("InLavaFlagComponent"));}
int PlayerSlot(lua_State* L){game::Context c{};uint64_t inv{};int32_t slot=0;if(game::Local(&c)&&game::Read(c.actor+0x5e0,&inv)&&inv)game::Read(inv+0x10,&slot);lua_pushnumber(L,slot+1);return 1;}
// Player writes: requested here, applied during the tick (game thread).
struct ScriptWrites{std::mutex guard;bool velocity=false,add=false,jump=false,swing=false,teleport=false;int slot=-1;float v[3]{},a[3]{},t[3]{};};ScriptWrites scriptWrites;
int PlayerSetVelocity(lua_State* L){float v[3];if(!ReadVectorStrict(L,1,v))luaL_error(L,"vector with finite x, y and z expected");std::lock_guard<std::mutex> lock(scriptWrites.guard);scriptWrites.velocity=true;memcpy(scriptWrites.v,v,12);return 0;}
int PlayerAddVelocity(lua_State* L){float v[3];if(!ReadVectorStrict(L,1,v))luaL_error(L,"vector with finite x, y and z expected");std::lock_guard<std::mutex> lock(scriptWrites.guard);if(!scriptWrites.add){scriptWrites.add=true;memset(scriptWrites.a,0,12);}for(int i=0;i<3;++i)scriptWrites.a[i]+=v[i];return 0;}
int PlayerTeleport(lua_State* L){float v[3];if(!ReadVectorStrict(L,1,v))luaL_error(L,"vector with finite x, y and z expected");
 game::Context c{};if(!game::Local(&c)){lua_pushboolean(L,0);lua_pushstring(L,"not in a world");return 2;}
 {std::lock_guard<std::mutex> lock(scriptWrites.guard);scriptWrites.teleport=true;memcpy(scriptWrites.t,v,12);}lua_pushboolean(L,1);return 1;}   // feet position (as player.position), applied on the next tick
int PlayerSetSlot(lua_State* L){const int slot=(int)luaL_checknumber(L,1);if(slot<1||slot>9)luaL_error(L,"slots are 1 to 9");std::lock_guard<std::mutex> lock(scriptWrites.guard);scriptWrites.slot=slot-1;return 0;}
int PlayerJump(lua_State* L){game::Player p{};const bool can=game::ReadPlayer(&p)&&p.onGround;if(can){std::lock_guard<std::mutex> lock(scriptWrites.guard);scriptWrites.jump=true;}lua_pushboolean(L,can);return 1;}
int PlayerSwing(lua_State* L){std::lock_guard<std::mutex> lock(scriptWrites.guard);scriptWrites.swing=true;return 0;}
int Unsupported(lua_State* L){lua_pushnil(L);return 1;}
int UnsupportedFalse(lua_State* L){lua_pushboolean(L,0);return 1;}
int UnsupportedPair(lua_State* L){lua_pushboolean(L,0);lua_pushstring(L,"not available in Nickel yet");return 2;}
int Nothing(lua_State*){return 0;}
// Entities: snapshots (id, type, name, player, local, health, positions, bounding box).
struct ScriptEntity{uint64_t id;bool player,local;char name[96];float health,pos[3],eye[3],vel[3],lo[3],hi[3];bool box;int type;};
int ReadScriptEntities(ScriptEntity* out,int capacity,float radius,bool playersOnly){
 game::Context c{};uintptr_t owners{},positions{};float origin[3];if(!game::Local(&c)||!game::Storage(c.registry,game::Fnv("ActorOwnerComponent"),&owners)||!LocalEye(origin))return 0;
 uint64_t begin{},end{};if(!game::Read(owners+0x20,&begin)||!game::Read(owners+0x28,&end)||end<begin||(end-begin)%4||(end-begin)>4*65536)return 0;int n=0;
 for(uint64_t at=begin;at<end&&n<capacity;at+=4){uint32_t entity{};uintptr_t slot{},element{};uint64_t actor{},state{},box{};if(!game::Read(at,&entity))break;if(entity>=0xfffc0000u)continue;
  if(!game::InStorage(owners,entity,8,&slot)||!game::Read(slot,&actor)||!actor||!game::Read(actor+0x240,&state)||!state)continue;ScriptEntity e{};if(!game::Floats(state,e.pos,3))continue;game::Floats(state+0x18,e.vel,3);
  const float dx=e.pos[0]-origin[0],dy=e.pos[1]-origin[1],dz=e.pos[2]-origin[2];if(radius>0&&dx*dx+dy*dy+dz*dz>radius*radius)continue;
  e.player=game::Has(c.registry,entity,"PlayerComponent");if(playersOnly&&!e.player)continue;e.local=entity==c.entity;
  if(!game::Element(c.registry,entity,"RuntimeIDComponent",8,&element)||!game::Read(element,&e.id))continue;
  if(e.player){if(!game::Text(actor+0xbe8,e.name,sizeof(e.name)))e.name[0]=0;}else game::EntityName(c,actor,e.name,sizeof(e.name));
  float mx{};e.health=-1;game::Attribute(c.registry,entity,6,&e.health,&mx);int32_t type{};if(game::Element(c.registry,entity,"ActorTypeComponent",4,&element)&&game::Read(element,&type))e.type=type;
  memcpy(e.eye,e.pos,12);float b[6],drop=0;if(game::Read(actor+0x248,&box)&&box&&game::Floats(box,b,6)){e.box=true;if(game::Element(c.registry,entity,"OffsetsComponent",0x44,&element))game::Floats(element,&drop,1);
   const float hx=(b[3]-b[0])/2,hz=(b[5]-b[2])/2,low=e.pos[1]-drop;e.lo[0]=e.pos[0]-hx;e.lo[1]=low;e.lo[2]=e.pos[2]-hz;e.hi[0]=e.pos[0]+hx;e.hi[1]=low+(b[4]-b[1]);e.hi[2]=e.pos[2]+hz;e.pos[1]=low;}
  out[n++]=e;}
 return n;}
void PushEntity(lua_State* L,const ScriptEntity& e){lua_createtable(L,0,16);lua_pushnumber(L,(double)e.id);lua_setfield(L,-2,"id");lua_pushstring(L,e.player?"player":("entity."+std::to_string(e.type&0xff)).c_str());lua_setfield(L,-2,"type");
 if(e.name[0]){lua_pushstring(L,e.name);lua_setfield(L,-2,"name");}lua_pushboolean(L,e.player);lua_setfield(L,-2,"isPlayer");lua_pushboolean(L,e.local);lua_setfield(L,-2,"isLocal");
 lua_pushboolean(L,0);lua_setfield(L,-2,"teammate");lua_pushboolean(L,0);lua_setfield(L,-2,"bot");if(e.health>=0){lua_pushnumber(L,e.health);lua_setfield(L,-2,"health");}
 PushVector(L,e.pos);lua_setfield(L,-2,"position");PushVector(L,e.eye);lua_setfield(L,-2,"eyePosition");PushVector(L,e.vel);lua_setfield(L,-2,"velocity");if(e.box){PushVector(L,e.lo);lua_setfield(L,-2,"boxMin");PushVector(L,e.hi);lua_setfield(L,-2,"boxMax");}}
int WorldEntities(lua_State* L){bool players=false;float radius=0;std::string type;if(lua_isboolean(L,1))players=lua_toboolean(L,1)!=0;else if(lua_istable(L,1)){players=OptBool(L,1,"players",false);radius=(float)OptNum(L,1,"radius",0);type=OptStr(L,1,"type","");}
 static ScriptEntity list[512];const int n=ReadScriptEntities(list,512,radius,players);lua_createtable(L,n,0);int k=0;for(int i=0;i<n;++i){if(!type.empty()&&type!="player"&&list[i].player)continue;PushEntity(L,list[i]);lua_rawseti(L,-2,++k);}return 1;}
int WorldEntity(lua_State* L){const uint64_t id=(uint64_t)luaL_checknumber(L,1);static ScriptEntity list[512];const int n=ReadScriptEntities(list,512,0,false);for(int i=0;i<n;++i)if(list[i].id==id){PushEntity(L,list[i]);return 1;}lua_pushnil(L);return 1;}
int WorldTarget(lua_State* L){const float range=(float)OptNum(L,1,"range",6);const bool playersOnly=OptBool(L,1,"playersOnly",true);static ScriptEntity list[512];const int n=ReadScriptEntities(list,512,range+2,playersOnly);float eye[3];if(!LocalEye(eye)){lua_pushnil(L);return 1;}
 int best=-1;float bestD=1e9f;for(int i=0;i<n;++i){if(list[i].local||!(list[i].health!=0))continue;const float dx=list[i].pos[0]-eye[0],dy=list[i].pos[1]+.9f-eye[1],dz=list[i].pos[2]-eye[2],d=std::sqrt(dx*dx+dy*dy+dz*dz);if(d<=range&&d<bestD){bestD=d;best=i;}}
 if(best<0)lua_pushnil(L);else PushEntity(L,list[best]);return 1;}
int WorldDimension(lua_State* L){int d=-1;static const char* n[]={"Overworld","Nether","TheEnd"};if(game::ReadDimension(&d)&&d>=0&&d<3)lua_pushstring(L,n[d]);else lua_pushnil(L);return 1;}
int WorldTime(lua_State* L){lua_pushnil(L);return 1;}   // the world time is not read yet: nil, never a made-up value
int PlayerLookingAt(lua_State* L){game::DebugInfo d{};if(!game::ReadDebug(&d,true)){lua_pushnil(L);return 1;}lua_createtable(L,0,4);
 static const char* faces[]={"Down","Up","North","South","West","East"};if(d.target==1){lua_pushstring(L,"block");lua_setfield(L,-2,"type");const float b[3]={(float)d.block[0],(float)d.block[1],(float)d.block[2]};PushVector(L,b);lua_setfield(L,-2,"block");PushVector(L,b);lua_setfield(L,-2,"position");if(d.face<6){lua_pushstring(L,faces[d.face]);lua_setfield(L,-2,"face");}}
 else if(d.target==2){lua_pushstring(L,"entity");lua_setfield(L,-2,"type");}else{lua_pushstring(L,"none");lua_setfield(L,-2,"type");}return 1;}

// ---------------------------------------------------------------- drawing
struct ScriptDraw{ImDrawList* d=nullptr;bool world=false;float m[16]{},origin[3]{};bool projected=false;float W=0,H=0;};ScriptDraw scriptDraw;
ImU32 Ink(lua_State* L,int idx,ImVec4 fallback=ImVec4(1,1,1,1)){return ImGui::ColorConvertFloat4ToU32(ReadColor(L,idx,fallback));}
ImDrawList* DrawOf(lua_State* L,bool world){if(!scriptDraw.d||scriptDraw.world!=world)luaL_error(L,"draw only works inside its %s handler",world?"render3d":"render2d");return scriptDraw.d;}
int DrawText(lua_State* L){ImDrawList* d=DrawOf(L,false);const char* text=luaL_checkstring(L,2);const float x=(float)luaL_checknumber(L,3),y=(float)luaL_checknumber(L,4),size=(float)luaL_optnumber(L,5,16);const ImU32 c=Ink(L,6);const bool shadow=lua_toboolean(L,7)!=0;
 const std::string font=OptStr(L,8,"font","Regular"),align=OptStr(L,8,"align","Left");ImFont* f=font=="Bold"||font=="ExtraBold"?bold:font=="SemiBold"?semibold:regular;const float w=TextWidth(f,size,text);const float left=align=="Center"?x-w/2:align=="Right"?x-w:x;
 if(shadow)Label(d,f,size,ImVec2(left+1,y+1),text,IM_COL32(0,0,0,(c>>24)*3/4));Label(d,f,size,ImVec2(left,y),text,c);return 0;}
int DrawFill(lua_State* L){ImDrawList* d=DrawOf(L,false);d->AddRectFilled(ImVec2((float)luaL_checknumber(L,2),(float)luaL_checknumber(L,3)),ImVec2((float)luaL_checknumber(L,4),(float)luaL_checknumber(L,5)),Ink(L,6),(float)luaL_optnumber(L,7,0));return 0;}
int DrawRect(lua_State* L){ImDrawList* d=DrawOf(L,false);d->AddRect(ImVec2((float)luaL_checknumber(L,2),(float)luaL_checknumber(L,3)),ImVec2((float)luaL_checknumber(L,4),(float)luaL_checknumber(L,5)),Ink(L,6),0,0,(float)luaL_optnumber(L,7,1));return 0;}
int DrawCircle(lua_State* L){ImDrawList* d=DrawOf(L,false);const ImVec2 p((float)luaL_checknumber(L,2),(float)luaL_checknumber(L,3));const float r=(float)luaL_checknumber(L,4);const ImU32 c=Ink(L,5);if(lua_toboolean(L,6))d->AddCircleFilled(p,r,c);else d->AddCircle(p,r,c,0,(float)luaL_optnumber(L,7,1));return 0;}
int DrawGradient(lua_State* L){ImDrawList* d=DrawOf(L,false);d->AddRectFilledMultiColor(ImVec2((float)luaL_checknumber(L,2),(float)luaL_checknumber(L,3)),ImVec2((float)luaL_checknumber(L,4),(float)luaL_checknumber(L,5)),Ink(L,6),Ink(L,7),Ink(L,8),Ink(L,9));return 0;}
bool ScriptProject(const float p[3],ImVec2& out){if(!scriptDraw.projected)return false;nickel::visual::Point point{};if(!nickel::visual::project(scriptDraw.m,scriptDraw.origin,p,scriptDraw.W,scriptDraw.H,&point))return false;out=ImVec2(point.x,point.y);return true;}
int DrawWorldToScreen(lua_State* L){DrawOf(L,false);float p[3];ImVec2 s;if(!ReadVector(L,2,p)||!ScriptProject(p,s)){lua_pushnil(L);lua_pushnil(L);return 2;}lua_pushnumber(L,s.x);lua_pushnumber(L,s.y);return 2;}
int DrawSize(lua_State* L){const ImVec2 s=ImGui::GetIO().DisplaySize;lua_pushnumber(L,s.x);lua_pushnumber(L,s.y);return 2;}
int Draw2DLine(lua_State* L){ImDrawList* d=DrawOf(L,false);d->AddLine(ImVec2((float)luaL_checknumber(L,2),(float)luaL_checknumber(L,3)),ImVec2((float)luaL_checknumber(L,4),(float)luaL_checknumber(L,5)),Ink(L,6),(float)luaL_optnumber(L,7,1));return 0;}
// World: edges projected to the screen (always drawn through walls, no depth test).
void Segment(ImDrawList* d,const float a[3],const float b[3],ImU32 c,float t){ImVec2 p,q;if(ScriptProject(a,p)&&ScriptProject(b,q))d->AddLine(p,q,c,t);}
void BoxEdges(ImDrawList* d,const float lo[3],const float hi[3],ImU32 c,float t){float v[8][3];for(int i=0;i<8;++i){v[i][0]=(i&1)?hi[0]:lo[0];v[i][1]=(i&2)?hi[1]:lo[1];v[i][2]=(i&4)?hi[2]:lo[2];}
 static const int e[12][2]={{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};for(auto& k:e)Segment(d,v[k[0]],v[k[1]],c,t);}
int DrawBox(lua_State* L){ImDrawList* d=DrawOf(L,true);float lo[3],hi[3];if(!ReadVector(L,2,lo)||!ReadVector(L,3,hi))luaL_error(L,"vector expected");BoxEdges(d,lo,hi,Ink(L,4),(float)luaL_optnumber(L,5,1));return 0;}
int DrawFilledBox(lua_State* L){ImDrawList* d=DrawOf(L,true);float lo[3],hi[3];if(!ReadVector(L,2,lo)||!ReadVector(L,3,hi))luaL_error(L,"vector expected");const ImU32 c=Ink(L,4);
 static const int faces[6][4]={{0,1,3,2},{4,5,7,6},{0,1,5,4},{2,3,7,6},{0,2,6,4},{1,3,7,5}};float v[8][3];for(int i=0;i<8;++i){v[i][0]=(i&1)?hi[0]:lo[0];v[i][1]=(i&2)?hi[1]:lo[1];v[i][2]=(i&4)?hi[2]:lo[2];}
 for(auto& f:faces){ImVec2 q[4];bool ok=true;for(int i=0;i<4;++i)ok=ok&&ScriptProject(v[f[i]],q[i]);if(ok)d->AddQuadFilled(q[0],q[1],q[2],q[3],c);}return 0;}
int Draw3DLine(lua_State* L){ImDrawList* d=DrawOf(L,true);float a[3],b[3];if(!ReadVector(L,2,a)||!ReadVector(L,3,b))luaL_error(L,"vector expected");Segment(d,a,b,Ink(L,4),(float)luaL_optnumber(L,5,1));return 0;}
int DrawEntityBox(lua_State* L){ImDrawList* d=DrawOf(L,true);const uint64_t id=(uint64_t)luaL_checknumber(L,2);static ScriptEntity list[512];const int n=ReadScriptEntities(list,512,0,false);
 for(int i=0;i<n;++i)if(list[i].id==id&&list[i].box){BoxEdges(d,list[i].lo,list[i].hi,Ink(L,3),(float)luaL_optnumber(L,4,1));lua_pushboolean(L,1);return 1;}lua_pushboolean(L,0);return 1;}

// ---------------------------------------------------------------- require
std::string ScriptCompile(const std::string& source,std::string& error){lua_CompileOptions options{};options.optimizationLevel=1;options.debugLevel=1;size_t size=0;char* bytecode=luau_compile(source.data(),source.size(),&options,&size);std::string out(bytecode,size);free(bytecode);
 if(!out.empty()&&out[0]==0)error=out.substr(1);return out;}
int Require(lua_State* L){Script* s=ScriptOf(L);RequireLoading(L,"require");const std::string name=luaL_checkstring(L,1);for(char c:name)if(!std::isalnum((unsigned char)c)&&c!='_'&&c!='-'&&c!='/')luaL_error(L,"invalid library name '%s'",name.c_str());
 auto found=s->required.find(name);if(found!=s->required.end()){lua_getref(L,found->second);return 1;}
 if(std::find(s->requiring.begin(),s->requiring.end(),name)!=s->requiring.end())luaL_error(L,"require loop on '%s'",name.c_str());
 const std::string source=ReadAll(ScriptFolder()+L"\\lib\\"+Wide(name)+L".luau");if(source.empty())luaL_error(L,"no library lib/%s.luau",name.c_str());std::string error;const std::string code=ScriptCompile(source,error);if(!error.empty())luaL_error(L,"%s",error.c_str());
 lua_State* T=lua_newthread(L);luaL_sandboxthread(T);const std::string chunk="=lib/"+name;if(luau_load(T,chunk.c_str(),code.data(),code.size(),0)!=0)lua_error(T);
 s->requiring.push_back(name);lua_call(T,0,1);s->requiring.pop_back();lua_xmove(T,L,1);lua_remove(L,-2);lua_pushvalue(L,-1);s->required[name]=lua_ref(L,-1);lua_pop(L,1);return 1;}

// 2.1 API (script_api21.hpp, included after Dispatch): declarations needed for registration.
int ModRotate21(lua_State*);int ModAimAt21(lua_State*);int ModUseItem21(lua_State*);int WorldRaycast(lua_State*);int WorldFindBlocks(lua_State*);int WorldPlaceBlock(lua_State*);
int PlayerUseItemOn(lua_State*);int PlayerReleaseItem(lua_State*);int InvItem(lua_State*);int InvSize(lua_State*);int InvHeld(lua_State*);int InvOffhand(lua_State*);int InvArmor(lua_State*);
int InvFind(lua_State*);int InvHotbarFind(lua_State*);int InvSwap(lua_State*);int InvEquip(lua_State*);int InvToOffhand(lua_State*);int InvDrop(lua_State*);
int ContainerIsOpen(lua_State*);int ContainerType(lua_State*);int ContainerSize(lua_State*);int ContainerItem(lua_State*);int ContainerItems(lua_State*);int ContainerTake(lua_State*);int ContainerPut(lua_State*);int ContainerClose(lua_State*);
int PathGoto(lua_State*);int PathFollow(lua_State*);int PathMine(lua_State*);int PathWander(lua_State*);int PathStop(lua_State*);int PathPause(lua_State*);int PathResume(lua_State*);int PathIsActive(lua_State*);int PathStatus(lua_State*);
int WorldBlock(lua_State*);int WorldIsLoaded(lua_State*);int PlayerAttack(lua_State*);int PlayerInteract(lua_State*);int PlayerStartBreak(lua_State*);int PlayerContinueBreak(lua_State*);int PlayerStopBreak(lua_State*);
int ProjectileTrace(lua_State*);int ProjectileAim(lua_State*);int PlayerSimulate(lua_State*);void ScriptsPathWatch();
// ---------------------------------------------------------------- API registration
void SetFunctions(lua_State* L,const char* global,const luaL_Reg* functions){lua_newtable(L);for(auto* f=functions;f->name;++f){lua_pushcfunction(L,f->func,f->name);lua_setfield(L,-2,f->name);}if(global)lua_setglobal(L,global);}
void Metatable(lua_State* L,const char* name,const luaL_Reg* methods){luaL_newmetatable(L,name);lua_newtable(L);for(auto* f=methods;f->name;++f){lua_pushcfunction(L,f->func,f->name);lua_setfield(L,-2,f->name);}lua_setfield(L,-2,"__index");lua_pop(L,1);}
void RegisterApi(lua_State* L){
 static const luaL_Reg setting[]={{"get",SettingGet},{"set",SettingSet},{"name",SettingName},{"type",SettingType},{nullptr,nullptr}};Metatable(L,"NickelSetting",setting);
 static const luaL_Reg module[]={{"boolean",ModBoolean},{"number",ModNumber},{"integer",ModInteger},{"choice",ModChoice},{"color",ModColor},{"text",ModText},{"key",ModKey},{"onEnable",ModOnEnable},{"onDisable",ModOnDisable},{"on",ModOn},
  {"isEnabled",ModIsEnabled},{"setEnabled",ModSetEnabled},{"hud",ModHud},{"hudFrame",ModHudFrame},{"rotate",ModRotate21},{"aimAt",ModAimAt21},{"useItem",ModUseItem21},{nullptr,nullptr}};Metatable(L,"NickelModule",module);
 static const luaL_Reg builtin[]={{"name",BuiltinName},{"category",BuiltinCategory},{"isEnabled",BuiltinIsEnabled},{"setEnabled",BuiltinSetEnabled},{"toggle",BuiltinToggle},{"settings",BuiltinSettings},{"setting",BuiltinSetting},{nullptr,nullptr}};Metatable(L,"NickelBuiltin",builtin);
 static const luaL_Reg screen[]={{"text",DrawText},{"fill",DrawFill},{"rect",DrawRect},{"line",Draw2DLine},{"circle",DrawCircle},{"gradient",DrawGradient},{"worldToScreen",DrawWorldToScreen},{"size",DrawSize},{nullptr,nullptr}};Metatable(L,"NickelScreenDraw",screen);
 static const luaL_Reg world3d[]={{"box",DrawBox},{"filledBox",DrawFilledBox},{"line",Draw3DLine},{"entityBox",DrawEntityBox},{nullptr,nullptr}};Metatable(L,"NickelWorldDraw",world3d);
 static const luaL_Reg api[]={{"module",ScriptModuleDecl},{"command",ScriptCommandDecl},{nullptr,nullptr}};SetFunctions(L,nullptr,api);
 static const luaL_Reg mods[]={{"list",ModulesList},{"get",ModulesGet},{nullptr,nullptr}};SetFunctions(L,nullptr,mods);lua_setfield(L,-2,"modules");lua_setglobal(L,"nickel");
 static const luaL_Reg player[]={{"inWorld",PlayerInWorld},{"position",PlayerPosition},{"eyePosition",PlayerEye},{"velocity",PlayerVelocity},{"rotation",PlayerRotation},{"anglesTo",PlayerAnglesTo},{"health",PlayerHealth},{"name",PlayerName},
  {"runtimeId",PlayerRuntimeId},{"actorUniqueId",PlayerUniqueId},{"onGround",PlayerOnGround},{"isSprinting",PlayerSprinting},{"isSneaking",PlayerSneaking},{"inWater",PlayerInWater},{"inLava",PlayerInLava},{"fallDistance",PlayerFall},{"gameMode",PlayerGameMode},
  {"slot",PlayerSlot},{"setSlot",PlayerSetSlot},{"setVelocity",PlayerSetVelocity},{"addVelocity",PlayerAddVelocity},{"teleport",PlayerTeleport},{"jump",PlayerJump},{"swing",PlayerSwing},{"lookingAt",PlayerLookingAt},
  {"attack",PlayerAttack},{"interact",PlayerInteract},{"startBreak",PlayerStartBreak},{"continueBreak",PlayerContinueBreak},{"stopBreak",PlayerStopBreak},{"useItemOn",PlayerUseItemOn},{"releaseItem",PlayerReleaseItem},{"simulate",PlayerSimulate},{nullptr,nullptr}};SetFunctions(L,"player",player);
 static const luaL_Reg world[]={{"entities",WorldEntities},{"entity",WorldEntity},{"target",WorldTarget},{"dimension",WorldDimension},{"time",WorldTime},{"block",WorldBlock},{"raycast",WorldRaycast},{"isLoaded",WorldIsLoaded},{"findBlocks",WorldFindBlocks},{"placeBlock",WorldPlaceBlock},{nullptr,nullptr}};SetFunctions(L,"world",world);
 static const luaL_Reg chat[]={{"print",ChatPrint},{"send",ChatSend},{"command",ChatCommand},{nullptr,nullptr}};SetFunctions(L,"chat",chat);
 static const luaL_Reg client[]={{"notify",ClientNotify},{"time",ClientTime},{"tick",ClientTick},{"after",ClientAfter},{"every",ClientEvery},{"cancel",ClientCancel},{"isKeyDown",ClientIsKeyDown},{"server",ClientServer},{"ping",ClientPing},{"transport",ClientTransport},{nullptr,nullptr}};SetFunctions(L,"client",client);
 static const luaL_Reg storage[]={{"get",StorageGet},{"set",StorageSet},{nullptr,nullptr}};SetFunctions(L,"storage",storage);
 static const luaL_Reg inventory[]={{"item",InvItem},{"size",InvSize},{"held",InvHeld},{"offhand",InvOffhand},{"armor",InvArmor},{"find",InvFind},{"hotbarFind",InvHotbarFind},{"swap",InvSwap},{"drop",InvDrop},{"equip",InvEquip},{"toOffhand",InvToOffhand},{nullptr,nullptr}};SetFunctions(L,"inventory",inventory);
 static const luaL_Reg container[]={{"isOpen",ContainerIsOpen},{"type",ContainerType},{"size",ContainerSize},{"item",ContainerItem},{"items",ContainerItems},{"take",ContainerTake},{"put",ContainerPut},{"close",ContainerClose},{nullptr,nullptr}};SetFunctions(L,"container",container);
 static const luaL_Reg path[]={{"goto",PathGoto},{"follow",PathFollow},{"mine",PathMine},{"wander",PathWander},{"stop",PathStop},{"pause",PathPause},{"resume",PathResume},{"isActive",PathIsActive},{"status",PathStatus},{nullptr,nullptr}};SetFunctions(L,"path",path);
 static const luaL_Reg projectile[]={{"aim",ProjectileAim},{"trace",ProjectileTrace},{nullptr,nullptr}};SetFunctions(L,"projectile",projectile);
 lua_pushcfunction(L,Require,"require");lua_setglobal(L,"require");
}

// ---------------------------------------------------------------- loading / unloading
void ScriptUnload(Script& s){
 for(auto& m:s.mods)if(m.index>=0){Module& mod=modules[m.index];if(m.was&&m.disableRef&&s.L&&s.state==1){ScriptInvoke(s,m.disableRef);}mod.category.clear();if(selected==m.index)selected=-1;}
 s.mods.clear();s.commands.clear();s.timers.clear();s.required.clear();if(s.L){lua_close(s.L);s.L=nullptr;}s.memory=0;}
void ScriptLoad(Script& s){
 std::lock_guard<std::recursive_mutex> lock(scriptGuard);ScriptUnload(s);s.errors.clear();s.errorTimes.clear();s.state=3;s.frameMs=0;
 const std::string source=ReadAll(s.file);std::string error;const std::string code=ScriptCompile(source,error);if(!error.empty()){ScriptError(s,error);return;}
 s.L=lua_newstate(ScriptAlloc,&s);if(!s.L){ScriptError(s,"not enough memory");return;}lua_callbacks(s.L)->userdata=&s;lua_callbacks(s.L)->interrupt=ScriptInterrupt;
 luaL_openlibs(s.L);RegisterApi(s.L);luaL_sandbox(s.L);
 lua_State* T=lua_newthread(s.L);s.threadRef=lua_ref(s.L,-1);lua_pop(s.L,1);luaL_sandboxthread(T);const std::string chunk="="+s.name;
 if(luau_load(T,chunk.c_str(),code.data(),code.size(),0)!=0){ScriptError(s,lua_tostring(T,-1)?lua_tostring(T,-1):"load error");ScriptUnload(s);s.state=3;return;}
 s.loading=true;s.limit=1;s.state=1;const bool ok=ScriptCall(s,T,0,0);s.loading=false;s.limit=.05;
 if(!ok){const auto errors=s.errors;ScriptUnload(s);s.errors=errors;s.state=3;return;}
 s.state=s.userStopped?0:1;if(s.state!=1)ScriptHideModules(s);
 if(WriteLog){std::string line="Script "+s.name+": loaded ("+std::to_string(s.mods.size())+" modules, "+std::to_string(s.commands.size())+" commands)";WriteLog(line.c_str());}}
uint64_t FileStamp(const std::wstring& path){WIN32_FILE_ATTRIBUTE_DATA a{};if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&a))return 0;return ((uint64_t)a.ftLastWriteTime.dwHighDateTime<<32|a.ftLastWriteTime.dwLowDateTime)^((uint64_t)a.nFileSizeLow<<1);}
// Every second: new files, modified files (reloaded, which restarts a script stopped after errors), removed files.
void ScanScripts(){
 const double now=ScriptNow();if(now<scriptScanAt)return;scriptScanAt=now+1;const std::wstring folder=ScriptFolder();if(folder.empty())return;
 static bool dirsMade=false;   // folders are created once; recreated if the scripts folder goes missing
 if(!dirsMade){CreateDirectoryW((folder.substr(0,folder.find_last_of(L'\\'))).c_str(),nullptr);CreateDirectoryW(folder.c_str(),nullptr);CreateDirectoryW((folder+L"\\lib").c_str(),nullptr);dirsMade=true;}
 std::vector<std::wstring> found;WIN32_FIND_DATAW data{};HANDLE h=FindFirstFileW((folder+L"\\*.luau").c_str(),&data);
 if(h==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_PATH_NOT_FOUND)dirsMade=false;
 if(h!=INVALID_HANDLE_VALUE){do{std::wstring n=data.cFileName;if(n.size()>7&&!_wcsicmp(n.c_str()+n.size()-7,L".d.luau"))continue;found.push_back(n);}while(FindNextFileW(h,&data));FindClose(h);}
 std::lock_guard<std::recursive_mutex> lock(scriptGuard);
 // The selection is kept by identity: the list is sorted by name, so indices shift when scripts come and go.
 Script* selected=scriptSelected>=0&&scriptSelected<(int)scripts.size()?scripts[scriptSelected].get():nullptr;bool added=false;
 for(size_t i=0;i<scripts.size();){auto& s=*scripts[i];const std::wstring n=s.file.substr(s.file.find_last_of(L'\\')+1);if(std::find(found.begin(),found.end(),n)==found.end()){if(&s==selected)selected=nullptr;ScriptUnload(s);scripts.erase(scripts.begin()+i);}else ++i;}
 // Libraries: any change under lib/ reloads every script.
 static uint64_t libStamp=0;uint64_t lib=0;h=FindFirstFileW((folder+L"\\lib\\*.luau").c_str(),&data);if(h!=INVALID_HANDLE_VALUE){do{lib^=FileStamp(folder+L"\\lib\\"+data.cFileName)*31+data.cFileName[0];}while(FindNextFileW(h,&data));FindClose(h);}
 const bool libChanged=lib!=libStamp;libStamp=lib;
 for(auto& n:found){Script* s=nullptr;for(auto& e:scripts)if(!_wcsicmp(e->file.substr(e->file.find_last_of(L'\\')+1).c_str(),n.c_str()))s=e.get();
  const std::wstring path=folder+L"\\"+n;const uint64_t stamp=FileStamp(path);
  if(!s){auto fresh=std::make_unique<Script>();fresh->file=path;fresh->name=Utf8(n.substr(0,n.size()-5));fresh->stamp=stamp;scripts.push_back(std::move(fresh));added=true;ScriptLoad(*scripts.back());continue;}
  if(stamp!=s->stamp||(libChanged&&s->state==1)){s->stamp=stamp;s->userStopped=false;ScriptLoad(*s);}}
 if(added)std::sort(scripts.begin(),scripts.end(),[](const std::unique_ptr<Script>& a,const std::unique_ptr<Script>& b){return _stricmp(a->name.c_str(),b->name.c_str())<0;});   // order only changes when a script is added
 scriptSelected=-1;if(selected)for(size_t i=0;i<scripts.size();++i)if(scripts[i].get()==selected){scriptSelected=(int)i;break;}}
void StopScript(Script& s){std::lock_guard<std::recursive_mutex> lock(scriptGuard);for(auto& m:s.mods)if(m.index>=0&&modules[m.index].enabled){modules[m.index].enabled=false;Save(modules[m.index]);}ScriptUnload(s);s.state=0;s.userStopped=true;}
void StartScript(Script& s){s.userStopped=false;ScriptLoad(s);}

// ---------------------------------------------------------------- event dispatch
// Plain event (no fields) or a table already pushed; `cancel` reads event.cancelled after the call.
int EventCancel(lua_State* L){luaL_checktype(L,1,LUA_TTABLE);lua_pushboolean(L,1);lua_setfield(L,1,"cancelled");return 0;}
void NewEvent(lua_State* L){lua_newtable(L);lua_pushcfunction(L,EventCancel,"cancel");lua_setfield(L,-2,"cancel");}
template<class Fill,class After>bool Dispatch(const char* event,Fill fill,After after){
 bool cancelled=false;std::lock_guard<std::recursive_mutex> lock(scriptGuard);
 for(auto& sp:scripts){Script& s=*sp;if(s.state!=1||!s.L)continue;for(size_t mi=0;mi<s.mods.size()&&s.state==1;++mi){const ScriptModule m=s.mods[mi];if(m.index<0||!modules[m.index].enabled)continue;
   for(auto& h:m.handlers){if(h.event!=event||s.state!=1)continue;lua_State* L=s.L;const int top=lua_gettop(L);lua_getref(L,h.ref);const int args=fill(L);
    if(args>0){lua_pushvalue(L,-1);lua_insert(L,top+1);}   // keep a copy of the event below the function
    if(ScriptCall(s,L,args,0)&&args>0){lua_rawgetfield(L,top+1,"cancelled");if(lua_toboolean(L,-1))cancelled=true;lua_pop(L,1);after(L,top+1);}lua_settop(L,top);}}}
 return cancelled;}
bool Fire(const char* event){return Dispatch(event,[](lua_State*){return 0;},[](lua_State*,int){});}
bool ScriptsWant(const char* event);
#include "script_api21.hpp"

// ---------------------------------------------------------------- frame (render thread): toggles, timers, update, render2d/3d, HUD
bool scriptWantsCamera=false;
void ScriptsWatch();
void ScriptsFrame(std::vector<std::string>& drawn){
 ScanScripts();std::lock_guard<std::recursive_mutex> lock(scriptGuard);const double now=ScriptNow();bool camera=false;
 for(auto& sp:scripts){Script& s=*sp;if(s.state!=1||!s.L)continue;s.frameStart=now;
  for(size_t i=0;i<s.mods.size()&&s.state==1;++i){ScriptModule& m=s.mods[i];if(m.index<0)continue;Module& mod=modules[m.index];
   if(m.activation==1){auto k=keycodes.find(mod.id);const bool held=k!=keycodes.end()&&(GetAsyncKeyState(k->second)&0x8000)!=0&&!opened&&KeysAllowed();if(mod.enabled!=held)mod.enabled=held;}
   if(mod.enabled!=m.was){m.was=mod.enabled;const int ref=mod.enabled?m.enableRef:m.disableRef;if(ref)ScriptInvoke(s,ref);}
   if(mod.enabled)for(auto& h:m.handlers)if(h.event=="render3d"||h.event=="render2d")camera=true;}
  // Expired timers (a timer cancelled during the loop is not called).
  std::vector<int> due;for(auto& t:s.timers)if(t.due<=now)due.push_back(t.id);
  for(int id:due){if(s.state!=1)break;auto it=std::find_if(s.timers.begin(),s.timers.end(),[&](const ScriptTimer& t){return t.id==id;});if(it==s.timers.end())continue;const int ref=it->ref;
   if(it->every>0)it->due=now+it->every;ScriptInvoke(s,ref);it=std::find_if(s.timers.begin(),s.timers.end(),[&](const ScriptTimer& t){return t.id==id;});
   if(it!=s.timers.end()&&it->every<=0){if(s.L)lua_unref(s.L,it->ref);s.timers.erase(it);}}}
 scriptWantsCamera=camera;
 ScriptsWatch();
 Fire("update");
 // Drawing: same projection as the ESP modules (hook #246, matrix captured less than 250 ms earlier).
 auto& view=game::View();uint64_t tick{};AcquireSRWLockShared(&view.lock);memcpy(scriptDraw.m,view.m,sizeof(scriptDraw.m));memcpy(scriptDraw.origin,view.origin,sizeof(scriptDraw.origin));tick=view.tick;ReleaseSRWLockShared(&view.lock);
 scriptDraw.projected=camera&&tick&&GetTickCount64()-tick<250&&!view.fault;scriptDraw.W=ImGui::GetIO().DisplaySize.x;scriptDraw.H=ImGui::GetIO().DisplaySize.y;if(camera)game::InstallScreenHook(WriteLog);
 for(auto& sp:scripts){Script& s=*sp;if(s.state!=1)continue;for(auto& m:s.mods){m.frame=false;if(!m.hud||m.index<0||!modules[m.index].enabled)continue;const std::string id=modules[m.index].id;
  auto& place=Place(id.c_str(),20,200);m.fx=place.x;m.fy=place.y;m.fs=place.scale;m.fw=m.hudW*place.scale;m.fh=m.hudH*place.scale;m.frame=true;place.lo=ImVec2(m.fx,m.fy);place.hi=ImVec2(m.fx+m.fw,m.fy+m.fh);drawn.push_back(id);}}
 scriptDraw.d=ImGui::GetBackgroundDrawList();scriptDraw.world=true;
 Dispatch("render3d",[](lua_State* L){lua_newtable(L);luaL_getmetatable(L,"NickelWorldDraw");lua_setmetatable(L,-2);return 1;},[](lua_State*,int){});
 scriptDraw.world=false;scriptDraw.d=HudList();   // script HUDs hide outside play, world drawing stays
 Dispatch("render2d",[](lua_State* L){lua_newtable(L);luaL_getmetatable(L,"NickelScreenDraw");lua_setmetatable(L,-2);return 1;},[](lua_State*,int){});
 scriptDraw.d=nullptr;
 for(auto& sp:scripts){Script& s=*sp;s.frameMs=s.frameMs*.9+s.frameSum*1000*.1;s.frameSum=0;}
}
// Key pressed with the menu closed (render thread, HandleKey); key release is not reported.
void ScriptsKey(unsigned vk){const std::string name=ScriptKeyName((int)vk);Dispatch("key",[&](lua_State* L){NewEvent(L);lua_pushstring(L,name.c_str());lua_setfield(L,-2,"key");lua_pushboolean(L,1);lua_setfield(L,-2,"pressed");return 1;},[](lua_State*,int){});}

// ---------------------------------------------------------------- tick (game thread): pending messages, player writes, tick
void ScriptsTickWork(){
 InterlockedIncrement(&scriptTicks);ScriptsHealthWatch();ScriptsPathWatch();game::Context c{};const bool local=game::Local(&c);
 std::vector<std::pair<int,std::string>> queue;{std::lock_guard<std::mutex> lock(scriptChatGuard);queue.swap(scriptChatQueue);}
 if(local)for(auto& q:queue){if(q.first==0)game::ClientMessage(c,q.second);else if(q.first==1)game::SendChat(c,q.second);else game::SendCommand(c,q.second);}
 bool velocity=false,add=false,jump=false,swing=false,teleport=false;int slot=-1;float v0[3]{},a0[3]{},t0[3]{};
 {std::lock_guard<std::mutex> lock(scriptWrites.guard);velocity=scriptWrites.velocity;add=scriptWrites.add;jump=scriptWrites.jump;swing=scriptWrites.swing;teleport=scriptWrites.teleport;slot=scriptWrites.slot;memcpy(v0,scriptWrites.v,12);memcpy(a0,scriptWrites.a,12);memcpy(t0,scriptWrites.t,12);
  scriptWrites.velocity=scriptWrites.add=scriptWrites.jump=scriptWrites.swing=scriptWrites.teleport=false;scriptWrites.slot=-1;}
 auto finite=[](const float* v){return std::isfinite(v[0])&&std::isfinite(v[1])&&std::isfinite(v[2]);};if(!finite(v0))velocity=false;if(!finite(a0))add=false;
 if(local&&slot>=0&&slot<9)if(int32_t* sel=game::ActionSelected(c.actor))if(*sel!=slot){*sel=slot;game::SendMobEquipment(c,c.actor,slot);}   // as the hotbar switch of the action queue
 if(local&&swing)game::ActionSwing(c,c.actor,4);   // arm swing, as Auto Clicker
 // teleport: the local move of Look TP (position, previous position, velocity cleared, collision box moved), with the
 // feet position turned into the eye-level position the movement state holds. Servers may refuse long moves.
 if(local&&teleport&&finite(t0)){float lift=1.62f;uintptr_t off{};if(game::Element(c.registry,c.entity,"OffsetsComponent",0x44,&off))game::Floats(off,&lift,1);
  if(std::isfinite(lift)&&lift>=0.f&&lift<8.f){const float dest[3]={t0[0],t0[1]+lift,t0[2]};game::LookTpMove(c.actor,dest);}}
 uint64_t state{};if(local&&game::Read(c.actor+0x240,&state)&&state){float* v=reinterpret_cast<float*>(state+0x18);   // velocity: movement state +0x18 (see ReadPlayer in game_read.hpp)
  if(velocity)memcpy(v,v0,12);if(add)for(int i=0;i<3;++i)v[i]+=a0[i];if(jump)v[1]=std::max(v[1],.42f);}
 Fire("tick");}
void ScriptsTick(){static volatile long stopped_=0;if(!game::FeatureStopped(&stopped_))__try{ScriptsTickWork();}__except(game::FeatureFault("ScriptsTick",&stopped_,GetExceptionInformation())){game::FeatureFlush();}}

// ---------------------------------------------------------------- commands (".name args" typed in chat, intercepted on send)
std::vector<std::string> SplitWords(const std::string& line){std::vector<std::string> out;std::string cur;bool quote=false;for(char c:line){if(c=='"'){quote=!quote;continue;}if(c==' '&&!quote){if(!cur.empty())out.push_back(cur);cur.clear();}else cur+=c;}if(!cur.empty())out.push_back(cur);return out;}
std::string CommandUsage(lua_State* L,int spec,const std::string& path){std::string out="."+path;lua_rawgetfield(L,spec,"args");
 if(lua_istable(L,-1))for(int i=1;;++i){lua_rawgeti(L,-1,i);if(!lua_istable(L,-1)){lua_pop(L,1);break;}lua_rawgeti(L,-1,1);const std::string n=lua_tostring(L,-1)?lua_tostring(L,-1):"?";lua_pop(L,1);const bool opt=OptBool(L,-1,"optional",false);out+=opt?" ["+n+"]":" <"+n+">";lua_pop(L,1);}
 lua_pop(L,1);lua_rawgetfield(L,spec,"subcommands");
 if(lua_istable(L,-1)){out+=" <";bool first=true;lua_pushnil(L);while(lua_next(L,-2)){if(lua_type(L,-2)==LUA_TSTRING){if(!first)out+="|";out+=lua_tostring(L,-2);first=false;}lua_pop(L,1);}out+=">";}
 lua_pop(L,1);return out;}
struct CommandValue{int kind;double number;std::string text;};   // 0 nil, 1 number, 2 text, 3 boolean
// The spec is at stack index `spec`; returns an error message, empty if the command ran.
std::string RunCommand(Script& s,int spec,const std::vector<std::string>& words,size_t at,const std::string& path,int depth){
 lua_State* L=s.L;const int top=lua_gettop(L);if(depth>4)return "subcommands nest at most four levels";const std::string usage=CommandUsage(L,spec,path);
 lua_rawgetfield(L,spec,"subcommands");
 if(lua_istable(L,-1)){if(at>=words.size()){lua_settop(L,top);return "Usage: "+usage;}lua_rawgetfield(L,-1,words[at].c_str());
  if(!lua_istable(L,-1)){lua_settop(L,top);return "Unknown subcommand. Usage: "+usage;}const std::string r=RunCommand(s,lua_gettop(L),words,at+1,path+" "+words[at],depth+1);lua_settop(L,top);return r;}
 lua_settop(L,top);std::vector<CommandValue> values;lua_rawgetfield(L,spec,"args");
 if(lua_istable(L,-1))for(int i=1;;++i){lua_rawgeti(L,-1,i);if(!lua_istable(L,-1)){lua_pop(L,1);break;}const int a=lua_gettop(L);
  lua_rawgeti(L,a,1);const std::string name=lua_tostring(L,-1)?lua_tostring(L,-1):"";lua_pop(L,1);lua_rawgeti(L,a,2);const std::string type=lua_tostring(L,-1)?lua_tostring(L,-1):"string";lua_pop(L,1);
  const bool optional=OptBool(L,a,"optional",false);CommandValue v{0,0,""};
  if(at>=words.size()){lua_settop(L,top);if(optional)break;return "Missing "+name+". Usage: "+usage;}
  if(type=="text"){std::string rest;for(size_t k=at;k<words.size();++k){if(!rest.empty())rest+=" ";rest+=words[k];}at=words.size();v={2,0,rest};}
  else if(type=="integer"||type=="number"){char* end{};const double n=strtod(words[at].c_str(),&end);if(!end||*end||(type=="integer"&&n!=std::floor(n))){lua_settop(L,top);return name+" must be "+(type=="integer"?"a whole number":"a number")+". Usage: "+usage;}
   const double lo=OptNum(L,a,"min",-1e300),hi=OptNum(L,a,"max",1e300);if(n<lo||n>hi){lua_settop(L,top);char b[200];snprintf(b,sizeof(b),"%s must be between %g and %g",name.c_str(),lo,hi);return b;}v={1,n,""};++at;}
  else if(type=="boolean"){std::string w=words[at];std::transform(w.begin(),w.end(),w.begin(),[](unsigned char c){return (char)std::tolower(c);});if(w!="true"&&w!="false"){lua_settop(L,top);return name+" must be true or false";}v={3,w=="true"?1.0:0.0,""};++at;}
  else if(type=="choice"){lua_rawgeti(L,a,3);bool ok=false;if(lua_istable(L,-1))for(int k=1;;++k){lua_rawgeti(L,-1,k);if(lua_isnil(L,-1)){lua_pop(L,1);break;}if(lua_tostring(L,-1)&&words[at]==lua_tostring(L,-1))ok=true;lua_pop(L,1);}lua_pop(L,1);
   if(!ok){lua_settop(L,top);return name+" is not one of the choices. Usage: "+usage;}v={2,0,words[at]};++at;}
  else{v={2,0,words[at]};++at;}
  values.push_back(v);lua_pop(L,1);}
 lua_settop(L,top);if(at<words.size())return "Too many arguments. Usage: "+usage;
 lua_rawgetfield(L,spec,"run");if(!lua_isfunction(L,-1)){lua_settop(L,top);return "";}
 for(auto& v:values){if(v.kind==1)lua_pushnumber(L,v.number);else if(v.kind==3)lua_pushboolean(L,v.number!=0);else if(v.kind==2)lua_pushstring(L,v.text.c_str());else lua_pushnil(L);}
 ScriptCall(s,L,(int)values.size(),0);lua_settop(L,top);return "";}
// True if the line is a script command (sending is cancelled).
bool ScriptsCommand(const std::string& line){
 if(line.size()<2||line[0]!='.')return false;const auto words=SplitWords(line.substr(1));if(words.empty())return false;std::lock_guard<std::recursive_mutex> lock(scriptGuard);
 for(auto& sp:scripts){Script& s=*sp;if(s.state!=1||!s.L)continue;for(auto& c:s.commands){if(c.name!=words[0])continue;lua_State* L=s.L;const int top=lua_gettop(L);lua_getref(L,c.specRef);
   const std::string error=RunCommand(s,lua_gettop(L),words,1,c.name,0);lua_settop(L,top);if(!error.empty()){std::lock_guard<std::mutex> q(scriptChatGuard);scriptChatQueue.push_back({0,error});}return true;}}
 return false;}
bool ScriptsHaveCommands(){std::lock_guard<std::recursive_mutex> lock(scriptGuard);for(auto& s:scripts)if(s->state==1&&!s->commands.empty())return true;return false;}
bool ScriptsWant(const char* event){std::lock_guard<std::recursive_mutex> lock(scriptGuard);for(auto& sp:scripts)if(sp->state==1)for(auto& m:sp->mods)if(m.index>=0&&modules[m.index].enabled)for(auto& h:m.handlers)if(h.event==event)return true;return false;}

// ---------------------------------------------------------------- packets (game thread)
const char* PacketName(int id){switch(id){case 9:return "Text";case 0x13:return "MovePlayer";case 0x1b:return "ActorEvent";case 0x1e:return "InventoryTransaction";case 0x1f:return "MobEquipment";case 0x21:return "Interact";case 0x24:return "PlayerAction";
 case 0x28:return "SetActorMotion";case 0x2c:return "Animate";case 0x2e:return "ContainerOpen";case 0x2f:return "ContainerClose";case 0x4d:return "CommandRequest";case 0x90:return "PlayerAuthInput";default:return "Unknown";}}
// TextPacket: variant at +0xa0, index at +0xe8; index 1 = { author +0xa8, message +0xc8 } (game_packets.hpp).
// Other variants are not read.
std::string TextMessage(uintptr_t packet,std::string* author){int8_t index{};if(!game::Read(packet+0xe8,&index)||index!=1)return {};char a[256]{},m[1024]{};if(author&&game::Text(packet+0xa8,a,sizeof(a)))*author=a;return game::Text(packet+0xc8,m,sizeof(m))?std::string(m):std::string();}
bool ScriptsSendWork(uintptr_t packet){
 const int id=game::PacketId(packet);if(id==0x1e)ScriptsPearlSend(packet);
 if(id==9){std::string author;const std::string message=TextMessage(packet,&author);if(!message.empty()&&message[0]=='.'&&(game::SchematicCommand(message)||game::PfChatCommand(message)||ScriptsCommand(message)))return true;}
 if(!ScriptsWant("packetSend"))return false;const char* name=PacketName(id);
 return Dispatch("packetSend",[&](lua_State* L){NewEvent(L);lua_pushnumber(L,id);lua_setfield(L,-2,"id");lua_pushstring(L,name);lua_setfield(L,-2,"name");return 1;},[](lua_State*,int){});}
bool ScriptsSend(uintptr_t packet){__try{return ScriptsSendWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
bool ScriptsReceiveWork(uintptr_t packet){
 const int id=game::PacketId(packet);bool cancel=false;
 if(id==0x13)ScriptsTeleport(packet);
 if(id==9&&ScriptsWant("chat")){std::string author;const std::string message=TextMessage(packet,&author);
  if(!message.empty())cancel|=Dispatch("chat",[&](lua_State* L){NewEvent(L);lua_pushstring(L,message.c_str());lua_setfield(L,-2,"message");lua_pushstring(L,author.c_str());lua_setfield(L,-2,"author");lua_pushstring(L,"Chat");lua_setfield(L,-2,"type");return 1;},[](lua_State*,int){});}
 if(id==0x2e&&ScriptsWant("containerOpen"))Dispatch("containerOpen",[&](lua_State* L){NewEvent(L);lua_pushstring(L,"container");lua_setfield(L,-2,"type");return 1;},[](lua_State*,int){});
 if(id==0x2f&&ScriptsWant("containerClose"))Fire("containerClose");
 if(ScriptsWant("packetReceive")){const char* name=PacketName(id);cancel|=Dispatch("packetReceive",[&](lua_State* L){NewEvent(L);lua_pushnumber(L,id);lua_setfield(L,-2,"id");lua_pushstring(L,name);lua_setfield(L,-2,"name");return 1;},[](lua_State*,int){});}
 return cancel;}
bool ScriptsReceive(uintptr_t packet){__try{return ScriptsReceiveWork(packet);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
bool ScriptsAttackWork(uintptr_t target){uint64_t id{};if(!ScriptsWant("attack")||!game::RuntimeId(target,&id))return false;
 return Dispatch("attack",[&](lua_State* L){NewEvent(L);lua_pushnumber(L,(double)id);lua_setfield(L,-2,"target");return 1;},[](lua_State*,int){});}
bool ScriptsAttack(uintptr_t local,uintptr_t target){(void)local;__try{return ScriptsAttackWork(target);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
// Changes checked every frame: screen, dimension, death / respawn.
void ScriptsWatch(){
 static std::string screen;static int dimension=-2;static bool dead=false;const std::string now=game::currentScreen;
 if(now!=screen&&!now.empty()){const std::string previous=screen;screen=now;Dispatch("screen",[&](lua_State* L){NewEvent(L);lua_pushstring(L,now.c_str());lua_setfield(L,-2,"name");lua_pushstring(L,previous.c_str());lua_setfield(L,-2,"previous");return 1;},[](lua_State*,int){});}
 int d=-1;if(game::ReadDimension(&d)&&d!=dimension){const bool first=dimension==-2;dimension=d;static const char* n[]={"Overworld","Nether","TheEnd"};
  if(!first&&d>=0&&d<3)Dispatch("dimension",[&](lua_State* L){NewEvent(L);lua_pushstring(L,n[d]);lua_setfield(L,-2,"dimension");return 1;},[](lua_State*,int){});}
 float h{},m{};if(game::ReadHealth(&h,&m)){const bool nowDead=h<=0;if(nowDead!=dead){dead=nowDead;Fire(nowDead?"death":"respawn");}}}
