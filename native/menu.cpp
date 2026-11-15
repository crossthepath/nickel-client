#include "menu_icons.hpp"
#include "hud_map_faces.hpp"
#include "icon_codepoints.hpp"
#include "game_sky.hpp"
#include "game_block_esp.hpp"
#include "game_utility.hpp"
#include <unordered_set>
// In-game menu (toggled with Right Shift): module list, settings panel, Home/Settings/Configs screens and the HUD.
// Layout sizes below are in menu units (1012 x 578 at scale 1). Setting definitions come from generated_catalog.hpp.
#include "menu.hpp"
#include "crosshair_pattern.hpp"
#include <shellapi.h>
#include <winhttp.h>
#include <thread>
#include <memory>
#include <mutex>
#include "lua.h"
#include "lualib.h"
#include "luacode.h"
#include "generated_catalog.hpp"
#include "menu_catalog.hpp"
#include "game_read.hpp"
#include "media.hpp"
#include "game_events.hpp"
#include "game_controls.hpp"
#include <deque>
#include <cstdarg>
#include "game_map.hpp"
#include "game_hand.hpp"
#include "game_view.hpp"
#include "game_filter.hpp"
#include "game_visual_entities.hpp"
#include "game_world.hpp"
#include "game_visual_filters.hpp"
#include "game_nametags.hpp"
#include "game_free_camera.hpp"
#include "game_free_look.hpp"
#include "game_highlight.hpp"
#include "game_item_physics.hpp"
#include "game_bed_breaker.hpp"
#include "game_combat.hpp"
#include "game_aim_assist.hpp"
#include "game_criticals.hpp"
#include "game_hitbox.hpp"
#include "game_reach.hpp"
#include "game_sticky_aim.hpp"
#include "game_crystal.hpp"
#include "game_auto_anchor.hpp"
#include "game_auto_crossbow.hpp"
#include "game_auto_crystal.hpp"
#include "game_auto_mace.hpp"
#include "game_bow_aimbot.hpp"
#include "game_backtrack.hpp"
#include "game_pvp_bot.hpp"
#include "game_discord.hpp"
#include "game_base_finder.hpp"
#include "game_xray.hpp"
#include "game_movement.hpp"
#include "game_movement_a.hpp"
#include "game_movement_b.hpp"
#include "game_movement_c.hpp"
#include "game_movement_d.hpp"
#include "game_movement_e.hpp"
#include "game_simulate.hpp"
#include "file_dialog.hpp"
#include "game_network.hpp"
#include "game_transport.hpp"
#include "game_player_c.hpp"
#include "game_items.hpp"
#include "game_sound.hpp"
#include "game_player_b.hpp"
#include "generated_arrow_shape.hpp"
#include "../include/nickel/visual_projection.hpp"
#include "../include/nickel/visual_world.hpp"
#include "../include/nickel/visual_nametags.hpp"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <map>
#include <set>
#include <array>
#include <atomic>
// Rotation manager: attached once, the first frame any module or script asks for it
static void AttachRotations(){static bool ready=false;if(ready)return;ready=game::InstallFreeLookHooks(overlay::WriteLog)==1&&game::InstallMapHooks(overlay::WriteLog)==1&&game::InstallScreenHook(overlay::WriteLog)==1&&game::InstallSendHook(overlay::WriteLog)==1;
 if(ready){game::RotInstall();game::rotationGameUpdate=game::DerpTick;game::utilityTick=game::UtilityTick;game::sprintUtility=game::JumpInput;game::InstallFreeCameraHooks(overlay::WriteLog);if(overlay::WriteLog)overlay::WriteLog("Rotations: rotation manager attached (GameUpdate, LevelTick, screen, camera, PlayerAuthInput)");}}
// Combat modules that submit action sequences, run on the action thread before the planner
static void CombatActionFrames(){game::AutoAnchorTick();game::AutoCrossbowTick();game::AutoCrystalTick();game::PvpBotFrame();}
// BuildBlock cancel shared by Schematics (auto place) and Auto Crystal (busy)
// Attack event subscribers: Hit Select can cancel; Auto Mace records the hit
static bool CombatAttackCancel(uintptr_t local,uintptr_t target){const bool cancel=game::HitSelectCancel(local,target);if(!cancel)game::AutoMaceAttack(target);return cancel;}
// GameUpdate and screen-render subscribers of the Combat modules
static void CombatGameUpdates(){game::CombatGameUpdate();game::PlayerCGameUpdate();game::BowAimbotTick();game::MoveCGameUpdate();game::MovementDGameUpdate();}
static void CombatScreens(const char* screen){game::CombatScreen(screen);game::BowAimbotScreen();game::MoveCScreen();}
// Player inventory modules on the screen render (before the inventory scheduler runs its queue) and their receive subscribers.
static void PlayerInventoryFrames(){game::InvModulesFrame();game::PlayerBFrame();}
static bool PlayerReceives(uintptr_t packet){game::PlayerBReceive(packet);return game::PlayerReceive(packet);}
static bool BuildCancels(){return game::SchematicCancelBuild()||game::AutoCrystalBuildCancelGuarded();}

namespace overlay {
namespace {
std::atomic<bool> opened{false};
float progress=0;int category=0,tab=0,selected=-1,listening=-1;char search[160]{};
ImFont *regular{},*bold{},*semibold{},*icons{},*tiny{};
// Menu geometry, adjustable with the mouse: drag the title to move, drag an edge to resize that side,
// drag a corner to change the overall scale, double-click the title to reset.
float menuW=1012,menuH=578,menuScale=1,menuX=-1,menuY=-1,dragW=0,dragH=0,dragScale=1,dragX=0,dragY=0;int dragMode=0;ImVec2 dragStart;std::map<std::string,float> animation;std::map<std::string,std::string> keybinds;std::map<std::string,int> keycodes;struct Notice{int kind;std::string title,body;};std::vector<Notice> notices;   // pending notifications (5 = module toggled by key, 7 = configs)
bool mapOpen=false,worldCameraWanted=false;bool waypointsOpen=false;HWND gameWindowForKeys=nullptr;const char* particleOptions[15]={"All Particles","Critical Hit","Sharpness","Totem","Potion Effects","Smoke","Flames","Explosions","Water Splashes","Bubbles","Portals","Hearts","Enchanting Table","Block Breaking","Redstone"};   // full-screen MiniMap view (World Map Key)
void ScriptsKey(unsigned vk);   // Luau scripts (script_engine.hpp)
Setting* listeningSetting=nullptr;Module* listeningModule=nullptr;const std::vector<std::string>& RegistryNames(bool blocks);   // key setting waiting for input (stored as "Name|code")
// Manrope hhea ascent/descent = 2132/-600, unitsPerEm = 2000.
// stb_truetype sizes against ascent-descent; the UI layout sizes against em.
constexpr float FontEmFactor=1250.f/1000.f;   // Satoshi: (ascent 1010 + descent 240) / 1000
// ---- Font cache: every family is rasterized at the exact pixel sizes drawn on screen. Drawing a font baked at one
// size at another size scales its bitmap, which blurs the text (menu captions, HUD elements enlarged in the layout
// editor, input fields). Sizes up to 48 px are kept per pixel, larger ones every 4 px. A size not baked yet is
// drawn with the closest baked one, queued, and added to the atlas between two frames once the requests settle
// (FontsRebuild, called by the renderer before NewFrame, which then recreates the font texture).
struct FontFamily{const void* data=nullptr;int bytes=0;std::map<int,ImFont*> sizes;};
std::map<ImFont*,FontFamily> fontFamilies;   // keyed by the family's base font (regular, bold, semibold, tiny)
std::set<std::pair<ImFont*,int>> fontPending;double fontRequestedAt=0;std::wstring fontSizesPath;
// Preferences and configs live in %APPDATA%\Nickel (with the scripts and schematics), so they survive updates: each
// build is extracted to its own data\runtime\<hash> folder. The first time, the preferences and configs of the most
// recently used build are copied there.
std::wstring NickelDataFolder(){wchar_t base[MAX_PATH]{};if(!GetEnvironmentVariableW(L"APPDATA",base,MAX_PATH))return L"";std::wstring folder=std::wstring(base)+L"\\Nickel";CreateDirectoryW(folder.c_str(),nullptr);return folder;}
void CopyFolderFiles(const std::wstring& from,const std::wstring& to){CreateDirectoryW(to.c_str(),nullptr);WIN32_FIND_DATAW data{};auto h=FindFirstFileW((from+L"\\*").c_str(),&data);if(h==INVALID_HANDLE_VALUE)return;
 do{if(!(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))CopyFileW((from+L"\\"+data.cFileName).c_str(),(to+L"\\"+data.cFileName).c_str(),TRUE);}while(FindNextFileW(h,&data));FindClose(h);}
std::wstring PreferencesPath(HMODULE module){const std::wstring folder=NickelDataFolder();wchar_t own[32768]{};GetModuleFileNameW(module,own,32768);std::wstring here=own;here=here.substr(0,here.find_last_of(L"\\/"));
 if(folder.empty())return here+L"\\module-preferences.ini";const std::wstring target=folder+L"\\module-preferences.ini";
 if(GetFileAttributesW(target.c_str())==INVALID_FILE_ATTRIBUTES){   // first start with this layout: newest runtime folder with preferences
  const std::wstring runtime=here.substr(0,here.find_last_of(L"\\/"));std::wstring best;FILETIME bestTime{};WIN32_FIND_DATAW data{};auto h=FindFirstFileW((runtime+L"\\*").c_str(),&data);
  if(h!=INVALID_HANDLE_VALUE){do{if(!(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||data.cFileName[0]==L'.')continue;const std::wstring candidate=runtime+L"\\"+data.cFileName+L"\\module-preferences.ini";WIN32_FILE_ATTRIBUTE_DATA info{};
   if(GetFileAttributesExW(candidate.c_str(),GetFileExInfoStandard,&info)&&CompareFileTime(&info.ftLastWriteTime,&bestTime)>0){bestTime=info.ftLastWriteTime;best=runtime+L"\\"+data.cFileName;}}while(FindNextFileW(h,&data));FindClose(h);}
  if(!best.empty()){CopyFileW((best+L"\\module-preferences.ini").c_str(),target.c_str(),TRUE);CopyFolderFiles(best+L"\\configs",folder+L"\\configs");}}
 return target;}
int FontKey(float px){const int n=(int)std::lround(px);return n<=48?std::max(n,6):std::min(256,(n+2)/4*4);}
ImFont* Sized(ImFont* base,float px){auto it=fontFamilies.find(base);if(it==fontFamilies.end()||!(px>0))return base;auto& sizes=it->second.sizes;const int key=FontKey(px);
 auto hit=sizes.find(key);if(hit!=sizes.end())return hit->second;
 if(fontPending.insert({base,key}).second)fontRequestedAt=ImGui::GetTime();
 ImFont* best=base;float gap=std::fabs(base->FontSize-px);for(auto& [size,font]:sizes){const float g=std::fabs(size-px);if(g<gap){gap=g;best=font;}}return best;}
ImFont* AddSizedFont(FontFamily& family,int key){ImFontConfig config{};config.FontDataOwnedByAtlas=false;config.OversampleH=1;config.OversampleV=1;config.PixelSnapH=true;
 ImFont* font=ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<void*>(family.data),family.bytes,(float)key,&config,nullptr);if(font)family.sizes[key]=font;return font;}
void SaveFontSizes(){std::string text;for(auto& [base,family]:fontFamilies){const char* name=base==regular?"regular":base==bold?"bold":base==semibold?"semibold":"tiny";for(auto& [size,font]:family.sizes){(void)font;text+=std::string(name)+":"+std::to_string(size)+",";}}
 if(!fontSizesPath.empty())WritePrivateProfileStringW(L"fonts",L"sizes",std::wstring(text.begin(),text.end()).c_str(),fontSizesPath.c_str());}
// Window text (ImGui widgets) at an exact size: pushes the baked font for this window; EndWindow pops it.
std::vector<ImDrawList*> windowFonts;
void WindowFont(float px){ImDrawList* list=ImGui::GetWindowDrawList();if(!windowFonts.empty()&&windowFonts.back()==list){ImGui::PopFont();windowFonts.pop_back();}
 ImFont* font=Sized(regular,px);if(std::fabs(font->FontSize-px)<1.f)px=font->FontSize;ImGui::PushFont(font);ImGui::SetWindowFontScale(px/font->FontSize);windowFonts.push_back(list);}
void EndChildWindow(){if(!windowFonts.empty()&&windowFonts.back()==ImGui::GetWindowDrawList()){ImGui::PopFont();windowFonts.pop_back();}ImGui::EndChild();}   // same for a child window
void EndWindow(){if(!windowFonts.empty()&&windowFonts.back()==ImGui::GetWindowDrawList()){ImGui::PopFont();windowFonts.pop_back();}ImGui::End();}
const char* names[]={"Combat","Movement","Player","Visuals","Utility","HUD","Settings","Configs","Scripting","Schematics","Wardrobe"};
const char* glyphs[]={"\xee\x80\x80","\xee\x80\x81","\xee\x80\x82","\xee\x80\x83","\xee\x80\x84","\xee\x80\x83","\xee\x80\x85","\xee\x80\x87","\xee\x80\x88","\xee\x80\xa2","\xee\x80\xa8"};
std::wstring configPath;RECT previousClip{};int cursorBalance=0;
// Settings screen entries: label, description, key and default value.
// Only "Module descriptions", "Panic key" and the interface options have an effect; the remaining toggles
// keep their state without affecting anything.
// SCHEMATICS defaults: Hide placed blocks, Block outline and Block info on; Auto place and Ghost place off;
// range 4.5, delay 75 ms, 1 placement per tick.
struct MenuOption{const char* key;const char* title;const char* description;bool value;bool initial;};
MenuOption interfaceOptions[]={{"background-blur","Background blur","Blurs the entire game behind the menu.",false,false},{"module-descriptions","Module descriptions","Shows descriptions on module cards and when you hover over a setting.",true,true},{"remember-menu-state","Remember menu state","Keeps your search, selected module and open settings when you close the menu.",false,false},{"auto-save-config","Auto save config","Saves setting changes to your current config automatically.",false,false},{"open-in-menus","Open in menus","Lets the menu and keybinds work while chat, pause or another game screen is open.",false,false},{"blur-theme","Blur theme","Blurs the theme background behind the menu so text and modules stand out.",false,false}};   // "Open in menus" lets client keys work while another game screen is open
MenuOption connectionOptions[]={{"raknet-fallback","RakNet fallback","Joins servers added by address over RakNet when they give no NetherNet answer, instead of failing. Applies from the next join.",true,true},{"force-raknet","Force RakNet","Always joins servers added by address over RakNet, even when they also accept NetherNet. Applies from the next join.",false,false}};
MenuOption socialOptions[]={{"render-nametag-logo","Render nametag logo","Shows the Nickel logo next to Nickel players in the same world.",true,true},{"render-cosmetics","Render cosmetics","Shows the cosmetics other Nickel players are wearing.",true,true},{"first-person-cosmetics","First person cosmetics","Shows your own cosmetics while in first person.",false,false}};
void JumpPreAll(uint64_t a7,uint64_t a8);void JumpPostAll(uint64_t a8);
MenuOption schematicOptions[]={{"schematic-hide-placed","Hide placed blocks","Hides ghost blocks that already match the world.",true,true},{"schematic-block-outline","Block outline","Outlines every ghost block that still needs placing.",true,true},{"schematic-block-info","Block info","Shows the schematic block you're looking at and what it is.",true,true},{"schematic-auto-place","Auto place","Places schematic blocks from your hotbar while you hold right click.",false,false},{"schematic-ghost-place","Ghost place","Places without visibly switching your hotbar slot or swinging.",false,false}};
float schematicRange=4.5f;int schematicDelay=75,schematicPerTick=1;
// Item profiles: name, items, Weapon flag, optional custom mining speed (1..50, step 0.5, default 8) and the blocks
// it is limited to. "Open" edits a profile in the side panel; Speed and Limit to blocks only show with Custom mining speed.
struct ItemProfile{std::string name;bool weapon=false;std::vector<std::string> items;bool customSpeed=false;float speed=8;std::vector<std::string> blocks;};
std::vector<ItemProfile> itemProfiles;char profileInput[49]{};int profileOpen=-1;double profileOpenedAt=0;bool profileDirty=false;Module profileModule;
std::string JoinList(const std::vector<std::string>& list){std::string all;for(auto& e:list){if(!all.empty())all+=",";all+=e;}return all;}
std::vector<std::string> SplitList(const std::string& all){std::vector<std::string> list;size_t from=0;while(from<all.size()){size_t to=all.find(',',from);if(to==std::string::npos)to=all.size();if(to>from)list.push_back(all.substr(from,to-from));from=to+1;}return list;}
// Readable name of an identifier ("minecraft:diamond_sword" -> "Diamond Sword").
std::string PrettyId(const std::string& id){const size_t colon=id.rfind(':');std::string t=colon==std::string::npos?id:id.substr(colon+1);bool up=true;for(auto& ch:t){if(ch=='_'){ch=' ';up=true;}else if(up){ch=(char)toupper((unsigned char)ch);up=false;}}return t;}
// Row description of a profile: the item name for a single item, Items matching "name" for none, otherwise "N items";
// then " · Weapon" and " · Mining speed {:g} on {every block | N block(s)}".
std::string ProfileDetail(const ItemProfile& p){std::string t=p.items.size()==1?PrettyId(p.items[0]):p.items.empty()?"Items matching \""+p.name+"\"":std::to_string(p.items.size())+" items";
 if(p.weapon)t+=" \xc2\xb7 Weapon";
 if(p.customSpeed){char speed[32];snprintf(speed,sizeof(speed),"%g",p.speed);t+=std::string(" \xc2\xb7 Mining speed ")+speed+" on "+(p.blocks.empty()?std::string("every block"):std::to_string(p.blocks.size())+(p.blocks.size()==1?" block":" blocks"));}
 return t;}
// Profile that applies to an item: one listing it explicitly, otherwise one whose name is contained in the identifier.
const ItemProfile* ProfileForItem(const std::string& id){for(auto& p:itemProfiles)if(p.items.empty()?(!p.name.empty()&&id.find(p.name)!=std::string::npos):std::find(p.items.begin(),p.items.end(),id)!=p.items.end())return &p;return nullptr;}
struct MenuKey{const char* key;const char* title;const char* description;std::string name;int code;const char* initialName;int initialCode;};
MenuKey menuKeys[]={{"panic-key","Panic key","Turns off every module at once.","",0,"",0},{"menu-key","Menu key","Opens and closes the Nickel menu (also set in the launcher).","Right Shift",VK_RSHIFT,"Right Shift",VK_RSHIFT}};
// Left / right variant of a modifier from the key message (Shift by scan code, Ctrl and Alt by the extended bit).
unsigned SidedKey(unsigned vk,long long lparam){const unsigned scan=(unsigned)((lparam>>16)&0xff);const bool extended=((lparam>>24)&1)!=0;
 if(vk==VK_SHIFT)return scan==0x36?VK_RSHIFT:VK_LSHIFT;if(vk==VK_CONTROL)return extended?VK_RCONTROL:VK_LCONTROL;if(vk==VK_MENU)return extended?VK_RMENU:VK_LMENU;return vk;}
bool MenuKeyIs(unsigned long long wparam,long long lparam){const int code=menuKeys[1].code>0?menuKeys[1].code:VK_RSHIFT;return (int)SidedKey((unsigned)wparam,lparam)==code;}
std::vector<MenuOption*> MenuToggles(){std::vector<MenuOption*> all;for(auto& o:interfaceOptions)all.push_back(&o);for(auto& o:connectionOptions)all.push_back(&o);for(auto& o:schematicOptions)all.push_back(&o);return all;}
void LoadMenuSettings();
// Config keybinds (Configs screen): key code and name per config. LoadConfig reloads everything from the config file.
std::map<std::string,int> configKeys;std::map<std::string,std::string> configKeyNames;extern std::vector<std::string> configListed;extern std::string configSelected;void LoadConfig(const std::string& name);void BindConfigKey(const std::string& name,unsigned vk,long long lparam);
double resetArmed=-1;extern bool hudEditing;extern std::string hudSelected;

std::wstring Wide(const std::string& text){if(text.empty())return {};int count=MultiByteToWideChar(CP_UTF8,0,text.data(),(int)text.size(),nullptr,0);std::wstring value(count,L'\0');MultiByteToWideChar(CP_UTF8,0,text.data(),(int)text.size(),value.data(),count);return value;}
std::string Utf8(const std::wstring& text){if(text.empty())return {};int count=WideCharToMultiByte(CP_UTF8,0,text.data(),(int)text.size(),nullptr,0,nullptr,nullptr);std::string value(count,'\0');WideCharToMultiByte(CP_UTF8,0,text.data(),(int)text.size(),value.data(),count,nullptr,nullptr);return value;}
float Animate(const std::string& id,float target,float dt,float speed=16){auto& value=animation[id];value+=(target-value)*(1-std::exp(-speed*dt));if(std::fabs(target-value)<.001f)value=target;return value;}
float NavHover(const std::string& id,float target,float dt){auto& value=animation[id];value+=std::clamp(target-value,-8.f*dt,8.f*dt);return value;}
ImU32 Rgba(int r,int g,int b,float alpha=1){return IM_COL32(r,g,b,(int)(std::clamp(alpha,0.f,1.f)*255));}
void Label(ImDrawList* d,ImFont* font,float size,ImVec2 pos,const char* text,ImU32 color){if(font!=icons){pos.y-=size*.1f;size*=FontEmFactor;}pos.x=std::round(pos.x);pos.y=std::round(pos.y);ImFont* baked=Sized(font,size);if(font!=icons&&std::fabs(baked->FontSize-size)<1.f)size=baked->FontSize;d->AddText(baked,size,pos,color,text);}   // whole pixels + exact-size font: sharp text
float TextWidth(ImFont* font,float size,const char* text){return Sized(font,size*FontEmFactor)->CalcTextSizeA(size*FontEmFactor,FLT_MAX,0,text).x;}
// Longest prefix (whole UTF-8 characters) that fits with "...", trailing spaces trimmed. Binary search over character
// boundaries, measured in place (it was one string copy and one measure per removed byte, every frame, per card).
std::string Fit(ImFont* font,float size,const std::string& text,float width){if(TextWidth(font,size,text.c_str())<=width)return text;const float px=size*FontEmFactor;ImFont* f=Sized(font,px);
 const float dots=f->CalcTextSizeA(px,FLT_MAX,0,"...").x;std::vector<size_t> cuts;for(size_t i=0;i<=text.size();++i)if(i==text.size()||((unsigned char)text[i]&0xC0)!=0x80)cuts.push_back(i);
 size_t lo=0,hi=cuts.size()-1;while(lo<hi){const size_t mid=(lo+hi+1)/2;if(f->CalcTextSizeA(px,FLT_MAX,0,text.data(),text.data()+cuts[mid]).x+dots<=width)lo=mid;else hi=mid-1;}
 std::string cut=text.substr(0,cuts[lo]);while(!cut.empty()&&cut.back()==' ')cut.pop_back();return cut+"...";}
bool Hit(const char* id,ImVec2 p,ImVec2 size){ImGui::SetCursorScreenPos(p);return ImGui::InvisibleButton(id,size);}
bool Hover(ImVec2 p,ImVec2 size){return ImGui::IsMouseHoveringRect(p,ImVec2(p.x+size.x,p.y+size.y));}
bool OwnProbe(){wchar_t image[MAX_PATH]{};GetModuleFileNameW(nullptr,image,MAX_PATH);auto file=wcsrchr(image,L'\\');return !_wcsicmp(file?file+1:image,L"Nickel.OverlayProbe.exe");}

#include "theme.hpp"
// Toggle: 34 x 18 track, knob radius 7; off = #2D2D39, on = the theme's dark accent.
void Toggle(ImDrawList* d,ImVec2 t,float s,float enabled,float opacity){
 if(!themeLive.ready)ThemeTick(0);d->AddRectFilled(t,ImVec2(t.x+34*s,t.y+18*s),ToU32(MixColor(ImVec4(45/255.f,45/255.f,57/255.f,1),themeLive.accentDeep,enabled),opacity),0);
 d->AddRectFilled(ImVec2(t.x+(3+16*enabled)*s,t.y+3*s),ImVec2(t.x+(15+16*enabled)*s,t.y+15*s),Rgba(245,245,248,opacity));   // square knob
}
// Scrolling areas: 16 u fades in the panel colour at the top and bottom edges while there is content beyond them,
// so rows slide out under the edge instead of being cut on it. Called inside the child, just before EndChild.
void FadeEdges(float s,float opacity){ImDrawList* d=ImGui::GetWindowDrawList();const ImVec2 lo=ImGui::GetWindowPos(),size=ImGui::GetWindowSize();const float h=16*s;
 const ImU32 solid=Rgba(15,15,20,opacity),clear=Rgba(15,15,20,0);
 if(ImGui::GetScrollY()>.5f)d->AddRectFilledMultiColor(lo,ImVec2(lo.x+size.x,lo.y+h),solid,solid,clear,clear);
 if(ImGui::GetScrollY()<ImGui::GetScrollMaxY()-.5f)d->AddRectFilledMultiColor(ImVec2(lo.x,lo.y+size.y-h),ImVec2(lo.x+size.x,lo.y+size.y),clear,clear,solid,solid);}
bool settingsDirty=false,configEdited=false;std::string currentConfig;
unsigned settingsRevision=1;   // bumped by every setting change and config load (caches of derived settings)
// Setting changes are queued and written to the INI 0.3 s after the last one (a slider being dragged changes every
// frame; one WritePrivateProfileString per frame made the menu stutter). FlushSaves() writes them at once; it runs
// before any copy of the preferences file.
std::map<std::pair<std::wstring,std::wstring>,std::wstring> pendingSaves;double settingsChangedAt=-10;
void FlushSaves(){for(auto& [key,value]:pendingSaves)WritePrivateProfileStringW(key.first.c_str(),key.second.c_str(),value.c_str(),configPath.c_str());pendingSaves.clear();}
void Save(const Module& module,const Setting* setting=nullptr){
 if(!module.id.empty()&&module.id[0]=='@'){profileDirty=true;return;}   // item profile settings are saved with the profile
 ++settingsRevision;if(configPath.empty())return;settingsDirty=true;if(!currentConfig.empty())configEdited=true;settingsChangedAt=ImGui::GetCurrentContext()?ImGui::GetTime():0;
 if(setting){auto value=(setting->kind=="number"||setting->kind=="bool")?std::to_string(setting->number):setting->kind=="range"?std::to_string(setting->number)+";"+setting->text:setting->text;pendingSaves[{Wide(module.id),Wide(setting->id)}]=Wide(value);}
 else pendingSaves[{Wide(module.id),L"enabled"}]=module.enabled?L"1":L"0";
}
std::string Plain(double value,double step){char text[48]{};snprintf(text,sizeof(text),step>=1?"%.0f":step>=.1?"%.1f":"%.2f",value);return text;}
std::string Unit(const Setting& setting,double value){if(value==1&&(setting.unit==" blocks"||setting.unit==" chunks"||setting.unit==" ticks"))return setting.unit.substr(0,setting.unit.size()-1);return setting.unit;}
std::string Number(const Setting& setting){return Plain(setting.number,setting.step)+Unit(setting,setting.number);}
// Resource pack files are opened through the game's own file access.
static bool AcceptAllPacks(){return true;}
static void ReadPackFileWork(const std::string& path,std::vector<std::string>& out){
 game::Context c{};uint64_t gui{};if(!game::Client(&c)||!game::Read(c.client+0x668,&gui)||!gui)return;const uint64_t hostOf=game::Virtual(c,c.client,0x2f0);if(!hostOf)return;
 const uint64_t host=reinterpret_cast<uint64_t(*)(uint64_t)>(hostOf)(c.client);uint64_t stack{},begin{},end{};if(!host||!game::Read(host+0x58,&stack)||!stack||!game::Read(stack+8,&begin)||!game::Read(stack+0x10,&end)||end<begin)return;
 struct Span{uint64_t count,begin;}span{(end-begin)/0x128,begin};/* gsl::span: size, then pointer */struct Location{uint32_t system;uint32_t pad;std::string path;uint64_t hash,full;}location{};location.path=path;location.hash=game::Fnv64(path);location.full=(0xcbf29ce484222325ull*0x100000001b3ull)^location.hash;
 struct Filter{bool(*fn)();uint64_t zero;}filter{&AcceptAllPacks,0};uint64_t results[3]{};
 reinterpret_cast<void(*)(uint64_t*,Span*,Location*,Filter*)>(c.base+game::Rva(0x17c2840))(results,&span,&location,&filter);
 for(uint64_t at=results[0];at&&at<results[1];at+=0xd8){uint64_t size{},cap{},data{};if(game::Read(at+0x10,&size)&&game::Read(at+0x18,&cap)&&size<=0x800000){data=cap<16?at:0;if(cap>=16)game::Read(at,&data);if(data)out.emplace_back((const char*)data,(size_t)size);}
  reinterpret_cast<void(*)(uint64_t)>(c.base+game::Rva(0x1158f0))(at);}   // the array itself stays allocated (game allocator)
}
static bool ReadPackFileGuarded(const std::string* path,std::vector<std::string>* out){__try{ReadPackFileWork(*path,*out);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
static std::vector<std::string> ReadPackFileForIcons(const std::string& path){std::vector<std::string> out;static bool broken=false;if(broken)return out;if(!ReadPackFileGuarded(&path,&out)){broken=true;out.clear();if(WriteLog)WriteLog("Icons: a fault was caught while reading a resource pack file; registry icons are disabled");}return out;}
// Icon of a block or item in a registry list: drawn into an icon atlas bound to image slot 5. Icons load progressively
// (at most 6 per frame) and the atlas is uploaded at most every 0.2 s while loading.
void DrawRegistryIcon(ImDrawList* d,const std::string& name,bool block,ImVec2 lo,ImVec2 hi,float opacity){
 auto& cache=icons::Icons();icons::ReadPackFile=ReadPackFileForIcons;icons::Log=WriteLog;static int frame=-1;if(ImGui::GetFrameCount()!=frame){frame=ImGui::GetFrameCount();cache.Work(6);if(cache.dirty&&UpdateImage&&ImGui::GetTime()-cache.uploaded>.2){cache.dirty=false;cache.uploaded=ImGui::GetTime();UpdateImage(5,cache.atlas.data(),icons::Cache::Size,icons::Cache::Size);}}
 const int cell=cache.Lookup(name,block);
 if(cell>=0&&GetImage&&GetImage(5)){const float u=(cell%icons::Cache::PerRow)/(float)icons::Cache::PerRow,v=(cell/icons::Cache::PerRow)/(float)icons::Cache::PerRow,step=1.f/icons::Cache::PerRow;d->AddImage(GetImage(5),lo,hi,ImVec2(u,v),ImVec2(u+step,v+step),IM_COL32(255,255,255,(int)(255*opacity)));}
 else if(cell==-2){const float h=(hi.y-lo.y)*.8f;const float w=Sized(bold,h)->CalcTextSizeA(h,FLT_MAX,0,"?").x;d->AddText(Sized(bold,h),h,ImVec2((lo.x+hi.x-w)/2,lo.y+(hi.y-lo.y-h)/2),IM_COL32(180,168,205,(int)(255*opacity)),"?");}
}
ImVec4 ParseColor(const std::string& text){unsigned v=text.size()==9?(unsigned)strtoul(text.c_str()+1,nullptr,16):0xffffffffu;return ImVec4(((v>>24)&255)/255.f,((v>>16)&255)/255.f,((v>>8)&255)/255.f,(v&255)/255.f);}
std::string FormatColor(ImVec4 c){char text[16]{};snprintf(text,sizeof(text),"#%02x%02x%02x%02x",(int)std::lround(c.x*255),(int)std::lround(c.y*255),(int)std::lround(c.z*255),(int)std::lround(c.w*255));return text;}
// Two-colour gradient, sampled by linear blend. Defaults (Nickel palette): Module List orange #ff8a3d -> grey #9494a0,
// FPS orange -> white.
void GradientEnds(const std::string& module,const std::string& text,std::string ends[2]){ends[0]="#ff8a3dff";ends[1]=module=="fps"?"#ffffffff":"#9494a0ff";if(text.size()==19&&text[9]==','){ends[0]=text.substr(0,9);ends[1]=text.substr(10);}}
// Catalogue defaults, kept so that loading a config starts from a clean state.
std::vector<Module> defaults;
// Preferences are local state only; no gameplay callbacks are invoked here.
void ReadPreferences(){
 ++settingsRevision;FlushSaves();WritePrivateProfileStringW(nullptr,nullptr,nullptr,configPath.c_str());   // flush the Windows INI cache (file was replaced)
 keybinds.clear();keycodes.clear();for(size_t i=0;i<defaults.size()&&i<modules.size();++i)modules[i]=defaults[i];
 // one section read per module (it was one INI parse per setting: several thousand calls on a config load)
 std::vector<wchar_t> buffer(1<<16);
 for(auto& item:modules){
  const std::wstring section=Wide(item.id);DWORD got=0;for(;;){got=GetPrivateProfileSectionW(section.c_str(),buffer.data(),(DWORD)buffer.size(),configPath.c_str());if(got<buffer.size()-2||buffer.size()>=(1u<<24))break;buffer.resize(buffer.size()*2);}
  std::map<std::wstring,std::wstring> values;for(const wchar_t* at=buffer.data();got&&*at;at+=wcslen(at)+1){const wchar_t* eq=wcschr(at,L'=');if(!eq)continue;std::wstring key(at,eq),value(eq+1);
   auto trim=[](std::wstring& t){while(!t.empty()&&(t.back()==L' '||t.back()==L'\t'))t.pop_back();size_t f=0;while(f<t.size()&&(t[f]==L' '||t[f]==L'\t'))++f;t.erase(0,f);};trim(key);trim(value);
   if(value.size()>=2&&((value.front()==L'"'&&value.back()==L'"')||(value.front()==L'\''&&value.back()==L'\'')))value=value.substr(1,value.size()-2);   // as GetPrivateProfileString
   for(auto& ch:key)ch=(wchar_t)towlower(ch);values.emplace(key,value);}
  auto get=[&](const std::wstring& key)->const std::wstring*{std::wstring low=key;for(auto& ch:low)ch=(wchar_t)towlower(ch);auto it=values.find(low);return it==values.end()?nullptr:&it->second;};
  {auto v=get(L"enabled");item.enabled=v?_wtoi(v->c_str())==1:item.id=="discord_presence";}   // Discord Presence is on until the user turns it off
  {auto b=get(L"keybind");auto c=get(L"keycode");const int code=c?_wtoi(c->c_str()):0;if(b&&!b->empty()&&code>0){keybinds[item.id]=Utf8(*b);keycodes[item.id]=code;}}
  for(auto& setting:item.settings){
   auto found=get(Wide(setting.id));if(!found||found->empty())continue;const wchar_t* text=found->c_str();   // long lists (Xray can exceed 600 characters)
   if(setting.kind=="range"){wchar_t* end{};double low=wcstod(text,&end);if(end&&*end==L';'){wchar_t* last{};double high=wcstod(end+1,&last);if(last&&!*last&&low>=setting.minimum&&high<=setting.maximum&&low<=high){setting.number=low;setting.text=std::to_string(high);}}}
   else if(setting.kind=="number"||setting.kind=="bool"){wchar_t* end{};double value=wcstod(text,&end);if(end&&!*end&&std::isfinite(value)&&(setting.kind=="bool"?(value==0||value==1):(value>=setting.minimum&&value<=setting.maximum)))setting.number=value;}
   else{auto value=Utf8(text);if(setting.kind=="color"?(value.size()==9&&value[0]=='#'):(setting.kind!="enum"||std::find(setting.options.begin(),setting.options.end(),value)!=setting.options.end()))setting.text=value;}
  }
 }
}
}

void InitializeMenu(HMODULE module){
 auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
 auto& style=ImGui::GetStyle();style.WindowRounding=0;style.FrameRounding=0;style.GrabRounding=0;style.PopupRounding=0;style.ScrollbarRounding=0;   // square corners everywhere
 style.Colors[ImGuiCol_Text]=ImVec4(.95f,.95f,.96f,1);style.Colors[ImGuiCol_TextDisabled]=ImVec4(.58f,.58f,.63f,1);
 style.Colors[ImGuiCol_FrameBg]=ImVec4(.08f,.08f,.1f,1);style.Colors[ImGuiCol_FrameBgHovered]=ImVec4(.11f,.11f,.14f,1);style.Colors[ImGuiCol_FrameBgActive]=ImVec4(.14f,.14f,.17f,1);style.Colors[ImGuiCol_TextSelectedBg]=ImVec4(.91f,.35f,.06f,.4f);style.Colors[ImGuiCol_CheckMark]=ImVec4(1,.541f,.239f,1);style.Colors[ImGuiCol_SliderGrab]=ImVec4(1,.541f,.239f,1);style.Colors[ImGuiCol_SliderGrabActive]=ImVec4(.91f,.35f,.06f,1);style.Colors[ImGuiCol_Header]=ImVec4(.91f,.35f,.06f,.31f);style.Colors[ImGuiCol_HeaderHovered]=ImVec4(.91f,.35f,.06f,.5f);style.Colors[ImGuiCol_HeaderActive]=ImVec4(.91f,.35f,.06f,.7f);style.Colors[ImGuiCol_Button]=ImVec4(.91f,.35f,.06f,.4f);style.Colors[ImGuiCol_ButtonHovered]=ImVec4(.91f,.35f,.06f,.7f);style.Colors[ImGuiCol_ButtonActive]=ImVec4(.91f,.35f,.06f,1);style.Colors[ImGuiCol_NavHighlight]=ImVec4(1,.541f,.239f,1);
 style.Colors[ImGuiCol_Border]=ImVec4(.2f,.2f,.24f,.8f);style.Colors[ImGuiCol_PopupBg]=ImVec4(.06f,.06f,.08f,.99f);
 auto font=[&](int id,float size,const ImWchar* ranges=nullptr){auto resource=FindResourceW(module,MAKEINTRESOURCEW(id),RT_RCDATA);auto data=LockResource(LoadResource(module,resource));ImFontConfig config{};config.FontDataOwnedByAtlas=false;return io.Fonts->AddFontFromMemoryTTF(data,(int)SizeofResource(module,resource),size,&config,ranges);};
 regular=font(102,30);bold=font(103,36);semibold=font(105,30);
 // Small section captions (FEATURES, OTHER...) use a dedicated font loaded at the displayed size
 // (9 x 2732/2000 ≈ 12.3 px); downscaling a 30 px font made them blurry.
 tiny=font(105,11.3f);{ImFontConfig config{};config.OversampleH=3;config.OversampleV=3;   // icon font (theme.hpp, DrawGlyph)
  auto resource=FindResourceW(module,MAKEINTRESOURCEW(107),RT_RCDATA);if(resource){config.FontDataOwnedByAtlas=false;
   iconFont=io.Fonts->AddFontFromMemoryTTF(LockResource(LoadResource(module,resource)),(int)SizeofResource(module,resource),48,&config,(const ImWchar*)nicons::kIconRanges);}}io.FontDefault=regular;
 {auto data=[&](int id,FontFamily& f){auto resource=FindResourceW(module,MAKEINTRESOURCEW(id),RT_RCDATA);f.data=LockResource(LoadResource(module,resource));f.bytes=(int)SizeofResource(module,resource);};
  data(102,fontFamilies[regular]);data(103,fontFamilies[bold]);data(105,fontFamilies[semibold]);data(105,fontFamilies[tiny]);
  fontSizesPath=PreferencesPath(module);
  // sizes used in earlier sessions are baked at start, so the menu and the HUD are sharp from the first frame
  wchar_t saved[8192]{};GetPrivateProfileStringW(L"fonts",L"sizes",L"",saved,8192,fontSizesPath.c_str());std::wstring list=saved;size_t at=0;
  while(at<list.size()){size_t end=list.find(L',',at);if(end==std::wstring::npos)end=list.size();const std::wstring item=list.substr(at,end-at);at=end+1;const size_t colon=item.find(L':');if(colon==std::wstring::npos)continue;
   const std::wstring name=item.substr(0,colon);const int size=_wtoi(item.substr(colon+1).c_str());ImFont* base=name==L"regular"?regular:name==L"bold"?bold:name==L"semibold"?semibold:name==L"tiny"?tiny:nullptr;
   if(base&&size>=6&&size<=256&&FontKey((float)size)==size)AddSizedFont(fontFamilies[base],size);}}
 configPath=PreferencesPath(module);
 // The catalogue derives names from identifiers ("Chest Esp", "Fov"); acronyms are written in capitals.
 auto acronyms=[](std::string& name){for(const char* word:{"Esp","Fov","Fps","Ip","Hud","Tps","Xp"}){size_t n=strlen(word),at=0;while((at=name.find(word,at))!=std::string::npos){bool start=at==0||name[at-1]==' ',end=at+n==name.size()||name[at+n]==' ';if(start&&end)for(size_t i=0;i<n;++i)name[at+i]=(char)std::toupper((unsigned char)name[at+i]);at+=n;}}};
 for(auto& item:modules){acronyms(item.name);for(auto& setting:item.settings)acronyms(setting.name);}
 {auto number=[&](const wchar_t* key,float fallback){wchar_t text[32]{};GetPrivateProfileStringW(L"menu",key,L"",text,32,configPath.c_str());wchar_t* end{};double value=wcstod(text,&end);return text[0]&&end&&!*end?(float)value:fallback;};
  menuW=std::clamp(number(L"width",1012),820.f,1600.f);menuH=std::clamp(number(L"height",578),430.f,1000.f);menuScale=std::clamp(number(L"scale",1),.6f,1.8f);menuX=number(L"x",-1);menuY=number(L"y",-1);}
 // Setting options missing from the catalogue (Block Hit); Item Inspect's default key is V.
 for(auto& item:modules)if(item.id=="block_hit")for(auto& setting:item.settings)if(setting.id=="animation"){setting.kind="enum";setting.options={"1.7","1.8","Exhibition"};setting.text="1.7";}
 // Nuker mode (Nickel): Auto breaks around the player all the time, On Break only around a block the player breaks.
 for(auto& item:modules)if(item.id=="nuker"&&!item.settings.empty()&&item.settings.front().id!="mode"){Setting st=item.settings.front();st.id="mode";st.name="Mode";
  st.description="Auto breaks around you all the time, On Break only around a block you break";st.kind="enum";st.options={"Auto","On Break"};st.text="Auto";st.unit="";st.dependsOn=-1;
  for(auto& o:item.settings)if(o.dependsOn>=0)++o.dependsOn;item.settings.insert(item.settings.begin(),st);}
 // Custom Sky mode names.
 for(auto& item:modules)if(item.id=="custom_sky")for(auto& setting:item.settings)if(setting.id=="mode"){setting.kind="enum";setting.options={"Aurora","Nebula","Galaxy","IonStorm","GoldenHour"};if(std::find(setting.options.begin(),setting.options.end(),setting.text)==setting.options.end())setting.text="Aurora";}
 // Block ESP per-block styles (Default / Gradient / Rainbow / Solid, at most 7) and the keys chosen in the HUD layout
 // editor for Keystrokes, stored in hidden settings (menu_catalog.hpp).
 AddEditorSettings(modules);
 // Custom Crosshair: 17 x 17 "Pattern" grid, visible when Shape = Custom.
 for(auto& item:modules)if(item.id=="custom_crosshair")for(auto& st:item.settings)if(st.id=="shape"&&st.dependsOn==0)st.dependsOn=-1;   // Shape depends on nothing (the catalogue linked it to itself)
 for(auto& item:modules)if(item.id=="custom_crosshair"&&!item.settings.empty()){bool has=false;for(auto& st:item.settings)if(st.id=="pattern")has=true;if(!has){int shape=0;for(int i=0;i<(int)item.settings.size();++i)if(item.settings[i].id=="shape")shape=i;
  Setting st=item.settings.front();st.id="pattern";st.name="Pattern";st.description="Click cells to draw your own crosshair";st.kind="pattern";st.text=crosshairDefault;st.options.clear();st.unit="";st.dependsOn=shape;st.dependsEqual=true;st.dependsValue=4;for(auto& o:item.settings)if(o.dependsOn>shape)++o.dependsOn;item.settings.insert(item.settings.begin()+shape+1,st);}}
 // Item ESP: "Render" multi-choice with fixed options (Box, Outline, Name Tag, Tracer; Box and Name Tag by default).
 for(auto& item:modules)if(item.id=="item_esp")for(auto& st:item.settings)if(st.id=="render"){st.options={"Box","Outline","Name Tag","Tracer"};if(st.text.empty())st.text="Box,Name Tag";}
 for(auto& item:modules)if(item.id=="anti_debuff")for(auto& st:item.settings)if(st.id=="effects"){st.options={"Blindness","Nausea","Slowness","Wither","Mining Fatigue","Instant Damage","Darkness","Poison","Fatal Poison","Weakness","Hunger","Levitation","Slow Falling","Bad Omen"};if(st.text.empty())st.text="Blindness,Nausea,Slowness,Wither,Mining Fatigue,Instant Damage,Darkness";}
 for(auto& item:modules)if(item.id=="base_finder")for(auto& st:item.settings)if(st.id=="signals"){st.options={"Storage","Amethyst","Light Sources","Utility Blocks","Building Blocks","Farms And Redstone","Valuables","Decoration","Portals And Obsidian","Underground","Out Of Place","Custom"};if(st.text.empty())st.text="Storage,Amethyst,Light Sources,Utility Blocks,Building Blocks,Farms And Redstone,Valuables,Decoration,Portals And Obsidian,Underground,Out Of Place";}
 for(auto& item:modules)if(item.id=="item_name")for(auto& setting:item.settings)if(setting.id=="format"&&setting.text.empty())setting.text="{name}";
 // Notifications: only Module Toggles applies; Messages, Friend Requests, Friend Activity and Connection need online
 // services and are hidden. Discord Presence is implemented in native/game_discord.hpp.
 for(auto& item:modules)if(item.id=="discord_presence"){item.description="Shows Nickel activity on Discord";item.enabled=true;}   // on by default
 for(auto& item:modules)if(item.id=="notifications"){item.description="Toggle alerts";for(auto& st:item.settings)if(st.id!="module_toggles")st.kind="hidden";}
 // Script modules are appended later without reallocation, since pointers into `modules` are kept (ModuleById caches them).
 // InitializeMenu can run again when the renderer is recreated, so the capacity is reserved only once.
 if(modules.capacity()<modules.size()+256)modules.reserve(modules.size()+1024);
 defaults=modules;ReadPreferences();if(!keycodes.count("item_inspect")){keybinds["item_inspect"]="V";keycodes["item_inspect"]='V';}
 LoadMenuSettings();ThemeLoad();{wchar_t name[128]{};GetPrivateProfileStringW(L"menu",L"current-config",L"",name,128,configPath.c_str());currentConfig=Utf8(name);}
 // Offline review (probe only): tab and module to display.
 if(OwnProbe()){char value[64]{};
  if(GetEnvironmentVariableA("NICKEL_REVIEW_CONFIG",value,sizeof(value)))configSelected=value;   // offline review of the details panel
  if(GetEnvironmentVariableA("NICKEL_REVIEW_BLUR",value,sizeof(value)))interfaceOptions[0].value=true;
  if(GetEnvironmentVariableA("NICKEL_REVIEW_THEME",value,sizeof(value))){themeIndex=std::clamp(atoi(value),0,themeCount-1);themeLive.ready=false;}   // offline review of a theme
  if(GetEnvironmentVariableA("NICKEL_REVIEW_TAB",value,sizeof(value))){tab=std::clamp(atoi(value),0,10);if(tab<6)category=tab;}
  if(GetEnvironmentVariableA("NICKEL_REVIEW_SELECT",value,sizeof(value)))for(int i=0;i<(int)modules.size();++i)if(modules[i].id==value)selected=i;
  if(GetEnvironmentVariableA("NICKEL_REVIEW_HUD",value,sizeof(value))){hudEditing=true;   // offline review of the HUD editor: a few elements enabled in memory, one selected
   for(auto& item:modules)for(const char* id:{"fps","clock","coordinates","speedometer","keystrokes","module_list","direction","server_ip","effect_display","spotify_manager","ping","reach_display","combo_counter","totem_pop_counter","target_hud","player_list","armor_hud","minimap","debug_overlay","notifications"})if(item.id==id)item.enabled=true;notices.push_back({5,"Full Bright","Enabled"});notices.push_back({7,"Configs","Loaded config 'pvp'"});hudSelected="clock";
   if(value[0]=='2')for(auto& item:modules){if(item.id=="fps")for(auto& setting:item.settings)if(setting.id=="gradient")setting.number=1;if(item.id=="module_list")for(auto& setting:item.settings)if(setting.id=="alignment")setting.text="Left";}}   // 2: FPS gradient and left-aligned module list
 }
}
bool MenuIsOpen(){return opened;}
// Right Shift, the map key and module keys only act in game with no other game screen open (chat, pause, inventory,
// main menu), unless "Open in menus" is enabled in Settings (off by default; fallback: open-in-menus=1 under [menu]
// in module-preferences.ini). "In game" means the game owns the mouse: cursor hidden or clipped to the window.
bool Playing(){
 if(OwnProbe())return true;CURSORINFO info{};info.cbSize=sizeof(info);const bool hidden=GetCursorInfo(&info)&&(!(info.flags&CURSOR_SHOWING)||!info.hCursor);
 RECT clip{};const bool clipped=GetClipCursor(&clip)&&(clip.right-clip.left<GetSystemMetrics(SM_CXVIRTUALSCREEN)||clip.bottom-clip.top<GetSystemMetrics(SM_CYVIRTUALSCREEN));return hidden||clipped;
}
bool KeysAllowed(){return interfaceOptions[4].value||Playing();}
bool CanToggleNow(){if(opened||KeysAllowed())return true;static ULONGLONG told=0;if(WriteLog&&GetTickCount64()-told>5000){told=GetTickCount64();WriteLog("Menu: Right Shift ignored, the game is showing another screen (Settings > Open in menus allows it)");}return false;}
void SetWindow(HWND hwnd){gameWindowForKeys=hwnd;}
// Key code of a key setting ("Name|code"), 0 if none.
int SettingKey(const Module& item,const char* id){for(auto& setting:item.settings)if(setting.id==id){const size_t bar=setting.text.find('|');return bar==std::string::npos?0:atoi(setting.text.c_str()+bar+1);}return 0;}
bool MenuKeyMatches(unsigned long long wparam,long long lparam){return MenuKeyIs(wparam,lparam);}
bool RecordingKey(){return opened&&(hudEditing||listeningSetting||listening>=0||listening<=-100);}
// Called on the render thread for each initial key press (overlay.cpp drains its queue before every frame).
// Menu open with a "Keybind" pill listening: the key is recorded (Escape, Backspace, Delete clear it).
// Menu closed: the key toggles every module bound to it.
// Keystrokes key being re-bound in the HUD layout editor (0..6 = forward, left, back, right, jump, attack, use; -1 = none)
int keystrokeListening=-1;ImVec2 keystrokeRects[7][2];bool keystrokeDrawn[7]{};
std::array<int,7> KeystrokeCustom(const Module& keys){std::array<int,7> out{};const Setting* st=nullptr;for(auto& one:keys.settings)if(one.id=="custom_keys")st=&one;if(!st)return out;size_t from=0;for(int i=0;i<7&&from<=st->text.size();++i){size_t to=st->text.find(',',from);if(to==std::string::npos)to=st->text.size();out[i]=atoi(st->text.substr(from,to-from).c_str());from=to+1;}return out;}
void KeystrokeBind(int index,int code){Module* keys=nullptr;for(auto& m:modules)if(m.id=="keystrokes")keys=&m;if(!keys||index<0||index>6)return;auto custom=KeystrokeCustom(*keys);custom[index]=code;
 for(auto& st:keys->settings)if(st.id=="custom_keys"){std::string text;for(int i=0;i<7;++i){if(i)text+=",";text+=std::to_string(custom[i]);}st.text=text;Save(*keys,&st);}}
void HandleKey(unsigned vk,long long lparam){
 // Menu key being recorded (Settings > Keybinds): modifiers count with their side; Escape cancels, Backspace / Delete
 // go back to Right Shift.
 if(opened&&listening==-101){auto& key=menuKeys[1];listening=-1;if(vk==VK_ESCAPE)return;unsigned code=SidedKey(vk,lparam);std::wstring name;
  if(vk==VK_BACK||vk==VK_DELETE){code=VK_RSHIFT;name=L"Right Shift";}
  else{wchar_t text[64]{};const LONG scan=(LONG)(lparam&0x01ff0000);if(!GetKeyNameTextW(scan,text,64))swprintf(text,64,L"Key %u",code);name=text;
   if(code==VK_RSHIFT)name=L"Right Shift";else if(code==VK_RCONTROL)name=L"Right Ctrl";else if(code==VK_RMENU)name=L"Right Alt";}
  key.name=Utf8(name);key.code=(int)code;WritePrivateProfileStringW(L"menu",L"menu-key",name.c_str(),configPath.c_str());WritePrivateProfileStringW(L"menu",L"menu-key-code",std::to_wstring(code).c_str(),configPath.c_str());return;}
 // re-binding a Keystrokes key: Escape cancels, Backspace / Delete goes back to the game's binding, any other key is taken
 if(keystrokeListening>=0){const int index=keystrokeListening;keystrokeListening=-1;if(vk==VK_ESCAPE)return;KeystrokeBind(index,vk==VK_BACK||vk==VK_DELETE?0:(int)vk);return;}
 if(vk==VK_SHIFT||vk==VK_RSHIFT||vk==VK_LSHIFT||vk==VK_CONTROL||vk==VK_MENU||vk==VK_LWIN||vk==VK_RWIN)return;
 if(opened&&waypointsOpen)return;   // Waypoints manager: text input goes through ImGui
 if(opened&&mapOpen){for(auto& item:modules)if(item.id=="minimap"&&SettingKey(item,"world_map_key")==(int)vk&&gameWindowForKeys)ToggleMenu(gameWindowForKeys);return;}   // the same key closes the map
 if(opened){
  if(hudEditing){if(vk==VK_ESCAPE)hudEditing=false;return;}   // "Press Escape To Exit"
  if(listeningSetting){auto* setting=listeningSetting;listeningSetting=nullptr;if(vk==VK_ESCAPE)return;if(vk==VK_BACK||vk==VK_DELETE)setting->text.clear();else{wchar_t name[64]{};if(!GetKeyNameTextW((LONG)lparam,name,64))swprintf(name,64,L"Key %u",vk);setting->text=Utf8(name)+"|"+std::to_string(vk);}if(listeningModule)Save(*listeningModule,setting);return;}
  if(listening<=-200){const int row=-200-listening;listening=-1;if(row<(int)configListed.size())BindConfigKey(configListed[row],vk,lparam);return;}   // config keybind
  if(listening<=-100&&listening>-102){   // Settings screen keybind (Panic key)
   auto& key=menuKeys[-100-listening];auto codeName=Wide(std::string(key.key)+"-code");
   // Escape cancels listening; Backspace or Delete clears the key.
   if(vk==VK_ESCAPE){}
   else if(vk==VK_BACK||vk==VK_DELETE){key.name.clear();key.code=0;WritePrivateProfileStringW(L"menu",Wide(key.key).c_str(),L"",configPath.c_str());WritePrivateProfileStringW(L"menu",codeName.c_str(),L"0",configPath.c_str());}
   else{wchar_t name[64]{};if(!GetKeyNameTextW((LONG)lparam,name,64))swprintf(name,64,L"Key %u",vk);key.name=Utf8(name);key.code=(int)vk;WritePrivateProfileStringW(L"menu",Wide(key.key).c_str(),name,configPath.c_str());WritePrivateProfileStringW(L"menu",codeName.c_str(),std::to_wstring(vk).c_str(),configPath.c_str());}
   listening=-1;return;
  }
  if(listening<0||listening>=(int)modules.size())return;auto& item=modules[listening];auto section=Wide(item.id);
  if(vk==VK_ESCAPE||vk==VK_BACK||vk==VK_DELETE){keybinds.erase(item.id);keycodes.erase(item.id);WritePrivateProfileStringW(section.c_str(),L"keybind",nullptr,configPath.c_str());WritePrivateProfileStringW(section.c_str(),L"keycode",nullptr,configPath.c_str());}
  else{wchar_t name[64]{};if(!GetKeyNameTextW((LONG)lparam,name,64))swprintf(name,64,L"Key %u",vk);keybinds[item.id]=Utf8(name);keycodes[item.id]=(int)vk;WritePrivateProfileStringW(section.c_str(),L"keybind",name,configPath.c_str());WritePrivateProfileStringW(section.c_str(),L"keycode",std::to_wstring(vk).c_str(),configPath.c_str());}
  listening=-1;return;
 }
 if(!KeysAllowed())return;ScriptsKey(vk);   // chat, pause, inventory...: no client key acts
 for(auto& item:modules)if(item.id=="minimap"&&item.enabled&&gameWindowForKeys&&SettingKey(item,"world_map_key")==(int)vk){mapOpen=true;ToggleMenu(gameWindowForKeys);return;}
 for(auto& item:modules)if(item.id=="waypoints"&&item.enabled&&gameWindowForKeys&&SettingKey(item,"manager_key")==(int)vk){waypointsOpen=true;ToggleMenu(gameWindowForKeys);return;}   // Waypoints Manager Key; World Map Key opens a full-screen map with input captured like the menu
 // "Panic key": turns off every module at once.
 if(menuKeys[0].code==(int)vk){for(auto& item:modules)if(item.enabled){item.enabled=false;Save(item);}return;}
 for(auto& bound:configKeys)if(bound.second==(int)vk){LoadConfig(std::string(bound.first));return;}   // config keys take precedence over module keys
 for(auto& item:modules){auto bound=keycodes.find(item.id);if(bound!=keycodes.end()&&bound->second==(int)vk&&item.id!="player_list"&&item.id!="zoom"&&item.id!="jetpack"){item.enabled=!item.enabled;Save(item);if(item.id!="item_inspect"&&notices.size()<16)notices.push_back({5,item.name,item.enabled?"Enabled":"Disabled"});}}   // Player List is shown while its key is held (see DrawHud)
}
// Mouse release is handled by the GameInput / ClipCursor detours in overlay.cpp; calling the game's own cursor
// functions here caused a downward camera jump when opening the menu.
ULONGLONG menuOpenedAt=0;uint64_t menuOpenScreens[game::kScreenSlots]{};bool menuOpenedInWorld=false,menuOpenedPlaying=false;bool menuClosingForScreen=false;   // MenuFollowScreens
std::atomic<ULONGLONG> gameLockAt{0},gameUnlockAt{0};
void NoteGameCursor(bool lock){(lock?gameLockAt:gameUnlockAt).store(GetTickCount64());}
void ToggleMenu(HWND hwnd){
 if(!opened&&!KeysAllowed()){mapOpen=false;return;}
 opened=!opened;game::menuPause=opened.load();
 if(opened){menuOpenedAt=GetTickCount64();for(int i=0;i<game::kScreenSlots;++i){const ULONGLONG at=game::otherScreenAt[i].load();menuOpenScreens[i]=at&&menuOpenedAt-at<250?game::otherScreenHash[i].load():0;}game::Context c{};menuOpenedInWorld=game::Local(&c);menuOpenedPlaying=Playing();}
 if(opened){GetClipCursor(&previousClip);ClipCursor(nullptr);cursorBalance=0;for(int i=0;i<16;++i){++cursorBalance;if(ShowCursor(TRUE)>=0)break;}SetCursor(LoadCursor(nullptr,IDC_ARROW));}
 else{while(cursorBalance>0){ShowCursor(FALSE);--cursorBalance;}if(!menuClosingForScreen&&GetForegroundWindow()==GetAncestor(hwnd,GA_ROOT))ClipCursor(&previousClip);listening=-1;hudEditing=false;mapOpen=false;waypointsOpen=false;dragMode=0;if(!interfaceOptions[2].value){search[0]=0;if(!OwnProbe())selected=-1;}}   // "Remember menu state": keep the search text and the open module
}
void CloseMenu(HWND hwnd){if(opened)ToggleMenu(hwnd);}
// The menu closes itself when the game opens another screen while it is up (chat, pause, inventory, disconnect, death
// screen...). A screen already showing when the menu was opened ("Open in menus") does not close it; a new one does.
// Also closes when the player leaves the world (disconnect, kick: the disconnect screen is not drawn through the
// screen hook). hud_crosshair_screen is part of the in-game HUD, not another screen.
static void CloseForScreen(HWND hwnd,const char* why){if(WriteLog){char line[96];wsprintfA(line,"Menu: closed, %s",why);WriteLog(line);}menuClosingForScreen=true;CloseMenu(hwnd);menuClosingForScreen=false;}
void MenuFollowScreens(HWND hwnd){if(!opened||OwnProbe())return;const ULONGLONG now=GetTickCount64();if(now-menuOpenedAt<300)return;
 game::Context c{};if(menuOpenedInWorld&&!game::Local(&c)){CloseForScreen(hwnd,"the player left the world");return;}
 // Ore UI screens (death, pause, disconnect...) are not drawn through the screen hook. While playing, the game keeps
 // locking the cursor (ignored while the menu is up); a release, or the requests stopping, means it opened a screen.
 if(menuOpenedPlaying){const ULONGLONG lock=gameLockAt.load(),unlock=gameUnlockAt.load();
  if(unlock>menuOpenedAt+150&&unlock>=lock){CloseForScreen(hwnd,"the game released the cursor");return;}
  if(lock>menuOpenedAt&&now-lock>400){CloseForScreen(hwnd,"the game stopped locking the cursor");return;}}
 for(int i=0;i<game::kScreenSlots;++i){const ULONGLONG at=game::otherScreenAt[i].load();if(!at||now-at>250||at<menuOpenedAt)continue;const uint64_t h=game::otherScreenHash[i].load();bool known=false;for(uint64_t k:menuOpenScreens)known|=k&&k==h;if(known)continue;
  CloseForScreen(hwnd,"the game opened another screen");return;}}
// Open / close animation: smoothstep over 0.28 s.
float MenuProgress(float dt){progress=std::clamp(progress+(opened?1.f:-1.f)*dt/.28f,0.f,1.f);return progress*progress*(3-2*progress);}

namespace {
// Floating settings panel: 297 x 578, 19 units to the right of the menu.
// Block ESP per-block styles live in the hidden "block_styles" setting ("name,mode,#c1,#c2 ; ...", mode g / r / s).
struct BlockOwnStyle{char mode='d';std::string a="#ff8a3dff",b="#9494a0ff";};
std::map<std::string,BlockOwnStyle> ParseBlockStyles(const std::string& text){std::map<std::string,BlockOwnStyle> out;size_t from=0;
 while(from<text.size()){size_t end=text.find(';',from);if(end==std::string::npos)end=text.size();const std::string item=text.substr(from,end-from);from=end+1;
  size_t c1=item.find(','),c2=c1==std::string::npos?c1:item.find(',',c1+1),c3=c2==std::string::npos?c2:item.find(',',c2+1);if(c3==std::string::npos)continue;
  BlockOwnStyle st;st.mode=item[c1+1];st.a=item.substr(c2+1,c3-c2-1);st.b=item.substr(c3+1);if(st.mode=='g'||st.mode=='r'||st.mode=='s')out[item.substr(0,c1)]=st;}
 return out;}
std::string SerializeBlockStyles(const std::map<std::string,BlockOwnStyle>& styles){std::string out;for(auto& e:styles){if(e.second.mode=='d')continue;if(!out.empty())out+=';';out+=e.first+','+e.second.mode+','+e.second.a+','+e.second.b;}return out;}
void DrawSettingsPanel(Module& m,ImVec2 origin,float s,float opacity){
 auto& io=ImGui::GetIO();auto P=[&](float x,float y){return ImVec2(origin.x+x*s,origin.y+y*s);};
 ImGui::SetNextWindowPos(origin);ImGui::SetNextWindowSize(ImVec2(297*s,menuH*s));ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
 ImGui::Begin("##NickelSettingsPanel",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBackground|(opened.load()?0:ImGuiWindowFlags_NoInputs));
 auto* d=ImGui::GetWindowDrawList();d->AddRectFilled(origin,P(297,menuH),Rgba(15,15,20,.98f*opacity),0);d->AddRect(origin,P(297,menuH),Rgba(26,26,33,opacity),0);
 // Header: 18 px bold title at (15,13), toggle at (216,13), close cross centred at (272,22), 13 px description
 // at (15,44), separator at y=76.
 d->PushClipRect(P(15,0),P(208,40),true);Label(d,bold,18*s,P(15,13),m.name.c_str(),Rgba(255,255,255,opacity));d->PopClipRect();
 Label(d,regular,13*s,P(15,44),Fit(regular,13,m.description,267).c_str(),Rgba(130,130,142,opacity));
 const bool profile=m.id=="@profile";
 if(!profile){Toggle(d,P(216,13),s,Animate("panel"+m.id,m.enabled?1:0,io.DeltaTime),opacity);if(Hit("##panelToggle",P(216,13),ImVec2(34*s,18*s))){m.enabled=!m.enabled;Save(m);}}
 const float cross=Hover(P(262,12),ImVec2(20*s,20*s))?1.f:.75f;if(!DrawGlyph(d,nicons::k_x,P(262,12),20*s,Rgba(235,235,240,cross*opacity))){d->AddLine(P(267,17),P(277,27),Rgba(235,235,240,cross*opacity),1.6f*s);d->AddLine(P(277,17),P(267,27),Rgba(235,235,240,cross*opacity),1.6f*s);}
 if(Hit("##panelClose",P(262,12),ImVec2(20*s,20*s))){if(profile)profileOpen=-1;else selected=-1;listening=-1;}
 d->AddLine(P(0,76),P(297,76),Rgba(255,255,255,.07f*opacity),1);
 // Scrolling area: rows from x=24 to x=273, 33 units between two labels.
 ImGui::SetCursorScreenPos(P(0,84));ImGui::BeginChild("##SettingsRows",ImVec2(297*s,(menuH-92)*s),false,ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoScrollbar);
 WindowFont(s*14.f*FontEmFactor);d=ImGui::GetWindowDrawList();const ImVec2 base=ImGui::GetCursorScreenPos();float y=6;auto R=[&](float x,float dy){return ImVec2(base.x+x*s,base.y+(y+dy)*s);};
 const ImU32 text=Rgba(235,235,240,opacity),muted=Rgba(170,170,180,opacity);
 auto pill=[&](const char* value,ImU32 fill,ImU32 color,float minimum,bool alignRight=false){float w=std::max(minimum,TextWidth(bold,12,value)+16);d->AddRectFilled(R(273-w,-1),R(273,19),fill,0);Label(d,bold,12*s,R(alignRight?273-8-TextWidth(bold,12,value):273-w+(w-TextWidth(bold,12,value))/2,2),value,color);return w;};
 // Keybind: "None" pill on the right.
 if(profile){
  // Item preview (132 units): first selected item, otherwise the first identifier containing the profile name.
  std::string shownId;{Setting* items=nullptr;for(auto& st:m.settings)if(st.id=="items")items=&st;auto list=items?SplitList(items->text):std::vector<std::string>{};
   if(!list.empty())shownId=list[0];else if(profileOpen>=0&&profileOpen<(int)itemProfiles.size()&&!itemProfiles[profileOpen].name.empty())for(auto& n:RegistryNames(false))if(n.find(itemProfiles[profileOpen].name)!=std::string::npos){shownId=n;break;}}
  d->AddRectFilled(R(24,0),R(273,104),Rgba(255,255,255,.04f*opacity),0);
  if(!shownId.empty()){DrawRegistryIcon(d,shownId,false,R(116,10),R(181,75),opacity);const std::string label=PrettyId(shownId);Label(d,regular,12*s,R(148.5f-TextWidth(regular,12,label.c_str())/2,82),label.c_str(),muted);}
  else Label(d,regular,13*s,R(148.5f-TextWidth(regular,13,"Failed to find item")/2,45),"Failed to find item",muted);
  y+=114;d->AddLine(R(24,-5),R(273,-5),Rgba(255,255,255,.07f*opacity),1);}
 else{Label(d,semibold,15*s,R(24,0),"Keybind",text);auto bound=keybinds.find(m.id);bool waiting=listening==(int)(&m-&modules[0]);
  float w=pill(waiting?"...":bound==keybinds.end()?"None":bound->second.c_str(),Rgba(255,255,255,(waiting?.16f:.08f)*opacity),muted,48);
  if(Hit("##keybind",R(273-w,-1),ImVec2(w*s,20*s)))listening=waiting?-1:(int)(&m-&modules[0]);
  y+=33;}
 // Visibility condition (e.g. Kill Aura shows "Aim Range" only in Silent mode).
 auto shown=[&](const Setting& setting){
  if(setting.dependsOn<0||setting.dependsOn>=(int)m.settings.size())return true;const auto& other=m.settings[setting.dependsOn];int value;
  if(other.kind=="enum")value=(int)(std::find(other.options.begin(),other.options.end(),other.text)-other.options.begin());else if(other.kind=="bool")value=other.number!=0?1:0;else return true;
  return (value==setting.dependsValue)==setting.dependsEqual;};
 for(auto& setting:m.settings){
  if(setting.kind=="hidden"||!shown(setting))continue;
  ImGui::PushID(setting.id.c_str());const float pad=setting.kind=="bool"?4.f:0.f;Label(d,semibold,15*s,R(24,pad),setting.name.c_str(),text);
  if(setting.kind=="enum"){
   // Checkbox list: box x 25..273, 5 units of padding, 28 per option, check mark at x=42, text at x=57.
   const float height=28.f*setting.options.size()+6;d->AddRectFilled(R(25,24),R(273,24+height),Rgba(23,23,29,.92f*opacity),0);
   for(int i=0;i<(int)setting.options.size();++i){const float top=27+28.f*i;bool active=setting.text==setting.options[i];bool hover=Hover(R(25,top),ImVec2(248*s,28*s));
    if(hover&&!active)d->AddRectFilled(R(28,top),R(270,top+28),Rgba(255,255,255,.04f*opacity),0);
    if(active){ImVec2 points[]={R(37,top+14),R(41.5f,top+18.5f),R(49,top+10)};d->AddPolyline(points,3,Rgba(255,255,255,opacity),0,1.8f*s);}
    Label(d,regular,14*s,R(57,top+6),setting.options[i].c_str(),active?Rgba(255,255,255,opacity):Rgba(224,224,230,opacity));
    if(Hit(("##option"+std::to_string(i)).c_str(),R(25,top),ImVec2(248*s,28*s))&&!active){setting.text=setting.options[i];Save(m,&setting);}}
   y+=24+height+15;
  }else if(setting.kind=="number"){
   // Slider: value pill on the right, track x 31..267 placed 27 units under the label, white knob of radius 7.
   auto value=Number(setting);pill(value.c_str(),AccDim(opacity),Rgba(255,255,255,opacity),81,true);
   const float span=(float)(setting.maximum-setting.minimum);float t=span>0?(float)((setting.number-setting.minimum)/span):0;
   Hit("##slider",R(24,25),ImVec2(250*s,20*s));
   if(ImGui::IsItemActive()&&span>0){float pick=std::clamp((io.MousePos.x-R(31,0).x)/(236*s),0.f,1.f);double next=setting.minimum+pick*span;if(setting.step>0)next=setting.minimum+std::round((next-setting.minimum)/setting.step)*setting.step;next=std::clamp(next,setting.minimum,setting.maximum);if(next!=setting.number){setting.number=next;Save(m,&setting);}t=(float)((setting.number-setting.minimum)/span);}
   // While not dragging, the knob follows the value with the same exponential smoothing.
   if(!ImGui::IsItemActive())t=Animate("slide"+m.id+setting.id,t,io.DeltaTime);else animation["slide"+m.id+setting.id]=t;
   d->AddRectFilled(R(31,33),R(267,37),Rgba(255,255,255,.13f*opacity),0);d->AddRectFilled(R(31,33),R(31+236*t,37),AccDeep(opacity),0);d->AddCircleFilled(R(31+236*t,35),7*s,Rgba(238,241,255,opacity));
   y+=60;
  }else if(setting.kind=="bool"){
   Toggle(d,R(239,pad),s,Animate("setting"+m.id+setting.id,setting.number!=0?1:0,io.DeltaTime),opacity);
   if(Hit("##bool",R(24,pad-4),ImVec2(250*s,26*s))){setting.number=setting.number!=0?0:1;Save(m,&setting);}
   y+=37;
  }else if(setting.kind=="color"||setting.kind=="gradient"){
   // Colour picker: 39 x 22 pill on the right, 248 x 120 saturation/value square, 13 px hue strip, 12 px alpha strip.
   const bool gradient=setting.kind=="gradient";auto& stop=animation["stop"+m.id+setting.id];std::string ends[2];GradientEnds(m.id,setting.text,ends);ImVec4 color=ParseColor(gradient?ends[stop<.5f?0:1]:setting.text);float h,sat,val;ImGui::ColorConvertRGBtoHSV(color.x,color.y,color.z,h,sat,val);auto& hue=animation["hue"+m.id+setting.id];if(sat>0&&val>0)hue=h;else h=hue;bool changed=false;
   if(gradient){for(int e=0;e<2;++e){const float x=e?234.f:190.f;ImVec4 c=ParseColor(ends[e]);d->AddRectFilled(R(x,-2),R(x+39,20),ImGui::ColorConvertFloat4ToU32(ImVec4(c.x,c.y,c.z,opacity)),0);if((stop<.5f)==(e==0))d->AddRect(R(x-1,-3),R(x+40,21),Rgba(255,255,255,opacity),0,0,1.5f*s);if(Hit(e?"##stop1":"##stop0",R(x,-2),ImVec2(39*s,22*s))){stop=(float)e;}}}else d->AddRectFilled(R(234,-2),R(273,20),ImGui::ColorConvertFloat4ToU32(ImVec4(color.x,color.y,color.z,opacity)),0);
   ImVec4 pure;ImGui::ColorConvertHSVtoRGB(h,1,1,pure.x,pure.y,pure.z);const ImU32 full=ImGui::ColorConvertFloat4ToU32(ImVec4(pure.x,pure.y,pure.z,opacity)),white=Rgba(255,255,255,opacity),black=Rgba(0,0,0,opacity),clear=Rgba(0,0,0,0);
   d->AddRectFilledMultiColor(R(25,34),R(273,154),white,full,full,white);d->AddRectFilledMultiColor(R(25,34),R(273,154),clear,clear,black,black);
   Hit("##sv",R(25,34),ImVec2(248*s,120*s));if(ImGui::IsItemActive()){sat=std::clamp((io.MousePos.x-R(25,0).x)/(248*s),0.f,1.f);val=1-std::clamp((io.MousePos.y-R(0,34).y)/(120*s),0.f,1.f);changed=true;}
   d->AddCircle(R(25+248*sat,34+120*(1-val)),4*s,Rgba(255,255,255,opacity),0,1.5f*s);d->AddCircle(R(25+248*sat,34+120*(1-val)),5.5f*s,Rgba(0,0,0,.8f*opacity),0,1*s);
   for(int i=0;i<6;++i){ImVec4 a,b;ImGui::ColorConvertHSVtoRGB(i/6.f,1,1,a.x,a.y,a.z);ImGui::ColorConvertHSVtoRGB((i+1)/6.f,1,1,b.x,b.y,b.z);const ImU32 left=ImGui::ColorConvertFloat4ToU32(ImVec4(a.x,a.y,a.z,opacity)),right=ImGui::ColorConvertFloat4ToU32(ImVec4(b.x,b.y,b.z,opacity));d->AddRectFilledMultiColor(R(25+248*i/6.f,165),R(25+248*(i+1)/6.f,178),left,right,right,left);}
   Hit("##hue",R(25,163),ImVec2(248*s,17*s));if(ImGui::IsItemActive()){h=hue=std::clamp((io.MousePos.x-R(25,0).x)/(248*s),0.f,.9999f);changed=true;}
   d->AddCircle(R(25+248*h,171.5f),4*s,Rgba(255,255,255,opacity),0,1.5f*s);d->AddCircle(R(25+248*h,171.5f),5.5f*s,Rgba(0,0,0,.8f*opacity),0,1*s);
   for(int i=0;i<31;++i)d->AddRectFilled(R(25+8.f*i,194),R(std::min(273.f,33+8.f*i),206),(i%2)?Rgba(70,70,74,opacity):Rgba(40,40,44,opacity));
   for(int i=0;i<31;i+=2)d->AddRectFilled(R(25+8.f*i,194),R(std::min(273.f,33+8.f*i),200),Rgba(70,70,74,opacity));
   d->AddRectFilledMultiColor(R(25,194),R(273,206),ImGui::ColorConvertFloat4ToU32(ImVec4(color.x,color.y,color.z,0)),ImGui::ColorConvertFloat4ToU32(ImVec4(color.x,color.y,color.z,opacity)),ImGui::ColorConvertFloat4ToU32(ImVec4(color.x,color.y,color.z,opacity)),ImGui::ColorConvertFloat4ToU32(ImVec4(color.x,color.y,color.z,0)));
   Hit("##alpha",R(25,192),ImVec2(248*s,16*s));if(ImGui::IsItemActive()){color.w=std::clamp((io.MousePos.x-R(25,0).x)/(248*s),0.f,1.f);changed=true;}
   d->AddCircle(R(25+248*color.w,200),4*s,Rgba(255,255,255,opacity),0,1.5f*s);d->AddCircle(R(25+248*color.w,200),5.5f*s,Rgba(0,0,0,.8f*opacity),0,1*s);
   if(changed){ImGui::ColorConvertHSVtoRGB(h,sat,val,color.x,color.y,color.z);if(gradient){ends[stop<.5f?0:1]=FormatColor(color);setting.text=ends[0]+","+ends[1];}else setting.text=FormatColor(color);Save(m,&setting);}
   y+=225;
  }else if(setting.kind=="range"){
   // Two-value range (e.g. "Clicks Per Second"): same track as the slider, with two knobs.
   double high=atof(setting.text.c_str());const float span=(float)(setting.maximum-setting.minimum);auto value=Plain(setting.number,setting.step)+" - "+Plain(high,setting.step)+setting.unit;pill(value.c_str(),AccDim(opacity),Rgba(255,255,255,opacity),81,true);
   float a=span>0?(float)((setting.number-setting.minimum)/span):0,b=span>0?(float)((high-setting.minimum)/span):0;auto& grabbed=animation["grab"+m.id+setting.id];
   Hit("##range",R(24,25),ImVec2(250*s,20*s));
   if(ImGui::IsItemActivated()){float pick=(io.MousePos.x-R(31,0).x)/(236*s);grabbed=std::fabs(pick-a)<=std::fabs(pick-b)?0.f:1.f;}
   if(ImGui::IsItemActive()&&span>0){float pick=std::clamp((io.MousePos.x-R(31,0).x)/(236*s),0.f,1.f);double next=setting.minimum+pick*span;if(setting.step>0)next=setting.minimum+std::round((next-setting.minimum)/setting.step)*setting.step;next=std::clamp(next,setting.minimum,setting.maximum);
    if(grabbed<.5f)setting.number=std::min(next,high);else{high=std::max(next,setting.number);setting.text=std::to_string(high);}Save(m,&setting);a=(float)((setting.number-setting.minimum)/span);b=(float)((high-setting.minimum)/span);}
   d->AddRectFilled(R(31,33),R(267,37),Rgba(255,255,255,.13f*opacity),0);d->AddRectFilled(R(31+236*a,33),R(31+236*b,37),AccDeep(opacity),0);d->AddCircleFilled(R(31+236*a,35),7*s,Rgba(238,241,255,opacity));d->AddCircleFilled(R(31+236*b,35),7*s,Rgba(238,241,255,opacity));
   y+=60;
  }else{
   if(setting.kind=="pattern"){
    // Custom Crosshair pixel editor: 17 x 17 cells, click toggles a cell. Clear / Invert / Mirror (Off, X, XY) buttons
    // above the grid; checkerboard background, hovered cell and centre highlighted.
    auto rows=CrosshairRows(setting.text.empty()?crosshairDefault:setting.text);const float cell=13.f;bool changed=false;static int mirror=0;
    {float px=25;auto chip=[&](const char* label){const float w=TextWidth(regular,12,label)+18;const bool hover=Hover(R(px,24),ImVec2(w*s,22*s));d->AddRectFilled(R(px,24),R(px+w,46),Rgba(255,255,255,(hover?.12f:.07f)*opacity),0);Label(d,regular,12*s,R(px+9,28),label,Rgba(224,224,230,opacity));
      const bool clicked=Hit((std::string("##grid")+label).c_str(),R(px,24),ImVec2(w*s,22*s));px+=w+6;return clicked;};
     if(chip("Clear")){for(auto& r:rows)r=0;changed=true;}if(chip("Invert")){for(auto& r:rows)r=~r&0x1ffffu;changed=true;}
     static const char* mirrors[3]={"Mirror: Off","Mirror: X","Mirror: XY"};if(chip(mirrors[mirror]))mirror=(mirror+1)%3;}
    for(int gy=0;gy<17;++gy)for(int gx=0;gx<17;++gx){const ImVec2 lo=R(25+gx*cell,54+gy*cell),hi=R(25+(gx+1)*cell-1,54+(gy+1)*cell-1);const bool lit=(rows[gy]>>gx)&1;const bool hover=Hover(lo,ImVec2((cell-1)*s,(cell-1)*s));
     d->AddRectFilled(lo,hi,lit?Acc(opacity):Rgba(255,255,255,(gx==8&&gy==8?.12f:((gx+gy)&1?.04f:.07f))*opacity),0);if(hover)d->AddRect(lo,hi,Rgba(255,255,255,.5f*opacity),0);
     if(Hit(("##px"+std::to_string(gy*17+gx)).c_str(),lo,ImVec2((cell-1)*s,(cell-1)*s))){const bool to=!lit;auto set=[&](int x,int yy){if(to)rows[yy]|=1u<<x;else rows[yy]&=~(1u<<x);};
      set(gx,gy);if(mirror>=1)set(16-gx,gy);if(mirror==2){set(gx,16-gy);set(16-gx,16-gy);}changed=true;}}
    if(changed){setting.text=CrosshairText(rows);Save(m,&setting);}y+=54+17*cell+12;
   }else if(setting.kind=="text"){
    // Text field under the label (Custom Text, Disconnect Command...).
    char buffer[257]{};strncpy_s(buffer,setting.text.c_str(),256);ImGui::SetCursorScreenPos(R(25,24));ImGui::SetNextItemWidth(248*s);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,0);ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(10*s,7*s));ImGui::PushStyleColor(ImGuiCol_FrameBg,ImVec4(.1f,.1f,.125f,.92f));
    if(ImGui::InputText("##text",buffer,sizeof(buffer))){setting.text=buffer;Save(m,&setting);}ImGui::PopStyleColor();ImGui::PopStyleVar(2);y+=66;
   }else if(setting.kind=="list"){
    // List of items, blocks or strings: "Nothing selected" / "{} selected", selected entries, an "Add an item..." /
    // "Add a block..." field and a dropdown of up to 5 rows of 26 filled from the game registry ("No matching entries",
    // "Load a world to select"). Entries are stored comma-separated with full names ("minecraft:...").
    // The list kind (items / blocks / strings) is inferred from the setting name.
    const bool fixed=m.id=="no_particle"||!setting.options.empty(),items=fixed||setting.name.find("Item")!=std::string::npos||m.id=="right_clicker",blocks=!items&&(setting.name.find("Block")!=std::string::npos||setting.id=="blocks");   // No Particle: fixed option list
    std::vector<std::string> entries;{size_t from=0;while(from<setting.text.size()){size_t to=setting.text.find(',',from);if(to==std::string::npos)to=setting.text.size();if(to>from)entries.push_back(setting.text.substr(from,to-from));from=to+1;}}
    char count[32]{};snprintf(count,sizeof(count),"%d selected",(int)entries.size());
    // Multi-choice summary: "None" if empty, "All" if everything, otherwise the first two names and "+n".
    std::string summary=entries.empty()?"Nothing selected":count;if(fixed&&!setting.options.empty()){if(entries.empty())summary="None";else if(entries.size()>=setting.options.size())summary="All";
     else{summary=entries[0];if(entries.size()>1)summary+=", "+entries[1];if(entries.size()>2)summary+=", +"+std::to_string(entries.size()-2);}}
    pill(summary.c_str(),Rgba(255,255,255,.05f*opacity),Rgba(120,120,132,opacity),48);
    float cx=25,cy=26;int removed=-1;bool cleared=false;std::vector<std::string> preset;auto bare=[](const std::string& name){const size_t colon=name.rfind(':');return colon==std::string::npos?name:name.substr(colon+1);};
    if((items||blocks)&&!fixed){
     // Icon grid: floor((width+6)/46) columns of square cells, 6 apart, "×" 9 units from the corner.
     const int cols=std::max(1,(int)((248.f+6)/46));const float cell=(248.f-(cols-1)*6)/cols;
     for(int i=0;i<(int)entries.size();++i){const float x=25+(i%cols)*(cell+6),y=cy+(i/cols)*(cell+6);const bool hover=Hover(R(x,y),ImVec2(cell*s,cell*s));
      d->AddRectFilled(R(x,y),R(x+cell,y+cell),Rgba(23,23,29,(hover?1.f:.92f)*opacity),0);DrawRegistryIcon(d,entries[i],blocks,R(x+cell*.18f,y+cell*.18f),R(x+cell*.82f,y+cell*.82f),opacity);
      if(hover){const ImVec2 cross=R(x+cell-9,y+9);d->AddCircleFilled(cross,7*s,Rgba(20,20,26,.9f*opacity));if(!DrawGlyph(d,nicons::k_x,ImVec2(cross.x-6*s,cross.y-6*s),12*s,Rgba(255,255,255,opacity))){d->AddLine(ImVec2(cross.x-3*s,cross.y-3*s),ImVec2(cross.x+3*s,cross.y+3*s),Rgba(255,255,255,opacity),1.3f*s);d->AddLine(ImVec2(cross.x-3*s,cross.y+3*s),ImVec2(cross.x+3*s,cross.y-3*s),Rgba(255,255,255,opacity),1.3f*s);}
       ImGui::SetTooltip("%s",bare(entries[i]).c_str());}
      if(Hit(("##remove"+std::to_string(i)).c_str(),R(x+cell-16,y+2),ImVec2(14*s,14*s)))removed=i;}
     if(!entries.empty())cy+=((int)entries.size()+cols-1)/cols*(cell+6)+2;
     // Block ESP: clicking a cell opens that block's own style.
     if(m.id=="block_esp"){Setting* store=nullptr;for(auto& other:m.settings)if(other.id=="block_styles")store=&other;static std::string editing;
      if(store){auto styles=ParseBlockStyles(store->text);bool dirty=false;
       for(int i=0;i<(int)entries.size();++i){const float x=25+(i%cols)*(cell+6),yy=26+(i/cols)*(cell+6);
        if(ImGui::IsMouseClicked(0)&&Hover(R(x,yy),ImVec2(cell*s,cell*s))&&!Hover(R(x+cell-16,yy+2),ImVec2(14*s,14*s)))editing=editing==entries[i]?std::string():entries[i];
        auto f=styles.find(entries[i]);if(f!=styles.end()){const ImVec4 c=f->second.mode=='r'?ImVec4(1,.4f,.9f,1):ParseColor(f->second.a);d->AddCircleFilled(R(x+7,yy+cell-7),3.5f*s,ImGui::ColorConvertFloat4ToU32(ImVec4(c.x,c.y,c.z,opacity)));}
        if(editing==entries[i])d->AddRect(R(x,yy),R(x+cell,yy+cell),AccDeep(opacity),0,0,1.5f*s);}
       if(!editing.empty()&&std::find(entries.begin(),entries.end(),editing)!=entries.end()){BlockOwnStyle st=styles.count(editing)?styles[editing]:BlockOwnStyle{};
        Label(d,regular,12*s,R(25,cy),(std::string("Style: ")+bare(editing)).c_str(),Rgba(170,170,180,opacity));cy+=20;
        float px=25;static const char modes[4]={'d','g','r','s'};static const char* names[4]={"Default","Gradient","Rainbow","Solid"};
        for(int k=0;k<4;++k){const float w=TextWidth(regular,12,names[k])+18;const bool on=st.mode==modes[k];d->AddRectFilled(R(px,cy),R(px+w,cy+22),on?AccDeep(opacity):Rgba(255,255,255,.07f*opacity),0);Label(d,regular,12*s,R(px+9,cy+4),names[k],Rgba(235,235,240,opacity));
         if(Hit((std::string("##bstyle")+names[k]).c_str(),R(px,cy),ImVec2(w*s,22*s))&&!on){st.mode=modes[k];dirty=true;}px+=w+6;}
        cy+=28;
        if(st.mode=='g'||st.mode=='s'){for(int e=0;e<(st.mode=='g'?2:1);++e){ImVec4 c=ParseColor(e?st.b:st.a);float col[4]={c.x,c.y,c.z,c.w};ImGui::SetCursorScreenPos(R(25+e*50,cy));
          if(ImGui::ColorEdit4(e?"##bstyleB":"##bstyleA",col,ImGuiColorEditFlags_NoInputs|ImGuiColorEditFlags_NoLabel|ImGuiColorEditFlags_AlphaBar|ImGuiColorEditFlags_AlphaPreviewHalf)){(e?st.b:st.a)=FormatColor(ImVec4(col[0],col[1],col[2],col[3]));dirty=true;}}cy+=30;}
        int own=0;for(auto& e2:entries){auto f=styles.find(e2);if(f!=styles.end()&&f->second.mode!='d'&&e2!=editing)++own;}
        if(own>=7&&st.mode!='d'){Label(d,regular,11*s,R(25,cy),"7 own styles at most: this block uses Default",Rgba(220,170,90,opacity));cy+=18;}
        if(dirty){styles[editing]=st;store->text=SerializeBlockStyles(styles);Save(m,store);}}
       // drop styles of blocks removed from the list
       bool stale=false;for(auto it=styles.begin();it!=styles.end();){if(std::find(entries.begin(),entries.end(),it->first)==entries.end()){it=styles.erase(it);stale=true;}else ++it;}if(stale){store->text=SerializeBlockStyles(styles);Save(m,store);}}}
     // Chips: "Ores", "Storage" (block presets), "Clear all".
     float px=25;auto chip=[&](const char* label){const float w=TextWidth(regular,12,label)+18;const bool hover=Hover(R(px,cy),ImVec2(w*s,22*s));d->AddRectFilled(R(px,cy),R(px+w,cy+22),Rgba(255,255,255,(hover?.12f:.07f)*opacity),0);Label(d,regular,12*s,R(px+9,cy+4),label,Rgba(224,224,230,opacity));
      const bool clicked=Hit((std::string("##chip")+label).c_str(),R(px,cy),ImVec2(w*s,22*s));px+=w+6;return clicked;};
     if(blocks){const auto& names=RegistryNames(true);auto ends=[](const std::string& t,const char* e){const size_t n=strlen(e);return t.size()>=n&&!t.compare(t.size()-n,n,e);};
      if(chip("Ores"))for(auto& n:names)if(ends(n,"_ore")||n=="minecraft:ancient_debris")preset.push_back(n);
      if(chip("Storage")){static const char* fixedStorage[]={"minecraft:chest","minecraft:trapped_chest","minecraft:ender_chest","minecraft:barrel","minecraft:hopper","minecraft:dropper","minecraft:dispenser","minecraft:crafter","minecraft:furnace","minecraft:lit_furnace","minecraft:blast_furnace","minecraft:lit_blast_furnace","minecraft:smoker","minecraft:lit_smoker","minecraft:brewing_stand","minecraft:chiseled_bookshelf","minecraft:decorated_pot"};
       for(auto n:fixedStorage)preset.push_back(n);for(auto& n:names)if(n.size()>11&&ends(n,"shulker_box"))preset.push_back(n);}}
     if(!entries.empty()&&chip("Clear all"))cleared=true;
     cy+=28;}
    else{
    // Multi-choice: "Select all" and "Clear".
    if(fixed&&!setting.options.empty()){float px=25;auto chip=[&](const char* label){const float w=TextWidth(regular,12,label)+18;const bool hover=Hover(R(px,cy),ImVec2(w*s,22*s));d->AddRectFilled(R(px,cy),R(px+w,cy+22),Rgba(255,255,255,(hover?.12f:.07f)*opacity),0);Label(d,regular,12*s,R(px+9,cy+4),label,Rgba(224,224,230,opacity));
      const bool clicked=Hit((std::string("##enum")+label).c_str(),R(px,cy),ImVec2(w*s,22*s));px+=w+6;return clicked;};
     if(chip("Select all"))for(auto& o:setting.options)preset.push_back(o);if(chip("Clear"))cleared=true;cy+=28;}
    for(int i=0;i<(int)entries.size();++i){const std::string shownName=Fit(regular,12,bare(entries[i]),200);const float w=TextWidth(regular,12,shownName.c_str())+30;if(cx+w>273&&cx>25){cx=25;cy+=26;}
     const bool hover=Hover(R(cx,cy),ImVec2(w*s,22*s));d->AddRectFilled(R(cx,cy),R(cx+w,cy+22),Rgba(23,23,29,(hover?1.f:.92f)*opacity),0);Label(d,regular,12*s,R(cx+8,cy+4),shownName.c_str(),Rgba(224,224,230,opacity));
     const ImVec2 cross=R(cx+w-12,cy+11);const ImU32 ink=Rgba(hover?255:150,hover?255:144,hover?255:162,opacity);d->AddLine(ImVec2(cross.x-3*s,cross.y-3*s),ImVec2(cross.x+3*s,cross.y+3*s),ink,1.3f*s);d->AddLine(ImVec2(cross.x-3*s,cross.y+3*s),ImVec2(cross.x+3*s,cross.y-3*s),ink,1.3f*s);
     if(Hit(("##remove"+std::to_string(i)).c_str(),R(cx,cy),ImVec2(w*s,22*s)))removed=i;cx+=w+6;}
    }
    if(!entries.empty()&&!((items||blocks)&&!fixed))cy+=28;
    static std::map<std::string,std::string> typed;auto& buffer=typed[m.id+"/"+setting.id];char raw[65]{};strncpy_s(raw,buffer.c_str(),_TRUNCATE);ImGui::SetCursorScreenPos(R(25,cy));ImGui::SetNextItemWidth(248*s);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,0);ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(10*s,7*s));ImGui::PushStyleColor(ImGuiCol_FrameBg,ImVec4(.1f,.1f,.125f,.92f));
    const bool enter=ImGui::InputTextWithHint("##add",fixed?(m.id=="no_particle"?"Add a particle...":"Add an option..."):items?"Add an item...":blocks?"Add a block...":"Add an entry...",raw,sizeof(raw),ImGuiInputTextFlags_EnterReturnsTrue);buffer=raw;ImGui::PopStyleColor();ImGui::PopStyleVar(2);cy+=34;std::string add;
    if(!buffer.empty()){
     if(items||blocks){static const std::vector<std::string> particleNames(particleOptions,particleOptions+15);const auto& names=fixed?(setting.options.empty()?particleNames:setting.options):RegistryNames(blocks);
      if(names.empty()){Label(d,regular,13*s,R(31,cy+5),"Load a world to select",Rgba(120,120,132,opacity));cy+=26;}
      else{std::string needle=buffer;std::transform(needle.begin(),needle.end(),needle.begin(),[](unsigned char ch){return (char)std::tolower(ch);});std::vector<const std::string*> found;
       for(auto& name:names){if(found.size()>=5)break;std::string plain=name;std::transform(plain.begin(),plain.end(),plain.begin(),[](unsigned char ch){return (char)std::tolower(ch);});if(plain.find(needle)==std::string::npos||std::find(entries.begin(),entries.end(),name)!=entries.end())continue;found.push_back(&name);}
       if(found.empty()){Label(d,regular,13*s,R(31,cy+5),"No matching entries",Rgba(120,120,132,opacity));cy+=26;}
       else{d->AddRectFilled(R(25,cy),R(273,cy+26.f*found.size()),Rgba(23,23,29,.92f*opacity),0);for(size_t i=0;i<found.size();++i){const bool hover=Hover(R(25,cy),ImVec2(248*s,26*s));if(hover)d->AddRectFilled(R(28,cy+1),R(270,cy+25),Rgba(255,255,255,.05f*opacity),0);
         if(!fixed){DrawRegistryIcon(d,*found[i],blocks,R(31,cy+3),R(51,cy+23),opacity);Label(d,regular,13*s,R(57,cy+5),Fit(regular,13,*found[i],206).c_str(),Rgba(224,224,230,opacity));}else Label(d,regular,13*s,R(35,cy+5),Fit(regular,13,*found[i],228).c_str(),Rgba(224,224,230,opacity));if(Hit(("##pick"+std::to_string(i)).c_str(),R(25,cy),ImVec2(248*s,26*s)))add=*found[i];cy+=26;}
        if(enter&&add.empty())add=*found[0];}}}
     else if(enter)add=buffer;}
    bool changed=false;if(cleared){entries.clear();changed=true;}for(auto& n:preset)if(std::find(entries.begin(),entries.end(),n)==entries.end()){entries.push_back(n);changed=true;}if(!add.empty()&&add.find(',')==std::string::npos&&std::find(entries.begin(),entries.end(),add)==entries.end()){entries.push_back(add);buffer.clear();changed=true;}if(removed>=0){entries.erase(entries.begin()+removed);changed=true;}
    if(changed){setting.text.clear();for(auto& entry:entries){if(!setting.text.empty())setting.text+=",";setting.text+=entry;}Save(m,&setting);}
    y+=cy+10;
   }else if(setting.kind=="key"){
    // Key setting: "Press a key..." while listening, otherwise the key name.
    const bool waiting=listeningSetting==&setting;const std::string label=waiting?"Press a key...":setting.text.empty()?"None":setting.text.substr(0,setting.text.find('|'));const float w=pill(label.c_str(),Rgba(255,255,255,(waiting?.16f:.08f)*opacity),muted,48);
    if(Hit("##key",R(273-w,-1),ImVec2(w*s,20*s))){listeningSetting=waiting?nullptr:&setting;listeningModule=&m;}y+=33;
   }else{
    pill("Not recovered",Rgba(255,255,255,.05f*opacity),Rgba(120,120,132,opacity),48);y+=33;
   }
  }
  ImGui::PopID();
 }
 ImGui::SetCursorPosY((y+10)*s);ImGui::Dummy(ImVec2(1,1));const float scroll=ImGui::GetScrollY(),limit=ImGui::GetScrollMaxY();FadeEdges(s,opacity);EndChildWindow();
 // Scrollbar (theme accent) at x=288.
 if(limit>0){d=ImGui::GetWindowDrawList();const float view=menuH-92,total=view+limit/s,bar=std::max(30.f,view*view/total),top=84+(view-bar)*(scroll/limit);d->AddRectFilled(P(288,top),P(291,top+bar),Acc(.85f*opacity),0);}
 EndWindow();ImGui::PopStyleVar();
}
}

namespace {
const Setting* Find(const Module& item,const char* id){for(auto& setting:item.settings)if(setting.id==id)return &setting;return nullptr;}
Module* ModuleById(const char* id){for(auto& item:modules)if(item.id==id)return &item;return nullptr;}
// Modules listed in the catalogue but not available in Nickel: shown as "Soon" cards (blurred, greyed), never enabled,
// whether from the card, a keybind or a saved config.
bool SoonModule(const std::string& id){return id=="disabler"||id=="spoofer"||id=="edition_faker";}
void KeepSoonModulesOff(){for(const char* id:{"disabler","spoofer","edition_faker"})if(Module* m=ModuleById(id))if(m->enabled){m->enabled=false;Save(*m);}}
bool Flag(const Module& item,const char* id,bool fallback){auto setting=Find(item,id);return setting&&setting->kind=="bool"?setting->number!=0:fallback;}
// Teams rules from the Teams module, rebuilt only when a setting changed (they were rebuilt by 5 modules every frame)
const nickel::visual::TeamRules& TeamRulesOf(const Module* teams){static nickel::visual::TeamRules rules;static unsigned seen=0;static const Module* who=nullptr;static bool was=false;const bool on=teams&&teams->enabled;
 if(seen==settingsRevision&&was==on&&who==teams)return rules;seen=settingsRevision;was=on;who=teams;rules=nickel::visual::TeamRules{};if(!on)return rules;
 rules.enabled=true;rules.auto_color=Flag(*teams,"auto_color",true);rules.name_prefix=Flag(*teams,"name_prefix",true);
 static const char* order[]={"black","dark_blue","dark_green","dark_aqua","dark_red","dark_purple","gold","gray","dark_gray","blue","green","aqua","red","light_purple","yellow","white"};
 for(int i=0;i<16;++i)if(Flag(*teams,order[i],false))rules.colors|=(uint16_t)(1u<<i);
 if(auto list=Find(*teams,"teammates")){size_t from=0;while(from<list->text.size()){size_t to=list->text.find(',',from);if(to==std::string::npos)to=list->text.size();if(to>from)rules.names.push_back(list->text.substr(from,to-from));from=to+1;}}
 return rules;}
// HUD, drawn even with the menu closed; each element is driven by its module and settings.
// FPS, Clock (system time), CPS and Keystrokes read nothing from the game. Coordinates, Speedometer, Direction,
// Server IP, Health Warning and Effect Display read player state through game_read.hpp (read-only).
// Each element has a position and a scale editable in the layout editor ("Edit layout", HUD tab): drag to move
// with snapping, bottom-right corner to resize, "Press Escape To Exit".
// user = scale chosen in the editor; scale = effective scale (user x screen scale min(W/1280, H/720) for elements
// that follow the screen); pending = no saved position yet, placed at the default once its size is known.
struct HudPlace{float x=-1,y=-1,scale=1,user=1;ImVec2 lo,hi;bool loaded=false,right=false,pending=false;};
// Default positions as a fraction of the free screen space (0 = left / top edge, 1 = right / bottom edge);
// the last field says whether the element follows the screen scale.
struct HudDefault{const char* id;float x,y;bool screenScaled;};
// ---- Keystrokes default keys: QWERTY (W A S D, Space, left and right mouse buttons). Players re-bind them in the HUD
// layout editor (custom_keys); codes are virtual keys, -99 / -98 / -97 = left / right / middle mouse button.
struct GameKeys{int forward='W',left='A',back='S',right='D',jump=VK_SPACE,attack=-99,use=-98;};
const GameKeys& CurrentGameKeys(){static const GameKeys keys;return keys;}
// virtual key of a binding (mouse codes mapped to VK_*), and its label
int BindingVk(int code){return code==-99?VK_LBUTTON:code==-98?VK_RBUTTON:code==-97?VK_MBUTTON:code>0?code:0;}
std::string BindingLabel(int code){if(code==-99)return "LMB";if(code==-98)return "RMB";if(code==-97)return "MMB";if(code<=0)return "?";
 if((code>='A'&&code<='Z')||(code>='0'&&code<='9'))return std::string(1,(char)code);const UINT scan=MapVirtualKeyW((UINT)code,MAPVK_VK_TO_VSC);
 wchar_t name[32]{};if(scan&&GetKeyNameTextW((LONG)(scan<<16),name,32)){std::string out=Utf8(name);if(out.size()>4)out=out.substr(0,4);return out;}return "?";}
const HudDefault hudDefaults[]={{"fps",.03f,.08f,true},{"cps",.03f,.08f,true},{"clock",.88f,.02f,true},{"coordinates",.012f,.88f,true},{"speedometer",.012f,.83f,true},{"direction",.5f,.02f,true},{"server_ip",.012f,.02f,true},{"ping",.012f,.088f,true},{"reach_display",.012f,.44f,true},{"combo_counter",.012f,.48f,true},{"totem_pop_counter",.85f,.3f,true},{"target_hud",.55f,.55f,true},{"player_list",.5f,.04f,true},{"armor_hud",.43f,.8f,true},{"minimap",.985f,.02f,true},{"item_counter",.57f,.8f,true},{"effect_display",.012f,.25f,true},{"keystrokes",.012f,.3f,true},{"spotify_manager",.02f,.98f,true},{"module_list",1,0,false}};
std::map<std::string,HudPlace> hudPlaces;bool hudEditing=false;std::string hudDragging;ImVec2 hudGrab;
// Editor state: last selected element (purple frame), element being dragged, resize (bottom-right corner) or move
// mode, mouse-to-corner offset, top-left corner and size at scale 1 when grabbed, snapping guides.
std::string hudSelected;bool hudResizing=false,hudSnapX=false,hudSnapY=false;ImVec2 hudOrigin,hudBase,hudGuide,hudFraction;
int hudIniVersion=0;   // bumped whenever the [hud] section changes (layout saved, config loaded)
void SaveHud(const std::string& id){++hudIniVersion;auto& place=hudPlaces[id];auto write=[&](const char* suffix,float value){WritePrivateProfileStringW(L"hud",Wide(id+suffix).c_str(),std::to_wstring(value).c_str(),configPath.c_str());};write("-x",place.x);write("-y",place.y);write("-scale",place.user);}
HudPlace& Place(const char* id,float defaultX,float defaultY){
 auto& place=hudPlaces[id];
 if(!place.loaded){place.loaded=true;auto read=[&](const char* suffix,float fallback){wchar_t text[32]{};GetPrivateProfileStringW(L"hud",Wide(std::string(id)+suffix).c_str(),L"",text,32,configPath.c_str());wchar_t* end{};double value=wcstod(text,&end);return text[0]&&end&&!*end?(float)value:fallback;};
  place.x=read("-x",-1);place.y=read("-y",-1);place.user=std::clamp(read("-scale",1),.5f,3.f);}
 const HudDefault* original=nullptr;for(auto& entry:hudDefaults)if(!strcmp(entry.id,id))original=&entry;const ImVec2 screen=ImGui::GetIO().DisplaySize;
 place.scale=place.user*(original&&original->screenScaled?std::min(screen.x/1280.f,screen.y/720.f):1.f);
 if(place.x<0||place.y<0){if(original){place.pending=true;place.x=place.y=-20000;}else{place.x=defaultX;place.y=defaultY;}}
 return place;
}
// Module List "Show Extra Info": per-module suffix text. Numbers use "{:.{}f}" with trailing zeros trimmed while
// keeping one decimal; ranges show "min-max", or a single value when both ends are equal.
// Game item / block names for the settings panel lists are refreshed at most every 3 s, empty outside a world.
const std::vector<std::string>& RegistryNames(bool blocks){
 static std::vector<std::string> names[2];static double next[2]={0,0};auto& list=names[blocks?1:0];if(ImGui::GetTime()<next[blocks?1:0])return list;next[blocks?1:0]=ImGui::GetTime()+3;
 static char raw[4096][64];const int count=game::ReadRegistryNames(raw,4096,blocks);list.clear();for(int i=0;i<count;++i)list.push_back(raw[i]);std::sort(list.begin(),list.end());list.erase(std::unique(list.begin(),list.end()),list.end());return list;
}
// The HUD (Nickel's elements, script HUDs, custom crosshair, subtitles and the armor / effect / item icons drawn by the
// game) shows only while playing: in a world, with the game owning the mouse. Chat, pause, inventory and the other game
// screens hide it; the Nickel menu, the HUD editor and the full-screen map keep it. While the game window is in the
// background the last state is kept (alt-tab, streams) unless the game draws one of its menu screens: a singleplayer
// game pauses when it loses the focus, and its pause screen hides the HUD like any other. Hidden elements are drawn
// into a list that is never rendered, so their per-frame state keeps running.
bool hudShown=true;
// True when the game drew one of its menu screens (pause, chat, inventory, death, disconnect, start) in the last 250 ms
// (names seen by the screen render hook, game_render.hpp NoteOtherScreen).
bool GameMenuScreenShown(){
 static const uint64_t menus[]={game::ScreenNameHash("pause_screen"),game::ScreenNameHash("chat_screen"),game::ScreenNameHash("inventory_screen"),
  game::ScreenNameHash("death_screen"),game::ScreenNameHash("disconnect_screen"),game::ScreenNameHash("start_screen")};
 const ULONGLONG now=GetTickCount64();
 for(int i=0;i<game::kScreenSlots;++i){const ULONGLONG at=game::otherScreenAt[i].load();if(!at||now-at>250)continue;const uint64_t h=game::otherScreenHash[i].load();for(uint64_t m:menus)if(h==m)return true;}
 return false;}
ImDrawList* HudSink(){static ImDrawList* sink=nullptr;if(!sink)sink=IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData());sink->_Data=ImGui::GetDrawListSharedData();return sink;}   // _Data refreshed: the ImGui context is recreated when the renderer restarts
ImDrawList* HudList(){return hudShown?ImGui::GetBackgroundDrawList():HudSink();}
void UpdateHudShown(){
 static ULONGLONG leftAt=0;const ULONGLONG now=GetTickCount64();
 const HWND root=gameWindowForKeys?GetAncestor(gameWindowForKeys,GA_ROOT):nullptr;const bool focused=!root||GetForegroundWindow()==root;
 game::Context c{};bool playing=hudShown;
 if(opened||hudEditing||mapOpen||waypointsOpen||OwnProbe())playing=true;
 else if(!game::Local(&c))playing=false;
 else if(GameMenuScreenShown())playing=false;   // focused or not: a pause / chat / inventory screen is on
 else if(focused)playing=Playing();
 // hide 120 ms after leaving play (no flicker when the game briefly shows the cursor); show at once
 if(playing){leftAt=0;hudShown=true;}else{if(!leftAt)leftAt=now;if(now-leftAt>=120)hudShown=false;}
 game::hudHidden.store(!hudShown,std::memory_order_relaxed);
 if(!hudShown){auto* sink=HudSink();sink->_ResetForNewFrame();sink->PushClipRectFullScreen();sink->PushTextureID(ImGui::GetIO().Fonts->TexID);}
}
#include "script_engine.hpp"
#include "hud_crosshair.hpp"
#include "hud_subtitles.hpp"
std::string ExtraNumber(double value,int precision,const char* suffix=""){char text[48]{};snprintf(text,sizeof(text),"%.*f",precision,value);std::string out=text;if(out.find('.')!=std::string::npos){while(out.size()>1&&out.back()=='0'&&out[out.size()-2]!='.')out.pop_back();}return out+suffix;}
std::string ExtraInfo(const Module& item){
 auto text=[&](const char* id){auto setting=Find(item,id);return setting?setting->text:std::string();};
 auto real=[&](const char* id,int precision,const char* suffix=""){auto setting=Find(item,id);return setting?ExtraNumber(setting->number,precision,suffix):std::string();};
 auto whole=[&](const char* id,const char* suffix=""){auto setting=Find(item,id);return setting?std::to_string((int)std::lround(setting->number))+suffix:std::string();};
 auto range=[&](const char* id,int precision){auto setting=Find(item,id);if(!setting)return std::string();const std::string low=ExtraNumber(setting->number,precision),high=ExtraNumber(atof(setting->text.c_str()),precision);return setting->kind!="range"||low==high?low:low+"-"+high;};
 static const char* modes[]={"aim_assist","auto_360","criticals","kill_aura","pvp_bot","auto_jump_reset","auto_sprint","flight","null_movement","phase","derp","disabler","chams","custom_sky"};
 for(auto id:modes)if(item.id==id)return text("mode");
 if(item.id=="base_finder"){auto& s=game::BaseFinder();std::lock_guard<std::mutex> lock(s.guard);return std::to_string(s.analysed)+"/"+std::to_string(s.tracked);}
 if(item.id=="bed_breaker"){auto& s=game::BedBreaker();std::lock_guard<std::mutex> lock(s.guard);return s.snap.state==0?std::string():s.snap.uncover?std::string("Uncovering"):std::string("Breaking");}
 if(item.id=="regen"){auto& s=game::Regen();std::lock_guard<std::mutex> lock(s.guard);return game::RegenLabel(s.snap);}
 if(item.id=="auto_fish")return std::to_string(game::AutoFish().catches.load());
 if(item.id=="esp")return text("style");if(item.id=="anti_crystal")return real("offset",2);if(item.id=="auto_clicker")return range("clicks_per_second",1);if(item.id=="trigger_bot")return range("attacks_per_second",1);
 if(item.id=="backtrack"||item.id=="right_clicker"||item.id=="knockback_desync")return whole("delay","ms");if(item.id=="hitbox")return real("width",2)+"x"+real("height",2);if(item.id=="reach")return range("reach",2);
 if(item.id=="bunny_hop")return Flag(item,"modify_velocity",false)?real("speed",2):std::string();if(item.id=="glide")return real("fall_speed",3);if(item.id=="jetpack"||item.id=="speed"||item.id=="spider")return real("speed",2);if(item.id=="step")return real("height",2);
 if(item.id=="velocity"){auto mode=Find(item,"mode");if(mode&&!mode->options.empty()&&mode->text!=mode->options[0])return mode->text;return real("horizontal_multiplier",0,"%")+" "+real("vertical_multiplier",0,"%");}
 if(item.id=="fake_lag")return text("mode")+" "+whole("max_freeze","ms");if(item.id=="nuker")return whole("radius");if(item.id=="timer"){auto rate=Find(item,"tick_rate");return rate?ExtraNumber(rate->number/20,2,"x"):std::string();}
 return {};
}
void DrawHud(ImTextureID logo){
 UpdateHudShown();ImDrawList* const worldList=ImGui::GetBackgroundDrawList();
 auto& io=ImGui::GetIO();ImDrawList* d=HudList();float stack=10;std::vector<std::string> drawn;ScriptsFrame(drawn);DrawCrosshair();lyricsLog=WriteLog;DrawSubtitles(drawn);
 // Motion Blur pixel shader: reprojection between adjacent camera frames.
 // The hand/body exclusion mask is not implemented.
 bool motionCameraWanted=false;
 {static Module* item=ModuleById("motion_blur");static game::Mat4 previous{};static bool previousValid=false;static uint64_t previousTime=0;
  const bool active=item&&item->enabled&&!opened&&!hudEditing&&RequestMotionBlur;
  if(active){motionCameraWanted=true;game::InstallScreenHook(WriteLog);game::Mat4 current{},inverse{};uint64_t tick{};auto& view=game::View();AcquireSRWLockShared(&view.lock);memcpy(current.m,view.m,64);tick=view.tick;ReleaseSRWLockShared(&view.lock);
   const bool valid=tick&&GetTickCount64()-tick<250&&!view.fault;
   if(valid){if(previousValid&&tick>=previousTime&&tick-previousTime<250&&game::Inverse(current,&inverse)){
     const game::Mat4 reprojection=previous*inverse;
     auto number=[&](const char* id,float fallback){auto setting=Find(*item,id);return setting?(float)setting->number:fallback;};
     if(auto texture=RequestMotionBlur(d,reprojection.m,number("strength",50)/100.f,(int)number("samples",12),number("max_blur",40)))d->AddImage(texture,ImVec2(0,0),io.DisplaySize);
    }previous=current;previousTime=tick;previousValid=true;
   }else previousValid=false;
  }else previousValid=false;
 }
 // Editor veil: 0.2 s fade to #0c0913 at 45/255.
 static float hudFade=0;hudFade=std::clamp(hudFade+(hudEditing?1.f:-1.f)*io.DeltaTime/.2f,0.f,1.f);const float es=std::min(io.DisplaySize.x/1280.f,io.DisplaySize.y/720.f);
 if(hudFade>0)d->AddRectFilled(ImVec2(0,0),io.DisplaySize,Rgba(12,9,19,45.f/255.f*hudFade));
 // HUD text: size S is the line height, used as is (no em correction).
 auto hudText=[&](ImFont* font,float size,ImVec2 at,const char* text,ImU32 color){ImFont* baked=Sized(font,size);if(std::fabs(baked->FontSize-size)<1.f)size=baked->FontSize;d->AddText(baked,size,ImVec2(std::round(at.x),std::round(at.y)),color,text);};   // exact-size font: sharp at every HUD scale
 auto hudWidth=[&](ImFont* font,float size,const char* text){return Sized(font,size)->CalcTextSizeA(size,FLT_MAX,0,text).x;};
 auto num=[&](const Module& item,const char* id,float fallback){auto setting=Find(item,id);return setting&&setting->kind=="number"?(float)setting->number:fallback;};
 // Animated gradient: colour = mix(start, end, (1 - cos(2π t)) / 2), t = x * 1.15 + time * 0.15 * Gradient Speed.
 auto stops=[&](const Module& item,const char* id){std::string ends[2];auto setting=Find(item,id);GradientEnds(item.id,setting?setting->text:"",ends);return std::pair<ImVec4,ImVec4>(ParseColor(ends[0]),ParseColor(ends[1]));};
 auto wave=[&](const std::pair<ImVec4,ImVec4>& g,float t){const float f=(1-std::cos(6.2831855f*t))*.5f;const ImVec4& a=g.first;const ImVec4& b=g.second;return ImGui::ColorConvertFloat4ToU32(ImVec4(a.x+(b.x-a.x)*f,a.y+(b.y-a.y)*f,a.z+(b.z-a.z)*f,a.w+(b.w-a.w)*f));};
 // FPS / CPS label: text size 20 at (9, 5), box = text + (18, 10), i.e. 32 high, square "Background Color" fill
 // (#14111baf by default). With Gradient (FPS) each character takes the gradient colour at
 // t = (x / width) * 1.15 + time * 0.15 * Gradient Speed.
 auto badge=[&](const char* id,const Module& item,const char* text){
  auto& place=Place(id,10,stack);const float k=place.scale,size=20*k,width=hudWidth(bold,size,text);auto fill=Find(item,"background_color");auto tint=Find(item,"text_color");
  place.lo=ImVec2(place.x,place.y);place.hi=ImVec2(place.x+width+18*k,place.y+32*k);
  if(Flag(item,"background",true))d->AddRectFilled(place.lo,place.hi,ImGui::ColorConvertFloat4ToU32(fill&&fill->kind=="color"?ParseColor(fill->text):ImVec4(.08f,.07f,.11f,.69f)));
  if(Flag(item,"gradient",false)){const auto g=stops(item,"gradient_colors");const float phase=(float)ImGui::GetTime()*.15f*num(item,"gradient_speed",3);float x=0;
   for(const char* c=text;*c;++c){const char one[2]={*c,0};hudText(bold,size,ImVec2(place.x+9*k+x,place.y+5*k),one,wave(g,x/std::max(1.f,width)*1.15f+phase));x+=hudWidth(bold,size,one);}}
  else hudText(bold,size,ImVec2(place.x+9*k,place.y+5*k),text,ImGui::ColorConvertFloat4ToU32(tint&&tint->kind=="color"?ParseColor(tint->text):ImVec4(1,1,1,1)));
  stack+=35;drawn.push_back(id);};
 // Text-line box shared by Clock, Coordinates, Speedometer and Server IP: font 18, line step 20, background radius 3
 // (Background / Background Color), text offset by (5, 2) when a background is drawn, black shadow at 62.75 % of the
 // text alpha offset by 1 or 2 px (Text Shadow).
 auto textBox=[&](const char* id,const Module& item,const std::vector<std::string>& lines,float defaultX,float defaultY){
  auto& place=Place(id,defaultX,defaultY);const float k=place.scale;const bool background=Flag(item,"background",true),shadow=Flag(item,"text_shadow",true);float widest=0;
  for(auto& line:lines)widest=std::max(widest,hudWidth(bold,18*k,line.c_str()));auto fill=Find(item,"background_color");auto tint=Find(item,"text_color");
  const ImVec4 ink=tint&&tint->kind=="color"?ParseColor(tint->text):ImVec4(1,1,1,1);const float count=(float)lines.size();
  place.lo=ImVec2(place.x,place.y);place.hi=ImVec2(place.x+widest+((background?10:0)+(shadow?1:0))*k,place.y+(18*count+2*(count-1)+(background?4:0)+(shadow?1:0))*k);
  if(background)d->AddRectFilled(place.lo,place.hi,ImGui::ColorConvertFloat4ToU32(fill&&fill->kind=="color"?ParseColor(fill->text):ImVec4(0,0,0,.51f)),3*k);
  for(size_t i=0;i<lines.size();++i){const ImVec2 at(place.x+(background?5:0)*k,place.y+(background?2:0)*k+i*20*k);const float offset=std::clamp(std::round(k),1.f,2.f);
   if(shadow)hudText(bold,18*k,ImVec2(at.x+offset,at.y+offset),lines[i].c_str(),ImGui::ColorConvertFloat4ToU32(ImVec4(0,0,0,ink.w*.6275f)));hudText(bold,18*k,at,lines[i].c_str(),ImGui::ColorConvertFloat4ToU32(ink));}
  stack+=place.hi.y-place.lo.y+8;drawn.push_back(id);};
 static Module* fps=ModuleById("fps");static Module* list=ModuleById("module_list");
 if(fps&&fps->enabled){
  static float shown=0,elapsed=0,since=1e9f;static int frames=0;++frames;elapsed+=io.DeltaTime;since+=io.DeltaTime;auto rate=Find(*fps,"update_rate");
  if(since>=(rate?(float)rate->number:.25f)&&elapsed>0){shown=frames/elapsed;frames=0;elapsed=0;since=0;}
  char text[32]{};snprintf(text,sizeof(text),"%.0f FPS",shown);badge("fps",*fps,text);
 }
 static Module* clock=ModuleById("clock");
 if(clock&&clock->enabled){
  // "System" source only; "In-Game" would need the world time.
  SYSTEMTIME now{};GetLocalTime(&now);auto format=Find(*clock,"format");const bool twelve=format&&format->text=="12 Hour",seconds=Flag(*clock,"show_seconds",false);int hour=now.wHour;const char* suffix="";
  if(twelve){suffix=hour>=12?" PM":" AM";hour%=12;if(!hour)hour=12;}char text[32]{};
  if(seconds)snprintf(text,sizeof(text),twelve?"%d:%02d:%02d%s":"%02d:%02d:%02d%s",hour,now.wMinute,now.wSecond,suffix);else snprintf(text,sizeof(text),twelve?"%d:%02d%s":"%02d:%02d%s",hour,now.wMinute,suffix);
  textBox("clock",*clock,{std::string(text)},10,stack);   // same text box as Coordinates
 }
 static Module* coordinates=ModuleById("coordinates");static Module* speedometer=ModuleById("speedometer");
 if((coordinates&&coordinates->enabled)||(speedometer&&speedometer->enabled)){
  game::Player player{};const bool live=game::ReadPlayer(&player);
  if(coordinates&&coordinates->enabled&&(live||hudEditing)){
   // "X: …", "Y: …", "Z: …" lines; Horizontal layout uses a single "{}  {}  {}" line.
   // The position read is at eye level (feet + 1.62); the feet position is displayed, as the game does.
   if(live)player.position[1]-=1.62f;
   auto layout=Find(*coordinates,"layout");auto precision=Find(*coordinates,"precision");const int digits=precision&&precision->kind=="number"?std::clamp((int)precision->number,0,4):1;const char* axes="XYZ";std::vector<std::string> rows;
   for(int i=0;i<3;++i){char row[40]{};snprintf(row,sizeof(row),"%c: %.*f",axes[i],digits,player.position[i]);rows.push_back(row);}
   if(layout&&layout->text=="Horizontal")rows={rows[0]+"  "+rows[1]+"  "+rows[2]};
   textBox("coordinates",*coordinates,rows,10,stack);
  }
  if(speedometer&&speedometer->enabled&&(live||hudEditing)){
   // Speed = movement per tick × 20 ticks per second; "Include Vertical" adds the vertical component.
   auto unit=Find(*speedometer,"unit");const bool vertical=Flag(*speedometer,"include_vertical",false);float dx=player.position[0]-player.previous[0],dy=vertical?player.position[1]-player.previous[1]:0,dz=player.position[2]-player.previous[2];
   float speed=std::sqrt(dx*dx+dy*dy+dz*dz)*20;const bool kmh=unit&&unit->text=="km/h";char text[40]{};snprintf(text,sizeof(text),kmh?"%.1f km/h":"%.2f b/s",kmh?speed*3.6f:speed);textBox("speedometer",*speedometer,{std::string(text)},10,stack);
  }
 }
 // ---- Server IP: one line, "Singleplayer", "Unknown", address or address:port; preview "play.example.net"
 static Module* serverIp=ModuleById("server_ip");
 if(serverIp&&serverIp->enabled){
  game::Server server{};const bool live=game::ReadServer(&server);std::string line;
  if(live){if(server.singleplayer)line="Singleplayer";else if(!server.address[0])line="Unknown";else{line=server.address;if(Flag(*serverIp,"show_port",false))line+=":"+std::to_string(server.port);}}
  else if(hudEditing)line="play.example.net";
  // "Custom" only replaces an existing line (preview first, then Custom).
  if(!line.empty()&&Flag(*serverIp,"custom",false)){auto custom=Find(*serverIp,"custom_text");line=custom&&!custom->text.empty()?custom->text:"Server";}
  // Stacked with the other labels so they do not overlap.
  if(!line.empty())textBox("server_ip",*serverIp,{line},10,stack);
 }
 // ---- Direction: 260 x 34 compass (52 with heading or axis); heading = fmod(yaw + 180, 360).
 // Default position: centred, 90 px from the top.
 static Module* direction=ModuleById("direction");
 // Bed Breaker: 140 x 30 progress bar, only while breaking.
 {static Module* bbm=ModuleById("bed_breaker");game::BbSnapshot sn{};{auto& s=game::BedBreaker();std::lock_guard<std::mutex> lock(s.guard);sn=s.snap;}
  if(bbm&&bbm->enabled&&(sn.state!=0||hudEditing)){auto& place=Place("bed_breaker",std::floor(io.DisplaySize.x*.45f),std::floor(io.DisplaySize.y*.66f));const float k=place.scale,x=place.x,y=place.y;
   place.lo=ImVec2(x,y);place.hi=ImVec2(x+140*k,y+30*k);d->AddRectFilled(place.lo,place.hi,IM_COL32(15,15,20,175),4*k);
   const ImVec2 a(x+6*k,y+8*k),b(x+134*k,y+25*k);d->AddRectFilled(a,b,IM_COL32(255,255,255,40),1.5f*k);const float p=std::clamp(sn.progress,0.f,1.f);
   if(p>0)d->AddRectFilled(a,ImVec2(a.x+(b.x-a.x)*p,b.y),Acc(255/255.f),1.5f*k);
   char label[48];snprintf(label,sizeof(label),"%s %.0f%%",sn.uncover?"Uncovering":"Breaking",p*100);hudText(regular,16*k,ImVec2(x+6*k,y+4*k),label,IM_COL32(255,255,255,255));}}
 // Regen: 140 x 30 progress bar while mining (label Mining / Uncovering / Queued), default (0.45, 0.62) of the screen.
 {static Module* rgm=ModuleById("regen");game::RgSnapshot sn{};{auto& s=game::Regen();std::lock_guard<std::mutex> lock(s.guard);sn=s.snap;}
  if(rgm&&rgm->enabled&&(sn.state!=0||hudEditing)){auto& place=Place("regen",std::floor(io.DisplaySize.x*.45f),std::floor(io.DisplaySize.y*.62f));const float k=place.scale,x=place.x,y=place.y;
   place.lo=ImVec2(x,y);place.hi=ImVec2(x+140*k,y+30*k);d->AddRectFilled(place.lo,place.hi,IM_COL32(15,15,20,175),4*k);
   const ImVec2 a(x+6*k,y+8*k),b(x+134*k,y+25*k);d->AddRectFilled(a,b,IM_COL32(255,255,255,40),1.5f*k);const float p=std::clamp(sn.progress,0.f,1.f);
   if(p>0)d->AddRectFilled(a,ImVec2(a.x+(b.x-a.x)*p,b.y),Acc(255/255.f),1.5f*k);
   const std::string name=sn.state?game::RegenLabel(sn):std::string("Mining");char label[48];snprintf(label,sizeof(label),"%s %.0f%%",name.c_str(),p*100);hudText(regular,16*k,ImVec2(x+6*k,y+4*k),label,IM_COL32(255,255,255,255));}}
 if(direction&&direction->enabled){
  float yaw=0;const bool live=game::ReadYaw(&yaw);
  if(live||hudEditing){
   float heading=live?std::fmod(yaw+180,360.f):0;if(heading<0)heading+=360;
   const bool showHeading=Flag(*direction,"show_heading",true),showAxis=Flag(*direction,"show_axis",false),degrees=Flag(*direction,"show_degrees",true),shadow=Flag(*direction,"text_shadow",true);
   auto& place=Place("direction",std::floor((io.DisplaySize.x-260)/2),90);const float k=place.scale,x=place.x,y=place.y,cx=x+130*k,cy=y+28*k;
   place.lo=ImVec2(x,y);place.hi=ImVec2(x+260*k,y+(showHeading||showAxis?52:34)*k);
   auto pick=[&](const char* id,ImVec4 fallback){auto setting=Find(*direction,id);return setting&&setting->kind=="color"?ParseColor(setting->text):fallback;};
   const ImVec4 ink=pick("text_color",ImVec4(1,1,1,1)),accent=pick("accent_color",ImVec4(1,.333f,.333f,1));
   if(Flag(*direction,"background",true))d->AddRectFilled(place.lo,place.hi,ImGui::ColorConvertFloat4ToU32(pick("background_color",ImVec4(0,0,0,.51f))),3*k);
   auto write=[&](ImFont* font,float size,ImVec2 at,const char* text,ImVec4 color){const float offset=std::clamp(std::round(size/18),1.f,2.f);if(shadow)hudText(font,size,ImVec2(at.x+offset,at.y+offset),text,ImGui::ColorConvertFloat4ToU32(ImVec4(0,0,0,color.w*.6275f)));hudText(font,size,at,text,ImGui::ColorConvertFloat4ToU32(color));};
   static const char* cardinals[8]={"N","NE","E","SE","S","SW","W","NW"};static const char* axes[4]={"-Z","+X","+Z","-X"};
   d->PushClipRect(place.lo,place.hi,true);
   for(float deg=std::floor((heading-60)/5)*5;deg<=heading+60;deg+=5){
    const float distance=std::fabs(deg-heading)/60;if(distance>1)continue;const int whole=(((int)std::lround(deg))%360+360)%360;const float px=cx+(deg-heading)*k*260/120,fade=1-distance*distance;
    ImVec4 color=whole==0?accent:ink;color.w*=fade;d->AddLine(ImVec2(px,cy-(whole%45==0?9:whole%15==0?6:3)*k),ImVec2(px,cy),ImGui::ColorConvertFloat4ToU32(color),(whole%45==0?2:1)*k);
    if(whole%45==0){const char* text=cardinals[whole/45];const float size=(whole%90==0?16:12)*k;write(bold,size,ImVec2(px-hudWidth(bold,size,text)/2,cy-10*k-size),text,color);}
    else if(whole%15==0&&degrees){char text[8]{};snprintf(text,sizeof(text),"%d",whole);const float size=10*k;write(regular,size,ImVec2(px-hudWidth(regular,size,text)/2,cy-10*k-size),text,color);}
   }
   d->PopClipRect();
   d->AddTriangleFilled(ImVec2(cx,y+29*k),ImVec2(cx+4*k,y+33*k),ImVec2(cx-4*k,y+33*k),ImGui::ColorConvertFloat4ToU32(accent));d->AddLine(ImVec2(cx,y+17*k),ImVec2(cx,y+28*k),ImGui::ColorConvertFloat4ToU32(accent),2*k);
   if(showHeading||showAxis){std::string text;if(showHeading){char value[24]{};snprintf(value,sizeof(value),"%03d\xc2\xb0 %s",((int)std::lround(heading))%360,cardinals[((int)std::lround(heading/45))&7]);text=value;}
    if(showAxis){if(!text.empty())text+="  ";text+=axes[((int)std::lround(heading/90))&3];}write(bold,14*k,ImVec2(cx-hudWidth(bold,14*k,text.c_str())/2,y+34*k),text.c_str(),ink);}
   drawn.push_back("direction");
  }
 }
 // ---- Effect Display: one line per effect (name + level, duration below). The icons (Show Icons) are drawn by the
 // game in native/game_render.hpp (IconWork). Hide Vanilla hides the native "mob_effects_renderer" control
 // (native/game_controls.hpp).
 static Module* effectDisplay=ModuleById("effect_display");
 if(effectDisplay&&effectDisplay->enabled){
  static const char* effectNames[31]={"Unknown","Speed","Slowness","Haste","Mining Fatigue","Strength","Instant Health","Instant Damage","Jump Boost","Nausea","Regeneration","Resistance","Fire Resistance","Water Breathing","Invisibility","Blindness","Night Vision","Hunger","Weakness","Poison","Wither","Health Boost","Absorption","Saturation","Levitation","Fatal Poison","Conduit Power","Slow Falling","Bad Omen","Hero of the Village","Darkness"};
  static const char* roman[11]={"","I","II","III","IV","V","VI","VII","VIII","IX","X"};
  game::Effect effects[64]{};const int count=game::ReadEffects(effects,64);std::vector<std::pair<std::string,std::string>> rows;game::IconRequest icons{};const bool showIcons=Flag(*effectDisplay,"show_icons",true);
  for(int i=0;i<count;++i){const auto& effect=effects[i];if(effect.id==0||effect.id>=31||effect.id==6||effect.id==7||effect.id==23)continue;   // effects without an icon are never shown
   std::string name=effectNames[effect.id];if(effect.amplifier>0&&effect.amplifier<10)name+=std::string(" ")+roman[effect.amplifier+1];
   char time[24]{};if(effect.duration>=0)snprintf(time,sizeof(time),"%d:%02d",effect.duration/1200,(effect.duration/20)%60);else snprintf(time,sizeof(time),"**:**");rows.push_back({name,time});if(icons.count<16)icons.ids[icons.count++]=(uint8_t)effect.id;}
  if(rows.empty()&&hudEditing)rows.push_back({"Speed II","1:23"});
  if(!rows.empty()){
   // Name 13 and duration 12 (14 lower), line 26 + 4, padding 6, background radius 5, Name Color / Duration Color,
   // Text Shadow. With "Show Icons" the text starts at 36 (24 px icon at 6, centred vertically).
   auto& place=Place("effect_display",10,stack);const float k=place.scale;float widest=0;const bool shadow=Flag(*effectDisplay,"text_shadow",true);
   auto pick=[&](const char* id,ImVec4 fallback){auto setting=Find(*effectDisplay,id);return setting&&setting->kind=="color"?ParseColor(setting->text):fallback;};const ImVec4 nameInk=pick("name_color",ImVec4(1,1,1,1)),timeInk=pick("duration_color",ImVec4(.72f,.72f,.72f,1));
   for(auto& row:rows)widest=std::max({widest,hudWidth(bold,13*k,row.first.c_str()),hudWidth(regular,12*k,row.second.c_str())});
   const float inset=showIcons?36*k:6*k;place.lo=ImVec2(place.x,place.y);place.hi=ImVec2(place.x+widest+inset+6*k,place.y+(rows.size()*26+(rows.size()-1)*4+12)*k);
   if(showIcons&&icons.count&&!hudEditing&&game::InstallScreenHook(WriteLog)==1){icons.enabled=true;icons.x=place.x;icons.y=place.y;icons.scale=k;static bool told=false;if(game::Icons().fault&&!told){told=true;if(WriteLog)WriteLog("Effect Display: a fault was caught while drawing icons; they are disabled until the game restarts");}static bool seen=false;if(!seen&&game::Icons().frames&&WriteLog){seen=true;WriteLog("Effect Display: first icons drawn by the game");}}
   if(Flag(*effectDisplay,"background",false))d->AddRectFilled(place.lo,place.hi,ImGui::ColorConvertFloat4ToU32(pick("background_color",ImVec4(.08f,.07f,.11f,.69f))),5*k);
   auto write=[&](ImFont* font,float size,ImVec2 at,const char* text,ImVec4 color){const float offset=std::clamp(std::round(size/18),1.f,2.f);if(shadow)hudText(font,size,ImVec2(at.x+offset,at.y+offset),text,ImGui::ColorConvertFloat4ToU32(ImVec4(0,0,0,color.w*.6275f)));hudText(font,size,at,text,ImGui::ColorConvertFloat4ToU32(color));};
   for(size_t i=0;i<rows.size();++i){const float top=place.y+6*k+i*30*k;write(bold,13*k,ImVec2(place.x+inset,top),rows[i].first.c_str(),nameInk);write(regular,12*k,ImVec2(place.x+inset,top+14*k),rows[i].second.c_str(),timeInk);}
   stack+=place.hi.y-place.lo.y+8;drawn.push_back("effect_display");
  }
  {auto& shared=game::Icons();AcquireSRWLockExclusive(&shared.lock);shared.request=icons;ReleaseSRWLockExclusive(&shared.lock);}
  if(Flag(*effectDisplay,"hide_vanilla",false))game::InstallControlHook(WriteLog);
 }
 {auto& shared=game::Controls();const bool hide=effectDisplay&&effectDisplay->enabled&&Flag(*effectDisplay,"hide_vanilla",false);AcquireSRWLockExclusive(&shared.lock);shared.slots[4].enabled=hide;shared.slots[4].hide=hide;shared.slots[4].x=shared.slots[4].y=-20000;ReleaseSRWLockExclusive(&shared.lock);}
 // ---- Health Warning: full-screen gradient veil on all four edges when health <= threshold; no position.
 static Module* healthWarning=ModuleById("health_warning");
 if(healthWarning&&healthWarning->enabled){
  float health=0,maximum=0;auto number=[&](const char* id,float fallback){auto setting=Find(*healthWarning,id);return setting&&setting->kind=="number"?(float)setting->number:fallback;};const float threshold=number("health_threshold",10);
  if(game::ReadHealth(&health,&maximum)&&health<=threshold&&io.DisplaySize.x>0&&io.DisplaySize.y>0){
   float strength=threshold<1?1.f:std::clamp((threshold-health)/threshold,0.f,1.f);strength*=number("strength",60)/100;
   // pulses between 70 % and 100 % (std::lerp(0.7, 1.0, f)), phased on our own clock
   if(Flag(*healthWarning,"pulse",true))strength*=.7f+.3f*(1-std::cos(6.2831853f*number("pulse_speed",1.5f)*(float)ImGui::GetTime()))/2;
   auto tint=Find(*healthWarning,"color");ImVec4 color=tint&&tint->kind=="color"?ParseColor(tint->text):ImVec4(1,0,0,1);color.w*=strength;const ImU32 full=ImGui::ColorConvertFloat4ToU32(color),none=full&0x00ffffff;
   const float w=io.DisplaySize.x,h=io.DisplaySize.y,cover=number("coverage",35)/100;
   d->AddRectFilledMultiColor(ImVec2(0,0),ImVec2(w,h*cover),full,full,none,none);d->AddRectFilledMultiColor(ImVec2(0,h-h*cover),ImVec2(w,h),none,none,full,full);
   d->AddRectFilledMultiColor(ImVec2(0,0),ImVec2(w*cover,h),full,none,none,full);d->AddRectFilledMultiColor(ImVec2(w-w*cover,0),ImVec2(w,h),none,full,full,none);
  }
 }
 // ---- Elements fed by game hooks (native/game_events.hpp: attack, ActorEventPacket, StartGamePacket); the hooks are
 // only installed when one of these four modules is enabled.
 // Reach Display: "{:.{}f} blocks", 0 after Timeout, min(d, Max Reach) with Limit Reach, "0.00 blocks" before any hit.
 // Combo Counter: "{} Combo". An attack records the target and a pending hit; a hurt event for that target within
 // one second adds 1; a hurt event for the local player or more than Reset Delay without a hit resets to 0.
 // Totem Pop Counter: "{}: {}" per player, event type 0x41 = +1, type 3 = removal (Reset On Death); cleared on world
 // join, "No pops" when empty, sorted by descending count, Max Entries, Include Self.
 // Target HUD: last hit target for Timeout, 190 x 48 panel; falls back to the crosshair target (Crosshair Target).
 // Skin head (64 x 64, hat blended, 34 px at (7, 7), radius 4, tinted red when hurt) and translated names of
 // non-player entities come from game_events.hpp. Health prediction: game::PredictedHealth.
 static Module* reach=ModuleById("reach_display");static Module* combo=ModuleById("combo_counter");static Module* totems=ModuleById("totem_pop_counter");static Module* targetHud=ModuleById("target_hud");static Module* reachRange=ModuleById("reach_range");
 if((reach&&reach->enabled)||(combo&&combo->enabled)||(totems&&totems->enabled)||(targetHud&&targetHud->enabled)||(reachRange&&reachRange->enabled)){
  const int hooked=game::InstallEvents(WriteLog);
  struct ComboState{uint64_t target=0,attackTick=0,lastHit=0;int count=0;bool pending=false,hasTarget=false;};static ComboState chain;static std::vector<std::pair<std::string,int>> pops;
  static uint32_t seenAttack=0,seenWorld=0;static uint64_t attackTarget=0,attackTick=0;static float lastReach=-1;static bool everHit=false;
  std::vector<game::PacketEvent> packets;bool attacked=false,world=false;
  if(hooked==1){auto& events=game::Shared();std::lock_guard<std::mutex> lock(events.guard);packets.swap(events.packets);if(events.attackSerial!=seenAttack){seenAttack=events.attackSerial;attacked=true;attackTarget=events.attackTarget;attackTick=events.attackTick;lastReach=events.reach;}if(events.worldSerial!=seenWorld){seenWorld=events.worldSerial;world=true;}}
  uint64_t localId=0;char localName[96]{};const bool inWorld=game::LocalIdentity(&localId,localName,sizeof(localName));const uint64_t now=GetTickCount64();
  if(world||!inWorld)pops.clear();if(!inWorld)chain=ComboState{};
  if(attacked){everHit=true;if(!chain.hasTarget||chain.target!=attackTarget){chain.count=0;chain.target=attackTarget;chain.hasTarget=true;}chain.pending=true;chain.attackTick=attackTick;
   static bool logged=false;if(!logged&&WriteLog){logged=true;char line[120]{};snprintf(line,sizeof(line),"Game events: first attack seen, reach %.2f",lastReach);WriteLog(line);}}
  for(auto& packet:packets){
   static bool logged=false;if(!logged&&WriteLog){logged=true;char line[120]{};snprintf(line,sizeof(line),"Game events: first ActorEvent packet seen, type %d",(int)packet.type);WriteLog(line);}
   if(packet.type==2){if(inWorld&&packet.actor==localId)chain.count=0;else if(chain.hasTarget&&packet.actor==chain.target&&chain.pending&&packet.tick-chain.attackTick<=1000){chain.pending=false;++chain.count;chain.lastHit=packet.tick;}}
   else if(totems&&totems->enabled&&inWorld&&(packet.actor!=localId||Flag(*totems,"include_self",false))){char name[96]{};if(!game::PlayerName(packet.actor,name,sizeof(name)))continue;auto found=std::find_if(pops.begin(),pops.end(),[&](auto& entry){return entry.first==name;});
    if(packet.type==0x41){if(found==pops.end())pops.push_back({name,1});else ++found->second;}else if(packet.type==3&&found!=pops.end()&&Flag(*totems,"reset_on_death",true))pops.erase(found);}
  }
  if(reach&&reach->enabled){std::string line;
   if(inWorld){float distance=everHit&&lastReach>=0&&now-attackTick<=(uint64_t)(num(*reach,"timeout",3)*1000)?lastReach:0;if(Flag(*reach,"limit_reach",false))distance=std::min(distance,num(*reach,"max_reach",3));char text[48]{};snprintf(text,sizeof(text),"%.*f blocks",std::clamp((int)num(*reach,"precision",2),0,3),distance);line=text;}
   else if(hudEditing)line="0.00 blocks";
   if(!line.empty())textBox("reach_display",*reach,{line},10,stack);}
  if(combo&&combo->enabled){if(chain.count>0&&now-chain.lastHit>(uint64_t)(num(*combo,"reset_delay",3)*1000))chain.count=0;std::string line;if(inWorld)line=std::to_string(chain.count)+" Combo";else if(hudEditing)line="0 Combo";
   if(!line.empty())textBox("combo_counter",*combo,{line},10,stack);}
  if(totems&&totems->enabled){std::vector<std::string> lines;
   if(inWorld){auto list=pops;if(Flag(*totems,"include_self",false)&&localName[0]&&std::none_of(list.begin(),list.end(),[&](auto& entry){return entry.first==localName;}))list.push_back({localName,0});
    std::stable_sort(list.begin(),list.end(),[](auto& a,auto& b){return a.second>b.second;});const size_t limit=(size_t)std::clamp((int)num(*totems,"max_entries",5),1,10);
    for(size_t i=0;i<list.size()&&i<limit;++i)lines.push_back(list[i].first+": "+std::to_string(list[i].second));if(lines.empty())lines.push_back("No pops");}
   else if(hudEditing)lines.push_back("Steve: 2");
   if(!lines.empty())textBox("totem_pop_counter",*totems,lines,10,stack);}
  if(targetHud&&targetHud->enabled){game::Target target{};bool have=inWorld&&everHit&&now-attackTick<=(uint64_t)(num(*targetHud,"timeout",3)*1000)&&game::ReadTarget(attackTarget,&target);if(!have&&inWorld&&Flag(*targetHud,"crosshair_target",true)){target=game::Target{};have=game::ReadCrosshairTarget(&target);}
   if(have&&!target.player&&Flag(*targetHud,"players_only",false))have=false;
   if(!have&&hudEditing){have=true;target=game::Target{};target.player=true;strcpy_s(target.name,"Steve");target.health=14;target.maximum=20;target.absorption=4;target.distance=3.2f;}
   // Predicted health: Health Mode Auto / Server / Predicted; the prediction system is installed with Target HUD.
   {static bool predicting=false;if(!predicting){predicting=true;game::InstallPrediction();}game::PredictCleanup(inWorld);
    if(have&&!hudEditing&&target.actor){uint64_t tid{};int mode=0;if(auto st=Find(*targetHud,"health_mode")){const auto at=std::find(st->options.begin(),st->options.end(),st->text);mode=at==st->options.end()?0:(int)(at-st->options.begin());}
     if(game::RuntimeId(target.actor,&tid))target.health=game::PredictedHealth(tid,target.health,target.maximum,mode);}}
   if(have){
    auto& place=Place("target_hud",10,stack);const float k=place.scale,x=place.x,y=place.y;place.lo=ImVec2(x,y);place.hi=ImVec2(x+190*k,y+48*k);const bool shadow=Flag(*targetHud,"text_shadow",true);
    auto pick=[&](const char* id,ImVec4 fallback){auto setting=Find(*targetHud,id);return setting&&setting->kind=="color"?ParseColor(setting->text):fallback;};const ImVec4 ink=pick("text_color",ImVec4(1,1,1,1));ImVec4 back=pick("background_color",ImVec4(0,0,0,.588f));
    static float shown=0;static std::string shownName;if(shownName==target.name)shown+=(target.health-shown)*(1-std::exp(-12*io.DeltaTime));else{shown=target.health;shownName=target.name;}
    const float flash=target.hurt*.45f;back.x+=(200/255.f-back.x)*flash;back.y+=(40/255.f-back.y)*flash;back.z+=(40/255.f-back.z)*flash;d->AddRectFilled(place.lo,place.hi,ImGui::ColorConvertFloat4ToU32(back),6*k);
    // head texture rebuilt only when the skin changes
    float textX=x+7*k;if(target.player&&target.actor&&UpdateImage&&GetImage){static uint64_t shownSkin=0;static double next=0;static bool valid=false;static std::vector<uint8_t> face(256*256*4);
     if(ImGui::GetTime()>=next){next=ImGui::GetTime()+.25;uint64_t skin=0;const int n=game::ReadHead(target.actor,face.data(),(int)face.size(),&skin);if(!n)valid=false;else if(skin!=shownSkin||!valid||!GetImage(0)){shownSkin=skin;valid=UpdateImage(0,face.data(),n,n)!=0;}}
     if(valid&&GetImage(0)){const int tint=std::max(150,(int)(255-105*target.hurt));d->AddImageRounded(GetImage(0),ImVec2(x+7*k,y+7*k),ImVec2(x+41*k,y+41*k),ImVec2(0,0),ImVec2(1,1),IM_COL32(255,tint,tint,255),4*k);textX=x+49*k;}}
    const ImVec2 centre(x+168*k,y+24*k);d->AddCircle(centre,15*k,IM_COL32(0,0,0,0x78),48,3*k);
    const float whole=std::max(shown+target.absorption,target.maximum),part=std::clamp(shown/whole,0.f,1.f),extra=std::min(target.absorption/whole,1-part);
    auto arc=[&](float from,float to,ImU32 color){if(to<=from)return;d->PathArcTo(centre,15*k,6.2831855f*from-1.5707964f,6.2831855f*to-1.5707964f,std::max(2,(int)((to-from)*64)));d->PathStroke(color,0,3*k);};
    arc(0,part,ImGui::ColorConvertFloat4ToU32(pick("ring_color",ImVec4(1,.541f,.239f,1))));arc(part,part+extra,IM_COL32(255,196,64,255));
    auto write=[&](ImFont* font,float size,ImVec2 at,const char* text){const float offset=std::clamp(std::round(size/18),1.f,2.f);if(shadow)hudText(font,size,ImVec2(at.x+offset,at.y+offset),text,ImGui::ColorConvertFloat4ToU32(ImVec4(0,0,0,ink.w*.6275f)));hudText(font,size,at,text,ImGui::ColorConvertFloat4ToU32(ink));};
    char life[16]{},range[40]{};snprintf(life,sizeof(life),"%.0f",std::round(target.health));snprintf(range,sizeof(range),"Distance: %.1f",target.distance);
    write(bold,13*k,ImVec2(centre.x-hudWidth(bold,13*k,life)/2,y+17.5f*k),life);d->PushClipRect(ImVec2(x,y),ImVec2(x+147*k,y+48*k),true);write(bold,17*k,ImVec2(textX,y+9*k),target.name);write(regular,13*k,ImVec2(textX,y+26*k),range);d->PopClipRect();
    stack+=56*k;drawn.push_back("target_hud");}}
 }
 // ---- Player List: centred "Players (N)" header (19 high), names in columns of 10 rows (font 16, step 19, column gap
 // 18, padding 6), at most 50 names or 49 + "+N more", background radius 4. Shown while the module key is held when a
 // key is bound, otherwise while the module is enabled. Names come from game::ReadPlayerNames (guarded game call).
 static Module* playerList=ModuleById("player_list");
 if(playerList&&playerList->enabled){
  auto bound=keycodes.find("player_list");const bool held=bound==keycodes.end()||((GetAsyncKeyState(bound->second)&0x8000)!=0&&(opened||KeysAllowed()));
  static game::PlayerNames names{};static int state=-1,logged=-99;static double next=0;
  if(held&&ImGui::GetTime()>=next){next=ImGui::GetTime()+.5;uint64_t rva=0;state=game::ReadPlayerNames(&names,&rva);if(state!=logged&&WriteLog){logged=state;char line[140]{};snprintf(line,sizeof(line),"Player List: result %d, getter RVA 0x%llx, %d entries, %d names",state,(unsigned long long)rva,names.total,names.count);WriteLog(line);}}
  std::vector<std::string> cells;int total=0;
  if(held&&state==1){total=names.total;const int kept=names.total>50?std::min(names.count,49):names.count;for(int i=0;i<kept;++i)cells.push_back(names.names[i]);if(names.total>50)cells.push_back("+"+std::to_string(names.total-kept)+" more");}
  else if(hudEditing){cells={"Steve","Alex"};total=2;}
  if(!cells.empty()){
   auto& place=Place("player_list",10,stack);const float k=place.scale;const bool header=Flag(*playerList,"show_header",true),shadow=Flag(*playerList,"text_shadow",true);const std::string title="Players ("+std::to_string(total)+")";
   auto pick=[&](const char* id,ImVec4 fallback){auto setting=Find(*playerList,id);return setting&&setting->kind=="color"?ParseColor(setting->text):fallback;};const ImVec4 ink=pick("text_color",ImVec4(1,1,1,1));
   const int count=(int)cells.size(),columns=(count+9)/10,rows=std::min(count,10);std::vector<float> widths(columns,0.f);for(int i=0;i<count;++i)widths[i/10]=std::max(widths[i/10],hudWidth(regular,16,cells[i].c_str()));
   float content=(columns-1)*18.f;for(float w:widths)content+=w;const float titleWidth=header?hudWidth(bold,16,title.c_str()):0,width=std::max(titleWidth,content)+12,height=rows*16+(rows-1)*3+(header?19:0)+12.f;
   place.lo=ImVec2(place.x,place.y);place.hi=ImVec2(place.x+width*k,place.y+height*k);if(Flag(*playerList,"background",true))d->AddRectFilled(place.lo,place.hi,ImGui::ColorConvertFloat4ToU32(pick("background_color",ImVec4(0,0,0,.588f))),4*k);
   auto write=[&](ImFont* font,ImVec2 at,const char* text){const float offset=std::clamp(std::round(16*k/18),1.f,2.f);if(shadow)hudText(font,16*k,ImVec2(at.x+offset,at.y+offset),text,ImGui::ColorConvertFloat4ToU32(ImVec4(0,0,0,ink.w*.6275f)));hudText(font,16*k,at,text,ImGui::ColorConvertFloat4ToU32(ink));};
   if(header)write(bold,ImVec2(place.x+(width-titleWidth)*k*.5f,place.y+6*k),title.c_str());
   float x=place.x+6*k;for(int i=0;i<count;++i){if(i&&i%10==0)x+=(widths[i/10-1]+18)*k;write(regular,ImVec2(x,place.y+(6+(header?19:0)+(i%10)*19)*k),cells[i].c_str());}
   stack+=height*k+8;drawn.push_back("player_list");}
 }
 // ---- Armor HUD: the icons are drawn by the game itself (native/game_render.hpp, hook on the "hud_screen" render);
 // this draws the frame, background and durability numbers: 20 px icons every 22, padding 5, size 12 text below the icon
 // (Horizontal) or to its right (Vertical), background radius 4. The position reaches the game one frame late.
 // The feature disables itself on the first game-side fault.
 static Module* armorHud=ModuleById("armor_hud");
 {auto& shared=game::Armor();game::ArmorRequest request{};game::ArmorResult result{};
  if(armorHud&&armorHud->enabled){
   const int hooked=game::InstallScreenHook(WriteLog);static bool told=false;if(shared.fault&&!told){told=true;if(WriteLog)WriteLog("Armor HUD: a fault was caught in the game render path; icons are disabled until the game restarts");}
   static long seen=0;if(!seen&&shared.frames&&WriteLog){seen=shared.frames;WriteLog("Armor HUD: first hud_screen frame handled");}
   auto mode=Find(*armorHud,"mode");const bool vertical=mode&&mode->text=="Vertical",shadow=Flag(*armorHud,"text_shadow",true);
   AcquireSRWLockShared(&shared.lock);result=shared.result;ReleaseSRWLockShared(&shared.lock);if(hooked!=1||shared.fault)result=game::ArmorResult{};
   auto& place=Place("armor_hud",10,stack);const float k=place.scale;const int n=result.count;float widest=0;for(int i=0;i<n;++i)widest=std::max(widest,hudWidth(regular,12,result.text[i]));
   const float along=n?n*20+2*(n-1.f):20,cell=widest>0?22.f:20.f,width=!n?30:vertical?widest+cell+10:along+10,height=!n?30:vertical?along+10:cell+(widest>0?12:0)+10;
   place.lo=ImVec2(place.x,place.y);place.hi=ImVec2(place.x+width*k,place.y+height*k);
   auto pick=[&](const char* id,ImVec4 fallback){auto setting=Find(*armorHud,id);return setting&&setting->kind=="color"?ParseColor(setting->text):fallback;};const ImVec4 ink=pick("color",ImVec4(1,1,1,1));
   if(Flag(*armorHud,"background",false)&&(n||hudEditing))d->AddRectFilled(place.lo,place.hi,ImGui::ColorConvertFloat4ToU32(pick("background_color",ImVec4(0,0,0,.588f))),4*k);
   for(int i=0;i<n;++i){if(!result.text[i][0])continue;const float w=hudWidth(regular,12*k,result.text[i]),offset=std::clamp(std::round(12*k/18),1.f,2.f);
    const ImVec2 at=vertical?ImVec2(place.x+27*k,place.y+5*k+i*22*k+(20*k-12*k)/2):ImVec2(place.x+5*k+i*22*k+(20*k-w)/2,place.y+27*k);
    if(shadow)hudText(regular,12*k,ImVec2(at.x+offset,at.y+offset),result.text[i],ImGui::ColorConvertFloat4ToU32(ImVec4(0,0,0,ink.w*.6275f)));hudText(regular,12*k,at,result.text[i],ImGui::ColorConvertFloat4ToU32(ink));}
   if(n||hudEditing){stack+=height*k+8;drawn.push_back("armor_hud");}
   request={!hudEditing&&place.x>-10000,vertical,Flag(*armorHud,"show_held_item",true),Flag(*armorHud,"show_durability",true),place.x,place.y,k};
  }
  AcquireSRWLockExclusive(&shared.lock);shared.request=request;ReleaseSRWLockExclusive(&shared.lock);}
 // ---- Item Counter: same layout as Armor HUD (20 px icons every 22, padding 5, size 12 count below or to the right,
 // background radius 4). Counting and icons come from the game (native/game_render.hpp, CounterWork);
 // the "Items" setting is a comma-separated list of names.
 static Module* itemCounter=ModuleById("item_counter");
 {auto& shared=game::Counter();game::CounterRequest request{};game::CounterResult result{};
  if(itemCounter&&itemCounter->enabled){
   const int hooked=game::InstallScreenHook(WriteLog);static bool told=false;if(shared.fault&&!told){told=true;if(WriteLog)WriteLog("Item Counter: a fault was caught in the game render path; the counter is disabled until the game restarts");}
   static long seen=0;if(!seen&&shared.frames&&WriteLog){seen=shared.frames;WriteLog("Item Counter: first hud_screen frame handled");}
   auto mode=Find(*itemCounter,"mode");const bool vertical=mode&&mode->text=="Vertical",shadow=Flag(*itemCounter,"text_shadow",true);
   if(auto items=Find(*itemCounter,"items")){size_t from=0;const std::string& all=items->text;while(from<all.size()&&request.count<16){size_t to=all.find(',',from);if(to==std::string::npos)to=all.size();if(to>from&&to-from<48)strncpy_s(request.names[request.count++],all.substr(from,to-from).c_str(),_TRUNCATE);from=to+1;}}
   AcquireSRWLockShared(&shared.lock);result=shared.result;ReleaseSRWLockShared(&shared.lock);if(hooked!=1||shared.fault||!request.count)result=game::CounterResult{};
   auto& place=Place("item_counter",10,stack);const float k=place.scale;const int n=result.count;char text[16][12]{};float widest=0;for(int i=0;i<n;++i){snprintf(text[i],sizeof(text[i]),"%d",result.amount[i]);widest=std::max(widest,hudWidth(regular,12,text[i]));}
   const float along=n?n*20+2*(n-1.f):20,width=!n?30:vertical?widest+32:along+10,height=!n?30:vertical?along+10:44;
   place.lo=ImVec2(place.x,place.y);place.hi=ImVec2(place.x+width*k,place.y+height*k);
   auto pick=[&](const char* id,ImVec4 fallback){auto setting=Find(*itemCounter,id);return setting&&setting->kind=="color"&&setting->text.size()==9?ParseColor(setting->text):fallback;};const ImVec4 ink=pick("text_color",ImVec4(1,1,1,1));
   if(Flag(*itemCounter,"background",false)&&(n||hudEditing))d->AddRectFilled(place.lo,place.hi,ImGui::ColorConvertFloat4ToU32(pick("background_color",ImVec4(0,0,0,.588f))),4*k);
   for(int i=0;i<n;++i){const float w=hudWidth(regular,12*k,text[i]),offset=std::clamp(std::round(12*k/18),1.f,2.f);
    const ImVec2 at=vertical?ImVec2(place.x+27*k,place.y+5*k+i*22*k+(20*k-12*k)/2):ImVec2(place.x+5*k+i*22*k+(20*k-w)/2,place.y+27*k);
    if(shadow)hudText(regular,12*k,ImVec2(at.x+offset,at.y+offset),text[i],ImGui::ColorConvertFloat4ToU32(ImVec4(0,0,0,ink.w*.6275f)));hudText(regular,12*k,at,text[i],ImGui::ColorConvertFloat4ToU32(ink));}
   if(n||hudEditing){stack+=height*k+8;drawn.push_back("item_counter");}
   request.enabled=!hudEditing&&place.x>-10000;request.vertical=vertical;request.x=place.x;request.y=place.y;request.scale=k;
  }
  AcquireSRWLockExclusive(&shared.lock);shared.request=request;ReleaseSRWLockExclusive(&shared.lock);}
 // ---- Custom Paperdoll / Scoreboard / Chat / Item Name: nothing is drawn here; the game keeps drawing its control and
 // native/game_controls.hpp hides it (Hide) or moves it to our element's position. Until the element is moved in
 // "Edit layout", the control stays where the game puts it.
 {static const char* ids[4]={"paperdoll","scoreboard","chat","item_name"};static Module* natives[4]={ModuleById(ids[0]),ModuleById(ids[1]),ModuleById(ids[2]),ModuleById(ids[3])};static bool installed=false;static float adoptedX[4]={-1,-1,-1,-1},adoptedY[4]={-1,-1,-1,-1};static bool custom[4]{};
  bool any=false;for(auto* item:natives)any=any||(item&&item->enabled);if(any&&!installed){installed=true;game::InstallControlHook(WriteLog);}
  if(installed){auto& shared=game::Controls();static bool told=false;if(shared.fault&&!told){told=true;if(WriteLog)WriteLog("Native controls: a fault was caught while handling a control; the feature is disabled until the game restarts");}
   for(int i=0;i<4;++i){const bool enabled=natives[i]&&natives[i]->enabled;AcquireSRWLockShared(&shared.lock);const game::ControlSlot slot=shared.slots[i];ReleaseSRWLockShared(&shared.lock);float x=-20000,y=-20000;
    static long logged[4]{};if(slot.seen&&!logged[i]&&WriteLog){logged[i]=1;char line[140]{};snprintf(line,sizeof(line),"Native controls: '%s' seen, %.0f x %.0f px at %.0f, %.0f",slot.name,slot.width,slot.height,slot.nativeX,slot.nativeY);WriteLog(line);}
    static int savedVersion[4]={-1,-1,-1,-1};static bool savedKnown[4]{};   // read once per [hud] change instead of every frame
    if(enabled&&savedVersion[i]!=hudIniVersion){savedVersion[i]=hudIniVersion;savedKnown[i]=GetPrivateProfileIntW(L"hud",Wide(std::string(ids[i])+"-x").c_str(),-1,configPath.c_str())>=0;}
    if(enabled){const bool saved=savedKnown[i];
     if(saved||slot.seen||hudEditing){auto& place=Place(ids[i],slot.seen?slot.nativeX:10,slot.seen?slot.nativeY:stack);const float w=slot.width>0?slot.width:110,h=slot.height>0?slot.height:32;
      if(adoptedX[i]<0&&!saved&&slot.seen){adoptedX[i]=place.x;adoptedY[i]=place.y;}if(saved||(adoptedX[i]>=0&&(place.x!=adoptedX[i]||place.y!=adoptedY[i])))custom[i]=true;
      place.lo=ImVec2(place.x,place.y);place.hi=ImVec2(place.x+w,place.y+h);drawn.push_back(ids[i]);if(custom[i]){x=place.x;y=place.y;}}}
    AcquireSRWLockExclusive(&shared.lock);auto& live=shared.slots[i];live.enabled=enabled;live.hide=enabled&&Flag(*natives[i],"hide",false);live.x=x;live.y=y;ReleaseSRWLockExclusive(&shared.lock);}}
  // Item Name format: separate hook, installed only when the format differs from the default "{name}".
  {auto& format=game::Format();auto setting=natives[3]?Find(*natives[3],"format"):nullptr;const std::string wanted=setting&&!setting->text.empty()?setting->text:std::string("{name}");   /* default: "{name}" */const bool on=natives[3]&&natives[3]->enabled&&wanted!="{name}";static bool hooked=false;if(on&&!hooked){hooked=true;game::InstallPopupHook(WriteLog);}
   AcquireSRWLockExclusive(&format.lock);format.enabled=on;strncpy_s(format.format,wanted.c_str(),_TRUNCATE);ReleaseSRWLockExclusive(&format.lock);static bool told=false;if(format.seen&&!told&&WriteLog){told=true;WriteLog(format.clipped?"Item Name: first item text rewritten (clipped to the game's buffer)":"Item Name: first item text rewritten");}}}
 // ---- Notifications: 344-wide cards, radius 14, top-left at (16, 16), gap 8; 0.26 s entrance with a 28 px slide,
 // 0.2 s exit, 3 s duration, at most four. Background #121119 at 66 %, 10 % white border, 16 % white top line;
 // #2a2833 disc of 40 with the initial, title #f5f4fa, body #bfbbcb.
 // Only the offline kinds exist: module toggled by its key ("Enabled" / "Disabled") and configs.
 {static Module* notifications=ModuleById("notifications");struct Card{std::string title,body;float age=0,left=3,leaving=-1,y=-1;};static std::vector<Card> cards;
  if(!notifications||!notifications->enabled){cards.clear();notices.clear();}
  else{for(auto& notice:notices){if(notice.kind==5&&!Flag(*notifications,"module_toggles",true))continue;int live=0;for(auto& card:cards)if(card.leaving<0)++live;if(live>=4)for(auto& card:cards)if(card.leaving<0){card.leaving=0;break;}Card card;card.title=notice.title;card.body=notice.body;cards.push_back(card);}notices.clear();
   const float dt=std::clamp(io.DeltaTime,0.f,.1f);float target=16;
   for(auto& card:cards){card.age+=dt;if(card.leaving<0){card.left-=dt;if(card.left<=0)card.leaving=0;}else card.leaving+=dt;const float h=64;if(card.y<0)card.y=target;card.y+=(target-card.y)*(1-std::exp(-18*dt));target+=h+8;
    const float t=std::clamp(card.age/.26f,0.f,1.f),shown=((2.3f*t-5.6f)*t+4.3f)*t,alpha=std::clamp(shown,0.f,1.f)*(card.leaving<0?1.f:std::clamp(1-card.leaving/.2f,0.f,1.f)),x=16-28*(1-shown),y=card.y;if(alpha<=0)continue;
    auto tint=[&](int red,int green,int blue,float opacity){return IM_COL32(red,green,blue,(int)(255*std::clamp(opacity*alpha,0.f,1.f)));};
    d->AddRectFilled(ImVec2(x,y),ImVec2(x+344,y+h),tint(18,17,25,.66f),14);d->AddRect(ImVec2(x,y),ImVec2(x+344,y+h),tint(255,255,255,.1f),14,0,1);d->AddLine(ImVec2(x+14,y+.5f),ImVec2(x+330,y+.5f),tint(255,255,255,.16f),1);
    d->AddCircleFilled(ImVec2(x+32,y+32),20,tint(42,40,51,1));const char initial[2]={card.title.empty()?'?':(char)std::toupper((unsigned char)card.title[0]),0};const float iw=hudWidth(bold,17,initial);hudText(bold,17,ImVec2(x+32-iw/2,y+32-10),initial,tint(191,187,203,1));
    hudText(bold,14,ImVec2(x+64,y+14),card.title.c_str(),tint(245,244,250,1));hudText(regular,13,ImVec2(x+64,y+34),card.body.c_str(),tint(191,187,203,1));}
   cards.erase(std::remove_if(cards.begin(),cards.end(),[](const Card& card){return card.leaving>=.2f;}),cards.end());}}
 // ---- Debug Overlay: font 14 x Scale, line 16, padding 2, black background at Background Opacity, text #e0e0e0,
 // black shadow 160. Left column: header, performance, position / world / player / network; right column (right-
 // aligned): display. Refreshed "Update Rate" times per second from game::ReadDebug. "Rotation" names the module whose
 // request the rotation manager applies (RotOwner), "None" otherwise. Time and FOV depend on the Time Changer / Zoom hooks.
 {static Module* debug=ModuleById("debug_overlay");
  if(debug&&debug->enabled){static std::vector<std::string> left,right;static double next=0,lastFrame=0;static int frames=0;static float worst=0,total=0;const double now=ImGui::GetTime();if(lastFrame>0){const float ms=(float)((now-lastFrame)*1000);total+=ms;worst=std::max(worst,ms);++frames;}lastFrame=now;
   if(now>=next){next=now+1.0/std::clamp(num(*debug,"update_rate",10),1.f,20.f);left.clear();right.clear();char line[200]{};auto add=[&](std::vector<std::string>& column,const char* format,...){va_list arguments;va_start(arguments,format);vsnprintf(line,sizeof(line),format,arguments);va_end(arguments);column.push_back(line);};
    // Groups are separated by an empty line (not drawn): header, performance, then the left-hand groups (position, world,
    // player, network); on the right, display then target.
    std::vector<std::string> group;auto flush=[&](std::vector<std::string>& column){if(group.empty())return;if(!column.empty())column.push_back("");for(auto& text:group)column.push_back(text);group.clear();};
    int enabledCount=0;for(auto& item:modules)if(item.enabled)++enabledCount;add(left,"Nickel %s","2.0");add(left,"Minecraft %s","1.26.62.01");add(left,"Modules: %d / %d enabled",enabledCount,(int)modules.size());
    if(Flag(*debug,"performance",true)&&frames>0){add(group,"FPS: %.0f (avg %.2f ms, max %.2f ms)",frames*1000/std::max(total,.001f),total/frames,worst);add(group,"Renderer: %s",backendName?backendName:"Unknown");flush(left);}frames=0;worst=0;total=0;
    std::vector<std::string> info;auto close=[&]{if(group.empty())return;if(!info.empty())info.push_back("");for(auto& text:group)info.push_back(text);group.clear();};
    game::Player me{};float yaw=0;const bool live=game::ReadPlayer(&me);game::DebugInfo extra{};const bool known=live&&game::ReadDebug(&extra,Flag(*debug,"target",true));
    auto yes=[](bool on){return on?"yes":"no";};
    if(live&&Flag(*debug,"position",true)){const float x=me.position[0],y=me.position[1]-1.62f,z=me.position[2];const int bx=(int)std::floor(x),by=(int)std::floor(y),bz=(int)std::floor(z);add(group,"XYZ: %.3f / %.3f / %.3f",x,y,z);add(group,"Block: %d %d %d",bx,by,bz);add(group,"Chunk: %d %d %d in %d %d %d",bx&15,by&15,bz&15,bx>>4,by>>4,bz>>4);
     // h = fmod(yaw + 180, 360) (+360 if negative); 8 directions = lround(h/45)&7, axis = lround(h/90)&3
     if(game::ReadYaw(&yaw)){float heading=std::fmod(yaw+180,360.f);if(heading<0)heading+=360;static const char* eight[8]={"N","NE","E","SE","S","SW","W","NW"};static const char* axis[4]={"-Z","+X","+Z","-X"};add(group,"Facing: %s (%s) (%.1f / %.1f)",eight[std::lround(heading/45)&7],axis[std::lround(heading/90)&3],std::fmod(yaw,360.f),extra.pitchKnown?extra.pitch:0.f);}
     {const int owner=game::RotOwner();const char* who="None";static const std::pair<int,const char*> owners[]={{game::kRotAutoBlockReset,"auto_block_reset"},{game::kRotWindCharge,"wind_charge"},{game::kRotPearlCatch,"pearl_catch"},
       {game::kRotDerp,"derp"},{game::kRotBedBreaker,"bed_breaker"},{game::kRotAntiAfk,"anti_afk"},{game::kRotAutoClicker,"auto_clicker"},{game::kRotKillAura,"kill_aura"},{game::kRotAimAssist,"aim_assist"},{game::kRotAuto360,"auto_360"},
       {game::kRotBowAimbot,"bow_aimbot"},{game::kRotAutoMace,"auto_mace"},{game::kRotPvpBot,"pvp_bot"},{game::kRotPathFinder,"path_finder"},{game::kRotPearlChaser,"pearl_chaser"},{game::kRotPearlAimbot,"pearl_aimbot"},{game::kRotRegen,"regen"}};
      std::string named;if(owner==game::kRotMovementD){static Module* mlg=ModuleById("auto_mlg");named=mlg&&mlg->enabled?"Auto MLG":"Auto Jump Reset";who=named.c_str();}
      else if(owner)for(auto& [id,key]:owners)if(id==owner){Module* m=ModuleById(key);named=m?m->name:key;who=named.c_str();break;}
      if(owner&&!strcmp(who,"None"))who="Script";add(group,"Rotation: %s",who);}
     int dimension=0;static const char* names[3]={"Overworld","Nether","The End"};add(group,"Dimension: %s",game::ReadDimension(&dimension)&&dimension>=0&&dimension<3?names[dimension]:"Unknown");close();}
    if(live&&Flag(*debug,"world",true)){if(game::Camera().timeTick>=0){const int tick=game::Camera().timeTick,day=tick/24000,hours=((tick%24000)/1000+6)%24,minutes=(tick%1000)*60/1000;add(group,"Time: %02d:%02d (day %d, tick %d)",hours,minutes,day,tick);}
     if(known&&extra.entities>=0)add(group,"Entities: %d",extra.entities);close();}
    if(live&&Flag(*debug,"player",true)){static const char* modes[7]={"Survival","Creative","Adventure","Unknown","Unknown","Default","Spectator"};if(known&&extra.gameMode!=-2)add(group,"Game Mode: %s",extra.gameMode>=0&&extra.gameMode<7?modes[extra.gameMode]:"Unknown");
     char health[24]="?",hunger[24]="?";if(known&&extra.healthKnown)snprintf(health,sizeof(health),"%.1f",extra.health);if(known&&extra.hungerKnown)snprintf(hunger,sizeof(hunger),"%.0f",extra.hunger);add(group,"Health: %s  Hunger: %s",health,hunger);
     const float vx=me.velocity[0]*20,vy=me.velocity[1]*20,vz=me.velocity[2]*20;add(group,"Velocity: %.2f / %.2f / %.2f b/s (horizontal %.2f)",vx,vy,vz,std::hypot(vx,vz));
     add(group,"Ground: %s  Sneak: %s  Sprint: %s  Fly: %s",yes(known?extra.ground:me.onGround),yes(extra.sneak),yes(extra.sprint),yes(extra.fly));close();}
    if(live&&Flag(*debug,"network",true)){game::Server server{};if(game::ReadServer(&server)){if(server.singleplayer)add(group,"Server: Singleplayer");else add(group,"Server: %s:%u",server.address,server.port);}game::PingTrace trace{};int64_t ping=0;if(game::ReadPing(&ping,&trace)==1)add(group,"Ping: %lld ms",(long long)ping);close();}
    if(!info.empty()){left.push_back("");for(auto& text:info)left.push_back(text);}
    if(known&&extra.gui){add(right,"Display: %.0fx%.0f",extra.display[0],extra.display[1]);if(extra.guiScale>0)add(right,"GUI Scale: %.2f",extra.guiScale);}else add(right,"Display: %.0fx%.0f",io.DisplaySize.x,io.DisplaySize.y);
    int perspective=0;if(live&&game::OptionValue(3,&perspective)){static const char* views[3]={"First Person","Third Person Back","Third Person Front"};add(right,"Perspective: %s",perspective>=0&&perspective<3?views[perspective]:"Unknown");}
    if(game::Camera().lastFov>0)add(right,"FOV: %.1f",game::Camera().lastFov*57.29578f);
    if(known&&Flag(*debug,"target",true)){static const char* faces[6]={"Down","Up","North","South","West","East"};
     if(extra.target==1){add(group,"Targeted Block: %d %d %d (%s)",extra.block[0],extra.block[1],extra.block[2],extra.face<6?faces[extra.face]:"?");if(extra.blockName[0])group.push_back(extra.blockName);}
     else if(extra.target==2)add(group,"Targeted Entity: %s (%.1f blocks)",extra.entityName,extra.entityDistance);flush(right);}}
   const float k=num(*debug,"scale",1),font=14*k,row=16*k,pad=2*k,shade=std::clamp(std::round(font/18),1.f,2.f);const bool shadow=Flag(*debug,"text_shadow",true);const int back=(int)(255*num(*debug,"background_opacity",55)/100);
   auto column=[&](const std::vector<std::string>& lines,bool rightSide){float y=pad;for(auto& text:lines){if(text.empty()){y+=row;continue;}const float w=hudWidth(regular,font,text.c_str()),x=rightSide?io.DisplaySize.x-pad-w-2*pad:pad;d->AddRectFilled(ImVec2(x,y),ImVec2(x+w+2*pad,y+row),IM_COL32(0,0,0,back));
     if(shadow)hudText(regular,font,ImVec2(x+pad+shade,y+(row-font)/2+shade),text.c_str(),IM_COL32(0,0,0,160));hudText(regular,font,ImVec2(x+pad,y+(row-font)/2),text.c_str(),IM_COL32(224,224,224,255));y+=row;}};
   column(left,false);column(right,true);}}
 d=worldList;   // module work and world visuals: drawn in every game screen
 // ---- Visuals: View Model, Block Hit, Item Inspect, No Armor, Third Person Nametags, Block Outline (native/game_hand.hpp).
 // This only passes the settings to the game's render thread and installs the hooks on first activation.
 // Item Inspect is a trigger: enabling it (V by default) plays the animation, then turns the module off.
 {static Module* viewModel=ModuleById("view_model");static Module* blockHit=ModuleById("block_hit");static Module* inspect=ModuleById("item_inspect");static Module* noArmor=ModuleById("no_armor");static Module* ownTag=ModuleById("third_person_nametags");static Module* outline=ModuleById("block_outline");
  auto& hand=game::Hand();static bool inspectUsed=false;if(inspect&&inspect->enabled){inspectUsed=true;InterlockedExchange(&hand.inspectRequest,1);inspect->enabled=false;Save(*inspect);}
  auto index=[&](Module* item,const char* id){auto setting=Find(*item,id);if(!setting)return 0;const auto found=std::find(setting->options.begin(),setting->options.end(),setting->text);return found==setting->options.end()?0:(int)(found-setting->options.begin());};
  const bool vm=viewModel&&viewModel->enabled,bh=blockHit&&blockHit->enabled,armor=noArmor&&noArmor->enabled,box=outline&&outline->enabled;InterlockedExchange(&hand.playing,Playing()?1:0);
  AcquireSRWLockExclusive(&hand.lock);hand.viewModel=vm;if(vm){const char* ids[6]={"main_hand_x","main_hand_y","main_hand_z","offhand_x","offhand_y","offhand_z"};for(int i=0;i<3;++i){hand.mainOffset[i]=num(*viewModel,ids[i],0);hand.offOffset[i]=num(*viewModel,ids[3+i],0);}}
  hand.blockHit=bh;if(bh){hand.animation=index(blockHit,"animation");hand.always=index(blockHit,"trigger")==1;hand.allItems=Flag(*blockHit,"all_items",false);hand.blockScale=num(*blockHit,"scale",1);}
  hand.inspect=inspectUsed;if(inspect){hand.inspectMode=index(inspect,"mode");hand.inspectSpeed=num(*inspect,"speed",1);hand.cancelOnAttack=Flag(*inspect,"cancel_on_attack",true);}
  hand.noArmor=armor;if(armor){const char* ids[4]={"helmet","chestplate","leggings","boots"};for(int i=0;i<4;++i)hand.armorSlot[i]=Flag(*noArmor,ids[i],true);hand.armorTarget=index(noArmor,"target");}
  hand.outline=box;if(box){auto read=[&](const char* id,ImVec4 fallback,float* out){auto setting=Find(*outline,id);const ImVec4 color=setting&&setting->kind=="color"&&setting->text.size()==9?ParseColor(setting->text):fallback;out[0]=color.x;out[1]=color.y;out[2]=color.z;out[3]=color.w;};read("outline_color",ImVec4(1,1,1,1),hand.outlineColor);read("overlay_color",ImVec4(1,1,1,.25f),hand.overlayColor);
   hand.thickness=num(*outline,"thickness",1.5f);hand.visibleOnly=Flag(*outline,"visible_only",true);hand.overlay=Flag(*outline,"overlay",true);hand.faceOnly=Flag(*outline,"selected_face_only",false);}
  ReleaseSRWLockExclusive(&hand.lock);
  if(vm||bh||inspectUsed)game::InstallHandHooks(WriteLog);if(armor)game::InstallArmorHook(WriteLog);if(box)game::InstallOutlineHook(WriteLog);
  // Freecam: catalogue settings; hooks installed on first activation.
  {static Module* freecam=ModuleById("freecam");nickel::visual::CameraSettings camera{};bool on=freecam&&freecam->enabled;
   if(on){camera.horizontal=std::clamp(num(*freecam,"horizontal_speed",1.f),.1f,5.f);camera.vertical=std::clamp(num(*freecam,"vertical_speed",1.f),.1f,5.f);camera.smoothing=std::clamp(num(*freecam,"smoothing",.02f),.02f,1.f);
    camera.offset={std::clamp(num(*freecam,"starting_x_offset",0.f),-100.f,100.f),std::clamp(num(*freecam,"starting_y_offset",0.f),-100.f,100.f),std::clamp(num(*freecam,"starting_z_offset",0.f),-100.f,100.f)};
    if(game::InstallFreeCameraHooks(WriteLog)!=1)on=false;}
   game::ConfigureFreeCamera(on,camera);
   static bool toldSeen=false;if(on&&!toldSeen&&game::FreeCameraState().seen){toldSeen=true;if(WriteLog)WriteLog("Freecam: first camera update written");}}
  // Free Look: active while the key is held; Perspective (0 ThirdPerson, 1 FirstPerson, 2 Current), Keep Camera Rotation.
  {static Module* freeLook=ModuleById("free_look");bool on=freeLook&&freeLook->enabled;int mode=0;bool keep=false;
   if(on){mode=std::clamp(index(freeLook,"perspective"),0,2);keep=Flag(*freeLook,"keep_camera_rotation",false);}
   if(on&&game::InstallFreeLookHooks(WriteLog)!=1)on=false;
   game::ConfigureFreeLook(on,mode,keep);
   static bool toldSeen=false;if(on&&!toldSeen&&game::FreeLookState().seen){toldSeen=true;if(WriteLog)WriteLog("Free Look: first held rotation written");}}
  // Chams, Walls and Outline ESP use the game's EditorHighlightScene pass, see native/game_highlight.hpp.
  {static Module* chams=ModuleById("chams");static Module* walls=ModuleById("walls");static Module* outlineEsp=ModuleById("outline_esp");
   game::HlModules hm{};auto colorOf=[&](Module* m,const char* id,float* out){auto st=Find(*m,id);if(st&&st->kind=="color"&&st->text.size()==9){const ImVec4 c=ParseColor(st->text);out[0]=c.x;out[1]=c.y;out[2]=c.z;out[3]=c.w;}};
   if(chams&&chams->enabled){hm.chams=true;hm.chamsMode=std::clamp(index(chams,"mode"),0,2);colorOf(chams,"color",hm.chamsColor);hm.chamsIntensity=num(*chams,"intensity",1.2f);hm.chamsSpeed=num(*chams,"speed",1.f);
    hm.chamsP=Flag(*chams,"players",true);hm.chamsM=Flag(*chams,"mobs",true);hm.chamsI=Flag(*chams,"items",false);hm.chamsPr=Flag(*chams,"projectiles",false);}
   if(walls&&walls->enabled){hm.walls=true;hm.wallsP=Flag(*walls,"players",true);hm.wallsM=Flag(*walls,"mobs",true);hm.wallsI=Flag(*walls,"items",true);hm.wallsPr=Flag(*walls,"projectiles",true);}
   if(outlineEsp&&outlineEsp->enabled){hm.outline=true;colorOf(outlineEsp,"color",hm.outlineColor);hm.outlineWidth=num(*outlineEsp,"width",2.f);hm.outlineGlow=Flag(*outlineEsp,"glow",false);
    hm.outlineGlowRadius=num(*outlineEsp,"glow_radius",8.f);hm.outlineGlowIntensity=num(*outlineEsp,"glow_intensity",1.5f);hm.outlineVisibleOnly=Flag(*outlineEsp,"visible_only",false);
    hm.outlinePlayersOnly=Flag(*outlineEsp,"players_only",true);hm.outlineSelf=Flag(*outlineEsp,"include_local_player",false);}
   // Hand Chams: same settings for the main hand, arm and offhand.
   {static Module* handChams=ModuleById("hand_chams");if(handChams&&handChams->enabled){nickel::visual::HighlightStyle st{};uint8_t* raw=reinterpret_cast<uint8_t*>(&st);
     const int fillMode=std::clamp(index(handChams,"fill_mode"),0,2),outlineMode=std::clamp(index(handChams,"outline_mode"),0,2);
     if(fillMode==2){const float w[4]{1,1,1,.25f};std::memcpy(st.fill,w,16);}else colorOf(handChams,"color",st.fill);st.mode=(uint8_t)fillMode;st.intensity=num(*handChams,"fill_intensity",.8f);st.speed=num(*handChams,"speed",0);st.fillEnabled=Flag(*handChams,"fill",true)?1:0;
     float second[4]{1,1,1,1};if(outlineMode==0)colorOf(handChams,"outline_color",st.outline);else if(outlineMode==2){const float w[4]{1,1,1,1};std::memcpy(st.outline,w,16);}
     else{auto g=Find(*handChams,"outline_gradient");std::string ends[2];if(g)GradientEnds("hand_chams",g->text,ends);const ImVec4 a=ParseColor(ends[0]),b=ParseColor(ends[1]);const float fa[4]{a.x,a.y,a.z,a.w};std::memcpy(st.outline,fa,16);second[0]=b.x;second[1]=b.y;second[2]=b.z;second[3]=b.w;}
     st.outlineB[0]=num(*handChams,"width",2);st.outlineB[1]=Flag(*handChams,"glow",false)?num(*handChams,"glow_radius",0):0.f;st.outlineC[0]=num(*handChams,"glow_intensity",0);raw[0x45]=(uint8_t)outlineMode;if(outlineMode==1)std::memcpy(raw+0x48,second,16);
     st.outlineEnabled=Flag(*handChams,"outline",false)?1:0;hm.hand=st.fillEnabled||st.outlineEnabled;hm.handMainOn=Flag(*handChams,"main_hand",true);hm.handOffOn=Flag(*handChams,"offhand",true);
     hm.handArm=hm.handMainOn&&Flag(*handChams,"arm",true);hm.handMain=st;hm.handOff=st;hm.handMerged=hm.handMainOn&&hm.handOffOn;if(!hm.handMainOn&&!hm.handOffOn)hm.hand=false;}}
   // Block ESP (native/game_block_esp.hpp): default style with fill alpha 0.25 and outline width 1.
   {static Module* blockEsp=ModuleById("block_esp");game::BeConfig bc{};if(blockEsp&&blockEsp->enabled){bc.enabled=true;auto st=Find(*blockEsp,"blocks");std::string text=st?st->text:"";size_t from=0;
     while(from<=text.size()){size_t comma=text.find(',',from);if(comma==std::string::npos)comma=text.size();std::string name=text.substr(from,comma-from);while(!name.empty()&&name.front()==' ')name.erase(0,1);while(!name.empty()&&name.back()==' ')name.pop_back();if(!name.empty())bc.names.push_back(name);from=comma+1;}
     bc.limit=Flag(*blockEsp,"limit_blocks",true);bc.maxBlocks=(int)num(*blockEsp,"max_blocks",4096);bc.scanMs=num(*blockEsp,"scan_speed",1);
     nickel::visual::HighlightStyle bs{};uint8_t* raw=reinterpret_cast<uint8_t*>(&bs);const int mode=std::clamp(index(blockEsp,"default_mode"),0,2);float color[4]{1,1,1,1},second[4]{1,1,1,1};
     if(mode==0){auto g=Find(*blockEsp,"default_gradient");std::string ends[2];GradientEnds("block_esp",g?g->text:"",ends);const ImVec4 a=ParseColor(ends[0]),b=ParseColor(ends[1]);color[0]=a.x;color[1]=a.y;color[2]=a.z;color[3]=a.w;second[0]=b.x;second[1]=b.y;second[2]=b.z;second[3]=b.w;}
     else colorOf(blockEsp,"default_color",color);
     bs.fill[0]=color[0];bs.fill[1]=color[1];bs.fill[2]=color[2];bs.fill[3]=.25f;bs.mode=0;bs.intensity=1;bs.speed=0;bs.shinyFlag=1;bs.fillEnabled=1;std::memcpy(bs.outline,color,16);bs.outlineB[0]=1;bs.outlineB[1]=0;bs.outlineC[0]=0;
     raw[0x44]=Flag(*blockEsp,"visible_only",false)?1:0;raw[0x45]=(uint8_t)(mode==0?1:mode==1?2:0);std::memcpy(raw+0x48,second,16);bs.outlineEnabled=1;hm.block=true;hm.blockStyle=bs;
     // Per-block styles (at most 7, in list order) -> indices 1..7, slots 9..15.
     {Setting* store=nullptr;for(auto& other:blockEsp->settings)if(other.id=="block_styles")store=&other;const auto own=store?ParseBlockStyles(store->text):std::map<std::string,BlockOwnStyle>{};
      bc.styleOf.assign(bc.names.size(),0);int next=1;hm.blockUsed=1;
      for(size_t i=0;i<bc.names.size()&&next<8;++i){auto f=own.find(bc.names[i]);if(f==own.end()||f->second.mode=='d')continue;const BlockOwnStyle& o=f->second;
       nickel::visual::HighlightStyle cs=bs;uint8_t* cr=reinterpret_cast<uint8_t*>(&cs);ImVec4 ca=ParseColor(o.a),cb=ParseColor(o.b);if(o.mode=='r')ca=ImVec4(1,1,1,1);
       cs.fill[0]=ca.x;cs.fill[1]=ca.y;cs.fill[2]=ca.z;cs.fill[3]=.25f;const float oc[4]{ca.x,ca.y,ca.z,ca.w},ob[4]{cb.x,cb.y,cb.z,cb.w};std::memcpy(cs.outline,oc,16);cr[0x45]=(uint8_t)(o.mode=='g'?1:o.mode=='r'?2:0);std::memcpy(cr+0x48,ob,16);
       hm.blockOwn[next]=cs;hm.blockUsed|=(uint8_t)(1<<next);bc.styleOf[i]=(uint8_t)next;++next;}}}
    static bool beSeen=false;if(bc.enabled||beSeen){beSeen=true;if(bc.enabled){game::InstallMapHooks(WriteLog);if(game::InstallWorldHook(WriteLog)!=1)bc.enabled=false;}game::ConfigureBlockEsp(bc,WriteLog);if(!bc.enabled)hm.block=false;}}
   // Item ESP, Outline option
   {static Module* itemEsp=ModuleById("item_esp");if(itemEsp&&itemEsp->enabled){auto render=Find(*itemEsp,"render");const auto picked=SplitList(render?render->text:std::string());
     if(std::find(picked.begin(),picked.end(),"Outline")!=picked.end()){hm.item=true;hm.itemWhite=Flag(*itemEsp,"whitelist",false);hm.itemBlack=Flag(*itemEsp,"blacklist",false);
      if(auto l=Find(*itemEsp,"whitelisted_items"))hm.itemWhiteList=SplitList(l->text);if(auto l=Find(*itemEsp,"blacklisted_items"))hm.itemBlackList=SplitList(l->text);}}}
   if((hm.chams||hm.walls||hm.outline||hm.hand||hm.block||hm.item)&&game::InstallHighlightHooks(WriteLog)!=1){hm.chams=hm.walls=hm.outline=hm.hand=hm.block=hm.item=false;}
   game::ConfigureHighlight(hm);
   game::ConfigureWallsBlockActors(hm.walls&&walls&&Flag(*walls,"block_actors",true),WriteLog);
   static bool told=false;if(!told&&game::Highlight().seen){told=true;if(WriteLog)WriteLog("Highlight: first style items inserted into the frame builder");}
   static bool toldFault=false;if(!toldFault&&game::Highlight().fault){toldFault=true;if(WriteLog)WriteLog("Highlight: a guarded game access failed; highlighting disabled until restart");}}
  static game::Context ctxXray{};
  // Xray: only listed blocks are meshed, at full brightness; the world is re-meshed when the list changes.
  {static Module* xray=ModuleById("xray");bool on=xray&&xray->enabled;std::string list;if(on){auto st=Find(*xray,"blocks");if(st)list=st->text;if(game::InstallXrayHooks(WriteLog)!=1)on=false;}
   if(on||game::Xray().enabled){game::InstallScreenHook(WriteLog);game::ConfigureXray(on,list);}}   // reload goes through the screen hook (game thread)
  // ---- Custom Sky (native/game_sky.hpp): hooks installed on first activation; the Sky material is patched at injection.
  {static Module* sky=ModuleById("custom_sky");game::SkyConfig c{};c.enabled=sky&&sky->enabled;if(sky){auto st=Find(*sky,"mode");if(st){const auto at=std::find(st->options.begin(),st->options.end(),st->text);c.mode=at==st->options.end()?0:(int)(at-st->options.begin());}
    c.speed=num(*sky,"speed",1);c.stars=Flag(*sky,"stars",true);c.clouds=Flag(*sky,"clouds",true);c.overridePack=Flag(*sky,"override_pack_sky",true);}
   if(c.enabled&&game::InstallSkyHooks(WriteLog)!=1)c.enabled=false;static bool configured=false;if(c.enabled||configured){configured=true;game::ConfigureSky(c);}}
  static int tagState=-1;const int wanted=ownTag&&ownTag->enabled?1:0;if(wanted!=tagState&&(wanted||tagState==1)){tagState=wanted;game::PatchOwnNametag(wanted!=0,WriteLog);}
  static bool told[4]{};auto once=[&](int i,bool when,const char* text){if(when&&!told[i]){told[i]=true;if(WriteLog)WriteLog(text);}};
  once(0,hand.fault!=0,"Held item: a fault was caught while changing the item transform; View Model, Block Hit and Item Inspect are disabled until the game restarts");once(1,hand.seen!=0,"Held item: first transform written");
  once(2,hand.outlineFault!=0,"Block Outline: a fault was caught while drawing; the game outline is back until the game restarts");once(3,hand.outlineSeen!=0,"Block Outline: first outline drawn");}
 // ---- Visuals: Zoom, Full Bright, Time Changer, No Camera Bob, Hurt Color (native/game_view.hpp). Zoom is active while
 // its key is held; with no key bound, while the module is enabled.
 {static Module* zoom=ModuleById("zoom");static Module* bright=ModuleById("full_bright");static Module* clock=ModuleById("time_changer");static Module* bob=ModuleById("no_camera_bob");static Module* hurt=ModuleById("hurt_color");auto& camera=game::Camera();
  bool zooming=false;if(zoom){auto bound=keycodes.find("zoom");zooming=bound!=keycodes.end()?(GetAsyncKeyState(bound->second)&0x8000)!=0&&!opened&&KeysAllowed():zoom->enabled;}
  if(zooming){game::InstallFovHook(WriteLog);camera.zoomAmount=num(*zoom,"zoom",50)/100;InterlockedExchange(&camera.zoomAnimation,Flag(*zoom,"animation",true)?1:0);}
  static bool wasZooming=false;if(zooming!=wasZooming){wasZooming=zooming;game::ZoomSensitivity(zooming,zoom?num(*zoom,"sensitivity",50)/100:1);}InterlockedExchange(&camera.zoom,zooming?1:0);
  {static Module* vsync=ModuleById("vsync_disabler");vsyncDisabled=vsync&&vsync->enabled;}
  {static Module* hide=ModuleById("stream_hide");streamHide=hide&&hide->enabled;}
  // Timer: the tick rate is written every level tick; the previous value is restored on the tick after disabling.
  bool useTick=false;
  game::utilityLog=WriteLog;game::actionLog=WriteLog;game::faultLog=WriteLog;game::instaLog=WriteLog;game::rotationLog=WriteLog;
  {static Module* timer=ModuleById("timer");auto& t=game::Timer();const bool on=timer&&timer->enabled;if(on){auto rate=Find(*timer,"tick_rate");t.rate=rate?(float)rate->number:20.f;}
   t.enabled=on;if(on||t.has)useTick=true;}
  // Visual Range, Spammer, Auto Disconnect (game_utility.hpp): level tick subscribers.
  {static Module* range=ModuleById("visual_range");auto& v=game::VisualRange();const bool on=range&&range->enabled;v.enabled=on;if(on){v.leave=Flag(*range,"leave_messages",true);useTick=true;}}
  {static Module* spam=ModuleById("spammer");auto& sp=game::Spammer();const bool on=spam&&spam->enabled;{std::lock_guard<std::mutex> lock(sp.guard);sp.enabled=on;
    if(on){auto list=Find(*spam,"messages");sp.messages=list?SplitList(list->text):std::vector<std::string>{};auto delay=Find(*spam,"delay");sp.delay=delay?delay->number:50;}}if(on||sp.armed)useTick=true;}
  {static Module* off=ModuleById("auto_disconnect");auto& ad=game::AutoDisconnect();const bool on=off&&off->enabled;
   if(ad.disableRequest.exchange(false)&&off&&off->enabled){off->enabled=false;Save(*off);}
   {std::lock_guard<std::mutex> lock(ad.guard);ad.enabled=on;if(on){auto mode=Find(*off,"mode");ad.command=mode&&mode->text=="Command Disconnect";auto cmd=Find(*off,"disconnect_command");ad.text=cmd?cmd->text:"";ad.disableOnExecute=Flag(*off,"disable_on_execute",false);auto th=Find(*off,"health_threshold");ad.threshold=th?(int)th->number:6;}}
   if(on)useTick=true;}
  // Skin Stealer: saves once on activation, on the game thread, then turns itself off.
  {static Module* steal=ModuleById("skin_stealer");static bool was=false;auto& ss=game::SkinStealer();const bool on=steal&&steal->enabled;
   if(on&&!was){ss.done=false;ss.request=true;}was=on;if(on)useTick=true;if(ss.done.exchange(false)&&steal&&steal->enabled){steal->enabled=false;Save(*steal);was=false;}}
  if(useTick){game::InstallMapHooks(WriteLog);game::utilityTick=game::UtilityTick;}
  // Scaffold (game_actions.hpp): level tick + action queue; disabling it or an invalid player cancels pending sequences.
  {static Module* sc=ModuleById("scaffold");static bool was=false;auto& s=game::Scaffold();const bool on=sc&&sc->enabled;{std::lock_guard<std::mutex> lock(s.guard);s.enabled=on;
    if(on){auto n=[&](const char* id,int d){auto x=Find(*sc,id);return x?(int)x->number:d;};s.extend=n("extend",1);s.placements=n("placements",1);s.range=n("range",1);s.delay=n("delay",0);s.switchDelay=n("switch_delay",50);
     s.lockY=Flag(*sc,"lock_y",false);s.ghost=Flag(*sc,"ghost_place",true);}}
   if(!on&&was){game::ActionCancelRequest(game::kScaffold);s.hasY=false;s.placed=false;s.hasPlaced=false;s.active=false;}was=on;
   if(on){game::InstallMapHooks(WriteLog);game::utilityTick=game::UtilityTick;if(game::InstallScreenHook(WriteLog)==1){game::screenUtility=game::ActionGameUpdate;game::actionReceive=game::ActionReceive;}}}
  // Replenish (game_inventory.hpp): runs from the screen render and the inventory scheduler.
  {static Module* rp=ModuleById("replenish");const bool on=rp&&rp->enabled;game::Replenish().enabled=on;
   if(on&&game::InstallScreenHook(WriteLog)==1){game::inventoryScreen=game::InventoryFrame;game::inventoryReceive=game::InventoryReceive;}}
  // Nuker (game_actions.hpp): level-tick subscriber.
  {static Module* nuk=ModuleById("nuker");auto& nk=game::Nuker();const bool on=nuk&&nuk->enabled;{std::lock_guard<std::mutex> lock(nk.guard);nk.enabled=on;
    if(on){auto n=[&](const char* id,int d){auto x=Find(*nuk,id);return x?(int)(int32_t)(int64_t)x->number:d;};nk.radius=n("radius",3);nk.perTick=n("breaks_per_tick",8);nk.height=n("height",1);nk.yRadius=n("y_radius",nk.radius);
     nk.noSwing=Flag(*nuk,"no_swing",false);nk.white=Flag(*nuk,"block_whitelist",false);nk.black=Flag(*nuk,"block_blacklist",false);nk.horizontal=Flag(*nuk,"horizontal_only",false);nk.yLimit=Flag(*nuk,"y_limit",false);{auto md=Find(*nuk,"mode");nk.onBreak=md&&md->text=="On Break";}
     auto w=Find(*nuk,"whitelisted_blocks");nk.whiteList=w?SplitList(w->text):std::vector<std::string>{};auto b=Find(*nuk,"blacklisted_blocks");nk.blackList=b?SplitList(b->text):std::vector<std::string>{};}}
   if(on){game::InstallMapHooks(WriteLog);game::utilityTick=game::UtilityTick;}}
  // Throw Pot, Spear Dash (game_actions.hpp): the sequence is queued on enable, runs on the game thread
  // and turns the module off when it finishes.
  bool wantActions=false,wantRotations=false;
  {auto& am=game::ActionMods();auto range=[&](Module& m,const char* id,float* low,float* high){if(auto r=Find(m,id)){*low=(float)r->number;*high=r->kind=="range"?(float)atof(r->text.c_str()):*low;}};
   {static Module* pot=ModuleById("throw_pot");static bool was=false;const bool on=pot&&pot->enabled;
    if(on&&!was){std::lock_guard<std::mutex> lock(am.guard);am.pot.request=true;am.pot.silent=Flag(*pot,"silent_use",false);am.pot.doublePot=Flag(*pot,"double_pot",false);
     auto n=[&](const char* id,int d){auto x=Find(*pot,id);return x?(int)x->number:d;};am.pot.switchDelay=n("switch_delay",50);am.pot.switchBack=n("switch_back_delay",0);am.pot.doubleHealth=n("double_pot_health",4);}
    was=on;if(on)wantActions=true;if(am.potDone.exchange(false)&&pot&&pot->enabled){pot->enabled=false;Save(*pot);was=false;}}
   {static Module* spear=ModuleById("spear_dash");static bool was=false;const bool on=spear&&spear->enabled;
    if(on&&!was){std::lock_guard<std::mutex> lock(am.guard);am.spear.request=true;am.spear.silent=Flag(*spear,"silent_use",false);range(*spear,"switch_back_delay",&am.spear.low,&am.spear.high);}
    was=on;if(on)wantActions=true;if(am.spearDone.exchange(false)&&spear&&spear->enabled){spear->enabled=false;Save(*spear);was=false;}}
   {static Module* wind=ModuleById("wind_charge");static bool was=false;const bool on=wind&&wind->enabled;game::Rotations().enabled[game::kRotWindCharge]=on;
    if(on&&!was){std::lock_guard<std::mutex> lock(am.guard);am.wind.request=true;am.wind.silent=Flag(*wind,"silent_use",false);auto a=Find(*wind,"aim_speed");am.wind.aim=a?(float)a->number/100.f:1.f;
     auto t=Find(*wind,"throw_delay");am.wind.throwDelay=t?(int)t->number:0;range(*wind,"switch_back_delay",&am.wind.low,&am.wind.high);}
    if(on){std::lock_guard<std::mutex> lock(am.guard);auto a=Find(*wind,"aim_speed");am.wind.aim=a?(float)a->number/100.f:1.f;}
    if(!on&&was){am.windAiming=false;game::ActionCancelRequest(game::kWindCharge);}   // cancel the pending action on disable
    {static Module* abrm=ModuleById("auto_block_reset");static bool awas=false;const bool aon=abrm&&abrm->enabled;game::Rotations().enabled[game::kRotAutoBlockReset]=aon;
     if(aon&&!awas){std::lock_guard<std::mutex> lock(am.guard);am.abr.request=true;am.abr.silentRotate=Flag(*abrm,"silent_rotate",true);am.abr.ghost=Flag(*abrm,"ghost_place",true);
      auto sb=Find(*abrm,"switch_back_delay");am.abr.switchBack=sb?(int)sb->number:50;auto ms=Find(*abrm,"max_stack");am.abr.maxStack=ms?(int)ms->number:1;}
     if(!aon&&awas)game::ActionCancelRequest(game::kAutoBlockReset);   // cancel the pending action on disable
     awas=aon;if(aon){wantActions=true;wantRotations=true;}if(am.abrDone.exchange(false)&&abrm&&abrm->enabled){abrm->enabled=false;Save(*abrm);awas=false;game::ActionCancelRequest(game::kAutoBlockReset);}}
    {static Module* pearl=ModuleById("pearl_catch");static bool pwas=false;const bool pon=pearl&&pearl->enabled;game::Rotations().enabled[game::kRotPearlCatch]=pon;
     if(pon){std::lock_guard<std::mutex> lock(am.guard);if(!pwas){am.pearl.request=true;auto md=Find(*pearl,"aim_mode");am.pearl.upward=!md||md->text!="CurrentAim";am.pearl.silentUse=Flag(*pearl,"silent_use",false);
       auto cd=Find(*pearl,"charge_delay");am.pearl.chargeDelay=cd?(int)cd->number:0;}am.pearl.silentAim=Flag(*pearl,"silent_aim",true);auto a=Find(*pearl,"aim_speed");am.pearl.aim=a?(float)a->number/100.f:1.f;}
     if(!pon&&pwas){am.pearlActive=false;game::ActionCancelRequest(game::kPearlCatch);}
     pwas=pon;if(pon){wantActions=true;wantRotations=true;}if(am.pearlDone.exchange(false)&&pearl&&pearl->enabled){pearl->enabled=false;Save(*pearl);pwas=false;am.pearlActive=false;game::ActionCancelRequest(game::kPearlCatch);}}
    was=on;if(on){wantActions=true;wantRotations=true;}if(am.windDone.exchange(false)&&wind&&wind->enabled){wind->enabled=false;Save(*wind);was=false;am.windAiming=false;game::ActionCancelRequest(game::kWindCharge);}}}

  // Blink, Fake Lag (game_lag.hpp): hold outgoing packets and release them later.
  {static Module* bl=ModuleById("blink");static bool was=false;auto& b=game::Blink();const bool on=bl&&bl->enabled;
   if(on){auto t=Find(*bl,"type");b.movementOnly=t&&t->text=="Movement Only";{auto dir=Find(*bl,"direction");b.bothWays=dir&&dir->text=="Bi-directional";}b.autoSend=Flag(*bl,"auto_send",false);auto th=Find(*bl,"send_threshold");b.threshold=th?(int)th->number:40;}
   if(on&&!was)b.hasReal=false;if(!on&&was)game::LagRelease(1);b.enabled=on;was=on;}
  {static Module* fl=ModuleById("fake_lag");static Module* teamsM=ModuleById("teams");static bool was=false;auto& f=game::FakeLag();const bool on=fl&&fl->enabled;
   if(on){auto md=Find(*fl,"mode");f.freezeMode=md&&md->text=="Freeze";auto mx=Find(*fl,"max_freeze");f.maxFreeze=mx?(int)mx->number:400;auto cd=Find(*fl,"cooldown");f.cooldown=cd?(int)cd->number:100;
    f.onlyTarget=Flag(*fl,"only_when_target",false);auto tr=Find(*fl,"target_range");f.range=tr?(float)tr->number:6.f;
    nickel::visual::TeamRules rules=TeamRulesOf(teamsM);rules.names.clear();f.teams=rules;}
   if(!on&&was)game::LagRelease(0);f.enabled=on;was=on;}
  if(game::LagAnyActive()){static bool ready=false;if(!ready){ready=game::InstallFreeLookHooks(WriteLog)==1&&game::InstallLagHooks(WriteLog)==1;
    if(ready){game::Lag().active=game::LagAnyActive;game::lagBlinkEvent=game::BlinkEvent;game::lagFakeEvent=game::FakeLagEvent;game::lagFakePass=game::FakeLagPass;game::lagGameUpdate=game::LagGameUpdate;}}
   if(ready&&(game::Blink().bothWays||game::FakeLag().freezeMode)&&game::InstallReceiveHooks(WriteLog)==1)game::receiveLag=game::LagReceive;}   // incoming packets held too
  // Derp (game_rotation.hpp): submits a rotation request every GameUpdate through the rotation manager.
  {static Module* derp=ModuleById("derp");const bool on=derp&&derp->enabled;game::Rotations().enabled[game::kRotDerp]=on;
   if(on){auto mode=Find(*derp,"mode");game::Derp().mode=!mode||mode->text=="Camera"?0:(mode->text=="Entity"?1:2);auto sp=Find(*derp,"speed");game::Derp().speed=sp?(float)sp->number/100.f:1.f;wantRotations=true;}}
  // Auto 360 (game_rotation.hpp): one eased full turn per activation through the rotation manager.
  {static Module* a3=ModuleById("auto_360");const bool on=a3&&a3->enabled;auto& s=game::Auto360();static bool was=false;
   if(on){auto mode=Find(*a3,"mode");s.mode=!mode||mode->text=="Camera"?0:(mode->text=="Entity"?1:2);auto sp=Find(*a3,"speed");s.speed=sp?(float)sp->number/100.f:0.7f;
    if(!was)s.start=true;wantRotations=true;}
   was=on;game::Rotations().enabled[game::kRotAuto360]=on;}
  // Player modules (game_player.hpp): Fast Use, Item Use Delay Fix, Auto Respawn, Fast Eat, Anti AFK,
  // No Server Rotation, Anti Debuff, Auto Tool.
  {auto& s=game::Players();static Module* fu=ModuleById("fast_use");static Module* iu=ModuleById("item_use_delay_fix");static Module* ar=ModuleById("auto_respawn");static Module* fe=ModuleById("fast_eat");
   static Module* afk=ModuleById("anti_afk");static Module* nr=ModuleById("no_server_rotation");static Module* ad=ModuleById("anti_debuff");static Module* at=ModuleById("auto_tool");
   auto on=[](Module* m){return m&&m->enabled;};bool tick=false;
   {std::lock_guard<std::mutex> lock(s.guard);s.fastUse=on(fu);s.respawn=on(ar);if(s.respawn){if(auto v=Find(*ar,"delay"))s.respawnDelay=(float)v->number;tick=true;}
    s.fastEat=on(fe);if(s.fastEat){if(auto v=Find(*fe,"speed"))s.eatSpeed=(int)v->number;tick=true;}
    s.afk=on(afk);if(s.afk){if(auto v=Find(*afk,"delay"))s.afkDelay=(float)v->number;s.afkJump=Flag(*afk,"jump",true);s.afkSwing=Flag(*afk,"swing",true);s.afkRotate=Flag(*afk,"rotate",false);tick=true;}
    s.noRotation=on(nr);s.debuff=on(ad);if(s.debuff){static const char* names[14]={"Blindness","Nausea","Slowness","Wither","Mining Fatigue","Instant Damage","Darkness","Poison","Fatal Poison","Weakness","Hunger","Levitation","Slow Falling","Bad Omen"};
     static const int ids[14]={15,9,2,20,4,7,30,19,25,18,17,24,27,28};auto list=Find(*ad,"effects");const auto picked=SplitList(list?list->text:std::string());s.effects.clear();for(int i=0;i<14;++i)if(std::find(picked.begin(),picked.end(),names[i])!=picked.end())s.effects.insert(ids[i]);}
    s.tool=on(at);if(s.tool){if(auto v=Find(*at,"minimum_destroy_progress"))s.toolMin=(float)v->number/100.f;s.toolBack=Flag(*at,"switch_back",true);
     s.profiles.clear();for(auto& p:itemProfiles)if(p.customSpeed)s.profiles.push_back({p.name,p.items,p.blocks,p.speed});tick=true;}else if(s.wasTool)tick=true;}
   game::Rotations().enabled[game::kRotAntiAfk]=on(afk);if(on(afk)&&Flag(*afk,"rotate",false))wantRotations=true;if(on(afk)&&Flag(*afk,"jump",true))wantRotations=true;
   if(on(fu))game::InstallFastUseHook(WriteLog);{static bool was=false;const bool want=on(iu);if(want!=was){if(game::PatchUseDelay(want,WriteLog)==1)was=want;}}
   if(on(nr)||on(ad)){game::playerReceive=PlayerReceives;if(game::InstallEvents(WriteLog)==1&&game::InstallReceiveHooks(WriteLog)==1)game::receiveUtility=game::UtilityReceive;}
   if(on(at)){game::InstallBlockActionHooks(WriteLog);game::autoToolDestroy=game::AutoToolDestroy;}
   if(tick||on(at)){game::InstallMapHooks(WriteLog);game::utilityTick=game::UtilityTick;}}
  // Auto Totem / Auto Armor (game_player.hpp): per-frame work through the inventory scheduler.
  {auto& m=game::InvMods();static Module* tt=ModuleById("auto_totem");static Module* aa=ModuleById("auto_armor");const bool t=tt&&tt->enabled,a=aa&&aa->enabled;
   {std::lock_guard<std::mutex> lock(m.guard);m.totem=t;m.armor=a;if(t){if(auto v=Find(*tt,"delay"))m.totemDelay=(int)v->number;m.spoof=Flag(*tt,"spoof_inventory",true);}
    if(a){if(auto v=Find(*aa,"delay")){m.armorLow=(float)v->number;m.armorHigh=(float)atof(v->text.c_str());if(m.armorHigh<m.armorLow)m.armorHigh=m.armorLow;}auto md=Find(*aa,"mode");m.armorMode=md&&md->text=="Always"?1:0;}}
   if(t||a){game::inventoryModulesFrame=PlayerInventoryFrames;if(game::InstallScreenHook(WriteLog)==1){game::inventoryScreen=game::InventoryFrame;game::screenUtility=game::ActionGameUpdate;game::actionReceive=game::ActionReceive;}
    game::inventoryReceive=game::InventoryReceive;if(game::InstallEvents(WriteLog)==1&&game::InstallReceiveHooks(WriteLog)==1)game::receiveUtility=game::UtilityReceive;}}
  // Player group B (game_player_b.hpp): Chest Stealer, Chest Filler, Inventory Manager, Auto Refill.
  {auto& s=game::PlayerB();static Module* cs=ModuleById("chest_stealer");static Module* cf=ModuleById("chest_filler");static Module* im=ModuleById("inventory_manager");static Module* ar=ModuleById("auto_refill");
   auto on=[](Module* x){return x&&x->enabled;};
   auto range=[](Module* x,const char* id,float* lo,float* hi){if(auto v=Find(*x,id)){*lo=(float)v->number;*hi=v->kind=="range"?(float)atof(v->text.c_str()):*lo;if(*hi<*lo)*hi=*lo;}};
   auto list=[](Module* x,const char* id){auto v=Find(*x,id);const auto items=SplitList(v?v->text:std::string());return std::set<std::string>(items.begin(),items.end());};
   auto num=[](Module* x,const char* id,int d){auto v=Find(*x,id);return v?(int)v->number:d;};
   auto text=[](Module* x,const char* id){auto v=Find(*x,id);return v?v->text:std::string();};
   {std::lock_guard<std::recursive_mutex> lock(s.guard);
    if(on(cs)){auto& k=s.stealCfg;range(cs,"delay",&k.delayLo,&k.delayHi);range(cs,"start_delay",&k.startLo,&k.startHi);k.order=text(cs,"order")=="Slot"?1:0;
     k.autoClose=Flag(*cs,"auto_close",true);k.skipMenus=Flag(*cs,"skip_menus",true);k.onlyUseful=Flag(*cs,"only_useful",false);k.ignored=list(cs,"ignored_items");
     k.utility=Flag(*cs,"utility_containers",false);k.mount=Flag(*cs,"mount_inventories",false);}
    if(on(cf)){auto& k=s.fillCfg;range(cf,"delay",&k.delayLo,&k.delayHi);range(cf,"start_delay",&k.startLo,&k.startHi);k.order=text(cf,"order")=="Slot"?1:0;
     k.autoClose=Flag(*cf,"auto_close",true);k.skipMenus=Flag(*cf,"skip_menus",true);k.keepHotbar=Flag(*cf,"keep_hotbar",true);
     const std::string md=text(cf,"mode");k.mode=md=="Except Kept"?1:(md=="Only Dumped"?2:0);k.kept=list(cf,"kept_items");k.dumped=list(cf,"dumped_items");k.utility=Flag(*cf,"utility_containers",false);}
    if(on(im)){auto& k=s.managerCfg;k.mode=text(im,"mode")=="Always"?1:0;k.delay=num(im,"delay",100);k.sword=num(im,"preferred_sword_slot",1);k.pick=num(im,"preferred_pickaxe_slot",2);
     k.axe=num(im,"preferred_axe_slot",3);range(im,"preferred_block_slots",&k.blockLo,&k.blockHi);k.drop=list(im,"droppable_items");k.food=Flag(*im,"drop_food",false);k.useless=Flag(*im,"drop_useless_items",false);}
    if(on(ar)){auto& k=s.refillCfg;k.items=list(ar,"items");k.mode=text(ar,"mode")=="Always"?1:0;k.close=Flag(*ar,"close_inventory",false);range(ar,"delay",&k.delayLo,&k.delayHi);range(ar,"slots",&k.slotsLo,&k.slotsHi);}}
   game::PlayerBEnable(on(cs),on(cf),on(im),on(ar));
   if(on(cs)||on(cf)||on(im)||on(ar)){game::inventoryModulesFrame=PlayerInventoryFrames;if(game::InstallScreenHook(WriteLog)==1)game::inventoryScreen=game::InventoryFrame;
    game::inventoryReceive=game::InventoryReceive;game::playerReceive=PlayerReceives;if(game::InstallEvents(WriteLog)==1&&game::InstallReceiveHooks(WriteLog)==1)game::receiveUtility=game::UtilityReceive;}
   if((on(cs)||on(cf))&&game::InstallSendHook(WriteLog)==1)game::sendPlayer=game::PlayerBSend;}
  // The client level tick always runs UtilityTick: it also keeps the item registry used by item descriptors on servers
  // (game_actions.hpp, DescriptorBuild); every subscriber inside tests its own module flag.
  if(game::InstallMapHooks(WriteLog)==1)game::utilityTick=game::UtilityTick;
  game::RakNetFallback(connectionOptions[0].value,WriteLog);game::ForceRakNet(connectionOptions[1].value,WriteLog);   // Settings > Connection
  game::InstallTransportDetection(WriteLog);game::PollTransport(WriteLog);game::screenNameLog=WriteLog;game::InstallScreenHook(WriteLog);MenuFollowScreens(gameWindowForKeys);KeepSoonModulesOff();filedialog::Pump();   // current server transport (RakNet / NetherNet), for transport-specific module code
  // Movement (game_movement2.hpp): No Jump Delay, High Jump (jump hook #16), Anti Immobile (tick), No Fall (send).
  {auto& m=game::Move2();static Module* nj=ModuleById("no_jump_delay");static Module* hj=ModuleById("high_jump");static Module* ai=ModuleById("anti_immobile");static Module* nf=ModuleById("no_fall");
   auto on=[](Module* x){return x&&x->enabled;};m.noJumpDelay=on(nj);m.highJump=on(hj);if(m.highJump)if(auto v=Find(*hj,"strength"))m.strength=(float)v->number;m.antiImmobile=on(ai);m.noFall=on(nf);if(m.noFall){m.fallingOnly=Flag(*nf,"falling_only",false);auto md=Find(*nf,"mode");m.noFallMode=md&&md->text=="Packet"?1:0;}
   if(on(nj)||on(hj)){if(game::InstallFreeCameraHooks(WriteLog)==1){game::jumpPre=JumpPreAll;game::jumpPost=JumpPostAll;}}
   if(on(ai)||(on(nf)&&m.noFallMode==0)){game::InstallMapHooks(WriteLog);game::utilityTick=game::UtilityTick;}
   if(on(nf)&&game::InstallSendHook(WriteLog)==1)game::sendMovement=game::MoveASendAll;}
  // Movement (game_movement3.hpp): Step, Spider, Flight, Bunny Hop (tick), Velocity (receive / send).
  {auto& m=game::Move3();static Module* st=ModuleById("step");static Module* sp=ModuleById("spider");static Module* fl=ModuleById("flight");static Module* bh=ModuleById("bunny_hop");static Module* ve=ModuleById("velocity");
   auto on=[](Module* x){return x&&x->enabled;};auto num2=[](Module* x,const char* id,float d){auto v=Find(*x,id);return v?(float)v->number:d;};bool tick=false;
   {std::lock_guard<std::mutex> lock(m.guard);m.step=on(st);if(m.step){m.stepHeight=num2(st,"height",1);tick=true;}if(m.wasStep)tick=true;m.spider=on(sp);if(m.spider){m.spiderSpeed=num2(sp,"speed",.2f);tick=true;}
    m.flight=on(fl);if(m.flight||m.wasFlight){tick=true;}if(m.flight){auto md=Find(*fl,"mode");m.flightMode=md&&md->text=="Motion"?1:0;m.flightV=num2(fl,"vertical_speed",.2f);m.flightH=num2(fl,"horizontal_speed",.05f);}
    m.bunny=on(bh);if(m.bunny){m.bunnyModify=Flag(*bh,"modify_velocity",true);m.bunnySpeed=num2(bh,"speed",.28f);m.bunnyHold=Flag(*bh,"hold_jump",false);m.bunnyMoving=Flag(*bh,"moving_only",false);tick=true;}
    m.velocity=on(ve);if(m.velocity){auto md=Find(*ve,"mode");m.velMode=md&&md->text=="Drop"?2:(md&&md->text=="Anchor"?1:0);m.anchorDelay=(int)num2(ve,"anchor_delay_ticks",5);m.anchorAmount=num2(ve,"anchor_amount",.1f);
     m.velH=num2(ve,"horizontal_multiplier",100);m.velV=num2(ve,"vertical_multiplier",100);m.chance=Flag(*ve,"chance",false);m.chanceAmount=num2(ve,"chance_amount",100);tick=true;}}
   if(on(bh)){wantRotations=true;}
   if(tick){game::InstallMapHooks(WriteLog);game::utilityTick=game::UtilityTick;}
   if(on(ve)){game::velocityReceive=game::Move3Receive;if(game::InstallEvents(WriteLog)==1&&game::InstallReceiveHooks(WriteLog)==1)game::receiveUtility=game::UtilityReceive;if(game::InstallSendHook(WriteLog)==1)game::sendMovement=game::MoveASendAll;}}
  // Movement group A (game_movement_a.hpp): Speed, Auto Sprint, Fast Stop, No Slow Down, Inventory Move.
  {static Module* sp=ModuleById("speed");static Module* as=ModuleById("auto_sprint");static Module* fs=ModuleById("fast_stop");static Module* ns=ModuleById("no_slow_down");static Module* im=ModuleById("inventory_move");
   auto& s=game::MoveA();auto on=[](Module* x){return x&&x->enabled;};
   s.speed=on(sp);if(s.speed){auto md=Find(*sp,"mode");s.speedMode=md&&md->text=="Air"?1:(md&&md->text=="Ground"?2:0);if(auto v=Find(*sp,"speed"))s.speedValue=(float)v->number;}
   game::MoveASetAutoSprint(on(as));if(on(as)){auto md=Find(*as,"mode");s.sprintMode=md&&md->text=="Omni"?1:0;s.fwd=Flag(*as,"forward",true);s.back=Flag(*as,"backwards",true);s.left=Flag(*as,"left",true);s.right=Flag(*as,"right",true);}
   s.fastStop=on(fs);
   s.noSlow=on(ns);if(s.noSlow){s.blocks=Flag(*ns,"blocks",true);s.all=Flag(*ns,"all",false);s.food=Flag(*ns,"food",false);s.charge=Flag(*ns,"chargeables",false);}
   game::MoveASetInventory(on(im));
   if(on(sp))game::InstallSpeedHooks(WriteLog);
   if(on(ns)&&game::InstallItemUseSlowdownHook(WriteLog)!=1)WriteLog("No Slow Down: item use hook unavailable, only Blocks is active");
   if(on(ns)||on(fs)){game::InstallMapHooks(WriteLog);game::utilityTick=game::UtilityTick;}
   if(on(as)||on(im)){if(game::InstallFreeCameraHooks(WriteLog)==1){game::sprintAutoSprint=game::MoveAAutoSprint;game::sprintAutoRestore=game::MoveAAutoSprintRestore;game::sprintInventory=game::InventorySprint;}}
   if(on(im)&&game::InstallSendHook(WriteLog)==1)game::sendMovement=game::MoveASendAll;}
  // Movement group B (game_movement_b.hpp): Air Jump, Long Jump (tick + jump hook), Reverse Step (SprintIntent), Safe Walk, Jesus.
  {auto& m=game::MoveB();static Module* aj=ModuleById("air_jump");static Module* lj=ModuleById("long_jump");static Module* rs=ModuleById("reverse_step");static Module* sw=ModuleById("safe_walk");static Module* je=ModuleById("jesus");static Module* pk=ModuleById("parkour");
   auto on=[](Module* x){return x&&x->enabled;};auto num2=[](Module* x,const char* id,float d){auto v=Find(*x,id);return v?(float)v->number:d;};
   m.log=WriteLog;m.airJump=on(aj);m.longJump=on(lj);m.safeWalk=on(sw);m.jesus=on(je);
   {std::lock_guard<std::mutex> lock(m.guard);
    if(on(lj)){m.ljSpeed=num2(lj,"speed",.6f);m.ljHeight=num2(lj,"height",0);m.ljRetention=num2(lj,"retention",98);m.ljAirTicks=(int)num2(lj,"air_ticks",10);m.ljAutoJump=Flag(*lj,"auto_jump",false);}
    if(on(rs)){m.rsHeight=num2(rs,"height",1);auto md=Find(*rs,"mode");m.rsPosition=md&&md->text=="Position";}}
   {static bool wasReverse=false;const bool now=on(rs);m.reverseStep=now;if(wasReverse&&!now)game::ReverseStepRelease();wasReverse=now;}
   // Level tick (Air Jump, Long Jump, client thread for Jesus). MoveBNeedsTick keeps it while a Long Jump state must still be cleared.
   if(game::MoveBNeedsTick()){game::InstallMapHooks(WriteLog);game::utilityTick=game::UtilityTick;}
   // Jump hook #16 (Long Jump), same wiring as No Jump Delay / High Jump. Auto Jump uses the jump request consumed on SprintIntent.
   if(on(lj)){if(game::InstallFreeCameraHooks(WriteLog)==1){game::jumpPre=JumpPreAll;game::jumpPost=JumpPostAll;game::sprintUtility=game::JumpInput;}}
   // SprintIntent (Reverse Step).
   if(on(rs)){if(game::InstallFreeCameraHooks(WriteLog)==1)game::sprintMovement=game::MovementSprintB;}
   // Parkour (SprintIntent + jump request consumed by JumpInput on the next SprintIntent).
   {static bool wasParkour=false;const bool now=on(pk);if(now){m.pkMinimumDrop=Flag(*pk,"minimum_drop",true);m.pkSprintOnly=Flag(*pk,"sprint_only",false);}m.parkour=now;if(wasParkour&&!now)game::ParkourRelease();wasParkour=now;
    if(now&&game::InstallFreeCameraHooks(WriteLog)==1){game::sprintMovement=game::MovementSprintB;game::sprintUtility=game::JumpInput;}}
   // New game hooks (installed once, refused on unexpected prologue bytes).
   if(on(sw))game::InstallSafeWalkHook(WriteLog);
   if(on(je)){game::InstallMapHooks(WriteLog);game::utilityTick=game::UtilityTick;game::InstallJesusHook(WriteLog);}}
  // Movement group C (game_movement_c.hpp): Glide, Jetpack, Boat Fly, Elytra Fly, Phase, Null Movement.
  {auto& m=game::MoveC();static Module* gl=ModuleById("glide");static Module* jp=ModuleById("jetpack");static Module* bf=ModuleById("boat_fly");static Module* ef=ModuleById("elytra_fly");static Module* ph=ModuleById("phase");static Module* nm=ModuleById("null_movement");
   auto on=[](Module* x){return x&&x->enabled;};auto num3=[](Module* x,const char* id,float d){auto v=Find(*x,id);return v?(float)v->number:d;};auto text3=[](Module* x,const char* id){auto v=Find(*x,id);return v?v->text:std::string();};
   // Glide
   m.glide=on(gl);if(on(gl))m.glideFall=num3(gl,"fall_speed",0.1f);
   // Jetpack: held key (or enabled when no key is bound), as Zoom
   {bool held=false;if(jp){auto bound=keycodes.find("jetpack");held=bound!=keycodes.end()?(GetAsyncKeyState(bound->second)&0x8000)!=0&&!opened&&KeysAllowed():jp->enabled;}
    m.jetpack=held;if(jp)m.jetSpeed=num3(jp,"speed",0.5f);static Module* lift[9]={ModuleById("flight"),ModuleById("air_jump"),ModuleById("high_jump"),ModuleById("long_jump"),ModuleById("spider"),ModuleById("glide"),ModuleById("elytra_fly"),ModuleById("boat_fly"),ModuleById("look_tp")};
    bool flying=held;for(Module* x:lift)flying|=x&&x->enabled;game::Move2().flyAssist=flying;}   // No Fall glides any descent near the ground while a module lifts the player
   // Boat Fly
   m.boat=on(bf);if(on(bf)){m.boatGlide=text3(bf,"mode")=="Glide";m.boatH=num3(bf,"horizontal_speed",0.5f);m.boatV=num3(bf,"vertical_speed",0.3f);m.boatGlideSpeed=num3(bf,"glide_speed",0.03f);m.boatAll=Flag(*bf,"all_rideables",false);}
   // Elytra Fly (Mode Control / Boost / Bounce, Firework None / Auto)
   game::MoveCElytraEdge(on(ef));m.elytra=on(ef);
   if(on(ef)){const std::string md=text3(ef,"mode");m.elytraMode=md=="Boost"?1:(md=="Bounce"?2:0);m.elytraH=num3(ef,"horizontal_speed",1.f);m.elytraV=num3(ef,"vertical_speed",0.5f);m.elytraFall=num3(ef,"fall_speed",0.f);
    m.pitchControl=Flag(*ef,"pitch_control",false);m.antiKick=Flag(*ef,"anti_kick",true);m.boost=num3(ef,"boost_acceleration",0.05f);m.maxSpeed=num3(ef,"max_speed",2.5f);
    m.fireworkAuto=text3(ef,"firework")=="Auto";m.fireworkSpeed=num3(ef,"firework_speed",1.f);m.fireworkDelay=(int)num3(ef,"firework_delay",1000);m.fromInventory=Flag(*ef,"from_inventory",false);m.silent=Flag(*ef,"silent",true);
    m.takeoff=Flag(*ef,"auto_takeoff",true);m.redeploy=Flag(*ef,"auto_redeploy",true);m.redeployDelay=(int)num3(ef,"redeploy_delay",2);}
   // Phase (Mode All / Horizontal / Vertical)
   m.phase=on(ph);if(on(ph)){const std::string md=text3(ph,"mode");m.phaseMode=md=="Horizontal"?1:(md=="Vertical"?2:0);}
   // Null Movement (Mode Last Input / First Input)
   game::MoveCNullEdge(on(nm));m.nullOn=on(nm);if(on(nm)){m.nullMode=text3(nm,"mode")=="First Input"?1:0;m.nullForward=Flag(*nm,"forward",true);m.nullStrafe=Flag(*nm,"strafe",true);}
   // Hooks (each install function attempts once and verifies the game bytes first)
   if(on(gl)||on(nm)){if(game::InstallFreeLookHooks(WriteLog)==1)game::combatGameUpdate=CombatGameUpdates;}
   if(m.jetpack||on(bf)||on(ef)||on(gl)){game::InstallMapHooks(WriteLog);game::utilityTick=game::UtilityTick;}   // (Glide does not need the tick; harmless)
   if(on(bf))game::InstallBoatDismountHook(WriteLog);
   if(on(ph))game::InstallPhaseHook(WriteLog);
   if(on(ef)||on(nm)){if(game::InstallFreeCameraHooks(WriteLog)==1)game::sprintMoveC=game::MoveCSprint;
    if(game::InstallSendHook(WriteLog)==1){game::moveSendC=game::MoveCSend;game::sendMovement=game::MoveASendAll;}}
   if(on(ef)&&game::InstallScreenHook(WriteLog)==1)game::combatScreen=CombatScreens;
   if(on(ef)&&game::InstallEvents(WriteLog)==1&&game::InstallReceiveHooks(WriteLog)==1)game::receiveUtility=game::UtilityReceive;}   // inventory scheduler needs the ContainerOpen answer (as Replenish)
  // Path Finder and Pearl Chaser (game_movement_e.hpp).
  {static Module* pf=ModuleById("path_finder");auto& s=game::PathFinder();const bool on=pf&&pf->enabled;s.enabled=on;
   if(on){auto num=[&](const char* id,float d){auto st=Find(*pf,id);return st?(float)st->number:d;};
    auto list=[&](const char* id){auto st=Find(*pf,id);return st?SplitList(st->text):std::vector<std::string>{};};
    auto col=[&](const char* id,float* out,ImVec4 d){ImVec4 v=d;auto st=Find(*pf,id);if(st&&st->kind=="color"&&st->text.size()==9)v=ParseColor(st->text);out[0]=v.x;out[1]=v.y;out[2]=v.z;out[3]=v.w;};
    std::lock_guard<std::mutex> lock(s.guard);auto& c=s.cfg;
    c.allowBreak=Flag(*pf,"allow_break",true);c.allowPlace=Flag(*pf,"allow_place",true);c.allowParkour=Flag(*pf,"allow_parkour",true);c.allowSprint=Flag(*pf,"allow_sprint",true);
    c.avoidMobs=Flag(*pf,"avoid_mobs",true);c.pauseOnInput=Flag(*pf,"pause_on_input",true);c.maxFall=(int)num("max_fall",3);c.bucketClutch=Flag(*pf,"bucket_clutch",true);
    c.maxClutchFall=(int)num("max_clutch_fall",40);c.throwaway=list("throwaway_blocks");c.protectedBlocks=list("protected_blocks");
    {auto r=Find(*pf,"rotations");c.silentRotations=r&&r->text=="Silent";}
    c.smoothing=num("smoothing",50);c.turnSpeed=num("turn_speed",45);c.searchTime=(int)num("search_time",2000);c.searchGreed=num("search_greed",1.25f);c.cacheRadius=(int)num("cache_radius",8);
    c.mineBlocks=list("mine_blocks");c.fastMine=Flag(*pf,"fast_mine",false);c.junkItems=list("junk_items");
    {auto r=Find(*pf,"junk_mode");c.junkMode=r&&r->text=="Always"?1:(r&&r->text=="Off"?2:0);}
    c.stopWhenFull=Flag(*pf,"stop_when_full",true);c.saveTools=Flag(*pf,"save_tools",true);c.autoEat=Flag(*pf,"auto_eat",true);c.wanderRadius=(int)num("wander_radius",96);c.followDistance=num("follow_distance",3);
    c.renderPath=Flag(*pf,"render_path",true);c.showThinking=Flag(*pf,"show_thinking",true);c.throughWalls=Flag(*pf,"through_walls",true);
    {static Module* wpm=ModuleById("waypoints");c.waypoints.clear();if(wpm)if(auto st=Find(*wpm,"entries")){const std::string& all=st->text;size_t from=0;
     while(from<all.size()){size_t to=all.find(',',from);if(to==std::string::npos)to=all.size();if(to>from)c.waypoints.push_back(all.substr(from,to-from));from=to+1;}}}
    col("path_color",c.pathColor,ImVec4(.4f,.8f,1,1));col("thinking_color",c.thinkingColor,ImVec4(1,.8f,.3f,.6f));col("goal_color",c.goalColor,ImVec4(.4f,1,.502f,1));}
   static bool was=false;
   if(on){was=true;wantActions=true;wantRotations=true;game::pfSay=[](const std::string& t){game::ScSay(t);};
    game::InstallMapHooks(WriteLog);game::utilityTick=game::UtilityTick;
    if(game::InstallFreeCameraHooks(WriteLog)==1){game::sprintPathFinder=game::PfSprint;game::sprintUtility=game::JumpInput;}
    game::InstallScreenHook(WriteLog);game::InstallWorldHook(WriteLog);}
   else if(was){was=false;}}
  {static Module* pc=ModuleById("pearl_chaser");auto& s=game::PearlChaser();const bool on=pc&&pc->enabled;s.enabled=on;
   if(on){auto num=[&](const char* id,float d){auto st=Find(*pc,id);return st?(float)st->number:d;};std::lock_guard<std::mutex> lock(s.guard);
    s.maxDistance=num("max_distance",16);s.tolerance=num("tolerance",1.5f);s.reactionDelay=(int)num("reaction_delay",0);s.silentAim=Flag(*pc,"silent_aim",true);s.silentSwitch=Flag(*pc,"silent_switch",false);s.teammates=Flag(*pc,"teammates",false);}
   if(on){wantActions=true;wantRotations=true;game::InstallMapHooks(WriteLog);game::utilityTick=game::UtilityTick;
    if(game::InstallEvents(WriteLog)==1&&game::InstallReceiveHooks(WriteLog)==1){game::receiveUtility=game::UtilityReceive;game::pearlChaserReceive=game::PearlChaserReceive;}}}
  // Schematics (game_schematic.hpp): the .schem command is always available in a world; settings come from the
  // SCHEMATICS section; placement uses the held right click.
  {game::actionLog=WriteLog;auto& sc=game::Schematics();{std::lock_guard<std::recursive_mutex> lock(sc.guard);sc.hidePlaced=schematicOptions[0].value;sc.outline=schematicOptions[1].value;sc.info=schematicOptions[2].value;sc.autoPlace=schematicOptions[3].value;sc.ghost=schematicOptions[4].value;
    sc.range=schematicRange;sc.delay=schematicDelay;sc.perTick=schematicPerTick;}
   sc.rightHeld=!opened&&KeysAllowed()&&(GetAsyncKeyState(VK_RBUTTON)&0x8000)!=0;
   {game::Context k{};if(game::Local(&k)){static bool wired=false;if(!wired&&game::InstallSendHook(WriteLog)==1&&game::InstallMapHooks(WriteLog)==1){wired=true;game::scriptSend=ScriptsSend;game::sendUtility=game::UtilitySend;game::utilityTick=game::UtilityTick;
      game::schematicSetBlock=game::SchematicSetBlock;game::schematicPrep=game::ScRenderGuarded;if(game::InstallBlockActionHooks(WriteLog)==1)game::buildCancel=BuildCancels;game::InstallWorldHook(WriteLog);}}}}
  // Base Finder (game_base_finder.hpp): level tick and changed sub-chunks.
  {static Module* bf=ModuleById("base_finder");auto& s=game::BaseFinder();const bool on=bf&&bf->enabled;
   {std::lock_guard<std::mutex> lock(s.guard);auto& c=s.cfg;c.enabled=on;
    if(on){static const char* names[12]={"Storage","Amethyst","Light Sources","Utility Blocks","Building Blocks","Farms And Redstone","Valuables","Decoration","Portals And Obsidian","Underground","Out Of Place","Custom"};
     auto sig=Find(*bf,"signals");const auto picked=SplitList(sig?sig->text:std::string());for(int i=0;i<12;++i)c.signals[i]=std::find(picked.begin(),picked.end(),names[i])!=picked.end();
     {auto list=Find(*bf,"custom_blocks");c.custom.clear();if(list)for(auto& e:SplitList(list->text))if(!e.empty())c.custom.push_back(e.find(':')==std::string::npos?"minecraft:"+e:e);}
     if(auto v=Find(*bf,"base_score"))c.baseScore=(int)v->number;if(auto v=Find(*bf,"stash_storage"))c.stash=(int)v->number;c.skipNatural=Flag(*bf,"skip_natural_structures",true);
     c.notify=Flag(*bf,"notify",true);c.autoWaypoint=Flag(*bf,"auto_waypoint",false);c.chunkAges=Flag(*bf,"chunk_ages",false);c.trails=Flag(*bf,"trails",true);if(auto v=Find(*bf,"trail_length"))c.trailLength=(int)v->number;}}
   if(on){game::InstallMapHooks(WriteLog);game::utilityTick=game::UtilityTick;game::baseSetBlock=game::BaseFinderSetBlock;
    // Auto Waypoint: appends a "Name x y z dimension" entry to the Waypoints module.
    std::vector<std::array<int,4>> req;std::vector<std::string> names;{std::lock_guard<std::mutex> lock(s.guard);req.swap(s.waypointRequests);names.swap(s.waypointNames);}
    if(!req.empty()){static Module* wp=ModuleById("waypoints");if(wp)for(auto& entrySetting:wp->settings)if(entrySetting.id=="entries"){auto* entries=&entrySetting;static const char* dims[3]={"overworld","nether","end"};
      for(size_t i=0;i<req.size()&&i<names.size();++i){std::string line=names[i];for(auto& ch:line)if(ch==',')ch=' ';line+=" "+std::to_string(req[i][0])+" "+std::to_string(req[i][1])+" "+std::to_string(req[i][2])+" "+dims[std::clamp(req[i][3],0,2)];
       if(!entries->text.empty())entries->text+=",";entries->text+=line;}Save(*wp,entries);Save(*wp);}}}}
  // Death Pos: when the local player's health drops to 0, a "Death dd/mm hh:mm x y z dimension" entry is added to the
  // Waypoints module (managed in its waypoint manager; Waypoints is turned on so the marker shows). Only the last
  // "Keep Last" death entries are kept.
  {static Module* dp=ModuleById("death_pos");static bool wasDead=true;game::Context c{};float h=1;
   const bool dead=dp&&dp->enabled&&game::Local(&c)&&game::PlayerHealth(c,&h)&&h<=0;
   if(dead&&!wasDead){float f[3];int dim=0;static Module* wp=ModuleById("waypoints");
    if(wp&&game::ReadVisualFeet(c,f)){game::ReadDimension(&dim);static const char* dims[3]={"overworld","nether","end"};SYSTEMTIME t{};GetLocalTime(&t);char name[32];wsprintfA(name,"Death %02d/%02d %02d:%02d",t.wDay,t.wMonth,t.wHour,t.wMinute);
     const int x=(int)std::floor(f[0]),y=(int)std::floor(f[1]),z=(int)std::floor(f[2]);
     Setting* entries=nullptr;for(auto& st:wp->settings)if(st.id=="entries")entries=&st;
     if(entries){std::vector<std::string> kept,deaths;for(auto& item:SplitList(entries->text))(item.rfind("Death ",0)==0?deaths:kept).push_back(item);
      deaths.push_back(std::string(name)+" "+std::to_string(x)+" "+std::to_string(y)+" "+std::to_string(z)+" "+dims[std::clamp(dim,0,2)]+" #ff4d4d");
      auto keep=Find(*dp,"keep_last");const size_t limit=(size_t)std::max(1.0,keep?keep->number:3.0);if(deaths.size()>limit)deaths.erase(deaths.begin(),deaths.end()-(ptrdiff_t)limit);
      std::string all;for(auto& item:kept){if(!all.empty())all+=",";all+=item;}for(auto& item:deaths){if(!all.empty())all+=",";all+=item;}entries->text=all;
      if(!wp->enabled)wp->enabled=true;Save(*wp,entries);Save(*wp);
      if(Flag(*dp,"notify",true)&&notices.size()<16)notices.push_back({5,"Death Pos",std::to_string(x)+" "+std::to_string(y)+" "+std::to_string(z)});}}}
   wasDead=dead||!dp||!dp->enabled;}
  // Bed Breaker (game_bed_breaker.hpp): level tick, changed sub-chunks, action queue and rotations.
  {static Module* bb=ModuleById("bed_breaker");auto& s=game::BedBreaker();const bool on=bb&&bb->enabled;game::Rotations().enabled[game::kRotBedBreaker]=on;
   {std::lock_guard<std::mutex> lock(s.guard);auto& c=s.cfg;c.enabled=on;
    if(on){if(auto v=Find(*bb,"range"))c.range=(float)v->number;c.beds=Flag(*bb,"beds",true);c.cakes=Flag(*bb,"cakes",false);c.eggs=Flag(*bb,"dragon_eggs",false);c.custom=Flag(*bb,"custom",false);
     {auto list=Find(*bb,"custom_blocks");c.blocks.clear();if(list)for(auto& e:SplitList(list->text))if(!e.empty())c.blocks.push_back(e);}c.through=Flag(*bb,"break_through",true);if(auto v=Find(*bb,"max_layers"))c.maxLayers=(int)v->number;
     {auto o=Find(*bb,"own_bed");const std::string t=o?o->text:"Both";c.ownBed=t=="Color"?1:t=="Spawn"?2:t=="Off"?3:0;}if(auto v=Find(*bb,"spawn_radius"))c.spawnRadius=(int)v->number;
     c.rotate=Flag(*bb,"rotate",true);c.silent=Flag(*bb,"silent_rotations",true);c.throughWalls=Flag(*bb,"through_walls",false);c.ghost=Flag(*bb,"ghost_switch",true);c.swing=Flag(*bb,"swing",true);c.outline=Flag(*bb,"outline",true);
     static Module* sc=ModuleById("scaffold");static Module* nk=ModuleById("nuker");static Module* rg=ModuleById("regen");c.othersOn=(sc&&sc->enabled)||(nk&&nk->enabled)||(rg&&rg->enabled);}}
   if(on){wantActions=true;wantRotations=true;game::InstallMapHooks(WriteLog);game::utilityTick=game::UtilityTick;game::bedSetBlock=game::BedBreakerSetBlock;}}
  // Player batch C (game_player_c.hpp): Pearl Aimbot, Faster Inventory, Auto Fish, Regen.
  {static Module* pa=ModuleById("pearl_aimbot");static Module* fi=ModuleById("faster_inventory");static Module* af=ModuleById("auto_fish");static Module* rg=ModuleById("regen");
   auto on=[](Module* m){return m&&m->enabled;};static bool paWas=false,fiWas=false,afWas=false;bool tick=false,update=false,send=false,receive=false;
   game::playerCLog=WriteLog;
   // Pearl Aimbot: send + receive + level tick (Snap) + GameUpdate (Track); rotation module 25.
   {auto& s=game::PearlAim();const bool o=on(pa);game::Rotations().enabled[game::kRotPearlAimbot]=o;
    if(o){std::lock_guard<std::mutex> lock(s.guard);auto& k=s.cfg;auto md=Find(*pa,"mode");k.mode=md&&md->text=="Track"?1:0;
     if(auto v=Find(*pa,"tracking_duration"))k.duration=(int)v->number;if(auto v=Find(*pa,"smoothing"))k.smoothing=(float)v->number;k.vertical=Flag(*pa,"vertical",true);
     k.closest=Flag(*pa,"closest_point",true);if(auto v=Find(*pa,"multipoint_horizontal"))k.mpH=(int)v->number;if(auto v=Find(*pa,"multipoint_vertical"))k.mpV=(int)v->number;
     k.silent=Flag(*pa,"silent",false);if(auto v=Find(*pa,"max_distance"))k.maxDistance=(float)v->number;}
    if(o!=paWas)game::PearlAimReset();paWas=o;s.enabled=o;if(o){tick=update=send=receive=true;wantRotations=true;}}
   // Faster Inventory: send + receive + GameUpdate (the GameUpdate part keeps running after it is turned off).
   {auto& s=game::FasterInv();const bool o=on(fi);if(o&&!fiWas)game::FasterInvActivate();fiWas=o;s.enabled=o;static bool used=false;if(o)used=true;
    if(o){send=receive=true;}if(used)update=true;}
   // Auto Fish: receive (ActorEvent) + level tick + GameUpdate; action module 25; suffix = catches.
   {auto& s=game::AutoFish();const bool o=on(af);
    if(o){std::lock_guard<std::mutex> lock(s.guard);auto& k=s.cfg;auto range=[&](const char* id,float* low,float* high){if(auto r=Find(*af,id)){*low=(float)r->number;*high=r->kind=="range"?(float)atof(r->text.c_str()):*low;}};
     range("reel_delay",&k.reelLow,&k.reelHigh);k.recast=Flag(*af,"recast",true);range("recast_delay",&k.recastLow,&k.recastHigh);k.motion=Flag(*af,"motion_detection",true);
     k.onGround=Flag(*af,"recast_on_ground",false);if(auto v=Find(*af,"ground_time"))k.groundTime=(int)v->number;k.onEntity=Flag(*af,"recast_on_entity",false);k.swap=Flag(*af,"swap_to_rod",false);}
    if(o&&!afWas)game::AutoFishReset(false);if(!o&&afWas)game::AutoFishReset(true);afWas=o;s.enabled=o;if(o){tick=update=receive=true;wantActions=true;}}
   // Regen: level tick (keeps running after it is turned off for the cleanup) + SubChunkSetBlock + receive (Steal)
   // + action queue (module 26) + rotations (module 26).
   {auto& s=game::Regen();const bool o=on(rg);game::Rotations().enabled[game::kRotRegen]=o;static bool used=false;if(o)used=true;
    {std::lock_guard<std::mutex> lock(s.guard);auto& k=s.cfg;k.enabled=o;
     if(o){if(auto v=Find(*rg,"range"))k.range=(float)v->number;auto tm=Find(*rg,"target_mode");k.targetMode=tm&&tm->text=="Switch"?1:0;k.uncover=Flag(*rg,"uncover",true);
      if(auto v=Find(*rg,"uncover_range"))k.uncoverRange=(int)v->number;auto am=Find(*rg,"absorption_mode");k.absorptionMode=!am||am->text=="Stop"?0:(am->text=="Queue"?1:2);
      if(auto v=Find(*rg,"queue_threshold"))k.queueThreshold=(float)v->number/100.f;if(auto v=Find(*rg,"absorption_cap"))k.absorptionCap=(int)v->number;
      k.rotate=Flag(*rg,"rotate",true);k.silent=Flag(*rg,"silent_rotations",true);k.ghost=Flag(*rg,"ghost_switch",true);k.swing=Flag(*rg,"swing",true);k.outline=Flag(*rg,"outline",true);k.steal=Flag(*rg,"steal",false);
      static Module* sc=ModuleById("scaffold");static Module* nk=ModuleById("nuker");k.othersOn=(sc&&sc->enabled)||(nk&&nk->enabled);
      k.profiles.clear();for(auto& p:itemProfiles)if(p.customSpeed)k.profiles.push_back({p.name,p.items,p.blocks,p.speed});}}
    if(o){receive=true;wantActions=true;wantRotations=true;}if(used){tick=true;game::InstallMapHooks(WriteLog);game::regenSetBlock=game::RegenSetBlock;}}
   if(tick){game::InstallMapHooks(WriteLog);game::utilityTick=game::UtilityTick;game::playerTickC=game::PlayerCTick;}
   if(update&&game::InstallFreeLookHooks(WriteLog)==1)game::combatGameUpdate=CombatGameUpdates;
   if(send&&game::InstallSendHook(WriteLog)==1){game::playerSendC=game::PlayerCSend;game::sendUtility=game::UtilitySend;}
   if(receive){game::playerReceiveC=game::PlayerCReceive;if(game::InstallEvents(WriteLog)==1&&game::InstallReceiveHooks(WriteLog)==1)game::receiveUtility=game::UtilityReceive;}}
  if(wantRotations||scriptWantsRotations||(game::AutoClicker().enabled&&game::AutoClicker().jitter))AttachRotations();
  if(wantActions&&game::InstallScreenHook(WriteLog)==1){game::screenUtility=game::ActionGameUpdate;game::actionReceive=game::ActionReceive;}
  // No Packet: subscriber of the shared packet-send hook.
  {static Module* block=ModuleById("no_packet");auto& n=game::NoPacket();const bool on=block&&block->enabled;AcquireSRWLockExclusive(&n.lock);n.enabled=on;
   if(on){n.filter=Flag(*block,"filter",false);n.ids.clear();if(auto list=Find(*block,"blocked_ids"))for(auto& e:SplitList(list->text)){char* end{};const long v=strtol(e.c_str(),&end,10);if(end&&!*end&&!e.empty())n.ids.push_back((int)v);}}
   ReleaseSRWLockExclusive(&n.lock);if(on&&game::InstallSendHook(WriteLog)==1)game::sendUtility=game::UtilitySend;}
  // Packet Logger, Nick Hider, Auto Sign, Insta Break (game_utility_packets.hpp).
  auto ids=[&](Module& m,const char* id){std::vector<int> out;if(auto list=Find(m,id))for(auto& e:SplitList(list->text)){char* end{};const long v=strtol(e.c_str(),&end,10);if(end&&!*end&&!e.empty())out.push_back((int)v);}return out;};
  bool wantReceive=false;
  {static Module* logger=ModuleById("packet_logger");auto& l=game::PacketLogger();const bool on=logger&&logger->enabled;{std::lock_guard<std::mutex> lock(l.guard);l.enabled=on;
    if(on){l.receiveOnly=Flag(*logger,"receive_only",false);l.sendOnly=Flag(*logger,"send_only",false);l.white=Flag(*logger,"whitelist",false);l.black=Flag(*logger,"blacklist",false);l.whiteIds=ids(*logger,"whitelisted_ids");l.blackIds=ids(*logger,"blacklisted_ids");}}
   if(on){wantReceive=true;if(game::InstallSendHook(WriteLog)==1)game::sendUtility=game::UtilitySend;}}
  {static Module* nick=ModuleById("nick_hider");auto& n=game::NickHider();const bool on=nick&&nick->enabled;{std::lock_guard<std::mutex> lock(n.guard);n.enabled=on;if(on){auto name=Find(*nick,"name");n.name=name?name->text:"";n.scoreboard=Flag(*nick,"scoreboard",true);}}
   if(on){wantReceive=true;if(game::InstallActorHooks(WriteLog)==1)game::tagRename=game::NickHiderTag;}}
  {static Module* sign=ModuleById("auto_sign");auto& a=game::AutoSign();const bool on=sign&&sign->enabled;{std::lock_guard<std::mutex> lock(a.guard);a.enabled=on;if(on){auto text=Find(*sign,"text");a.lines=text?SplitList(text->text):std::vector<std::string>{};if(a.lines.size()>4)a.lines.resize(4);auto side=Find(*sign,"side");a.both=side&&side->text=="Both";}else a.spots.clear();}
   if(on){wantReceive=true;game::InstallBlockActionHooks(WriteLog);}}
  {static Module* insta=ModuleById("insta_break");auto& ib=game::InstaBreak();const bool on=insta&&insta->enabled;{std::lock_guard<std::mutex> lock(ib.guard);ib.enabled=on;
    if(on){auto mode=Find(*insta,"mode");ib.client=!mode||mode->text!="Packet";{auto pk=Find(*insta,"packets");ib.packets=pk?(int)pk->number:10;}ib.haste=Flag(*insta,"haste",true);ib.white=Flag(*insta,"block_whitelist",false);auto list=Find(*insta,"whitelisted_blocks");ib.blocks=list?SplitList(list->text):std::vector<std::string>{};}}
   if(on){game::InstallBlockActionHooks(WriteLog);if(game::InstallSendHook(WriteLog)==1)game::sendUtility=game::UtilitySend;}}
  if(wantActions)wantReceive=true;
  // Auto Clicker (game_combat.hpp): settings are read here, clicks happen in the screen render (ScreenViewRenderEvent).
  {static Module* ac=ModuleById("auto_clicker");auto& s=game::AutoClicker();const bool on=ac&&ac->enabled;bool changed=false;
   {std::lock_guard<std::mutex> lock(s.guard);changed=s.enabled!=on;s.enabled=on;
    if(on){auto pick=[&](const char* id){auto st=Find(*ac,id);return st?st->text:std::string();};
     s.hold=Flag(*ac,"hold",true);s.breakBlocks=Flag(*ac,"break_blocks",true);s.weaponsOnly=Flag(*ac,"weapons_only",false);s.inventory=Flag(*ac,"inventory",false);s.jitter=Flag(*ac,"jitter",false);
     if(auto r=Find(*ac,"clicks_per_second")){s.low=(float)r->number;s.high=r->text.empty()?s.low:(float)atof(r->text.c_str());}if(auto j=Find(*ac,"jitter_strength"))s.jitterStrength=(float)j->number;
     s.mode=pick("mode")=="Packet-Based"?1:0;const std::string rz=pick("randomization");s.randomization=rz=="Extra+"?2:rz=="Extra"?1:0;}
    if(changed)game::AutoClickerReset(s);}
   if(on){static double nextProfiles=0;if(ImGui::GetTime()>=nextProfiles){nextProfiles=ImGui::GetTime()+1;std::vector<std::string> weapons;for(auto& prof:itemProfiles)if(prof.weapon)for(auto& it:prof.items)weapons.push_back(it);
     auto& cs=game::Combat();std::lock_guard<std::mutex> lock(cs.guard);cs.weaponItems=std::move(weapons);}
    if(game::InstallScreenHook(WriteLog)==1){game::combatScreen=CombatScreens;wantRotations=true;}}}
  // Right Clicker (game_combat.hpp): GameUpdate hook #30 plus the item-use delay reader (game function at RVA 0x65c0c40).
  {static Module* rc=ModuleById("right_clicker");auto& s=game::RightClicker();const bool on=rc&&rc->enabled;
   {std::lock_guard<std::mutex> lock(s.guard);if(s.enabled!=on)game::RightClickerReset(s);s.enabled=on;
    if(on){auto pick=[&](const char* id){auto st=Find(*rc,id);return st?st->text:std::string();};
     if(auto d=Find(*rc,"delay"))s.delayMs=(int)d->number;if(auto d=Find(*rc,"start_delay"))s.startDelay=(int)d->number;const std::string rz=pick("randomization");s.randomization=rz=="Extra+"?2:rz=="Extra"?1:0;
     s.onlyHolding=Flag(*rc,"only_when_holding",true);s.blocksOnly=Flag(*rc,"blocks_only",true);s.useWhite=Flag(*rc,"item_whitelist",false);s.useBlack=Flag(*rc,"item_blacklist",false);
     s.white=SplitList(pick("whitelist"));s.black=SplitList(pick("blacklist"));}}
   if(on){if(game::InstallFreeLookHooks(WriteLog)==1)game::combatGameUpdate=CombatGameUpdates;if(game::InstallFastUseHook(WriteLog)==1)game::useDelayHook=game::RightClickerUseDelay;
    if(game::InstallScreenHook(WriteLog)==1)game::combatScreen=CombatScreens;}}
  // Click Assist (game_combat.hpp): GameUpdate hook #30.
  {static Module* ca=ModuleById("click_assist");auto& s=game::ClickAssist();const bool on=ca&&ca->enabled;
   {std::lock_guard<std::mutex> lock(s.guard);if(s.enabled!=on)game::ClickAssistReset(s);s.enabled=on;
    if(on){if(auto v=Find(*ca,"chance"))s.chance=(int)v->number;if(auto v=Find(*ca,"extra_clicks"))s.extra=(int)v->number;
     if(auto r=Find(*ca,"delay")){s.low=(float)r->number;s.high=r->text.empty()?s.low:(float)atof(r->text.c_str());}s.blocks=Flag(*ca,"blocks",false);}}
   if(on){if(game::InstallFreeLookHooks(WriteLog)==1)game::combatGameUpdate=CombatGameUpdates;if(game::InstallScreenHook(WriteLog)==1)game::combatScreen=CombatScreens;}}
  // Teams: the rules shared by every combat target filter (Kill Aura, Trigger Bot, Aim Assist, Hitbox, Piercing...).
  {static Module* teamsM=ModuleById("teams");nickel::visual::TeamRules rules=TeamRulesOf(teamsM);
   auto& t=game::CombatTargetRules();std::lock_guard<std::mutex> lock(t.guard);t.teams=std::move(rules);}
  // Trigger Bot (game_combat.hpp): screen render (ScreenViewRenderEvent); Teams rules filter the target.
  {static Module* tb=ModuleById("trigger_bot");auto& s=game::TriggerBot();const bool on=tb&&tb->enabled;
   {std::lock_guard<std::mutex> lock(s.guard);if(s.enabled!=on)s.scheduled=false;s.enabled=on;
    if(on){s.hold=Flag(*tb,"hold_click",false);s.weaponsOnly=Flag(*tb,"weapons_only",false);s.playersOnly=Flag(*tb,"players_only",true);
     if(auto r=Find(*tb,"attacks_per_second")){s.low=(float)r->number;s.high=r->text.empty()?s.low:(float)atof(r->text.c_str());}}}
   if(on&&game::InstallScreenHook(WriteLog)==1)game::combatScreen=CombatScreens;}
  // Kill Aura (game_combat.hpp): GameUpdate hook #30, packet send (Teleport mode), rotation manager (Silent mode).
  {static Module* ka=ModuleById("kill_aura");auto& s=game::KillAura();const bool on=ka&&ka->enabled;
   {std::lock_guard<std::mutex> lock(s.guard);if(s.enabled!=on){if(on)game::KillAuraActivate(s);else game::RotRelease(game::kRotKillAura);}
    if(on){auto& k=s.cfg;auto pick=[&](const char* id){auto st=Find(*ka,id);return st?st->text:std::string();};auto num=[&](const char* id,float d){auto st=Find(*ka,id);return st?(float)st->number:d;};
     const std::string mode=pick("mode");k.mode=mode=="Teleport"?2:mode=="Silent"?1:0;
     if(auto r=Find(*ka,"attacks_per_second")){k.low=(float)r->number;k.high=r->text.empty()?k.low:(float)atof(r->text.c_str());}
     k.distance=num("distance",3);k.aimRange=num("aim_range",3);k.tpRange=num("teleport_range",8);k.boxSize=num("box_size",1);k.maxAngle=num("max_angle",90);
     k.multi=Flag(*ka,"multi_targets",false);k.mouseDown=Flag(*ka,"require_mouse_down",false);k.weaponsOnly=Flag(*ka,"weapons_only",false);k.playersOnly=Flag(*ka,"players_only",false);}
    s.enabled=on;}
   game::Rotations().enabled[game::kRotKillAura]=on;
   if(on){if(game::InstallFreeLookHooks(WriteLog)==1)game::combatGameUpdate=CombatGameUpdates;if(game::InstallSendHook(WriteLog)==1)game::sendCombat=game::CombatSend;
    if(game::InstallScreenHook(WriteLog)==1)game::combatScreen=CombatScreens;wantRotations=true;}}
  // Discord Presence (game_discord.hpp): a background thread talks to Discord; text is refreshed on GameUpdate #30.
  {static Module* dp=ModuleById("discord_presence");const bool on=dp&&dp->enabled;game::DiscordSetEnabled(on,WriteLog);
   if(on&&game::InstallFreeLookHooks(WriteLog)==1)game::discordGameUpdate=game::DiscordGameUpdate;}
  // Aim Assist (game_aim_assist.hpp): GameUpdate hook #30, packet send (Bypass), rotation manager.
  {static Module* aa=ModuleById("aim_assist");auto& s=game::AimAssist();const bool on=aa&&aa->enabled;
   {std::lock_guard<std::mutex> lock(s.guard);if(s.enabled!=on){if(on)game::AimAssistActivate(s);else game::AimAssistReset(s);}
    if(on){auto& k=s.cfg;auto pick=[&](const char* id){auto st=Find(*aa,id);return st?st->text:std::string();};auto num=[&](const char* id,float d){auto st=Find(*aa,id);return st?(float)st->number:d;};
     const std::string mode=pick("mode");k.mode=mode=="Lock On"?2:mode=="Interpolate"?1:0;
     const std::string tm=pick("target_mode");k.targetMode=tm=="Switch"?1:0;
     const std::string sb=pick("sort_by");k.sortBy=sb=="Aim Angle"?2:sb=="Hurt Time"?1:0;
     k.fov=num("fov",40);k.distanceRange=Flag(*aa,"distance_range",true);k.distance=num("distance",6);k.increasedFov=Flag(*aa,"increased_fov_while_locked",false);
     k.hSpeed=(int)std::lround(num("horizontal_speed",50));k.vSpeed=(int)std::lround(num("vertical_speed",50));k.smoothing=num("smoothing",0.1f);
     k.multiPoint=Flag(*aa,"multi_point",false);k.mpH=(int)std::lround(num("multipoint_horizontal",100));k.mpV=(int)std::lround(num("multipoint_vertical",100));k.predict=(int)std::lround(num("predict",0));
     k.stopAtHitbox=Flag(*aa,"stop_at_hitbox",false);k.holdClick=Flag(*aa,"hold_click",true);k.playersOnly=Flag(*aa,"players_only",true);k.vertical=Flag(*aa,"vertical",false);
     k.weaponsOnly=Flag(*aa,"weapons_only",false);k.throughWalls=Flag(*aa,"through_walls",true);k.silent=Flag(*aa,"silent",false);s.bypass=Flag(*aa,"bypass",false);}
    s.enabled=on;}
   game::Rotations().enabled[game::kRotAimAssist]=on;
   if(on){if(game::InstallFreeLookHooks(WriteLog)==1)game::combatGameUpdate=CombatGameUpdates;if(game::InstallSendHook(WriteLog)==1)game::sendCombat=game::CombatSend;
    if(game::InstallScreenHook(WriteLog)==1)game::combatScreen=CombatScreens;wantRotations=true;}}
  // Auto Anchor (game_auto_anchor.hpp): placed respawn anchors charged with glowstone and detonated (action sequences)
  {static Module* aa=ModuleById("auto_anchor");auto& s=game::AutoAnchor();const bool on=aa&&aa->enabled;
   s.ghost=Flag(*aa,"ghost_use",true);if(auto d=Find(*aa,"switch_delay"))s.switchDelay=(int)std::lround(d->number);
   if(s.enabled!=on){s.enabled=on;s.resetPending=true;}
   if(on){if(game::InstallScreenHook(WriteLog)==1){game::screenUtility=game::ActionGameUpdate;game::actionCombatFrame=CombatActionFrames;game::actionReceive=game::ActionReceive;}
    if(game::InstallBlockActionHooks(WriteLog)==1)game::buildObserve=game::AutoAnchorBuildGuarded;}}
  // Auto Crossbow (game_auto_crossbow.hpp): charged hotbar crossbows fired once on enable, then off (action sequences)
  {static Module* ac=ModuleById("auto_crossbow");static bool was=false;auto& s=game::AutoCrossbow();const bool on=ac&&ac->enabled;
   if(ac){s.silent=Flag(*ac,"silent_use",true);if(auto d=Find(*ac,"delay"))s.delay=(int)std::lround(d->number);
    if(auto d=Find(*ac,"switch_delay"))s.switchDelay=(int)std::lround(d->number);}
   s.enabled=on;if(on&&!was){s.done=false;s.request=true;}if(!on&&was)s.cancel=true;was=on;
   if(on){if(game::InstallScreenHook(WriteLog)==1){game::screenUtility=game::ActionGameUpdate;game::actionCombatFrame=CombatActionFrames;game::actionReceive=game::ActionReceive;}}
   if(s.done.exchange(false)&&ac&&ac->enabled){ac->enabled=false;Save(*ac);was=false;s.enabled=false;s.cancel=true;}}
  // Auto Crystal (game_auto_crystal.hpp): end crystals placed on / broken from bedrock and obsidian while right button held (action sequences)
  {static Module* ay=ModuleById("auto_crystal");auto& s=game::AutoCrystal();const bool on=ay&&ay->enabled;
   if(ay){if(auto d=Find(*ay,"place_delay"))s.placeDelay=(int)std::lround(d->number);if(auto d=Find(*ay,"break_delay"))s.breakDelay=(int)std::lround(d->number);
    s.requireCrystal=Flag(*ay,"require_crystal",false);s.ghost=Flag(*ay,"ghost_switch",true);if(auto d=Find(*ay,"switch_delay"))s.switchDelay=(int)std::lround(d->number);}
   if(s.enabled!=on){s.enabled=on;if(!on)s.resetPending=true;}   // reset only on disable, as the original
   s.rightHeld=on&&!opened&&KeysAllowed()&&(GetAsyncKeyState(VK_RBUTTON)&0x8000)!=0;
   if(on){if(game::InstallScreenHook(WriteLog)==1){game::screenUtility=game::ActionGameUpdate;game::actionCombatFrame=CombatActionFrames;game::actionReceive=game::ActionReceive;}
    if(game::InstallBlockActionHooks(WriteLog)==1)game::buildCancel=BuildCancels;}}
  // Auto Mace (game_auto_mace.hpp): falling player switches to a mace and hits a nearby target
  {static Module* am=ModuleById("auto_mace");static bool was=false;auto& s=game::AutoMace();const bool on=am&&am->enabled;
   if(am){auto num=[&](const char* id,float d){auto f=Find(*am,id);return f?(float)f->number:d;};
    s.attackRange=num("attack_range",3.0f);s.targetRange=num("target_range",8.0f);s.minFall=num("minimum_fall_distance",2.0f);s.cooldown=num("attack_cooldown",1.0f);
    {auto m=Find(*am,"target_mode");s.targetMode=m&&m->text=="Retarget"?1:0;}
    s.targetOnly=Flag(*am,"target_only",false);s.silent=Flag(*am,"silent_rotations",true);s.onlyHeld=Flag(*am,"only_held",false);
    s.swing=Flag(*am,"swing",true);s.playersOnly=Flag(*am,"players_only",true);}
   if(on&&!was)s.enablePending=true;if(!on&&was)s.disablePending=true;   // enable: forget state; disable: put the original slot back (done on the next render)
   was=on;s.enabled=on;game::Rotations().enabled[game::kRotAutoMace]=on;
   if(on){if(game::InstallFreeLookHooks(WriteLog)==1)game::combatCamera=game::AutoMaceCamera;
    if(game::InstallEvents(WriteLog)==1){game::combatCancel=CombatAttackCancel;if(game::InstallReceiveHooks(WriteLog)==1){game::combatReceiveCancel=game::AutoMaceReceive;game::receiveUtility=game::UtilityReceive;}}
    if(game::InstallSendHook(WriteLog)==1)game::sendCombat=game::CombatSend;
    if(game::InstallScreenHook(WriteLog)==1)game::combatScreen=CombatScreens;wantRotations=true;}}
  // Bow Aimbot (game_bow_aimbot.hpp): GameUpdate hook #30 + ScreenViewRender, rotation manager (id 15), indicator drawn by DrawBowIndicator
  {static Module* ba=ModuleById("bow_aimbot");auto& s=game::BowAimbot();const bool on=ba&&ba->enabled;
   {std::lock_guard<std::mutex> lock(s.guard);
    if(on){auto& k=s.cfg;auto pick=[&](const char* id){auto st=Find(*ba,id);return st?st->text:std::string();};auto num=[&](const char* id,float d){auto st=Find(*ba,id);return st?(float)st->number:d;};
     k.targetMode=pick("target_mode")=="Distance"?1:0;k.fov=num("fov",40);k.distance=num("distance",64);k.playersOnly=Flag(*ba,"players_only",true);
     k.silent=Flag(*ba,"silent",false);k.indicator=Flag(*ba,"target_indicator",true);}}
   if(s.enabled!=on){s.enabled=on;game::BowAimbotActivate();}   // same handler for enable and disable: valid flag cleared, timer restarted, request released
   game::Rotations().enabled[game::kRotBowAimbot]=on;
   if(on){if(game::InstallFreeLookHooks(WriteLog)==1)game::combatGameUpdate=CombatGameUpdates;
    if(game::InstallScreenHook(WriteLog)==1)game::combatScreen=CombatScreens;wantRotations=true;}}
  // PvP Bot (game_pvp_bot.hpp): LevelTick (UtilityTick), GameUpdate clicker, SprintIntent input, SetActorMotion (Jump Reset),
  // screen render (Refill Hotbar), action thread (item sequences), rotation manager (id 17).
  {static Module* pb=ModuleById("pvp_bot");static bool was=false;auto& s=game::PvpBot();const bool on=pb&&pb->enabled;
   if(on){game::PvpBotConfig k;auto pick=[&](const char* id){auto st=Find(*pb,id);return st?st->text:std::string();};auto num=[&](const char* id,float d){auto st=Find(*pb,id);return st?(float)st->number:d;};
    k.noDebuff=pick("mode")=="No Debuff";k.lastHit=pick("target_mode")=="Last Hit";k.range=num("target_range",24);k.playersOnly=Flag(*pb,"players_only",true);k.chase=Flag(*pb,"chase",true);
    k.silent=pick("rotations")=="Silent";
    if(auto r=Find(*pb,"turn_speed")){k.turnLow=(float)r->number;k.turnHigh=r->text.empty()?k.turnLow:(float)atof(r->text.c_str());}
    if(auto r=Find(*pb,"cps")){k.cpsLow=(float)r->number;k.cpsHigh=r->text.empty()?k.cpsLow:(float)atof(r->text.c_str());}
    k.jumpReset=Flag(*pb,"jump_reset",true);k.edgeSafety=Flag(*pb,"edge_safety",true);k.comboJump=Flag(*pb,"combo_jump",true);
    k.healThreshold=num("heal_threshold",10);k.potDown=pick("pot_mode")=="Down";k.potSpeed=std::max(0.1f,num("pot_speed",2));k.potComboed=Flag(*pb,"pot_while_comboed",false);
    k.refill=Flag(*pb,"refill_hotbar",true);k.sidePearl=Flag(*pb,"side_pearl",true);k.pearlOut=Flag(*pb,"pearl_out",true);
    k.comboThreshold=(int)std::lround(num("combo_threshold",1));k.pearlCooldownMs=(double)num("pearl_cooldown",15)*1000.0;
    {std::lock_guard<std::mutex> lock(s.guard);s.cfg=k;}}
   if(on&&!was)game::PvpBotActivate();if(!on&&was)game::PvpBotDeactivate();was=on;s.enabled=on;
   game::Rotations().enabled[game::kRotPvpBot]=on;
   if(on){wantRotations=true;game::InstallMapHooks(WriteLog);game::utilityTick=game::UtilityTick;
    AttachRotations();
    if(game::InstallFreeLookHooks(WriteLog)==1)game::combatGameUpdate=CombatGameUpdates;
    if(game::InstallFreeCameraHooks(WriteLog)==1){game::sprintPvpBot=game::PvpBotSprint;game::sprintUtility=game::JumpInput;}
    if(game::InstallScreenHook(WriteLog)==1){game::combatScreen=CombatScreens;game::screenUtility=game::ActionGameUpdate;game::actionCombatFrame=CombatActionFrames;
     game::actionReceive=game::ActionReceive;game::inventoryScreen=game::InventoryFrame;game::inventoryReceive=game::InventoryReceive;}
    if(game::InstallEvents(WriteLog)==1&&game::InstallReceiveHooks(WriteLog)==1){game::receiveCombat=game::CombatReceive;game::receiveUtility=game::UtilityReceive;}}}
  // Crystal Optimizer (game_crystal.hpp): attacked end crystals removed client-side at the next screen render
  {static Module* co=ModuleById("crystal_optimizer");auto& s=game::CrystalOptimizer();const bool on=co&&co->enabled;
   if(s.enabled!=on){s.enabled=on;game::CrystalClear();}
   if(on){if(game::InstallSendHook(WriteLog)==1)game::sendCombat=game::CombatSend;if(game::InstallReceiveHooks(WriteLog)==1)game::receiveCombat=game::CombatReceive;
    if(game::InstallScreenHook(WriteLog)==1)game::combatScreen=CombatScreens;}}
  // Anti Crystal (game_crystal.hpp): sent movement packets lowered by Offset
  {static Module* ac=ModuleById("anti_crystal");auto& s=game::AntiCrystal();const bool on=ac&&ac->enabled;
   if(auto o=Find(*ac,"offset"))s.offset=(float)o->number;s.safe=Flag(*ac,"safe",false);s.enabled=on;
   if(on&&game::InstallSendHook(WriteLog)==1)game::sendCombat=game::CombatSend;}
  // Sticky Aim (game_sticky_aim.hpp): GameUpdate lowers the sensitivity while the crosshair is on an entity
  {static Module* sa=ModuleById("sticky_aim");auto& s=game::StickyAim();const bool on=sa&&sa->enabled;
   if(auto r=Find(*sa,"reduction"))s.reduction=(int)std::lround(r->number);s.targetOnly=Flag(*sa,"target_only",false);
   if(s.enabled!=on){s.enabled=on;s.resetPending=true;}
   if(on&&game::InstallFreeLookHooks(WriteLog)==1){game::combatGameUpdate=CombatGameUpdates;game::InstallEvents(WriteLog);}}
  // Movement group D (game_movement_d.hpp): Knockback Desync, Auto Jump Reset, Look TP, Auto MLG.
  {static Module* kd=ModuleById("knockback_desync");static Module* aj=ModuleById("auto_jump_reset");static Module* lt=ModuleById("look_tp");static Module* ml=ModuleById("auto_mlg");
   auto on=[](Module* x){return x&&x->enabled;};auto num=[](Module* x,const char* id,float d){auto v=Find(*x,id);return v?(float)v->number:d;};
   bool wantUpdate=false,wantPackets=false,wantTick=false;
   // Knockback Desync: packets held in lots, released on GameUpdate (turning it off releases everything)
   {auto& s=game::KnockbackDesync();const bool e=on(kd);
    if(e){s.delay=(int)std::lround(num(kd,"delay",500));s.safe=Flag(*kd,"safe_mode",false);s.chance=Flag(*kd,"chance",false);s.amount=(int)std::lround(num(kd,"chance_amount",100));wantPackets=true;wantUpdate=true;}
    if(s.enabled!=e){s.enabled=e;if(e)s.resetPending=true;else s.releasePending=true;}}
   // Auto Jump Reset: Delay is a range (min in number, max in text, like Auto Armor / Wind Charge)
   {auto& s=game::AutoJumpReset();const bool e=on(aj);
    if(e){if(auto d=Find(*aj,"delay")){s.delayLow=(float)d->number;s.delayHigh=d->kind=="range"?(float)atof(d->text.c_str()):s.delayLow.load();if(s.delayHigh<s.delayLow)s.delayHigh=s.delayLow.load();}
     s.chance=(int)std::lround(num(aj,"chance",100));s.requireTarget=Flag(*aj,"require_target",false);s.secondHit=Flag(*aj,"second_hit_only",false);s.face=Flag(*aj,"face_knockback",false);wantPackets=true;wantUpdate=true;}
    if(s.enabled!=e){s.enabled=e;s.resetPending=true;}}
   // Look TP: one-shot action, runs on the next GameUpdate then turns the module off
   {auto& s=game::LookTp();static bool was=false;const bool e=on(lt);
    if(e)s.range=num(lt,"range",128);
    if(e&&!was)s.request=true;was=e;if(e)wantUpdate=true;
    if(s.done.exchange(false)&&lt&&lt->enabled){lt->enabled=false;Save(*lt);was=false;}}
   // Auto MLG: tick on LevelTick, sequences in the screen render (action planner), aim through the rotation manager
   {auto& s=game::AutoMlg();const bool e=on(ml);
    if(e){auto t=Find(*ml,"trigger");s.trigger=t&&(t->text=="MinDamage"||t->text=="Min Damage")?1:0;s.minDamage=num(ml,"min_damage",6);s.water=Flag(*ml,"water_bucket",true);s.cobweb=Flag(*ml,"cobweb",true);
     s.pickUp=Flag(*ml,"pick_up_water",false);s.pull=Flag(*ml,"pull_from_inventory",false);s.silent=Flag(*ml,"silent",true);s.aim=num(ml,"aim_speed",100);s.highlight=Flag(*ml,"highlight",true);{static Module* nf=ModuleById("no_fall");s.noFall=nf&&nf->enabled;}
     auto col=Find(*ml,"highlight_color");const ImVec4 c=col&&col->kind=="color"&&col->text.size()==9?ParseColor(col->text):ImVec4(.3f,.75f,1.f,.3f);
     {std::lock_guard<std::mutex> lock(s.guard);s.color[0]=c.x;s.color[1]=c.y;s.color[2]=c.z;s.color[3]=c.w;}
     wantTick=true;wantRotations=true;
     if(game::InstallScreenHook(WriteLog)==1){game::screenUtility=game::ActionGameUpdate;game::actionReceive=game::ActionReceive;
      if(s.pull){game::inventoryScreen=game::InventoryFrame;game::inventoryReceive=game::InventoryReceive;}}
     wantReceive=true;}
    if(s.enabled!=e){s.enabled=e;s.resetPending=true;}}
   game::Rotations().enabled[game::kRotMovementD]=on(aj)||on(ml);
   if(on(aj)&&Flag(*aj,"face_knockback",false))wantRotations=true;
   if(wantTick){game::InstallMapHooks(WriteLog);game::utilityTick=game::UtilityTick;}
   if(wantUpdate&&game::InstallFreeLookHooks(WriteLog)==1)game::combatGameUpdate=CombatGameUpdates;
   if(wantPackets&&game::InstallEvents(WriteLog)==1&&game::InstallReceiveHooks(WriteLog)==1)game::receiveMovementD=game::MovementDReceive;}
  // Backtrack (game_backtrack.hpp): incoming movement of the attacked entity delayed (receive hook + replay), indicator box
  {static Module* bt=ModuleById("backtrack");auto& s=game::Backtrack();const bool on=bt&&bt->enabled;
   if(on){auto md=Find(*bt,"mode");s.mode=md&&md->text=="Freeze"?1:0;if(auto d=Find(*bt,"delay"))s.delay=(int)std::lround(d->number);
    s.playersOnly=Flag(*bt,"players_only",true);s.targetRequired=Flag(*bt,"target_required",true);s.positionsOnly=Flag(*bt,"positions_only",false);
    if(auto d=Find(*bt,"cooldown"))s.cooldown=(float)d->number;s.indicator=Flag(*bt,"real_position_indicator",true);
    auto col=Find(*bt,"indicator_color");const ImVec4 c=col&&col->kind=="color"&&col->text.size()==9?ParseColor(col->text):ImVec4(1,.35f,.35f,.25f);
    {std::lock_guard<std::recursive_mutex> lock(s.guard);s.color[0]=c.x;s.color[1]=c.y;s.color[2]=c.z;s.color[3]=c.w;}}
   if(s.enabled!=on){s.enabled=on;s.resetPending=true;}   // enable / disable: records and held packets cleared (held packets released on disable)
   if(on){if(game::InstallFreeLookHooks(WriteLog)==1)game::combatGameUpdate=CombatGameUpdates;
    if(game::InstallSendHook(WriteLog)==1)game::sendBacktrack=game::BacktrackSend;            // edit 3 (outgoing)
    if(game::InstallReceiveHooks(WriteLog)==1)game::receiveBacktrack=game::BacktrackReceive;}}   // edit 2 (incoming)
  // Hitbox and Piercing (game_hitbox.hpp): entity pick hook (RVA 0x3d281b0); Teams rules filter the targets like Trigger Bot.
  {static Module* hb=ModuleById("hitbox");static Module* pr=ModuleById("piercing");auto& s=game::Hitbox();const bool on=hb&&hb->enabled;
   const bool pierce=pr&&pr->enabled;game::Piercing().playersOnly=pr?Flag(*pr,"players_only",true):true;game::Piercing().enabled=pierce;
   {std::lock_guard<std::mutex> lock(s.guard);auto& k=s.cfg;k.enabled=on;
    if(on){auto num=[&](const char* id,float d){auto st=Find(*hb,id);return st?(float)st->number:d;};
     k.width=num("width",.4f);k.height=num("height",.4f);k.playersOnly=Flag(*hb,"players_only",true);k.render=Flag(*hb,"render",false);
     auto color=Find(*hb,"color");const ImVec4 c=color&&color->kind=="color"&&color->text.size()==9?ParseColor(color->text):ImVec4(1,1,1,1);k.color[0]=c.x;k.color[1]=c.y;k.color[2]=c.z;k.color[3]=c.w;}}
   if(on||pierce)game::InstallHitboxHook(WriteLog);
   game::PiercingPatch(pierce,WriteLog);}   // through blocks: the game's block/entity choice is bypassed while Piercing is on
  // Criticals (game_criticals.hpp): packet send listener (crafted or edited PlayerAuthInputPackets).
  {static Module* cr=ModuleById("criticals");auto& s=game::Criticals();const bool on=cr&&cr->enabled;
   {std::lock_guard<std::mutex> lock(s.guard);if(s.enabled!=on){if(on)game::CriticalsActivate(s);else game::CriticalsReset(s);}
    if(on){auto& k=s.cfg;auto st=Find(*cr,"mode");const std::string mode=st?st->text:std::string();k.mode=mode=="Jump"?3:mode=="Sprint"?2:mode=="Levitate"?1:0;
     k.weaponsOnly=Flag(*cr,"weapons_only",false);k.targetRequired=Flag(*cr,"target_required",false);}
    s.enabled=on;}
   if(on&&game::InstallSendHook(WriteLog)==1)game::sendCombat=game::CombatSend;
   if(on&&s.cfg.mode==3){if(game::InstallFreeLookHooks(WriteLog)==1)game::combatGameUpdate=CombatGameUpdates;if(game::InstallFreeCameraHooks(WriteLog)==1)game::sprintUtility=game::JumpInput;}}
  // Reach (Combat): attack reach drawn from the range on enable, after each attack and when the range changes; block reach
  {static Module* rm=ModuleById("reach");auto& s=game::Reach();const bool on=rm&&rm->enabled;
   {std::lock_guard<std::mutex> lock(s.guard);float low=s.low,high=s.high;
    if(auto r=Find(*rm,"reach")){low=(float)r->number;high=r->text.empty()?low:(float)atof(r->text.c_str());}
    s.blockReach=Flag(*rm,"block_reach",false);if(auto d=Find(*rm,"block_reach_distance"))s.blockDistance=(float)d->number;
    const bool changed=low!=s.low||high!=s.high;s.low=low;s.high=high;
    if(on&&(!s.enabled||changed||s.reroll.exchange(false)))game::ReachDraw(s);
    s.enabled=on;game::ReachData().enabled=on?1:0;}
   if(on){game::InstallReachHook(WriteLog);game::InstallPickRangeHook(WriteLog);if(game::InstallEvents(WriteLog)==1)game::combatAttack=game::ReachAfterAttack;}}
  // Hit Select (Combat): attack hook, skips hits while the target is immune
  {static Module* hs=ModuleById("hit_select");auto& s=game::HitSelect();const bool on=hs&&hs->enabled;s.enabled=on;
   if(auto d=Find(*hs,"tick_delay"))s.delay=(int)d->number;
   if(on&&game::InstallEvents(WriteLog)==1)game::combatCancel=CombatAttackCancel;}
  // Misplace (Combat): incoming movement packets pulled towards the player
  {static Module* mp=ModuleById("misplace");auto& s=game::Misplace();const bool on=mp&&mp->enabled;s.enabled=on;s.playersOnly=Flag(*mp,"players_only",true);
   if(auto o=Find(*mp,"offset_factor"))s.offset=(float)o->number;
   if(on&&game::InstallReceiveHooks(WriteLog)==1)game::receiveCombat=game::CombatReceive;}
  if(wantRotations)AttachRotations();   // Combat modules below this point ask for it too
  // Air Place (game_air_place.hpp): level tick; right click is read here on the render thread.
  {static Module* ap=ModuleById("air_place");auto& s=game::AirPlace();const bool on=ap&&ap->enabled;{std::lock_guard<std::mutex> lock(s.guard);s.enabled=on;
    if(on){if(auto r=Find(*ap,"range"))s.range=(float)r->number;if(auto dl=Find(*ap,"delay"))s.delay=(int)dl->number;s.support=Flag(*ap,"prefer_support",true);s.preview=Flag(*ap,"preview",true);}}
   s.rightHeld=on&&!opened&&KeysAllowed()&&(GetAsyncKeyState(VK_RBUTTON)&0x8000)!=0;if(on){game::InstallMapHooks(WriteLog);game::utilityTick=game::UtilityTick;}}
  // Item Physics (game_item_physics.hpp): uses the shared actor-render detour (hook #0).
  {static Module* ip=ModuleById("item_physics");auto& s=game::Physics();const bool on=ip&&ip->enabled;{std::lock_guard<std::mutex> lock(s.guard);if(!on&&s.enabled)s.items.clear();s.enabled=on;
    if(on){if(auto v=Find(*ip,"spin_speed"))s.spin=(float)v->number;if(auto v=Find(*ip,"settle_speed"))s.settle=(float)v->number;s.randomYaw=Flag(*ip,"random_yaw",true);s.blockItems=Flag(*ip,"block_items",true);}}
   if(on&&game::InstallActorHooks(WriteLog)==1){game::actorPhysicsPre=game::PhysicsPre;game::actorPhysicsPost=game::PhysicsPost;}}
  // Strafe (game_movement.hpp): SprintIntent hook plus incoming SetActorMotion.
  {static Module* st=ModuleById("strafe");auto& s=game::Strafe();const bool on=st&&st->enabled;
   {std::lock_guard<std::mutex> lock(s.guard);if(!on&&s.enabled)s.knocked=false;s.enabled=on;if(on){auto mode=Find(*st,"mode"),sm=Find(*st,"speed_mode");s.always=mode&&mode->text=="Always";s.fixed=sm&&sm->text=="Fixed";
     s.stop=Flag(*st,"stop_without_input",false);if(auto a=Find(*st,"acceleration"))s.acceleration=(float)a->number;if(auto sp=Find(*st,"speed"))s.speed=(float)sp->number;if(auto kp=Find(*st,"knockback_pause"))s.pause=(int)kp->number;}}
   if(on&&game::InstallFreeCameraHooks(WriteLog)==1){game::sprintMovement=game::MovementSprintB;game::movementReceive=game::MovementReceive;wantReceive=true;}}
  // Luau scripts: game tick, outgoing commands and packets, incoming events (chat, packets, containers), attacks.
  {bool any=false;{std::lock_guard<std::recursive_mutex> lock(scriptGuard);for(auto& sc:scripts)if(sc->state==1)any=true;}
   if(any){game::InstallMapHooks(WriteLog);game::scriptTick=ScriptsTick;game::utilityTick=game::UtilityTick;
    if(game::InstallSendHook(WriteLog)==1){game::scriptSend=ScriptsSend;game::sendUtility=game::UtilitySend;}
    if(ScriptsWant("chat")||ScriptsWant("packetReceive")||ScriptsWant("containerOpen")||ScriptsWant("containerClose")||ScriptsWant("teleport")){wantReceive=true;game::scriptReceive=ScriptsReceive;}
    if(ScriptsWant("jump")||ScriptsWant("move")){if(game::InstallFreeCameraHooks(WriteLog)==1){game::jumpPre=JumpPreAll;game::jumpPost=JumpPostAll;game::jumpCancel=ScriptsJumpCancel;game::sprintScript=ScriptsMoveGuarded;}}
    if(scriptWantsRotations)wantRotations=true;
    if(ScriptsWant("attack")&&game::InstallEvents(WriteLog)==1)game::scriptAttack=ScriptsAttack;}}
  if(game::Replenish().enabled)wantReceive=true;
  if(wantReceive&&game::InstallEvents(WriteLog)==1&&game::InstallReceiveHooks(WriteLog)==1)game::receiveUtility=game::UtilityReceive;
  const bool lit=bright&&bright->enabled;if(lit){game::InstallLightHooks(WriteLog);float intensity=100;for(auto& setting:bright->settings)if(setting.kind=="number")intensity=(float)setting.number;camera.brightness=intensity/100;
   static double next=0;if(ImGui::GetTime()>=next){next=ImGui::GetTime()+2;int mode=0;InterlockedExchange(&camera.vibrant,game::OptionValue(0x2bf,&mode)&&mode==2?1:0);}}InterlockedExchange(&camera.fullBright,lit?1:0);
  const bool timed=clock&&clock->enabled;if(timed){game::InstallTimeHook(WriteLog);camera.time=num(*clock,"time",0);}InterlockedExchange(&camera.timeChanger,timed?1:0);
  const bool still=bob&&bob->enabled;if(still)game::InstallBobHook(WriteLog);InterlockedExchange(&camera.noBob,still?1:0);
  const bool tinted=hurt&&hurt->enabled;if(tinted){game::InstallHurtHooks(WriteLog);auto setting=Find(*hurt,"color");const ImVec4 color=setting&&setting->kind=="color"&&setting->text.size()==9?ParseColor(setting->text):ImVec4(1,0,0,.6f);camera.hurt[0]=color.x;camera.hurt[1]=color.y;camera.hurt[2]=color.z;camera.hurt[3]=color.w;}InterlockedExchange(&camera.hurtColor,tinted?1:0);
  static bool told[5]{};auto once=[&](int i,bool when,const char* text){if(when&&!told[i]){told[i]=true;if(WriteLog)WriteLog(text);}};
  once(0,camera.fault!=0,"Camera modules: a fault was caught in a game hook; Zoom, Full Bright and Hurt Color are disabled until the game restarts");once(1,camera.fovSeen!=0,"Zoom: first field of view changed");once(2,camera.lightSeen!=0,"Full Bright: first light block changed");once(3,camera.timeSeen!=0,"Time Changer: first time value replaced");once(4,camera.hurtSeen!=0,"Hurt Color: first hurt colour replaced");}
 // ---- Visuals: No Particle, No Render, Optimizations (game_filter.hpp).
 // Particles defaults to Critical Hit + Sharpness.
 {static Module* particle=ModuleById("no_particle");static Module* hidden=ModuleById("no_render");static Module* lighter=ModuleById("optimizations");auto& filter=game::Filter();game::effectHidden=game::EffectHidden;game::actorVerdict=game::ActorVerdict;
  static bool particleDefault=false;if(particle&&!particleDefault){particleDefault=true;wchar_t saved[8]{};for(auto& setting:particle->settings)if(setting.id=="particles"&&setting.text.empty()&&!GetPrivateProfileStringW(L"no_particle",L"particles",L"",saved,8,configPath.c_str()))setting.text="Critical Hit,Sharpness";}
  const bool noParticle=particle&&particle->enabled;if(noParticle){long mask=0;if(auto list=Find(*particle,"particles")){const std::string wanted=","+list->text+",";for(int i=0;i<15;++i)if(wanted.find(std::string(",")+particleOptions[i]+",")!=std::string::npos)mask|=1L<<i;}
   InterlockedExchange(&filter.particleMask,mask);game::InstallParticleHook(WriteLog);if(mask&1)game::InstallEffectHook(WriteLog);}InterlockedExchange(&filter.particles,noParticle?1:0);
  const bool noRender=hidden&&hidden->enabled;if(noRender){auto flag=[&](const char* id,bool fallback){return Flag(*hidden,id,fallback)?1L:0L;};
   InterlockedExchange(&filter.fireOverlay,flag("fire_overlay",true));InterlockedExchange(&filter.entityFire,flag("entity_fire",false));InterlockedExchange(&filter.fog,flag("fog",false));InterlockedExchange(&filter.waterFog,flag("water_fog",false));InterlockedExchange(&filter.lavaFog,flag("lava_fog",false));InterlockedExchange(&filter.snowFog,flag("powder_snow_fog",false));
   InterlockedExchange(&filter.weather,flag("weather",true));InterlockedExchange(&filter.lightning,flag("lightning",false));InterlockedExchange(&filter.mobs,flag("mobs",false));InterlockedExchange(&filter.items,flag("items",false));InterlockedExchange(&filter.stands,flag("armor_stands",false));InterlockedExchange(&filter.foliage,flag("foliage",false));if(filter.foliage)game::InstallFoliageHooks(WriteLog);
   if(filter.fireOverlay||filter.entityFire)game::InstallEffectHook(WriteLog);if(filter.fog||filter.waterFog||filter.lavaFog||filter.snowFog)game::InstallFogHook(WriteLog);if(filter.weather)game::InstallWeatherHook(WriteLog);if(filter.lightning||filter.mobs||filter.items||filter.stands)game::InstallActorHooks(WriteLog);}
  InterlockedExchange(&filter.noRender,noRender?1:0);
  {static Module* antiBot=ModuleById("anti_bot");const bool botFilter=antiBot&&antiBot->enabled;const bool foliageRefresh=noRender&&filter.foliage!=0&&!filter.foliageFault;
   game::ConfigureVisualFilters(botFilter,foliageRefresh,WriteLog);
   if(botFilter||foliageRefresh)game::InstallScreenHook(WriteLog);if(botFilter)game::InstallEvents(WriteLog);}
  const bool optimize=lighter&&lighter->enabled;if(optimize){InterlockedExchange(&filter.farItems,Flag(*lighter,"items",true)?1:0);InterlockedExchange(&filter.farMobs,Flag(*lighter,"mobs",true)?1:0);InterlockedExchange(&filter.farBlocks,Flag(*lighter,"block_entities",true)?1:0);
   filter.itemRadius=num(*lighter,"item_distance",32);filter.mobRadius=num(*lighter,"mob_distance",64);filter.blockRadius=num(*lighter,"block_entity_distance",64);game::InstallScreenHook(WriteLog);if(filter.farItems||filter.farMobs)game::InstallActorHooks(WriteLog);if(filter.farBlocks)game::InstallBlockActorHook(WriteLog);}
  InterlockedExchange(&filter.optimize,optimize?1:0);worldCameraWanted=optimize||motionCameraWanted;
  static bool told[4]{};auto once=[&](int i,bool when,const char* text){if(when&&!told[i]){told[i]=true;if(WriteLog)WriteLog(text);}};
  once(0,filter.fault!=0,"Render filters: a fault was caught in a game hook; fog and block entity filters are disabled until the game restarts");once(1,filter.particleSeen!=0,"No Particle: first particle hidden");once(2,filter.fogSeen!=0,"No Render: first fog cleared");once(3,filter.actorSeen!=0,"Render filters: first actor skipped");}
 // True Sight runs from the guarded level-render-prep hook; disabling it stops further changes.
 {static Module* sight=ModuleById("true_sight");const bool active=sight&&sight->enabled;
  InterlockedExchange(&game::Sight().playersOnly,active&&Flag(*sight,"players_only",true)?1:0);
  if(active&&game::InstallWorldHook(WriteLog)==1)game::levelPrepStart=game::SightGuarded;InterlockedExchange(&game::Sight().enabled,active?1:0);
 }
 // Entity visuals share one read-only snapshot per frame. Invisible actors are detected through
 // ActorDataFlagComponent bit 5. No hook is needed beyond the camera reader.
 game::WorldFrame worldFrame{};worldFrame.tick=GetTickCount64();
 {game::Context context{};if(game::Local(&context))worldFrame.actor=context.actor;}
 {std::lock_guard<std::mutex> lock(game::Shared().guard);worldFrame.worldSerial=game::Shared().worldSerial;}
 game::ReadDimension(&worldFrame.dimension);
 {static Module* esp=ModuleById("esp");static Module* tracers=ModuleById("tracers");static Module* arrows=ModuleById("arrows");static Module* teams=ModuleById("teams");
  const bool useEsp=esp&&esp->enabled,useTracers=tracers&&tracers->enabled,useArrows=arrows&&arrows->enabled,useReach=reachRange&&reachRange->enabled;static Module* bowAim=ModuleById("bow_aimbot");const bool useBowIndicator=bowAim&&bowAim->enabled&&Flag(*bowAim,"target_indicator",true);
  static std::unordered_map<uint64_t,float> arrowAngles;if(!useArrows)arrowAngles.clear();
  if(useEsp||useTracers||useArrows||useReach||useBowIndicator){worldCameraWanted=true;game::InstallScreenHook(WriteLog);auto& view=game::View();
   float m[16],origin[3];uint64_t tick;AcquireSRWLockShared(&view.lock);memcpy(m,view.m,sizeof(m));memcpy(origin,view.origin,sizeof(origin));tick=view.tick;ReleaseSRWLockShared(&view.lock);
   if(tick&&GetTickCount64()-tick<250&&!view.fault){
    const float W=io.DisplaySize.x,H=io.DisplaySize.y;
    auto project=[&](const float* p,ImVec2& out){nickel::visual::Point point{};if(!nickel::visual::project(m,origin,p,W,H,&point))return false;out=ImVec2(point.x,point.y);return true;};
    if(useBowIndicator){game::BowIndicator indicator;game::BowLine lines[16];   // Bow Aimbot target indicator
     if(game::BowIndicatorSnapshot(&indicator)&&GetTickCount64()-indicator.tick<250){const int bowCount=game::BowIndicatorLines(indicator,m,origin,W,H,lines);
      for(int i=0;i<bowCount;++i)d->AddLine(ImVec2(lines[i].x0,lines[i].y0),ImVec2(lines[i].x1,lines[i].y1),lines[i].color,lines[i].thickness);}}
    nickel::visual::TeamRules rules=TeamRulesOf(teams);
    auto tint=[&](Module* item,const game::VisualEntity& entity){auto color=Find(*item,entity.teammate?"teammate_color":"color");return ImGui::ColorConvertFloat4ToU32(color?ParseColor(color->text):ImVec4(1,.541f,.239f,1));};
    std::array<game::VisualEntity,512> entities{};const float espRange=useEsp?num(*esp,"distance",128):0;
    const int count=(useEsp||useTracers||useArrows||useReach)?game::ReadVisualEntities(entities.data(),(int)entities.size(),(useTracers||useArrows||useReach)?FLT_MAX:espRange,&rules):0;
    std::unordered_set<uint64_t> liveArrows;
    for(int i=0;i<count;++i){const auto& entity=entities[i];
     if(useArrows&&(!Flag(*arrows,"players_only",true)||entity.player)){
      const float dx=(entity.minimum[0]+entity.maximum[0])*.5f-origin[0],dy=(entity.minimum[1]+entity.maximum[1])*.5f-origin[1],dz=(entity.minimum[2]+entity.maximum[2])*.5f-origin[2];
      const float x=m[0]*dx+m[4]*dy+m[8]*dz+m[12],z=m[3]*dx+m[7]*dy+m[11]*dz+m[15];
      if(std::isfinite(x)&&std::isfinite(z)&&x*x+z*z>.0001f){
       const float wanted=std::atan2(-z,x);auto result=arrowAngles.emplace(entity.id,wanted);float& angle=result.first->second;
       if(!result.second)angle+=std::remainder(wanted-angle,6.2831853f)*(1-std::exp(-14.f*std::max(0.f,io.DeltaTime)));
       liveArrows.insert(entity.id);const float cs=std::cos(angle),sn=std::sin(angle),radius=num(*arrows,"radius",100),size=num(*arrows,"scale",15);const ImVec2 center(W*.5f+cs*radius,H*.5f+sn*radius);
       ImVec2 path[26];for(int vertex=0;vertex<26;++vertex){const float a=arrowShape[vertex][0]*size,b=arrowShape[vertex][1]*size;path[vertex]=ImVec2(center.x+cs*a-sn*b,center.y+sn*a+cs*b);}d->AddConcavePolyFilled(path,26,tint(arrows,entity));
      }
     }
     if(useTracers&&(!Flag(*tracers,"players_only",true)||entity.player)){float target[3]={(entity.minimum[0]+entity.maximum[0])*.5f,(entity.minimum[1]+entity.maximum[1])*.5f,(entity.minimum[2]+entity.maximum[2])*.5f};ImVec2 to;if(project(target,to))d->AddLine(ImVec2(W*.5f,H*.5f),to,tint(tracers,entity),num(*tracers,"thickness",1));}
     if(!useEsp||!entity.living||entity.distance>espRange||(Flag(*esp,"players_only",true)&&!entity.player)||(!Flag(*esp,"invisibles",false)&&entity.invisible))continue;
     float worldCorners[8][3];ImVec2 edgeFrom[12],edgeTo[12];int edgeCount=0;ImVec2 lo(FLT_MAX,FLT_MAX),hi(-FLT_MAX,-FLT_MAX);
     for(int c=0;c<8;++c){worldCorners[c][0]=c&1?entity.maximum[0]:entity.minimum[0];worldCorners[c][1]=c&2?entity.maximum[1]:entity.minimum[1];worldCorners[c][2]=c&4?entity.maximum[2]:entity.minimum[2];}
     for(int c=0;c<8;++c)for(int axis=1;axis<=4;axis*=2)if(!(c&axis)){
      nickel::visual::Point from{},to{};if(!nickel::visual::project_segment(m,origin,worldCorners[c],worldCorners[c|axis],W,H,&from,&to))continue;
      edgeFrom[edgeCount]=ImVec2(from.x,from.y);edgeTo[edgeCount]=ImVec2(to.x,to.y);++edgeCount;
      lo.x=std::min(lo.x,std::min(from.x,to.x));lo.y=std::min(lo.y,std::min(from.y,to.y));hi.x=std::max(hi.x,std::max(from.x,to.x));hi.y=std::max(hi.y,std::max(from.y,to.y));
     }
     if(!edgeCount||hi.x<0||hi.y<0||lo.x>W||lo.y>H)continue;
     const ImU32 color=tint(esp,entity);const float thickness=num(*esp,"thickness",1.5f);auto style=Find(*esp,"style");std::string mode=style?style->text:"3D";mode.erase(std::remove(mode.begin(),mode.end(),' '),mode.end());
     if(Flag(*esp,"bounding_box",true)){
      if(mode=="3D"){for(int edge=0;edge<edgeCount;++edge)d->AddLine(edgeFrom[edge],edgeTo[edge],color,thickness);}
      else if(mode=="Corners"){const float x=(hi.x-lo.x)*.25f,y=(hi.y-lo.y)*.25f;for(int corner=0;corner<4;++corner){const ImVec2 a(corner&1?hi.x:lo.x,corner&2?hi.y:lo.y);d->AddLine(a,ImVec2(a.x+(corner&1?-x:x),a.y),color,thickness);d->AddLine(a,ImVec2(a.x,a.y+(corner&2?-y:y)),color,thickness);}}
      else{if(mode=="Glass"){ImVec4 fill=ImGui::ColorConvertU32ToFloat4(color);fill.w*=std::clamp(num(*esp,"fill_opacity",0),0.f,1.f);d->AddRectFilled(lo,hi,ImGui::ColorConvertFloat4ToU32(fill));}d->AddRect(lo,hi,color,0,0,thickness);}
     }
     if(Flag(*esp,"health_bar",false)&&std::isfinite(entity.health)&&std::isfinite(entity.maxHealth)&&entity.maxHealth>0){const float ratio=std::clamp(entity.health/entity.maxHealth,0.f,1.f);d->AddRectFilled(ImVec2(lo.x-6,lo.y-1),ImVec2(lo.x-2,hi.y+1),IM_COL32(0,0,0,180));d->AddRectFilled(ImVec2(lo.x-5,hi.y-(hi.y-lo.y)*ratio),ImVec2(lo.x-3,hi.y),IM_COL32(92+(int)(163*(1-ratio)),(int)(220*ratio),62,255));}
    }
    for(auto it=arrowAngles.begin();it!=arrowAngles.end();)if(!liveArrows.count(it->first))it=arrowAngles.erase(it);else ++it;
    if(useReach&&worldFrame.actor){float limit=0;if(game::ReadReachLimit(&limit)){const float offset=num(*reachRange,"offset",.9f);const bool playersOnly=Flag(*reachRange,"players_only",true);
     for(int i=0;i<count;++i){const auto& entity=entities[i];if(!entity.living||(playersOnly&&!entity.player))continue;
      const nickel::visual::Vec3 feet{(entity.minimum[0]+entity.maximum[0])*.5f,entity.minimum[1],(entity.minimum[2]+entity.maximum[2])*.5f};const auto ring=nickel::visual::reach_ring(feet,offset,limit);
      float distance=0;const bool inside=game::Reach(worldFrame.actor,entity.actor,&distance)&&distance<=limit;const float ink[4]={inside?.2f:1.f,inside?1.f:.2f,.2f,1.f};
      for(int p=0;p<48;++p){game::WorldLine segment{};segment.a[0]=ring[p].x;segment.a[1]=ring[p].y;segment.a[2]=ring[p].z;segment.b[0]=ring[p+1].x;segment.b[1]=ring[p+1].y;segment.b[2]=ring[p+1].z;memcpy(segment.colorA,ink,16);memcpy(segment.colorB,ink,16);segment.thickness=2;segment.depth=true;worldFrame.lines.push_back(segment);}}}}
   }
  }
 }
 static Module* nametagsModule=ModuleById("nametags");
 {game::NametagRequest request{};
  if(nametagsModule&&nametagsModule->enabled){
   request.enabled=true;request.useDisplayName=Flag(*nametagsModule,"use_displayname",false);request.background=Flag(*nametagsModule,"show_background",true);request.distance=Flag(*nametagsModule,"distance",false);request.health=Flag(*nametagsModule,"health",true);request.equipment=Flag(*nametagsModule,"equipment",true);request.durability=Flag(*nametagsModule,"durability",true)&&request.equipment;request.hideVanilla=Flag(*nametagsModule,"hide_vanilla",true);request.mobs=Flag(*nametagsModule,"mobs",false);request.scale=num(*nametagsModule,"scale",1);request.width=io.DisplaySize.x;request.height=io.DisplaySize.y;
   worldCameraWanted=true;game::InstallScreenHook(WriteLog);game::InstallEvents(WriteLog);if(request.hideVanilla)game::InstallActorHooks(WriteLog);
   static long seen=0;if(!seen&&game::Nametags().seen){seen=1;if(WriteLog)WriteLog("Nametags: first hud_screen frame handled");}
  }
  game::ConfigureNametags(request,WriteLog);
  static std::unordered_map<uint64_t,float> hurtValues;static uintptr_t hurtActor=0;static uint32_t hurtSerial=0;static int hurtDimension=-1;static bool hurtReady=false;
  auto frame=game::SnapshotNametags();game::Context paintContext{};uint32_t paintSerial=0;int paintDimension=0;bool usable=false;
  if(frame&&game::Local(&paintContext)){ {std::lock_guard<std::mutex> lock(game::Shared().guard);paintSerial=game::Shared().worldSerial;} usable=game::ReadDimension(&paintDimension)&&nickel::visual::nametag_frame_usable(request.enabled,game::Nametags().fault!=0,GetTickCount64(),frame->tick,frame->actor,paintContext.actor,frame->worldSerial,paintSerial,frame->dimension,paintDimension);}
  if(!usable){hurtValues.clear();hurtReady=false;}
  else{
   if(!hurtReady||hurtActor!=paintContext.actor||hurtSerial!=frame->worldSerial||hurtDimension!=frame->dimension){hurtValues.clear();hurtActor=paintContext.actor;hurtSerial=frame->worldSerial;hurtDimension=frame->dimension;hurtReady=true;}
   std::unordered_set<uint64_t> seen;const float dt=(float)io.DeltaTime;
   const float nb[4]={.0470588244497776f,.054901961237192154f,.07058823853731155f,.6980392336845398f},hb[4]={.699999988079071f,.07999999821186066f,.07999999821186066f,.6980392336845398f},nt[4]={.95686274766922f,.9647058844566345f,.9803921580314636f,1.f},ht[4]={1.f,.699999988079071f,.699999988079071f,1.f};
   for(const auto& row:frame->rows){seen.insert(row.id);const float h=nickel::visual::nametag_hurt(hurtValues[row.id],row.hurtTicks,dt);hurtValues[row.id]=h;
    char healthText[16]{},distanceText[16]{};const bool showHealth=request.health&&row.healthKnown&&std::isfinite(row.health);if(showHealth)snprintf(healthText,sizeof(healthText),"%.0f",row.health);
    const bool showDistance=request.distance&&std::isfinite(row.distance);if(showDistance)snprintf(distanceText,sizeof(distanceText),"%.1fm",row.distance);
    const float size=18.f*row.k,nameW=hudWidth(regular,size,row.label.c_str()),healthW=showHealth?hudWidth(regular,size,healthText):0,distanceW=showDistance?hudWidth(regular,size,distanceText):0;
    const auto lay=nickel::visual::nametag_layout(row.anchorX,row.anchorY,row.k,nameW,healthW,distanceW);
    float bg[4],ink[4];for(int c=0;c<4;++c){bg[c]=nb[c]+(hb[c]-nb[c])*h;ink[c]=nt[c]+(ht[c]-nt[c])*h;}
    if(request.background)d->AddRectFilled(ImVec2(lay.left,lay.top),ImVec2(lay.right,lay.bottom),ImGui::ColorConvertFloat4ToU32(ImVec4(bg[0],bg[1],bg[2],bg[3])));
    hudText(regular,size,ImVec2(lay.nameX,lay.textY),row.label.c_str(),ImGui::ColorConvertFloat4ToU32(ImVec4(ink[0],ink[1],ink[2],ink[3])));
    if(showHealth)hudText(regular,size,ImVec2(lay.healthX,lay.textY),healthText,(ImU32)nickel::visual::nametag_health_color(row.health));
    if(showDistance&&distanceW>0)hudText(regular,size,ImVec2(lay.distanceX,lay.textY),distanceText,(ImU32)0xffcabeb8u);
    if(request.equipment&&request.durability)for(const auto& icon:row.icons){if(!(icon.maximum>0)||!(icon.remaining<icon.maximum)||icon.ratioY==0)continue;
     const float bh=std::max(3.f,3.f*row.k*icon.ratioX/icon.ratioY),bl=std::round(icon.x+3.f*row.k),bt=std::round(icon.y+21.f*row.k*icon.ratioX/icon.ratioY),bw=18.f*row.k,inset=std::max(1.f,.2f*bh);
     const float ratio=std::clamp((float)icon.remaining/icon.maximum,0.f,1.f),fg=std::round((bw-2.f*inset)*ratio);
     d->AddRectFilled(ImVec2(bl,bt),ImVec2(bl+bw,bt+bh),IM_COL32(0,0,0,255));
     d->AddRectFilled(ImVec2(bl+inset,bt+inset),ImVec2(bl+inset+fg,bt+bh-inset),(ImU32)nickel::visual::nametag_durability_color(ratio));}}
   for(auto it=hurtValues.begin();it!=hurtValues.end();)if(!seen.count(it->first))it=hurtValues.erase(it);else ++it;
   if(hurtValues.size()>512)hurtValues.clear();}
 }
 // ---- Visuals drawn in the world: Breadcrumbs, Chunk Borders, Waypoints, Damage Indicators.
 // Geometry is projected to the screen through game::View and drawn in 2D over the game, so everything shows
 // through blocks and "Through Walls" has no effect. Line width = Thickness x 0.001 x Proj[1][1] x screen height.
 {static Module* crumbs=ModuleById("breadcrumbs");static Module* borders=ModuleById("chunk_borders");static Module* waypoints=ModuleById("waypoints");static Module* damage=ModuleById("damage_indicators");
  static Module* chestEsp=ModuleById("chest_esp");const bool useChests=chestEsp&&chestEsp->enabled;
  static Module* susFinder=ModuleById("sus_chunk_finder");const bool useSus=susFinder&&susFinder->enabled;
  if(useSus){game::SusConfig c;auto option=Find(*susFinder,"mode");const std::string mode=option?option->text:"Score";c.mode=mode=="Types"?1:mode=="Tunnels"?2:0;c.radius=(int)num(*susFinder,"scan_radius",8);c.score=(int)num(*susFinder,"score_to_flag",3);
   c.minCount=(int)num(*susFinder,"min_count",3);c.tunnelLength=(int)num(*susFinder,"tunnel_length",8);c.storage=Flag(*susFinder,"storage",true);c.placed=Flag(*susFinder,"placed_blocks",true);c.rotated=Flag(*susFinder,"rotated_deepslate",true);
   c.flat=Flag(*susFinder,"flat_rooms",true);c.tunnels=Flag(*susFinder,"tunnels",true);c.skipNatural=Flag(*susFinder,"skip_natural_structures",true);c.notify=Flag(*susFinder,"notify",true);
   if(auto list=Find(*susFinder,"blocks")){size_t from=0;while(from<list->text.size()){size_t to=list->text.find(',',from);if(to==std::string::npos)to=list->text.size();if(to>from)c.types.push_back(list->text.substr(from,to-from));from=to+1;}}
   auto& shared=game::Sus();AcquireSRWLockExclusive(&shared.lock);shared.config=c;ReleaseSRWLockExclusive(&shared.lock);game::InstallMapHooks(WriteLog);
   static bool told=false;if(shared.fault&&!told){told=true;if(WriteLog)WriteLog("Sus Chunk Finder: a fault was caught while scanning chunks; it is disabled until the game restarts");}}
  InterlockedExchange(&game::Sus().enabled,useSus?1:0);
  static Module* itemEspModule=ModuleById("item_esp");const bool useItems=itemEspModule&&itemEspModule->enabled;
  static Module* jumpCircles=ModuleById("jump_circles");const bool useJumps=jumpCircles&&jumpCircles->enabled;
  static Module* airPlaceM=ModuleById("air_place");const bool useAir=airPlaceM&&airPlaceM->enabled&&Flag(*airPlaceM,"preview",true);static Module* bedBreakerM=ModuleById("bed_breaker");const bool useBed=bedBreakerM&&bedBreakerM->enabled&&Flag(*bedBreakerM,"outline",true);static Module* regenM=ModuleById("regen");const bool useRegen=regenM&&regenM->enabled&&Flag(*regenM,"outline",true);static Module* baseFinderM=ModuleById("base_finder");const bool useBase=baseFinderM&&baseFinderM->enabled;const bool useSchem=[]{auto& sc=game::Schematics();std::lock_guard<std::recursive_mutex> l(sc.guard);return !sc.list.empty();}();
  static Module* hitboxM=ModuleById("hitbox");const bool useHitbox=hitboxM&&hitboxM->enabled&&Flag(*hitboxM,"render",false);
  static Module* backtrackM=ModuleById("backtrack");const bool useBacktrackBox=backtrackM&&backtrackM->enabled&&Flag(*backtrackM,"real_position_indicator",true);
  static Module* mlgM=ModuleById("auto_mlg");const bool useMlgBox=mlgM&&mlgM->enabled&&Flag(*mlgM,"highlight",true);static Module* pfm=ModuleById("path_finder");const bool usePf=pfm&&pfm->enabled;
  static Module* soundEsp=ModuleById("sound_esp");const bool useSounds=soundEsp&&soundEsp->enabled;InterlockedExchange(&game::Sounds().enabled,useSounds?1:0);if(useSounds)game::InstallSoundHook(WriteLog);
  static Module* beTracerModule=ModuleById("block_esp");const bool useBeTracers=beTracerModule&&beTracerModule->enabled&&Flag(*beTracerModule,"tracers",false);
  const bool any=(crumbs&&crumbs->enabled)||(borders&&borders->enabled)||(waypoints&&waypoints->enabled)||(damage&&damage->enabled);auto& view=game::View();static Module* subtitlesM=ModuleById("subtitles");const Setting* subMode=subtitlesM?Find(*subtitlesM,"mode"):nullptr;const bool useSubtitlesSky=subtitlesM&&subtitlesM->enabled&&!(subMode&&subMode->text=="HUD");   // Subtitles in Sky / Both mode: text is placed in the sky
  InterlockedExchange(&view.wanted,any||useChests||useSus||useBeTracers||useItems||useJumps||useSounds||useAir||useBed||useRegen||useBase||useSchem||useHitbox||useBacktrackBox||useMlgBox||usePf||worldCameraWanted||scriptWantsCamera||useSubtitlesSky?1:0);
  InterlockedExchange(&game::Chests().range,useChests?(long)num(*chestEsp,"range",128):128);InterlockedExchange(&game::Chests().enabled,useChests?1:0);if(useChests)game::InstallMapHooks(WriteLog);
  {static Module* paths=ModuleById("trajectories");const bool usePaths=paths&&paths->enabled;auto& shared=game::Trajectory();
   if(usePaths){ImVec4 c(.298f,.8f,1,1);{auto setting=Find(*paths,"color");if(setting&&setting->kind=="color"&&setting->text.size()==9)c=ParseColor(setting->text);}AcquireSRWLockExclusive(&shared.lock);shared.color[0]=c.x;shared.color[1]=c.y;shared.color[2]=c.z;shared.color[3]=c.w;shared.thickness=num(*paths,"thickness",1.5f);shared.impact=Flag(*paths,"impact",true);shared.others=Flag(*paths,"other_players",false);ReleaseSRWLockExclusive(&shared.lock);game::InstallWorldHook(WriteLog);}
   InterlockedExchange(&shared.enabled,usePaths?1:0);}
  {static bool told=false;if(useChests&&game::Chests().fault&&!told){told=true;if(WriteLog)WriteLog("Chest ESP: a fault was caught while scanning chunks; Chest ESP is disabled until the game restarts");}}
  const bool useReach=reachRange&&reachRange->enabled,worldDraw=(crumbs&&crumbs->enabled)||(borders&&borders->enabled)||(waypoints&&waypoints->enabled)||useReach||useChests||useItems||useJumps||useSounds||useAir||useBed||useRegen||useBase||useSchem||useHitbox||useBacktrackBox||useMlgBox||usePf;
  if(worldDraw){game::InstallWorldHook(WriteLog);game::InstallEvents(WriteLog);}
  InterlockedExchange(&game::WorldReach().enabled,useReach?1:0);InterlockedExchange(&game::WorldReach().cancel,useReach&&Flag(*reachRange,"cancel_attacks",false)?1:0);game::cancelAttack=game::CancelOutOfReach;
  struct Crumb{float x,y,z;double time;};static std::deque<Crumb> trail;static uintptr_t crumbActor=0;static uint32_t crumbSerial=0;static int crumbDimension=-1;
  if(!crumbs||!crumbs->enabled||!nickel::visual::same_world(crumbActor,worldFrame.actor,crumbSerial,worldFrame.worldSerial,crumbDimension,worldFrame.dimension))trail.clear();
  crumbActor=worldFrame.actor;crumbSerial=worldFrame.worldSerial;crumbDimension=worldFrame.dimension;
  if(any||useChests||useSus||useBeTracers||useItems||useJumps||useSounds||useAir||useBed||useRegen||useBase||useSchem||useHitbox||useBacktrackBox||useMlgBox||usePf||useSubtitlesSky){game::InstallScreenHook(WriteLog);static bool told=false,first=false;if(view.fault&&!told){told=true;if(WriteLog)WriteLog("World drawing: a fault was caught while reading the camera; world drawing is disabled until the game restarts");}if(view.seen&&!first){first=true;if(WriteLog)WriteLog("World drawing: first camera matrices read");}
   float m[16],origin[3];uint64_t tick;AcquireSRWLockShared(&view.lock);memcpy(m,view.m,sizeof(m));memcpy(origin,view.origin,sizeof(origin));tick=view.tick;ReleaseSRWLockShared(&view.lock);
   game::Context selfContext{};float feet[3]{};const bool hasFeet=game::Local(&selfContext)&&game::ReadVisualFeet(selfContext,feet);const bool live=hasFeet&&tick&&GetTickCount64()-tick<250&&!view.fault;const float W=io.DisplaySize.x,H=io.DisplaySize.y;const double now=ImGui::GetTime();
   struct Clip{float x,y,w;};
   auto clip=[&](float wx,float wy,float wz){const float dx=wx-origin[0],dy=wy-origin[1],dz=wz-origin[2];return Clip{m[0]*dx+m[4]*dy+m[8]*dz+m[12],m[1]*dx+m[5]*dy+m[9]*dz+m[13],m[3]*dx+m[7]*dy+m[11]*dz+m[15]};};
   auto toScreen=[&](const Clip& c){return ImVec2(W/2+c.x/c.w*W/2,H/2-c.y/c.w*H/2);};
   auto line=[&](float ax,float ay,float az,float bx,float by,float bz,const std::array<float,4>& color,float thickness,bool depth){game::WorldLine segment{};segment.a[0]=ax;segment.a[1]=ay;segment.a[2]=az;segment.b[0]=bx;segment.b[1]=by;segment.b[2]=bz;memcpy(segment.colorA,color.data(),16);memcpy(segment.colorB,color.data(),16);segment.thickness=thickness;segment.depth=depth;worldFrame.lines.push_back(segment);};
   auto tone=[](ImVec4 color,float alpha){color.w*=std::clamp(alpha,0.f,1.f);return ImGui::ColorConvertFloat4ToU32(color);};auto smooth=[](float t){t=std::clamp(t,0.f,1.f);return t*t*(3-2*t);};
   auto rgba=[](ImVec4 color,float alpha){return std::array<float,4>{color.x,color.y,color.z,color.w*std::clamp(alpha,0.f,1.f)};};
   auto colorOf=[&](Module* item,const char* id,ImVec4 fallback){auto setting=Find(*item,id);return setting&&setting->kind=="color"&&setting->text.size()==9?ParseColor(setting->text):fallback;};
   // Breadcrumbs: a point at feet + 0.05 every Spacing; the trail resets after a jump of more than 8 blocks;
   // at most 4096 points, each kept for Duration.
   {if(!crumbs||!crumbs->enabled||!hasFeet)trail.clear();
    else{const float x=feet[0],y=feet[1]+.05f,z=feet[2],keep=num(*crumbs,"duration",15),spacing=num(*crumbs,"spacing",.25f);while(!trail.empty()&&now-trail.front().time>keep)trail.pop_front();
     float gap=1e9f;if(!trail.empty()){const Crumb& last=trail.back();gap=std::sqrt((x-last.x)*(x-last.x)+(y-last.y)*(y-last.y)+(z-last.z)*(z-last.z));}if(trail.empty()||gap>=spacing){if(!trail.empty()&&gap>8)trail.clear();if(trail.size()>=4096)trail.pop_front();trail.push_back({x,y,z,now});}
     if(live){const auto color=rgba(colorOf(crumbs,"color",ImVec4(.35f,.75f,1,1)),1);const float thickness=num(*crumbs,"thickness",2);const bool depth=!Flag(*crumbs,"through_walls",false);for(size_t i=1;i<trail.size();++i)line(trail[i-1].x,trail[i-1].y,trail[i-1].z,trail[i].x,trail[i].y,trail[i].z,color,thickness,depth);if(!trail.empty())line(trail.back().x,trail.back().y,trail.back().z,x,y,z,color,thickness,depth);}}}
   // Chunk Borders: chunk walls from floor(y)-32 (even) to floor(y)+32; grid every 2 blocks; neighbouring chunk corners.
   if(borders&&borders->enabled&&live){const float cx=std::floor(feet[0]/16)*16,cz=std::floor(feet[2]/16)*16,top=std::floor(feet[1])+32,bottom=std::floor((std::floor(feet[1])-32)/2)*2,thickness=num(*borders,"thickness",1.5f);const bool depth=!Flag(*borders,"through_walls",false);
    auto ring=[&](float y,const std::array<float,4>& color){line(cx,y,cz,cx+16,y,cz,color,thickness,depth);line(cx+16,y,cz,cx+16,y,cz+16,color,thickness,depth);line(cx+16,y,cz+16,cx,y,cz+16,color,thickness,depth);line(cx,y,cz+16,cx,y,cz,color,thickness,depth);};
    if(Flag(*borders,"grid",true)){const auto color=rgba(colorOf(borders,"grid_color",ImVec4(.3f,.6f,1,.7f)),1);for(int i=2;i<=14;i+=2){line(cx+i,bottom,cz,cx+i,top,cz,color,thickness,depth);line(cx+i,bottom,cz+16,cx+i,top,cz+16,color,thickness,depth);line(cx,bottom,cz+i,cx,top,cz+i,color,thickness,depth);line(cx+16,bottom,cz+i,cx+16,top,cz+i,color,thickness,depth);}
     for(float y=bottom;y<=top;y+=2)if(std::fmod(std::fabs(y),16.f)!=0)ring(y,color);}
    if(Flag(*borders,"neighbors",true)){const auto color=rgba(colorOf(borders,"neighbor_color",ImVec4(1,.3f,.3f,1)),1);for(int i=-1;i<=2;++i)for(int j=-1;j<=2;++j){if((i==0||i==1)&&(j==0||j==1))continue;line(cx+16*i,bottom,cz+16*j,cx+16*i,top,cz+16*j,color,thickness,depth);}}
    {const auto color=rgba(colorOf(borders,"chunk_color",ImVec4(1,.85f,.2f,1)),1);const float corners[4][2]={{0,0},{16,0},{16,16},{0,16}};for(auto& corner:corners)line(cx+corner[0],bottom,cz+corner[1],cx+corner[0],top,cz+corner[1],color,thickness,depth);for(float y=std::ceil(bottom/16)*16;y<=top;y+=16)ring(y,color);}}
   // Chest ESP: 6 filled faces, then 12 outline edges if Draw Outline; name_tag material (visible through walls).
   // Hitbox Render: enlarged boxes as 12 depth-tested edges in the Color setting
   if(useHitbox&&live){std::vector<game::WorldLine> boxes;if(game::HitboxLines(origin,boxes))worldFrame.lines.insert(worldFrame.lines.end(),boxes.begin(),boxes.end());}
  if(useBacktrackBox&&live){std::vector<game::WorldLine> ghost;if(game::BacktrackLines(origin,ghost))worldFrame.lines.insert(worldFrame.lines.end(),ghost.begin(),ghost.end());}
  if(useMlgBox&&live){std::vector<game::WorldLine> rescue;if(game::AutoMlgLines(origin,rescue))worldFrame.lines.insert(worldFrame.lines.end(),rescue.begin(),rescue.end());}
  if(usePf&&live)game::PfDraw(worldFrame.lines,worldFrame.boxes,worldFrame.quads);
   if(useChests&&live){std::vector<std::array<float,6>> chests;{auto& shared=game::Chests();AcquireSRWLockShared(&shared.lock);chests=shared.boxes;ReleaseSRWLockShared(&shared.lock);}
    const ImVec4 fill=colorOf(chestEsp,"fill",ImVec4(1,1,0,.25f)),ink=colorOf(chestEsp,"outline",ImVec4(1,1,0,1));const bool outline=Flag(*chestEsp,"draw_outline",true);const float thickness=num(*chestEsp,"thickness",1.5f);const auto edge=rgba(ink,1);
    for(const auto& c:chests){game::WorldBox box{};box.low[0]=c[0];box.low[1]=c[1];box.low[2]=c[2];box.high[0]=c[3];box.high[1]=c[4];box.high[2]=c[5];box.color[0]=fill.x;box.color[1]=fill.y;box.color[2]=fill.z;box.color[3]=fill.w;box.depth=false;worldFrame.boxes.push_back(box);
     if(outline){static const int edges[12][2]={{0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7}};const float lo[3]={c[0],c[1],c[2]},hi[3]={c[3],c[4],c[5]};
      for(auto& e:edges){const int a=e[0],b=e[1];line(a&1?hi[0]:lo[0],a&4?hi[1]:lo[1],a&2?hi[2]:lo[2],b&1?hi[0]:lo[0],b&4?hi[1]:lo[1],b&2?hi[2]:lo[2],edge,thickness,false);}}}}
   // Item ESP (game_items.hpp): dropped items filtered by Whitelist / Blacklist on the identifier. Items with the same
   // id and custom name whose centres are closer than Merge Radius are merged (union box, summed counts).
   // Box: 6 faces at Fill Opacity then 12 edges of Thickness, depth-tested unless Through Walls.
   // Name tag: "name" or "name xN", plus "12m" with Show Distance, 0.3 above the box and 16 px higher, centred.
   // Tracer: from screen centre to group centre. Display name: custom name, otherwise the formatted identifier.
   if(useItems&&live){static game::GroundItem raw[1024];const int count=game::ReadGroundItems(raw,1024,128);auto render=Find(*itemEspModule,"render");const auto picked=SplitList(render?render->text:std::string());
    auto has=[&](const char* o){return std::find(picked.begin(),picked.end(),o)!=picked.end();};const bool white=Flag(*itemEspModule,"whitelist",false),black=Flag(*itemEspModule,"blacklist",false);
    std::vector<std::string> wl,bl;if(auto l=Find(*itemEspModule,"whitelisted_items"))wl=SplitList(l->text);if(auto l=Find(*itemEspModule,"blacklisted_items"))bl=SplitList(l->text);
    auto bare=[](std::string s){const size_t c=s.rfind(':');return c==std::string::npos?s:s.substr(c+1);};auto in=[&](const std::vector<std::string>& l,const std::string& id){for(auto& e:l)if(bare(e)==bare(id))return true;return false;};
    struct Group{float lo[3],hi[3];uint64_t item;std::string id,custom;int count;};std::vector<Group> groups;const float merge=num(*itemEspModule,"merge_radius",1.5f);
    for(int i=0;i<count;++i){const auto& g=raw[i];if((white&&!in(wl,g.id))||(black&&in(bl,g.id)))continue;const float cx=(g.lo[0]+g.hi[0])*.5f,cy=(g.lo[1]+g.hi[1])*.5f,cz=(g.lo[2]+g.hi[2])*.5f;Group* into=nullptr;
     if(merge>0)for(auto& o:groups){if(o.item!=g.item||o.custom!=g.custom)continue;const float dx=(o.lo[0]+o.hi[0])*.5f-cx,dy=(o.lo[1]+o.hi[1])*.5f-cy,dz=(o.lo[2]+o.hi[2])*.5f-cz;if(dx*dx+dy*dy+dz*dz<=merge*merge){into=&o;break;}}
     if(into){for(int k=0;k<3;++k){into->lo[k]=std::min(into->lo[k],g.lo[k]);into->hi[k]=std::max(into->hi[k],g.hi[k]);}into->count+=g.count;}else{Group n{};memcpy(n.lo,g.lo,12);memcpy(n.hi,g.hi,12);n.item=g.item;n.id=g.id;n.custom=g.custom;n.count=g.count;groups.push_back(n);}}
    const ImVec4 color=colorOf(itemEspModule,"color",ImVec4(.35f,.85f,1,1));const bool depth=!Flag(*itemEspModule,"through_walls",true);const float thickness=num(*itemEspModule,"thickness",1.5f),fillOpacity=num(*itemEspModule,"fill_opacity",20)/100.f;
    const auto edge=rgba(ImVec4(color.x,color.y,color.z,1),1);const ImU32 solid=ImGui::ColorConvertFloat4ToU32(ImVec4(color.x,color.y,color.z,1));
    for(auto& g:groups){const float cx=(g.lo[0]+g.hi[0])*.5f,cy=(g.lo[1]+g.hi[1])*.5f,cz=(g.lo[2]+g.hi[2])*.5f;
     if(has("Box")){game::WorldBox box{};memcpy(box.low,g.lo,12);memcpy(box.high,g.hi,12);box.color[0]=color.x;box.color[1]=color.y;box.color[2]=color.z;box.color[3]=fillOpacity;box.depth=depth;if(fillOpacity>0)worldFrame.boxes.push_back(box);
      static const int edges[12][2]={{0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7}};
      for(auto& e:edges){const int a=e[0],b=e[1];line(a&1?g.hi[0]:g.lo[0],a&4?g.hi[1]:g.lo[1],a&2?g.hi[2]:g.lo[2],b&1?g.hi[0]:g.lo[0],b&4?g.hi[1]:g.lo[1],b&2?g.hi[2]:g.lo[2],edge,thickness,depth);}}
     if(has("Tracer")){const Clip c=clip(cx,cy,cz);if(c.w>0)d->AddLine(ImVec2(W/2,H/2),toScreen(c),solid,1);}
     if(has("Name Tag")){const Clip c=clip(cx,g.hi[1]+.3f,cz);if(c.w>0){std::string label=g.custom.empty()?PrettyId(g.id):g.custom;if(g.count>=2)label+=" x"+std::to_string(g.count);
       if(Flag(*itemEspModule,"show_distance",false)){const float dx=origin[0]-cx,dy=origin[1]-cy,dz=origin[2]-cz;char b[24];snprintf(b,sizeof(b)," %.0fm",std::sqrt(dx*dx+dy*dy+dz*dz));label+=b;}
       const ImVec2 at=toScreen(c);const float size=13;const float w=TextWidth(regular,size,label.c_str());const ImVec2 p(std::round(at.x-w/2),std::round(at.y-16));
       d->AddRectFilled(ImVec2(p.x-3,p.y-1),ImVec2(p.x+w+3,p.y+size+3),IM_COL32(0,0,0,110),3);Label(d,regular,size,p,label.c_str(),solid);}}}}
   // Jump Circles (render prep): for each tracked player (Targets: Self / Others / Both, others within Range), a jump
   // (left the ground and rose more than 0.01) adds a ring at the previous position and a landing adds one at the current
   // position; at least 150 ms between rings of one player, at most 64 rings (oldest dropped), each living Duration.
   // Ring at feet + 0.02: f = 1 - t, radius = (1 - f^3) x Radius, thickness = Width % of the radius, alpha x f^2,
   // 48 segments. Outline: outer edge 1.5 thick. Through Walls: material without depth test.
   // Colour: Static; Gradient blends both colours per segment with s = 1 - |2u - 1| (u = position on the ring);
   // Rainbow = hue rotating with time x Rainbow Speed. Colour is computed per segment, not per vertex.
   {struct Ring{float x,y,z;double at;};static std::deque<Ring> rings;struct Track{bool ground;float feet[3];double last;};static std::unordered_map<uint64_t,Track> tracks;
    if(!useJumps||!live){rings.clear();tracks.clear();}
    else{static game::JumpBody bodies[256];const int count=game::ReadJumpBodies(bodies,256);auto pick=[&](const char* id){auto st=Find(*jumpCircles,id);return st?st->text:std::string();};
     const std::string trigger=pick("trigger"),targets=pick("targets");const float range=num(*jumpCircles,"range",32),duration=std::max(.05f,num(*jumpCircles,"duration",1));
     std::unordered_set<uint64_t> alive;
     for(int i=0;i<count;++i){const auto& b=bodies[i];alive.insert(b.id);if(b.local?targets=="Others":targets=="Self")continue;
      if(!b.local){const float dx=b.feet[0]-feet[0],dy=b.feet[1]-feet[1],dz=b.feet[2]-feet[2];if(std::sqrt(dx*dx+dy*dy+dz*dz)>range){tracks.erase(b.id);continue;}}
      auto found=tracks.find(b.id);if(found==tracks.end()){tracks[b.id]={b.ground,{b.feet[0],b.feet[1],b.feet[2]},-1e9};continue;}Track& t=found->second;
      const bool jumped=t.ground&&!b.ground&&b.feet[1]>t.feet[1]+.01f,landed=!t.ground&&b.ground;
      if(((jumped&&trigger!="Land")||(landed&&trigger!="Jump"))&&now-t.last>=.15){const float* at=jumped?t.feet:b.feet;rings.push_back({at[0],at[1]+.02f,at[2],now});t.last=now;while(rings.size()>64)rings.pop_front();}
      t.ground=b.ground;memcpy(t.feet,b.feet,12);}
     for(auto it=tracks.begin();it!=tracks.end();)if(!alive.count(it->first))it=tracks.erase(it);else ++it;
     while(!rings.empty()&&now-rings.front().at>duration)rings.pop_front();
     const std::string mode=pick("color_mode");const ImVec4 base=colorOf(jumpCircles,"color",ImVec4(.35f,.75f,1,.8f));std::string ends[2];GradientEnds("jump_circles",pick("gradient"),ends);
     const ImVec4 g0=ParseColor(ends[0]),g1=ParseColor(ends[1]);const float radius=num(*jumpCircles,"radius",1.2f),width=num(*jumpCircles,"width",35)/100.f,speed=num(*jumpCircles,"rainbow_speed",1);
     const bool depth=!Flag(*jumpCircles,"through_walls",false),outline=Flag(*jumpCircles,"outline",true);
     for(const auto& r:rings){const float t=std::clamp((float)((now-r.at)/duration),0.f,1.f),f=1-t,outer=(1-f*f*f)*radius,inner=outer*(1-width),fade=f*f;
      for(int k=0;k<48;++k){const float a0=k/48.f*6.2831853f,a1=(k+1)/48.f*6.2831853f,u=(k+.5f)/48.f;ImVec4 c=base;
       if(mode=="Gradient"){const float s=1-std::fabs(2*u-1);c=ImVec4(g0.x+(g1.x-g0.x)*s,g0.y+(g1.y-g0.y)*s,g0.z+(g1.z-g0.z)*s,g0.w+(g1.w-g0.w)*s);}
       else if(mode=="Rainbow"){float rr,gg,bb;ImGui::ColorConvertHSVtoRGB(std::fmod(u+(float)now*.25f*speed,1.f),.7f,1,rr,gg,bb);c=ImVec4(rr,gg,bb,base.w);}
       game::WorldQuad q{};const float cs0=std::cos(a0),sn0=std::sin(a0),cs1=std::cos(a1),sn1=std::sin(a1);
       const float pts[4][3]={{r.x+cs0*inner,r.y,r.z+sn0*inner},{r.x+cs0*outer,r.y,r.z+sn0*outer},{r.x+cs1*outer,r.y,r.z+sn1*outer},{r.x+cs1*inner,r.y,r.z+sn1*inner}};memcpy(q.p,pts,sizeof(pts));
       q.color[0]=c.x;q.color[1]=c.y;q.color[2]=c.z;q.color[3]=c.w*fade;q.depth=depth;worldFrame.quads.push_back(q);
       if(outline)line(pts[1][0],pts[1][1],pts[1][2],pts[2][0],pts[2][1],pts[2][2],std::array<float,4>{c.x,c.y,c.z,c.w*fade},1.5f,depth);}}}}
   // Air Place: preview of the targeted cell
   {static Module* ap=ModuleById("air_place");auto& s=game::AirPlace();bool show=false;int32_t cell[3]{};{std::lock_guard<std::mutex> lock(s.guard);show=s.enabled&&s.preview&&s.hasTarget;memcpy(cell,s.target,12);}
    if(show&&live&&ap){const ImVec4 fill=colorOf(ap,"fill_color",ImVec4(1,1,1,.2f)),ink=colorOf(ap,"preview_color",ImVec4(1,1,1,1));const float lo[3]={(float)cell[0],(float)cell[1],(float)cell[2]},hi[3]={lo[0]+1,lo[1]+1,lo[2]+1};
     game::WorldBox b{};memcpy(b.low,lo,12);memcpy(b.high,hi,12);b.color[0]=fill.x;b.color[1]=fill.y;b.color[2]=fill.z;b.color[3]=fill.w;b.depth=true;worldFrame.boxes.push_back(b);
     static const int edges[12][2]={{0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7}};const auto edge=rgba(ink,1);
     for(auto& e:edges){const int a=e[0],c2=e[1];line(a&1?hi[0]:lo[0],a&4?hi[1]:lo[1],a&2?hi[2]:lo[2],c2&1?hi[0]:lo[0],c2&4?hi[1]:lo[1],c2&2?hi[2]:lo[2],edge,1.5f,true);}}}
   // Base Finder: chunk box (alpha 0x40) and corners (16 %, alpha FF), Highlight Blocks, Tracer, Labels (type + distance),
   // card for the targeted find, trails (Trail Color) and chunk-age squares.
   {static Module* bfm=ModuleById("base_finder");if(bfm&&bfm->enabled&&live){std::vector<game::BfFindView> finds;std::vector<game::BfTrailView> trails;std::vector<std::pair<uint64_t,uint8_t>> ages;float feet[3]{};
     {auto& s=game::BaseFinder();std::lock_guard<std::mutex> lock(s.guard);finds=s.finds;trails=s.trails;ages=s.ages;memcpy(feet,s.feet,12);}
     const float sc=std::min(W/1280.f,H/720.f);const ImVec4 baseC=colorOf(bfm,"base_color",ImVec4(.35f,.85f,1,1)),stashC=colorOf(bfm,"stash_color",ImVec4(1,.82f,.25f,1)),hiC=colorOf(bfm,"highlight_color",ImVec4(1,1,1,.8f)),trailC=colorOf(bfm,"trail_color",ImVec4(1,.541f,.239f,1));
     const bool labels=Flag(*bfm,"labels",true),highlight=Flag(*bfm,"highlight_blocks",true),tracer=Flag(*bfm,"tracer",false);const float hiRange=num(*bfm,"highlight_range",48);
     static const int edges[12][2]={{0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7}};
     auto P=[&](const float* lo,const float* hi,int k,float* out){out[0]=k&1?hi[0]:lo[0];out[1]=k&4?hi[1]:lo[1];out[2]=k&2?hi[2]:lo[2];};
     int aimed=-1;float aimedD=1e30f;
     for(size_t i=0;i<finds.size();++i){const auto& f=finds[i];const float lo[3]={(float)f.cx*16,(float)f.ymin,(float)f.cz*16},hi[3]={lo[0]+16,(float)f.ymax+1,lo[2]+16};float mnx=1e9f,mny=1e9f,mxx=-1e9f,mxy=-1e9f;bool ok=true;
      for(int k=0;k<8;++k){float q[3];P(lo,hi,k,q);const Clip c=clip(q[0],q[1],q[2]);if(c.w<=0){ok=false;break;}const ImVec2 v=toScreen(c);mnx=std::min(mnx,v.x);mny=std::min(mny,v.y);mxx=std::max(mxx,v.x);mxy=std::max(mxy,v.y);}
      if(!ok||W/2<mnx||W/2>mxx||H/2<mny||H/2>mxy)continue;const float dx=(lo[0]+hi[0])/2-origin[0],dy=(lo[1]+hi[1])/2-origin[1],dz=(lo[2]+hi[2])/2-origin[2],dd=dx*dx+dy*dy+dz*dz;if(dd<aimedD){aimedD=dd;aimed=(int)i;}}
     for(size_t i=0;i<finds.size();++i){const auto& f=finds[i];const ImVec4 col=f.type==1?stashC:baseC;const float lo[3]={(float)f.cx*16,(float)f.ymin,(float)f.cz*16},hi[3]={lo[0]+16,(float)f.ymax+1,lo[2]+16};const bool isAimed=(int)i==aimed;
      for(auto& e:edges){float a[3],b[3];P(lo,hi,e[0],a);P(lo,hi,e[1],b);line(a[0],a[1],a[2],b[0],b[1],b[2],std::array<float,4>{col.x,col.y,col.z,64/255.f},sc,false);
       const float t=.16f;const float a2[3]={a[0]+(b[0]-a[0])*t,a[1]+(b[1]-a[1])*t,a[2]+(b[2]-a[2])*t},b2[3]={b[0]+(a[0]-b[0])*t,b[1]+(a[1]-b[1])*t,b[2]+(a[2]-b[2])*t};const float th=(isAimed?3.f:2.f)*sc;
       line(a[0],a[1],a[2],a2[0],a2[1],a2[2],std::array<float,4>{col.x,col.y,col.z,1},th,false);line(b[0],b[1],b[2],b2[0],b2[1],b2[2],std::array<float,4>{col.x,col.y,col.z,1},th,false);}
      if(isAimed){game::WorldBox box{};memcpy(box.low,lo,12);memcpy(box.high,hi,12);box.color[0]=col.x;box.color[1]=col.y;box.color[2]=col.z;box.color[3]=18/255.f;box.depth=false;worldFrame.boxes.push_back(box);}
      if(highlight)for(auto& p:f.spots){const float cx=p[0]+.5f-origin[0],cy=p[1]+.5f-origin[1],cz=p[2]+.5f-origin[2];if(cx*cx+cy*cy+cz*cz>hiRange*hiRange)continue;const float bl[3]={(float)p[0],(float)p[1],(float)p[2]},bh[3]={bl[0]+1,bl[1]+1,bl[2]+1};
       for(auto& e:edges){float a[3],b[3];P(bl,bh,e[0],a);P(bl,bh,e[1],b);line(a[0],a[1],a[2],b[0],b[1],b[2],std::array<float,4>{hiC.x,hiC.y,hiC.z,hiC.w},1.5f,false);}}
      if(tracer){const Clip c=clip((lo[0]+hi[0])/2,(lo[1]+hi[1])/2,(lo[2]+hi[2])/2);if(c.w>0)d->AddLine(ImVec2(W/2,H/2),toScreen(c),ImGui::ColorConvertFloat4ToU32(ImVec4(col.x,col.y,col.z,1)),1.5f);}
      const float ddx=f.x-origin[0],ddy=f.y-origin[1],ddz=f.z-origin[2];char away[32];snprintf(away,sizeof(away),"%.0fm",std::sqrt(ddx*ddx+ddy*ddy+ddz*ddz));
      if(labels&&!isAimed){const Clip c=clip((lo[0]+hi[0])/2,hi[1]+1.f,(lo[2]+hi[2])/2);if(c.w>0){const ImVec2 at=toScreen(c);if(at.x>=0&&at.x<=W&&at.y>=0&&at.y<=H){const float s2=sc*.8f,size=17.758f*s2;const char* kind=game::BfKindName(f.type);
        const float w1=TextWidth(bold,size,kind),w2=TextWidth(regular,size,away),dia=5.6f*s2,pad=8*s2,w=dia*2+6*s2+w1+6*s2+w2+pad*2,h=size+10*s2;const ImVec2 lo2(std::round(at.x-w/2),std::round(at.y-h)),hi2(lo2.x+w,lo2.y+h);
        d->AddRectFilled(lo2,hi2,IM_COL32(13,13,15,204),h/2);d->AddRect(lo2,hi2,IM_COL32(255,255,255,26),h/2);const ImVec2 dc(lo2.x+pad+dia,lo2.y+h/2);const ImU32 ci=ImGui::ColorConvertFloat4ToU32(ImVec4(col.x,col.y,col.z,1));
        d->AddQuadFilled(ImVec2(dc.x,dc.y-dia),ImVec2(dc.x+dia,dc.y),ImVec2(dc.x,dc.y+dia),ImVec2(dc.x-dia,dc.y),ci);Label(d,bold,size,ImVec2(dc.x+dia+6*s2,lo2.y+5*s2),kind,IM_COL32(255,255,255,255));
        Label(d,regular,size,ImVec2(dc.x+dia+12*s2+w1,lo2.y+5*s2),away,IM_COL32(201,197,214,255));}}}
      if(isAimed){const float s2=sc*.8f,size=17.758f*s2,small=14*s2;std::vector<std::pair<std::string,std::string>> rows;char xyz[64];snprintf(xyz,sizeof(xyz),"%d %d %d",f.x,f.y,f.z);
       rows.push_back({std::string(away)+" away",xyz});for(auto& l:f.lines)rows.push_back({game::BfPretty(l.name),"\xc3\x97"+std::to_string(l.count)});
       float w=TextWidth(bold,size,game::BfKindName(f.type))+40*s2;for(auto& r:rows)w=std::max(w,TextWidth(regular,small,r.first.c_str())+TextWidth(regular,small,r.second.c_str())+40*s2);
       const float h=size+16*s2+rows.size()*(small+6*s2)+10*s2;const ImVec2 lo2(std::round(W/2+24*s2),std::round(H/2-h/2)),hi2(lo2.x+w,lo2.y+h);const ImU32 ci=ImGui::ColorConvertFloat4ToU32(ImVec4(col.x,col.y,col.z,1));
       d->AddRectFilled(lo2,hi2,IM_COL32(13,13,15,220),8*s2);d->AddRectFilled(lo2,ImVec2(hi2.x,lo2.y+size+12*s2),ImGui::ColorConvertFloat4ToU32(ImVec4(col.x,col.y,col.z,.14f)),8*s2,ImDrawFlags_RoundCornersTop);
       d->AddRectFilled(lo2,ImVec2(lo2.x+3*s2,hi2.y),ImGui::ColorConvertFloat4ToU32(ImVec4(col.x,col.y,col.z,.5f)),8*s2,ImDrawFlags_RoundCornersLeft);Label(d,bold,size,ImVec2(lo2.x+12*s2,lo2.y+6*s2),game::BfKindName(f.type),ci);
       float y=lo2.y+size+18*s2;for(size_t r=0;r<rows.size();++r){Label(d,regular,small,ImVec2(lo2.x+12*s2,y),rows[r].first.c_str(),r==0?IM_COL32(201,197,214,255):IM_COL32(255,255,255,255));
        const float rw=TextWidth(regular,small,rows[r].second.c_str());Label(d,regular,small,ImVec2(hi2.x-12*s2-rw,y),rows[r].second.c_str(),IM_COL32(167,162,181,255));y+=small+6*s2;}}}
     const float trailLen=num(*bfm,"trail_length",48);
     for(auto& t:trails){const auto& g=t.seg;if(g.end-g.begin+1<trailLen)continue;const float x1=(g.axis==0?g.begin:g.perp)+.5f,z1=(g.axis==0?g.perp:g.begin)+.5f,x2=(g.axis==0?g.end:g.perp)+.5f,z2=(g.axis==0?g.perp:g.end)+.5f,y=g.y+.2f;
      const float ax=x2-x1,az=z2-z1,len2=ax*ax+az*az;float tt=len2>0?((origin[0]-x1)*ax+(origin[2]-z1)*az)/len2:0;tt=std::clamp(tt,0.f,1.f);const float nx=x1+ax*tt-origin[0],nz=z1+az*tt-origin[2],ny=y-origin[1];
      if(nx*nx+ny*ny+nz*nz>768.f*768.f)continue;line(x1,y,z1,x2,y,z2,std::array<float,4>{trailC.x,trailC.y,trailC.z,trailC.w},1.5f*sc,false);}
     if(Flag(*bfm,"chunk_ages",false)){const ImVec4 nc=colorOf(bfm,"new_chunk_color",ImVec4(1,.31f,.31f,.3f)),oc=colorOf(bfm,"old_chunk_color",ImVec4(.31f,1,.47f,.3f));const float Y=std::floor(feet[1])+num(*bfm,"age_overlay_offset",-1);
      for(auto& [key,age]:ages){if(!age)continue;const ImVec4 col=age==1?nc:oc;const float x0=(float)(int32_t)(key>>32)*16,z0=(float)(int32_t)(uint32_t)key*16;game::WorldQuad q{};
       const float pts[4][3]={{x0,Y,z0},{x0+16,Y,z0},{x0+16,Y,z0+16},{x0,Y,z0+16}};memcpy(q.p,pts,sizeof(pts));q.color[0]=col.x;q.color[1]=col.y;q.color[2]=col.z;q.color[3]=col.w;q.depth=true;worldFrame.quads.push_back(q);}}}}
   // Schematics: blue frame around each schematic (1.5), edges of unresolved blocks in red and, with Block outline,
   // edges of nearby ghost blocks (at most 3000); Block info text under the crosshair.
   {auto& sc=game::Schematics();std::vector<std::array<int,6>> boxes;std::vector<std::array<int,3>> red;std::vector<std::array<int,3>> ghosts;bool outline=false,info=false;std::string iname,iwrong;int ipos[3]{};
    {std::lock_guard<std::recursive_mutex> lock(sc.guard);boxes=sc.boxes;red=sc.red;outline=sc.outline;info=sc.infoValid;iname=sc.infoName;iwrong=sc.infoWrong;memcpy(ipos,sc.infoPos,12);
     if(outline&&sc.render)for(auto& sub:*sc.render)for(auto& b:sub.blocks){if(ghosts.size()>=3000)break;const float dx=sub.x+b.x+.5f-origin[0],dy=sub.y+b.y+.5f-origin[1],dz=sub.z+b.z+.5f-origin[2];if(dx*dx+dy*dy+dz*dz<=24.f*24.f)ghosts.push_back({sub.x+b.x,sub.y+b.y,sub.z+b.z});}}
    if(live&&(!boxes.empty()||!red.empty()||!ghosts.empty())){static const int edges[12][2]={{0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7}};
     auto cube=[&](const float* lo,const float* hi,std::array<float,4> col,float th){for(auto& e:edges){const int a2=e[0],c2=e[1];line(a2&1?hi[0]:lo[0],a2&4?hi[1]:lo[1],a2&2?hi[2]:lo[2],c2&1?hi[0]:lo[0],c2&4?hi[1]:lo[1],c2&2?hi[2]:lo[2],col,th,true);}};
     for(auto& b:boxes){const float lo[3]={(float)b[0],(float)b[1],(float)b[2]},hi[3]={(float)b[3],(float)b[4],(float)b[5]};cube(lo,hi,{.55f,.75f,1.f,1.f},1.5f);}
     for(auto& r:red){const float lo[3]={r[0]-.03f,r[1]-.03f,r[2]-.03f},hi[3]={r[0]+1.03f,r[1]+1.03f,r[2]+1.03f};cube(lo,hi,{1.f,.35f,.35f,1.f},1.f);}
     for(auto& g:ghosts){const float lo[3]={g[0]-.03f,g[1]-.03f,g[2]-.03f},hi[3]={g[0]+1.03f,g[1]+1.03f,g[2]+1.03f};cube(lo,hi,{.55f,.75f,1.f,1.f},1.f);}}
    if(live&&info){char t[160];snprintf(t,sizeof(t),"%s at %d, %d, %d",iname.c_str(),ipos[0],ipos[1],ipos[2]);const float size=14;const float w=TextWidth(regular,size,t);Label(d,regular,size,ImVec2(std::round(W/2-w/2),std::round(H/2+24)),t,IM_COL32(255,255,255,230));
     if(!iwrong.empty()){const std::string wr="Wrong block: "+iwrong;const float w2=TextWidth(regular,size,wr.c_str());Label(d,regular,size,ImVec2(std::round(W/2-w2/2),std::round(H/2+42)),wr.c_str(),IM_COL32(246,160,171,230));}}}
   // Bed Breaker outline: target in red while searching, the block being broken grows, purple contour.
   {static Module* bbm=ModuleById("bed_breaker");game::BbSnapshot sn{};bool show=false;{auto& s=game::BedBreaker();std::lock_guard<std::mutex> lock(s.guard);show=s.cfg.enabled&&s.cfg.outline;sn=s.snap;}
    if(show&&live&&bbm&&sn.state!=0){static const int edges[12][2]={{0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7}};
     auto cube=[&](const int32_t* b,std::array<float,4> col){const float lo[3]={(float)b[0],(float)b[1],(float)b[2]},hi[3]={lo[0]+1,lo[1]+1,lo[2]+1};
      for(auto& e:edges){const int a=e[0],c2=e[1];line(a&1?hi[0]:lo[0],a&4?hi[1]:lo[1],a&2?hi[2]:lo[2],c2&1?hi[0]:lo[0],c2&4?hi[1]:lo[1],c2&2?hi[2]:lo[2],col,1.5f,true);}};
     if(sn.uncover)cube(sn.p,{245/255.f,90/255.f,90/255.f,230/255.f});
     if(sn.progress>0){const float h=.5f*sn.progress;game::WorldBox b{};for(int i=0;i<3;++i){b.low[i]=sn.n[i]+.5f-h;b.high[i]=sn.n[i]+.5f+h;}b.color[0]=255/255.f;b.color[1]=138/255.f;b.color[2]=61/255.f;b.color[3]=70/255.f;b.depth=true;worldFrame.boxes.push_back(b);}
     cube(sn.n,{255/255.f,138/255.f,61/255.f,230/255.f});}}
   // Regen outline: same drawing as Bed Breaker (target in red while uncovering, growing box, purple contour).
   {static Module* rgm=ModuleById("regen");game::RgSnapshot sn{};bool show=false;{auto& s=game::Regen();std::lock_guard<std::mutex> lock(s.guard);show=s.cfg.enabled&&s.cfg.outline;sn=s.snap;}
    if(show&&live&&rgm&&sn.state!=0){static const int edges[12][2]={{0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7}};
     auto cube=[&](const int32_t* b,std::array<float,4> col){const float lo[3]={(float)b[0],(float)b[1],(float)b[2]},hi[3]={lo[0]+1,lo[1]+1,lo[2]+1};
      for(auto& e:edges){const int a=e[0],c2=e[1];line(a&1?hi[0]:lo[0],a&4?hi[1]:lo[1],a&2?hi[2]:lo[2],c2&1?hi[0]:lo[0],c2&4?hi[1]:lo[1],c2&2?hi[2]:lo[2],col,1.5f,true);}};
     if(sn.uncover)cube(sn.p,{245/255.f,90/255.f,90/255.f,230/255.f});
     if(sn.progress>0){const float h=.5f*sn.progress;game::WorldBox b{};for(int i=0;i<3;++i){b.low[i]=sn.n[i]+.5f-h;b.high[i]=sn.n[i]+.5f+h;}b.color[0]=255/255.f;b.color[1]=138/255.f;b.color[2]=61/255.f;b.color[3]=70/255.f;b.depth=true;worldFrame.boxes.push_back(b);}
     cube(sn.n,{255/255.f,138/255.f,61/255.f,230/255.f});}}
   // Sound ESP (game_sound.hpp): checked categories within Range; the local player's own sounds only with Show Self.
   // Players Only: sounds within 2 blocks of a player; Show Names adds that player's name.
   // Label at the sound position for Duration seconds, fading, sized by Scale. Draw Box: 0.4 cube (Box Color),
   // depth-tested unless Through Walls.
   if(useSounds&&live){std::vector<game::HeardSound> heard;{auto& s=game::Sounds();std::lock_guard<std::mutex> lock(s.guard);const uint64_t keep=(uint64_t)(num(*soundEsp,"duration",3)*1000);
     s.heard.erase(std::remove_if(s.heard.begin(),s.heard.end(),[&](const game::HeardSound& h){return GetTickCount64()-h.tick>keep;}),s.heard.end());heard=s.heard;}
    static const char* cats[8]={"footsteps","blocks","containers","eating","projectiles","combat","mobs","other"};const float range=num(*soundEsp,"range",32),duration=std::max(.1f,num(*soundEsp,"duration",3)),scale=num(*soundEsp,"scale",1);
    const ImVec4 labelColor=colorOf(soundEsp,"label_color",ImVec4(1,1,1,1)),boxColor=colorOf(soundEsp,"box_color",ImVec4(1,.6f,.2f,.6f));const bool playersOnly=Flag(*soundEsp,"players_only",false),self=Flag(*soundEsp,"show_self",false),names=Flag(*soundEsp,"show_names",true),box=Flag(*soundEsp,"draw_box",true),depth=!Flag(*soundEsp,"through_walls",true);
    static game::JumpBody bodies[256];const int nb=(playersOnly||names||!self)?game::ReadJumpBodies(bodies,256):0;
    for(const auto& h:heard){std::string label;const int cat=game::ClassifySound(h.name,h.volume,label);if(!Flag(*soundEsp,cats[cat],cat<6))continue;
     const float dx=h.pos[0]-feet[0],dy=h.pos[1]-feet[1],dz=h.pos[2]-feet[2];if(std::sqrt(dx*dx+dy*dy+dz*dz)>range)continue;
     const game::JumpBody* who=nullptr;float best=4;for(int i=0;i<nb;++i){const float ex=bodies[i].feet[0]-h.pos[0],ey=bodies[i].feet[1]+.9f-h.pos[1],ez=bodies[i].feet[2]-h.pos[2],e=ex*ex+ey*ey+ez*ez;if(e<best){best=e;who=&bodies[i];}}
     if(who&&who->local&&!self)continue;if(playersOnly&&!who)continue;if(names&&who&&!who->local){char n[64]{};if(game::PlayerName(who->id,n,sizeof(n)))label=std::string(n)+": "+label;}
     const float age=(float)(GetTickCount64()-h.tick)/1000.f,fade=std::clamp(1-age/duration,0.f,1.f);
     if(box){game::WorldBox b{};for(int k=0;k<3;++k){b.low[k]=h.pos[k]-.2f;b.high[k]=h.pos[k]+.2f;}b.color[0]=boxColor.x;b.color[1]=boxColor.y;b.color[2]=boxColor.z;b.color[3]=boxColor.w*fade;b.depth=depth;worldFrame.boxes.push_back(b);}
     const Clip c=clip(h.pos[0],h.pos[1]+.35f,h.pos[2]);if(c.w>0){const ImVec2 at=toScreen(c);const float size=13*scale,w=TextWidth(regular,size,label.c_str());const ImVec2 p(std::round(at.x-w/2),std::round(at.y-size));
      d->AddRectFilled(ImVec2(p.x-3,p.y-1),ImVec2(p.x+w+3,p.y+size+3),IM_COL32(0,0,0,(int)(110*fade)),3);Label(d,regular,size,p,label.c_str(),tone(labelColor,fade));}}}
   // Block ESP tracers: from screen centre to each matched block centre, width 1, style outline colour.
   // Rainbow: hue t = (x/H*0.8 + y/H*0.6)/0.6 - time*0.25 (channels x0.7 + 0.3); Gradient: blend outline -> second
   // colour with s = 1 - |2t - 1|; alpha = outline alpha.
   {static Module* blockEspT=ModuleById("block_esp");if(live&&blockEspT&&blockEspT->enabled&&Flag(*blockEspT,"tracers",false)&&H>0){
     std::shared_ptr<const std::vector<game::BeSub>> subs;{auto& be=game::BlockEsp();AcquireSRWLockShared(&be.lock);subs=be.selected;ReleaseSRWLockShared(&be.lock);}
     if(subs){int mode=0;{auto ms=Find(*blockEspT,"default_mode");if(ms){const auto at=std::find(ms->options.begin(),ms->options.end(),ms->text);mode=at==ms->options.end()?0:(int)(at-ms->options.begin());}}mode=std::clamp(mode,0,2);ImVec4 a(1,1,1,1),b(1,1,1,1);
      if(mode==0){auto g=Find(*blockEspT,"default_gradient");std::string ends[2];GradientEnds("block_esp",g?g->text:"",ends);a=ParseColor(ends[0]);b=ParseColor(ends[1]);}else a=colorOf(blockEspT,"default_color",ImVec4(1,1,1,1));
      const float T=(float)ImGui::GetTime()*.25f;const ImVec2 centre(W/2,H/2);
      // per-block styles, in configuration order (at most 7)
      std::vector<std::pair<int,std::pair<ImVec4,ImVec4>>> ownStyles(8,{mode==1?1:mode==0?0:2,{a,b}});{Setting* store=nullptr;for(auto& other:blockEspT->settings)if(other.id=="block_styles")store=&other;
       auto st=Find(*blockEspT,"blocks");std::string text=st?st->text:"";const auto own=store?ParseBlockStyles(store->text):std::map<std::string,BlockOwnStyle>{};int next=1;size_t from=0;
       while(from<=text.size()&&next<8){size_t comma=text.find(',',from);if(comma==std::string::npos)comma=text.size();std::string name=text.substr(from,comma-from);from=comma+1;auto f=own.find(name);if(f==own.end()||f->second.mode=='d')continue;
        ownStyles[next++]={f->second.mode=='g'?0:f->second.mode=='r'?1:2,{ParseColor(f->second.a),ParseColor(f->second.b)}};}}
      for(const auto& sub:*subs)for(const auto& bl:sub.blocks){const Clip c=clip(sub.x+bl.x+.5f,sub.y+bl.y+.5f,sub.z+bl.z+.5f);if(c.w<=.001f)continue;const ImVec2 to=toScreen(c);
       const int bm=bl.style?ownStyles[bl.style&7].first:mode;const ImVec4 ca=bl.style?ownStyles[bl.style&7].second.first:a,cb=bl.style?ownStyles[bl.style&7].second.second:b;
       float t=(to.x/H*.8f+to.y/H*.6f)/.6f-T;t-=std::floor(t);ImVec4 col=ca;
       if(bm==1){auto ch=[&](float o){float u=t+o;u-=std::floor(u);return std::clamp(std::fabs(u*6-3)-1,0.f,1.f)*.7f+.3f;};col=ImVec4(ch(0),ch(2/3.f),ch(1/3.f),ca.w);}
       else if(bm==0){const float k=1-std::fabs(2*t-1);col=ImVec4(ca.x+(cb.x-ca.x)*k,ca.y+(cb.y-ca.y)*k,ca.z+(cb.z-ca.z)*k,ca.w);}
       d->AddLine(centre,to,ImGui::ColorConvertFloat4ToU32(col),1.f);}}}}
   // Sus Chunk Finder: chunk box (camera-facing faces filled, 12 opaque edges of 1.5), tracer from screen centre, and the
   // reasons for the chunk nearest the crosshair in 16 px text, 12 px above, with a shadow.
   if(useSus&&live){std::vector<game::SusFlag> flags;{auto& shared=game::Sus();AcquireSRWLockShared(&shared.lock);flags=shared.flagged;ReleaseSRWLockShared(&shared.lock);}
    const ImVec4 base=colorOf(susFinder,"color",ImVec4(1,1,1,1));const ImU32 fillInk=ImGui::ColorConvertFloat4ToU32(base),edgeInk=ImGui::ColorConvertFloat4ToU32(ImVec4(base.x,base.y,base.z,1));const bool tracer=Flag(*susFinder,"tracer",false);
    const int r=(int)num(*susFinder,"scan_radius",8)+1;const int pcx=(int)std::floor(feet[0])>>4,pcz=(int)std::floor(feet[2])>>4;const ImVec2 centre(W/2,H/2);const game::SusFlag* hovered=nullptr;float nearest=3.4028235e38f;
    for(const auto& f:flags){const int dx=f.cx-pcx,dz=f.cz-pcz;if((unsigned)(dx*dx+dz*dz)>(unsigned)(r*r))continue;
     const float x0=(float)(f.cx*16),z0=(float)(f.cz*16),y0=(float)f.lowY,x1=x0+16,z1=z0+16,y1=(float)(f.highY+1),mx=(x0+x1)*.5f,my=(y0+y1)*.5f,mz=(z0+z1)*.5f;
     if(tracer){const Clip c=clip(mx,my,mz);if(c.w>.001f)d->AddLine(centre,toScreen(c),edgeInk,1.5f);}
     const float corner[8][3]={{x0,y0,z0},{x1,y0,z0},{x1,y0,z1},{x0,y0,z1},{x0,y1,z0},{x1,y1,z0},{x1,y1,z1},{x0,y1,z1}};ImVec2 p[8];bool ok=true;
     for(int i=0;i<8&&ok;++i){const Clip c=clip(corner[i][0],corner[i][1],corner[i][2]);if(c.w<=.001f)ok=false;else p[i]=toScreen(c);}if(!ok)continue;
     const float cam[3]={origin[0],origin[1],origin[2]};
     if(cam[1]<y0)d->AddQuadFilled(p[0],p[1],p[2],p[3],fillInk);if(y1<cam[1])d->AddQuadFilled(p[4],p[5],p[6],p[7],fillInk);if(cam[2]<z0)d->AddQuadFilled(p[0],p[1],p[5],p[4],fillInk);
     if(x1<cam[0])d->AddQuadFilled(p[1],p[2],p[6],p[5],fillInk);if(z1<cam[2])d->AddQuadFilled(p[2],p[3],p[7],p[6],fillInk);if(cam[0]<x0)d->AddQuadFilled(p[3],p[0],p[4],p[7],fillInk);
     static const int edges[12][2]={{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};for(auto& e:edges)d->AddLine(p[e[0]],p[e[1]],edgeInk,1.5f);
     ImVec2 lo=p[0],hi=p[0];for(auto& q:p){lo.x=std::min(lo.x,q.x);lo.y=std::min(lo.y,q.y);hi.x=std::max(hi.x,q.x);hi.y=std::max(hi.y,q.y);}
     if(lo.x<=centre.x&&lo.y<=centre.y&&centre.x<=hi.x&&centre.y<=hi.y){const float ex=mx-cam[0],ey=my-cam[1],ez=mz-cam[2],dist=ex*ex+ey*ey+ez*ez;if(dist<nearest){nearest=dist;hovered=&f;}}}
    if(hovered){const std::string text=game::SusReasons(*hovered);hudText(regular,16,ImVec2(centre.x+13,centre.y+13),text.c_str(),IM_COL32(0,0,0,180));hudText(regular,16,ImVec2(centre.x+12,centre.y+12),text.c_str(),edgeInk);}}
   // Waypoints: entries "Name x y z [overworld|nether|end] [#RRGGBB]", read from the end
   if(waypoints&&waypoints->enabled&&live){int dimension=0;game::ReadDimension(&dimension);const ImVec4 base=colorOf(waypoints,"color",ImVec4(.35f,.85f,1,1));auto entries=Find(*waypoints,"entries");const std::string all=entries?entries->text:std::string();size_t from=0;
    while(from<all.size()){size_t to=all.find(',',from);if(to==std::string::npos)to=all.size();const std::string entry=all.substr(from,to-from);from=to+1;std::vector<std::string> parts;{size_t at=0;while(at<entry.size()){size_t space=entry.find(' ',at);if(space==std::string::npos)space=entry.size();if(space>at)parts.push_back(entry.substr(at,space-at));at=space+1;}}
     ImVec4 color=base;int where=-1;for(int pass=0;pass<2&&!parts.empty();++pass){std::string last=parts.back();std::transform(last.begin(),last.end(),last.begin(),[](unsigned char ch){return (char)std::tolower(ch);});
      if(last.size()==7&&last[0]=='#'){color=ParseColor(last+"ff");parts.pop_back();}else if(last=="overworld"||last=="nether"||last=="end"){where=last=="overworld"?0:last=="nether"?1:2;parts.pop_back();}}
     if(parts.size()<3||(where>=0&&where!=dimension))continue;char* stop=nullptr;const long bz=strtol(parts[parts.size()-1].c_str(),&stop,10);if(*stop)continue;const long by=strtol(parts[parts.size()-2].c_str(),&stop,10);if(*stop)continue;const long bx=strtol(parts[parts.size()-3].c_str(),&stop,10);if(*stop)continue;
     std::string name;for(size_t i=0;i+3<parts.size();++i){if(!name.empty())name+=" ";name+=parts[i];}
     const float X=(float)bx,Y=(float)by,Z=(float)bz,mx=X+.5f,my=Y+.5f,mz=Z+.5f,dx=mx-origin[0],dy=my-origin[1],dz=mz-origin[2],distance=std::sqrt(dx*dx+dy*dy+dz*dz),flat=std::sqrt(dx*dx+dz*dz);
     const float nearF=smooth((distance-1)/2),farF=smooth((distance-24)/24),f=((1-nearF)*.35f+nearF)*(1-farF);
     if(f>0){const float lo[3]={X-.002f,Y-.002f,Z-.002f},hi[3]={X+1.002f,Y+1.002f,Z+1.002f};game::WorldBox box{};memcpy(box.low,lo,12);memcpy(box.high,hi,12);box.color[0]=color.x;box.color[1]=color.y;box.color[2]=color.z;box.color[3]=color.w*.18f*f;box.depth=true;worldFrame.boxes.push_back(box);
      static const int edges[12][2]={{0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7}};const auto ink=rgba(color,f);for(auto& edge:edges){const int a=edge[0],b=edge[1];line(a&1?hi[0]:lo[0],a&2?hi[1]:lo[1],a&4?hi[2]:lo[2],b&1?hi[0]:lo[0],b&2?hi[1]:lo[1],b&4?hi[2]:lo[2],ink,2,true);}}
     if(Flag(*waypoints,"beam",true)){const float k1=smooth((flat-128)/896),k2=smooth((flat-2)/6),fade=(.6f*k1+(1-k1))*k2;if(fade>0)for(int pass=0;pass<2;++pass){const float factor=pass?.85f:.2f,thick=pass?2.f:7.f;
      for(int i=0;i<16;++i){game::WorldLine segment{};segment.a[0]=mx;segment.a[1]=Y+i*16.f;segment.a[2]=mz;segment.b[0]=mx;segment.b[1]=Y+(i+1)*16.f;segment.b[2]=mz;const float a0=(1-smooth((float)i/16))*fade*factor,a1=(1-smooth((float)(i+1)/16))*fade*factor;
       segment.colorA[0]=color.x;segment.colorA[1]=color.y;segment.colorA[2]=color.z;segment.colorA[3]=color.w*a0;segment.colorB[0]=color.x;segment.colorB[1]=color.y;segment.colorB[2]=color.z;segment.colorB[3]=color.w*a1;segment.thickness=thick;segment.depth=false;worldFrame.lines.push_back(segment);}}}
     // 2D label: name and distance ("{:.0f}m" / "{:.1f}km" / "{:.0f}km"); an arrow 40 px from the edge when off screen
     {const Clip c=clip(mx,my,mz);float alpha=smooth((distance-2)/4);const float scale=1+(.8f-1)*smooth((distance-16)/240);char away[24]{};if(std::round(distance)<1000)snprintf(away,sizeof(away),"%.0fm",distance);else if(distance/1000>=9.95f)snprintf(away,sizeof(away),"%.0fkm",distance/1000);else snprintf(away,sizeof(away),"%.1fkm",distance/1000);
      ImVec2 at(0,0);bool on=false;if(c.w>.001f){at=toScreen(c);on=at.x>=40&&at.x<=W-40&&at.y>=40&&at.y<=H-40;}
      if(on){const float fromCentre=std::sqrt((at.x-W/2)*(at.x-W/2)+(at.y-H/2)*(at.y-H/2));alpha*=.25f+.75f*smooth((fromCentre-24)/56);
       if(alpha>.01f){const bool showFar=Flag(*waypoints,"show_distance",true);const float size=14*scale,small=12*scale,nameW=hudWidth(bold,size,name.c_str()),farW=showFar?hudWidth(regular,small,away):0,gap=nameW>0&&farW>0?6*scale:0,w=nameW+gap+farW+16*scale,h=23.8f*scale;const ImVec2 lo(at.x-w/2,at.y-h-8*scale);
        d->AddRectFilled(lo,ImVec2(lo.x+w,lo.y+h),IM_COL32(13,13,15,(int)(205*alpha)),6*scale);d->AddCircleFilled(at,3*scale,tone(color,alpha));if(nameW>0)hudText(bold,size,ImVec2(lo.x+8*scale,lo.y+(h-size)/2),name.c_str(),IM_COL32(255,255,255,(int)(255*alpha)));if(showFar)hudText(regular,small,ImVec2(lo.x+8*scale+nameW+gap,lo.y+(h-small)/2),away,IM_COL32(172,168,186,(int)(255*alpha)));}}
      else if(Flag(*waypoints,"off_screen_arrows",true)&&alpha>.01f){const float sign=c.w<0?-1.f:1.f;ImVec2 way(W*sign*c.x,-H*sign*c.y);const float length=std::sqrt(way.x*way.x+way.y*way.y);if(length>1e-4f){way.x/=length;way.y/=length;const float reach=std::min(std::fabs(way.x)>1e-4f?(W/2-40)/std::fabs(way.x):1e9f,std::fabs(way.y)>1e-4f?(H/2-40)/std::fabs(way.y):1e9f);
        const ImVec2 tip(W/2+way.x*reach,H/2+way.y*reach),side(-way.y,way.x);const float long16=16*scale,half=8*scale;const ImVec2 a(tip.x+way.x*half,tip.y+way.y*half),b(tip.x-way.x*half+side.x*half,tip.y-way.y*half+side.y*half),e(tip.x-way.x*half-side.x*half,tip.y-way.y*half-side.y*half);(void)long16;
        d->AddTriangleFilled(a,b,e,tone(color,alpha));d->AddTriangle(a,b,e,IM_COL32(10,10,14,(int)(220*alpha)),1.5f*scale);}}}}}
   // Damage Indicators: a number for each health change of at least 0.05 within 48 blocks
   {struct Number{float x,y,z;char text[12];double born;bool heal;};static std::vector<Number> numbers;static std::unordered_map<uint64_t,float> before;static double nextRead=0;
    if(!damage||!damage->enabled||!live){numbers.clear();before.clear();}
    else{const float keep=num(*damage,"duration",1);if(now>=nextRead){nextRead=now+.05;static game::Vital vitals[128];const int count=game::ReadVitals(vitals,128,48);std::unordered_map<uint64_t,float> seen;const bool playersOnly=Flag(*damage,"players_only",false),healing=Flag(*damage,"show_healing",false);
      for(int i=0;i<count;++i){const auto& vital=vitals[i];if(playersOnly&&!vital.player)continue;seen[vital.id]=vital.health;auto found=before.find(vital.id);if(found==before.end())continue;const float delta=vital.health-found->second;
       if(std::fabs(delta)>=.05f&&(delta<0||healing)&&numbers.size()<128){Number number{};number.x=vital.x+(rand()/(float)RAND_MAX*.7f-.35f);number.z=vital.z+(rand()/(float)RAND_MAX*.7f-.35f);number.y=vital.top+.25f;const float value=std::round(std::fabs(delta)*10)/10;snprintf(number.text,sizeof(number.text),value!=std::floor(value)?"%.1f":"%.0f",value);number.born=now;number.heal=delta>0;numbers.push_back(number);}}
      before.swap(seen);}
     numbers.erase(std::remove_if(numbers.begin(),numbers.end(),[&](const Number& number){return now-number.born>keep;}),numbers.end());const float scale=num(*damage,"scale",1);
     for(auto& number:numbers){const float t=std::clamp((float)((now-number.born)/keep),0.f,1.f),rise=(1-(1-t)*(1-t)*(1-t))*.8f;const Clip c=clip(number.x,number.y+rise,number.z);if(c.w<=.001f)continue;const ImVec2 at=toScreen(c);
      const float dx=number.x-origin[0],dy=number.y-origin[1],dz=number.z-origin[2],distance=std::sqrt(dx*dx+dy*dy+dz*dz),size=20*std::max(.45f,std::sqrt(6/std::max(6.f,distance)))*scale,alpha=1-std::clamp((t-.6f)/.4f,0.f,1.f);
      const ImVec4 color=colorOf(damage,number.heal?"heal_color":"damage_color",number.heal?ImVec4(.4f,1,.5f,1):ImVec4(1,.33f,.33f,1));const float w=hudWidth(bold,size,number.text);
      hudText(bold,size,ImVec2(at.x-w/2+1,at.y-size/2+1),number.text,IM_COL32(0,0,0,(int)(180*alpha*color.w)));hudText(bold,size,ImVec2(at.x-w/2,at.y-size/2),number.text,tone(color,alpha));}}}}
  game::SubmitWorld(std::move(worldFrame));
 }
 d=HudList();   // HUD elements again: hidden outside play
 // ---- MiniMap: square of Size (+17 with coordinates), anchored at (0.985, 0.02), Background Color fill.
 // Terrain is a 16W x 16W texel ring buffer filled on the game thread (game_map.hpp), drawn as 1 to 4 quads at
 // (Size/2)/Zoom px per block, north up or view direction up (Rotate With Player). Mobs: face from the embedded atlas
 // built from the game's own textures (hud_map_faces.hpp) with half-size Icon Size / 2 and a frame in the type colour, otherwise a dot of radius
 // Icon Size x 0.3. Compass N / E / S / W at 12, white arrow, 2 px black border (alpha 220), coordinates at 14 below.
 // Terrain is uploaded to the GPU at most five times per second.
 static Module* minimap=ModuleById("minimap");static bool mapKeyChecked=false;
 if(minimap&&!mapKeyChecked){mapKeyChecked=true;wchar_t saved[8]{};for(auto& key:minimap->settings)if(key.id=="world_map_key"&&key.text.empty()&&!GetPrivateProfileStringW(L"minimap",L"world_map_key",L"",saved,8,configPath.c_str()))key.text="M|77";}   // default key: M
 {auto& map=game::Map();const bool on=minimap&&minimap->enabled;InterlockedExchange(&map.enabled,on?1:0);if(on){InterlockedExchange(&map.zoom,(long)std::max(1.f,num(*minimap,"zoom",64)));InterlockedExchange(&map.rotate,Flag(*minimap,"rotate_with_player",false)?1:0);InterlockedExchange(&map.shading,Flag(*minimap,"height_shading",true)?1:0);}else mapOpen=false;}
 if(minimap&&minimap->enabled){
  game::Player player{};float yaw=0;const bool live=game::ReadPlayer(&player)&&game::ReadYaw(&yaw);auto& map=game::Map();
  if(live){static bool hooked=false;if(!hooked){hooked=true;game::InstallMapHooks(WriteLog);}static bool told=false,first=false;if(map.fault&&!told){told=true;if(WriteLog)WriteLog("MiniMap: a fault was caught while reading the terrain; the terrain is disabled until the game restarts");}
   if(!first&&map.chunksRead&&WriteLog){first=true;char line[120]{};snprintf(line,sizeof(line),"MiniMap: first terrain chunks read (%ld), lowest Y %d, dimension %d",map.chunksRead,map.minimumY,map.dimension);WriteLog(line);}
   static long loadTold=-1;static int loadLines=0;const long loadMs=map.loadMs;if(loadMs>=50&&loadMs!=loadTold&&loadLines<8&&WriteLog){loadTold=loadMs;++loadLines;char line[96]{};snprintf(line,sizeof(line),"MiniMap: every visible chunk read in %ld ms",loadMs);WriteLog(line);}}
  auto pick=[&](const char* id,ImVec4 fallback){auto setting=Find(*minimap,id);return setting&&setting->kind=="color"&&setting->text.size()==9?ParseColor(setting->text):fallback;};
  if(live||hudEditing){
   auto& place=Place("minimap",io.DisplaySize.x-170,10);const float k=place.scale,size=num(*minimap,"size",144)*k,opacity=num(*minimap,"opacity",100)/100,zoom=std::max(1.f,num(*minimap,"zoom",64));
   const bool coords=Flag(*minimap,"show_coordinates",true),rotate=Flag(*minimap,"rotate_with_player",false);place.lo=ImVec2(place.x,place.y);place.hi=ImVec2(place.x+size,place.y+size+(coords?17*k:0));
   ImVec4 back=pick("background_color",ImVec4(.05f,.05f,.06f,.75f));back.w*=opacity;
   const ImVec2 corner(place.x+size,place.y+size),centre(place.x+size/2,place.y+size/2);d->AddRectFilled(place.lo,corner,ImGui::ColorConvertFloat4ToU32(back));
   // orientation: without rotation north (-Z) is up; with rotation the view direction is up
   const float radians=yaw*3.14159265f/180,hx=-std::sin(radians),hz=std::cos(radians),scale=size/2/zoom;auto project=[&](float wx,float wz){return rotate?ImVec2(-wx*hz+wz*hx,-(wx*hx+wz*hz)):ImVec2(wx,wz);};
   d->PushClipRect(place.lo,corner,true);
   if(live&&UpdateImage&&GetImage){
    static std::vector<uint32_t> ring;static int cells=0;static uint64_t seen=~0ull;static double next=0;
    if(ImGui::GetTime()>=next){bool fresh=false;AcquireSRWLockShared(&map.lock);if((map.version!=seen||!GetImage(2))&&map.cells>0&&map.ring.size()==(size_t)map.cells*16*map.cells*16){ring=map.ring;cells=map.cells;seen=map.version;fresh=true;}ReleaseSRWLockShared(&map.lock);
     if(fresh&&cells*16<=2048){UpdateImage(2,(const unsigned char*)ring.data(),cells*16,cells*16);next=ImGui::GetTime()+.2;}}
    if(GetImage(2)&&cells>0){const float period=cells*16.f,reach=(rotate?zoom*1.41421356f:zoom)+1,px=player.position[0],pz=player.position[2];const ImU32 tint=IM_COL32(255,255,255,(int)(255*opacity));
     auto cut=[&](float low,float high,float out[3]){const float edge=std::floor(low/period)*period+period;out[0]=low;if(edge<high){out[1]=edge;out[2]=high;return 2;}out[1]=high;return 1;};float xs[3],zs[3];const int nx=cut(px-reach,px+reach,xs),nz=cut(pz-reach,pz+reach,zs);
     auto screen=[&](float wx,float wz){const ImVec2 way=project(wx-px,wz-pz);return ImVec2(centre.x+way.x*scale,centre.y+way.y*scale);};
     for(int i=0;i<nx;++i)for(int j=0;j<nz;++j){const float x0=xs[i],x1=xs[i+1],z0=zs[j],z1=zs[j+1],ox=std::floor((x0+x1)/2/period)*period,oz=std::floor((z0+z1)/2/period)*period,u0=(x0-ox)/period,u1=(x1-ox)/period,v0=(z0-oz)/period,v1=(z1-oz)/period;
      d->AddImageQuad(GetImage(2),screen(x0,z0),screen(x1,z0),screen(x1,z1),screen(x0,z1),ImVec2(u0,v0),ImVec2(u1,v0),ImVec2(u1,v1),ImVec2(u0,v1),tint);}}}
   if(live){static game::Mob mobs[128];static int count=0;static double next=0;if(ImGui::GetTime()>=next){next=ImGui::GetTime()+.1;count=game::ReadMobs(mobs,128,zoom*1.42f+1);}
    // faces: built from the game's own entity textures and the players' skins (hud_map_faces.hpp, image 4); a mob
    // whose face cannot be built stays a dot.
    const bool wantFaces=Flag(*minimap,"mob_faces",true)&&GetImage&&UpdateImage;auto& atlas=mapfaces::Faces();
    if(wantFaces){icons::ReadPackFile=ReadPackFileForIcons;game::Context fc{};const bool local=game::Local(&fc);atlas.Work(2,[&](uint64_t actor){return local?game::EntityId(fc,actor):std::string();},WriteLog);
     if(atlas.dirty&&ImGui::GetTime()-atlas.uploaded>.2){atlas.dirty=false;atlas.uploaded=ImGui::GetTime();UpdateImage(4,atlas.pixels.data(),mapfaces::Atlas::Size,mapfaces::Atlas::Size);}}
    auto faceUv=[](int cell,ImVec2* uv0,ImVec2* uv1){const float n=(float)mapfaces::Atlas::PerRow,c=(float)(cell%mapfaces::Atlas::PerRow),r=(float)(cell/mapfaces::Atlas::PerRow);*uv0=ImVec2(c/n,r/n);*uv1=ImVec2((c+1)/n,(r+1)/n);};
    const float icon=num(*minimap,"icon_size",10)*k;
    for(int i=0;i<count;++i){const bool hostile=mobs[i].hostile;if(!Flag(*minimap,hostile?"show_hostile_mobs":"show_passive_mobs",true))continue;const ImVec2 way=project(mobs[i].dx,mobs[i].dz),at(centre.x+way.x*scale,centre.y+way.y*scale);if(std::fabs(at.x-centre.x)>size/2-icon/2||std::fabs(at.y-centre.y)>size/2-icon/2)continue;
     ImVec4 color=pick(hostile?"hostile_color":"passive_color",ImVec4(1,1,1,1));color.w*=opacity;const ImU32 ink=ImGui::ColorConvertFloat4ToU32(color);const int face=wantFaces?atlas.Mob(mobs[i].type,mobs[i].actor):-1;
     if(face>=0&&GetImage(4)){ImVec2 uv0,uv1;faceUv(face,&uv0,&uv1);const ImVec2 lo(at.x-icon/2,at.y-icon/2),hi(at.x+icon/2,at.y+icon/2);d->AddRectFilled(ImVec2(lo.x-k,lo.y-k),ImVec2(hi.x+k,hi.y+k),IM_COL32(0,0,0,(int)(200*opacity)));d->AddImage(GetImage(4),lo,hi,uv0,uv1,IM_COL32(255,255,255,(int)(255*opacity)));d->AddRect(ImVec2(lo.x-k,lo.y-k),ImVec2(hi.x+k,hi.y+k),ink,0,0,k);}
     else{d->AddCircleFilled(at,icon*.3f+k,IM_COL32(0,0,0,(int)(200*opacity)));d->AddCircleFilled(at,icon*.3f,ink);}}
    // players: Show Players; type 0 player / 1 teammate (Teams). Excluded: bots (Anti Bot), invisible actors (flag 5)
    // and those out of range. Arrow oriented by head yaw, unit IconSize*s*0.5/7, Player Color / Teammate Color.
    // Show Finds: Base Finder finds in the current dimension with state != 2, fixed colours.
    if(Flag(*minimap,"show_finds",true)){int dimension=0;game::ReadDimension(&dimension);std::vector<game::BfDbEntry> found;{auto& db=game::BaseDb();std::lock_guard<std::mutex> lock(db.guard);for(auto& e:db.entries)if(e.dim==dimension&&e.state!=2)found.push_back(e);}
     static const ImVec4 fixed[4]={ImVec4(89/255.f,217/255.f,1,1),ImVec4(1,210/255.f,63/255.f,1),ImVec4(240/255.f,86/255.f,106/255.f,1),ImVec4(211/255.f,107/255.f,1,1)};const float px=player.position[0],pz=player.position[2];
     auto where=[&](float wx,float wz){const ImVec2 way=project(wx-px,wz-pz);return ImVec2(centre.x+way.x*scale,centre.y+way.y*scale);};d->PushClipRect(place.lo,corner,true);
     for(auto& e:found){ImVec4 col=fixed[std::clamp(e.type,0,3)];col.w*=opacity;const ImU32 ci=ImGui::ColorConvertFloat4ToU32(col);
      if(e.type==3){d->AddLine(where(e.x+.5f,e.z+.5f),where(e.x2+.5f,e.z2+.5f),ci,2.f);continue;}const ImVec2 at=where(e.x+.5f,e.z+.5f);const float r=(icon+1.5f)/2;
      d->AddQuadFilled(ImVec2(at.x,at.y-r-k),ImVec2(at.x+r+k,at.y),ImVec2(at.x,at.y+r+k),ImVec2(at.x-r-k,at.y),IM_COL32(0,0,0,(int)(220*opacity)));d->AddQuadFilled(ImVec2(at.x,at.y-r),ImVec2(at.x+r,at.y),ImVec2(at.x,at.y+r),ImVec2(at.x-r,at.y),ci);}
     d->PopClipRect();}
    if(Flag(*minimap,"show_players",true)){static Module* teamsM=ModuleById("teams");nickel::visual::TeamRules rules=TeamRulesOf(teamsM);
     static game::VisualEntity people[64];static int peopleCount=0;static double nextPeople=0;if(ImGui::GetTime()>=nextPeople){nextPeople=ImGui::GetTime()+.1;peopleCount=game::ReadVisualEntities(people,64,zoom*1.42f+1,&rules);}
     const float unit=icon*.5f/7;
     for(int i=0;i<peopleCount;++i){const auto& e=people[i];if(!e.player||e.invisible)continue;const float px=(e.minimum[0]+e.maximum[0])*.5f-player.position[0],pz=(e.minimum[2]+e.maximum[2])*.5f-player.position[2];
      const ImVec2 way=project(px,pz),at(centre.x+way.x*scale,centre.y+way.y*scale);if(std::fabs(at.x-centre.x)>size/2||std::fabs(at.y-centre.y)>size/2)continue;
      float theirYaw=0;{uint64_t rot{};float v[2]{};if(e.actor&&game::Read(e.actor+0x250,&rot)&&rot&&game::Read(rot+4,&v[1]))theirYaw=v[1];}
      const float r=theirYaw*3.14159265f/180;const ImVec2 dir=project(-std::sin(r),std::cos(r)),right(-dir.y,dir.x);
      auto pt=[&](float side,float ahead,float grow){return ImVec2(at.x+(right.x*side+dir.x*ahead)*unit*grow,at.y+(right.y*side+dir.y*ahead)*unit*grow);};
      ImVec4 c=pick(e.teammate?"teammate_color":"player_color",e.teammate?ImVec4(.3f,1,.3f,1):ImVec4(1,1,1,1));c.w*=opacity;
      const int face=wantFaces?atlas.PlayerFace(e.actor,ImGui::GetTime(),[&](uint64_t actor,unsigned char* px){game::Context pc{};return game::Local(&pc)&&game::SkinFaceOf(pc,actor,px);}):-1;
      if(face>=0&&GetImage(4)){   // skin face in a frame of the player / teammate colour, with a notch towards the head direction
       const float half=icon*.6f;ImVec2 uv0,uv1;faceUv(face,&uv0,&uv1);const ImU32 col=ImGui::ColorConvertFloat4ToU32(c);const ImVec2 lo(at.x-half,at.y-half),hi(at.x+half,at.y+half);
       d->AddTriangleFilled(ImVec2(at.x+dir.x*(half+4*k)*1.f,at.y+dir.y*(half+4*k)),ImVec2(at.x+dir.x*half+right.x*half*.55f,at.y+dir.y*half+right.y*half*.55f),ImVec2(at.x+dir.x*half-right.x*half*.55f,at.y+dir.y*half-right.y*half*.55f),col);
       d->AddRectFilled(ImVec2(lo.x-k*1.5f,lo.y-k*1.5f),ImVec2(hi.x+k*1.5f,hi.y+k*1.5f),col,2*k);d->AddImage(GetImage(4),lo,hi,uv0,uv1,IM_COL32(255,255,255,(int)(255*opacity)));continue;}
      for(int pass=0;pass<2;++pass){const float grow=pass?1.f:1.4f;const ImU32 col=pass?ImGui::ColorConvertFloat4ToU32(c):IM_COL32(0,0,0,(int)(200*opacity));d->AddTriangleFilled(pt(0,7,grow),pt(5,-5,grow),pt(0,-2,grow),col);d->AddTriangleFilled(pt(0,7,grow),pt(0,-2,grow),pt(-5,-5,grow),col);}}}}
   {const ImVec2 forward=rotate?ImVec2(0,-1):ImVec2(hx,hz),right(-forward.y,forward.x);auto point=[&](float side,float ahead,float grow){return ImVec2(centre.x+(right.x*side+forward.x*ahead)*k*grow,centre.y+(right.y*side+forward.y*ahead)*k*grow);};
    for(int pass=0;pass<2;++pass){const float grow=pass?1.f:1.4f;const ImU32 color=pass?IM_COL32(255,255,255,(int)(255*opacity)):IM_COL32(0,0,0,(int)(200*opacity));d->AddTriangleFilled(point(0,7,grow),point(5,-5,grow),point(0,-2,grow),color);d->AddTriangleFilled(point(0,7,grow),point(0,-2,grow),point(-5,-5,grow),color);}}
   d->PopClipRect();d->AddRect(place.lo,corner,IM_COL32(0,0,0,(int)(220*opacity)),0,0,2*k);
   auto write=[&](float size12,ImVec2 middle,const char* text,int shade){const float w=hudWidth(bold,size12,text),offset=std::clamp(std::floor(size12/18),1.f,2.f);hudText(bold,size12,ImVec2(middle.x-w/2+offset,middle.y+offset),text,IM_COL32(0,0,0,shade));hudText(bold,size12,ImVec2(middle.x-w/2,middle.y),text,IM_COL32(255,255,255,(int)(255*opacity)));};
   if(Flag(*minimap,"show_compass",true)){const char* letters[4]={"N","E","S","W"};const float axis[4][2]={{0,-1},{1,0},{0,1},{-1,0}};for(int i=0;i<4;++i){const ImVec2 way=project(axis[i][0],axis[i][1]);write(12*k,ImVec2(centre.x+way.x*(size/2-8*k),centre.y+way.y*(size/2-8*k)-6*k),letters[i],(int)(160*opacity));}}
   if(coords){char text[64]{};snprintf(text,sizeof(text),"%d %d %d",(int)std::floor(player.position[0]),(int)std::floor(player.position[1]-1.62f),(int)std::floor(player.position[2]));write(14*k,ImVec2(centre.x,place.y+size+3*k),text,160);}
   drawn.push_back("minimap");}
  // ---- Full-screen map (World Map Key): 0.12 s fade; opens centred on the player at 2 px per block; panel = screen minus
  // 4 % on each side; wheel scales by 1.25 per notch (1/32 .. 16) around the cursor; drag pans. Black veil (150), Background
  // Color fill, player arrow, 2 u border (220), "X {}  Z {}" under the cursor at the bottom right (14 u),
  // u = min(W/1280, H/720). The texture is rebuilt from the seen-chunk store at most five times per second,
  // 1 texel = bpt blocks, bpt = smallest power of 2 with scale x bpt >= 1.
  {static float fade=0;static double viewX=0,viewZ=0;static float viewScale=2;static bool wasOpen=false;if(!live)mapOpen=false;fade=std::clamp(fade+(mapOpen?1.f:-1.f)*io.DeltaTime/.12f,0.f,1.f);
   if(mapOpen&&!wasOpen){viewX=player.position[0];viewZ=player.position[2];viewScale=2;}wasOpen=mapOpen;
   if(fade>0&&live){const float u=std::min(io.DisplaySize.x/1280.f,io.DisplaySize.y/720.f);const ImVec2 lo(io.DisplaySize.x*.04f,io.DisplaySize.y*.04f),hi(io.DisplaySize.x*.96f,io.DisplaySize.y*.96f),middle(io.DisplaySize.x/2,io.DisplaySize.y/2);
    const bool inside=io.MousePos.x>=lo.x&&io.MousePos.x<hi.x&&io.MousePos.y>=lo.y&&io.MousePos.y<hi.y;
    if(mapOpen&&inside&&io.MouseWheel!=0){const float before=viewScale;viewScale=std::clamp(viewScale*std::pow(1.25f,io.MouseWheel),.03125f,16.f);viewX+=(io.MousePos.x-middle.x)/before-(io.MousePos.x-middle.x)/viewScale;viewZ+=(io.MousePos.y-middle.y)/before-(io.MousePos.y-middle.y)/viewScale;}
    if(mapOpen&&ImGui::IsMouseDragging(0,0)){viewX-=io.MouseDelta.x/viewScale;viewZ-=io.MouseDelta.y/viewScale;}
    static std::vector<uint32_t> pixels;static int builtW=0,builtH=0,builtStep=1;static double builtX=0,builtZ=0,nextBuild=0;static uint64_t builtVersion=~0ull;
    int step=1;while(viewScale*step<1&&step<4096)step*=2;const double halfX=(hi.x-lo.x)/2/viewScale,halfZ=(hi.y-lo.y)/2/viewScale;
    const bool covered=builtW>0&&builtStep==step&&viewX-halfX>=builtX&&viewZ-halfZ>=builtZ&&viewX+halfX<=builtX+(double)builtW*step&&viewZ+halfZ<=builtZ+(double)builtH*step;
    if(UpdateImage&&ImGui::GetTime()>=nextBuild&&(!covered||builtVersion!=map.storeVersion||!GetImage(3))){nextBuild=ImGui::GetTime()+.2;
     const double originX=std::floor((viewX-halfX*1.25)/step)*step,originZ=std::floor((viewZ-halfZ*1.25)/step)*step;const int w=std::min(2048,(int)std::ceil(halfX*2.5/step)+2),h=std::min(2048,(int)std::ceil(halfZ*2.5/step)+2),centreOffset=step>=16?8:0;pixels.assign((size_t)w*h,0u);
     AcquireSRWLockShared(&map.lock);builtVersion=map.storeVersion;uint64_t lastKey=~0ull;const std::array<uint32_t,256>* tile=nullptr;
     for(int j=0;j<h;++j){const int64_t wz=(int64_t)originZ+(int64_t)j*step+centreOffset;for(int i=0;i<w;++i){const int64_t wx=(int64_t)originX+(int64_t)i*step+centreOffset;const uint64_t key=game::ChunkKey((int)(wx>>4),(int)(wz>>4));
       if(key!=lastKey){lastKey=key;auto found=map.store.find(key);tile=found==map.store.end()?nullptr:&found->second;}if(tile)pixels[(size_t)j*w+i]=step>16?(*tile)[136]:(*tile)[(size_t)(wz&15)*16+(wx&15)];}}
     ReleaseSRWLockShared(&map.lock);if(UpdateImage(3,(const unsigned char*)pixels.data(),w,h)){builtW=w;builtH=h;builtStep=step;builtX=originX;builtZ=originZ;}}
    auto screen=[&](double wx,double wz){return ImVec2(middle.x+(float)((wx-viewX)*viewScale),middle.y+(float)((wz-viewZ)*viewScale));};
    d->AddRectFilled(ImVec2(0,0),io.DisplaySize,IM_COL32(0,0,0,(int)(150*fade)));ImVec4 back=pick("background_color",ImVec4(.05f,.05f,.06f,.75f));back.w*=fade;d->AddRectFilled(lo,hi,ImGui::ColorConvertFloat4ToU32(back));
    d->PushClipRect(lo,hi,true);if(GetImage&&GetImage(3)&&builtW>0)d->AddImage(GetImage(3),screen(builtX,builtZ),screen(builtX+(double)builtW*builtStep,builtZ+(double)builtH*builtStep),ImVec2(0,0),ImVec2(1,1),IM_COL32(255,255,255,(int)(255*fade)));
    {const ImVec2 me=screen(player.position[0],player.position[2]);const float grow=1.5f*u,margin=7*grow;if(me.x>lo.x+margin&&me.x<hi.x-margin&&me.y>lo.y+margin&&me.y<hi.y-margin){const float radians=yaw*3.14159265f/180;const ImVec2 forward(-std::sin(radians),std::cos(radians)),right(-forward.y,forward.x);
      auto point=[&](float side,float ahead,float more){return ImVec2(me.x+(right.x*side+forward.x*ahead)*grow*more,me.y+(right.y*side+forward.y*ahead)*grow*more);};
      for(int pass=0;pass<2;++pass){const float more=pass?1.f:1.4f;const ImU32 color=pass?IM_COL32(255,255,255,(int)(255*fade)):IM_COL32(0,0,0,(int)(200*fade));d->AddTriangleFilled(point(0,7,more),point(5,-5,more),point(0,-2,more),color);d->AddTriangleFilled(point(0,7,more),point(0,-2,more),point(-5,-5,more),color);}}}
    d->PopClipRect();d->AddRect(lo,hi,IM_COL32(0,0,0,(int)(220*fade)),0,0,2*u);
    if(inside){char text[64]{};snprintf(text,sizeof(text),"X %d  Z %d",(int)std::floor(viewX+(io.MousePos.x-middle.x)/viewScale),(int)std::floor(viewZ+(io.MousePos.y-middle.y)/viewScale));const float font=14*u,w=hudWidth(bold,font,text),offset=std::clamp(std::floor(font/18),1.f,2.f);const ImVec2 at(hi.x-8*u-w,hi.y-8*u-font);
     hudText(bold,font,ImVec2(at.x+offset,at.y+offset),text,IM_COL32(0,0,0,(int)(160*fade)));hudText(bold,font,at,text,IM_COL32(255,255,255,(int)(255*fade)));}}}
 }
 // ---- Ping: one "{} ms" line, refreshed at most Update Frequency times per second; "0 ms" when the player exists but
 // latency is unavailable; nothing without a player, except "42 ms" in the layout editor. Latency comes from two game
 // calls (game::ReadPing); the first read and every stage change are logged as "Ping: ...".
 static Module* ping=ModuleById("ping");
 if(ping&&ping->enabled){
  static double next=0;static int state=-1,logged=-99;static int64_t value=0;
  if(ImGui::GetTime()>=next){next=ImGui::GetTime()+1.0/std::clamp(num(*ping,"update_frequency",5),1.f,30.f);game::PingTrace trace{};int64_t read=0;state=game::ReadPing(&read,&trace);if(state==1)value=read;
   const int mark=state*16+trace.step;if(mark!=logged&&WriteLog){logged=mark;char line[160]{};snprintf(line,sizeof(line),"Ping: result %d at step %d, getter RVA 0x%llx, status RVA 0x%llx, value %lld ms",state,trace.step,(unsigned long long)trace.getter,(unsigned long long)trace.status,(long long)(state==1?read:0));WriteLog(line);}}
  std::string line;if(state>=0)line=std::to_string(state==1?value:0)+" ms";else if(hudEditing)line="42 ms";
  if(!line.empty())textBox("ping",*ping,{line},10,stack);
 }
 static Module* cps=ModuleById("cps");static Module* keys=ModuleById("keystrokes");
 // clicks: rising edges of the button state, counted over the last second (no click is sent to the game)
 static std::vector<double> clicks[2];static bool held[2]{};const double time=ImGui::GetTime();const int buttons[2]={VK_LBUTTON,VK_RBUTTON};
 for(int i=0;i<2;++i){const bool down=(GetAsyncKeyState(buttons[i])&0x8000)!=0;if(down&&!held[i])clicks[i].push_back(time);for(int k=TakeSyntheticClicks(i);k>0;--k)clicks[i].push_back(time);held[i]=down;while(!clicks[i].empty()&&clicks[i].front()<time-1)clicks[i].erase(clicks[i].begin());}
 if(cps&&cps->enabled){char text[40]{};if(Flag(*cps,"right_click",false))snprintf(text,sizeof(text),"%d | %d CPS",(int)clicks[0].size(),(int)clicks[1].size());else snprintf(text,sizeof(text),"%d CPS",(int)clicks[0].size());badge("cps",*cps,text);}
 if(keys&&keys->enabled){
  // Keystrokes: 22 px keys on a 25 px pitch (W on top, A S D below), a 72 x 13 space bar with a stroke, then 34.5 x 22
  // LMB / RMB with Mouse Buttons. Radius 4, border in Color at 65 %, pressed keys filled with Color at 75 %,
  // 12 px labels. Total size 72 x 88.
  auto& place=Place("keystrokes",10,stack);const float k=place.scale;auto fill=Find(*keys,"background_color");auto tint=Find(*keys,"color");const ImVec4 base=fill&&fill->kind=="color"?ParseColor(fill->text):ImVec4(0,0,0,.51f),ink=tint&&tint->kind=="color"?ParseColor(tint->text):ImVec4(1,1,1,1);
  const bool background=Flag(*keys,"background",true),shadow=Flag(*keys,"text_shadow",true);const float size=12*k/FontEmFactor;
  auto key=[&](const char* label,int vk,float x,float y,float w,float h){const bool down=vk>0&&(GetAsyncKeyState(vk)&0x8000)!=0;ImVec2 a(place.x+x*k,place.y+y*k),b(place.x+(x+w)*k,place.y+(y+h)*k);
   if(down)d->AddRectFilled(a,b,ImGui::ColorConvertFloat4ToU32(ImVec4(ink.x,ink.y,ink.z,ink.w*.75f)),4*k);else if(background)d->AddRectFilled(a,b,ImGui::ColorConvertFloat4ToU32(base),4*k);
   d->AddRect(a,b,ImGui::ColorConvertFloat4ToU32(ImVec4(ink.x,ink.y,ink.z,ink.w*.65f)),4*k,0,std::max(1.f,k));
   if(!label[0]){d->AddLine(ImVec2((a.x+b.x)/2-w*.2f*k,(a.y+b.y)/2),ImVec2((a.x+b.x)/2+w*.2f*k,(a.y+b.y)/2),ImGui::ColorConvertFloat4ToU32(ImVec4(down?.08f:ink.x,down?.07f:ink.y,down?.1f:ink.z,ink.w*.65f)),std::max(1.f,2*k));return;}
   const ImVec2 at((a.x+b.x-TextWidth(bold,size,label))/2,(a.y+b.y)/2-.7f*size);const float offset=std::max(1.f,std::round(12*k/18));
   if(shadow&&!down)Label(d,bold,size,ImVec2(at.x+offset,at.y+offset),label,Rgba(0,0,0,.6275f*ink.w));Label(d,bold,size,at,label,down?Rgba(20,19,27):ImGui::ColorConvertFloat4ToU32(ink));};
  // default QWERTY keys;  // keys chosen in the layout editor override them; the key being re-bound shows "..."
  const GameKeys& bind=CurrentGameKeys();const auto custom=KeystrokeCustom(*keys);const int game[7]={bind.forward,bind.left,bind.back,bind.right,bind.jump,bind.attack,bind.use};
  static const float spots[7][4]={{25,0,22,22},{0,25,22,22},{25,25,22,22},{50,25,22,22},{0,50,72,13},{0,66,34.5f,22},{37.5f,66,34.5f,22}};
  const bool mice=Flag(*keys,"mouse_buttons",true);float height=mice?88.f:63.f;
  for(int i=0;i<7;++i){keystrokeDrawn[i]=false;if(i>=5&&!mice)continue;const int code=custom[i]?custom[i]:game[i];const bool waiting=hudEditing&&keystrokeListening==i;
   const std::string label=waiting?"...":i==4?"":BindingLabel(code);key(label.c_str(),waiting?0:BindingVk(code),spots[i][0],spots[i][1],spots[i][2],spots[i][3]);
   keystrokeRects[i][0]=ImVec2(place.x+spots[i][0]*k,place.y+spots[i][1]*k);keystrokeRects[i][1]=ImVec2(place.x+(spots[i][0]+spots[i][2])*k,place.y+(spots[i][1]+spots[i][3])*k);keystrokeDrawn[i]=true;
   if(waiting)d->AddRect(keystrokeRects[i][0],keystrokeRects[i][1],Acc(1),4*k,0,std::max(1.f,2*k));}
  place.lo=ImVec2(place.x,place.y);place.hi=ImVec2(place.x+72*k,place.y+height*k);stack+=height*k+8;drawn.push_back("keystrokes");
 }
 // ---- Spotify Manager: 320 x 110 card, default position (0.02, 0.98). Six-layer black shadow at 2.5 %, #1b1b1b fill at
 // 96 % with radius 20, 6 % white border; 84 x 84 cover at (12, 12) (cropped square, radius 10; logo scaled to 72 wide
 // when missing). Title 14 #fafafa at (108, 13), artist 12 #919191 at (108, 34), clipped to 198. Buttons centred at y = 65:
 // previous (x 120, 18), play / pause (x 155, 28, filled disc), next (x 190, 18); hover = 9 % white disc of radius
 // 0.6 x size, disabled = 30 %. "m:ss / m:ss" 10 #878787 right-aligned; 2 px bar at y = 94, track #464646, fill #f5f5f5,
 // click to seek. Controls go through the Windows media API (media.cpp). Without a player: "Spotify" plus
 // "Open Spotify to start listening" or "Media controls unavailable". The card is blurred underneath with the menu's
 // blur primitive.
 static Module* spotify=ModuleById("spotify_manager");
 if(spotify&&spotify->enabled){
  static const bool review=GetEnvironmentVariableA("NICKEL_REVIEW_HUD",nullptr,0)!=0;if(!review)media::Start();const media::State state=review?media::State{}:media::Get();const float a=Animate("spotify-card",1.f,io.DeltaTime,12);
  if(a>.01f){
   auto& place=Place("spotify_manager",10,io.DisplaySize.y-130);const float k=place.scale,x=place.x,y=place.y,left=x+108*k,right=left+198*k;place.lo=ImVec2(x,y);place.hi=ImVec2(x+320*k,y+110*k);
   const bool interactive=MenuIsOpen()&&!hudEditing;auto tone=[&](int red,int green,int blue,float alpha){return IM_COL32(red,green,blue,(int)(std::clamp(alpha*a,0.f,1.f)*255));};
   // blur under the card: same primitive as the menu background blur (RequestBlur), game frame blurred and clipped to
   // the card, radius 20*s
   if(RequestBlur&&!review){const ImVec2 screen=io.DisplaySize;if(screen.x>0&&screen.y>0)if(auto blurred=RequestBlur(d))d->AddImageRounded(blurred,place.lo,place.hi,ImVec2(place.lo.x/screen.x,place.lo.y/screen.y),ImVec2(place.hi.x/screen.x,place.hi.y/screen.y),IM_COL32(255,255,255,(int)(std::clamp(a,0.f,1.f)*255)),20*k);}
   for(int i=6;i>=1;--i)d->AddRectFilled(ImVec2(x-i*k,y-i*k+3*k),ImVec2(x+(320+i)*k,y+(113+i)*k),tone(0,0,0,.025f),(20+i)*k);
   d->AddRectFilled(place.lo,place.hi,tone(27,27,27,.96f),20*k);d->AddRect(place.lo,place.hi,tone(255,255,255,.06f),20*k,0,1);
   static unsigned shownCover=0;static int coverW=0,coverH=0;static double nextCover=0;bool haveCover=false;
   if(!review&&UpdateImage&&GetImage){if(ImGui::GetTime()>=nextCover){nextCover=ImGui::GetTime()+.5;static std::vector<unsigned char> pixels;int w=0,h=0;const unsigned serial=media::Cover(&pixels,&w,&h);if(!serial)shownCover=0;else if(serial!=shownCover||!GetImage(1)){if(UpdateImage(1,pixels.data(),w,h)){shownCover=serial;coverW=w;coverH=h;}}}
    haveCover=shownCover&&GetImage(1)&&state.available&&coverW>0&&coverH>0;}
   if(haveCover){const float ratio=(float)coverW/coverH;ImVec2 uv0(0,0),uv1(1,1);if(ratio<=1){uv0.y=(1-ratio)/2;uv1.y=1-uv0.y;}else{uv0.x=(1-1/ratio)/2;uv1.x=1-uv0.x;}d->AddImageRounded(GetImage(1),ImVec2(x+12*k,y+12*k),ImVec2(x+96*k,y+96*k),uv0,uv1,tone(255,255,255,1),10*k);}
   else if(logo){const float w=60*k,h=w;   /* square Nickel mark */const ImVec2 corner(x+12*k+(84*k-w)/2,y+12*k+(84*k-h)/2);d->AddImage(logo,corner,ImVec2(corner.x+w,corner.y+h),ImVec2(0,0),ImVec2(1,1),tone(255,255,255,1));}
   const std::string title=state.available?state.title:std::string("Spotify"),second=state.available?state.artist:std::string(state.error?"Media controls unavailable":"Open Spotify to start listening");
   d->PushClipRect(ImVec2(left,y),ImVec2(right,y+110*k),true);hudText(bold,14*k,ImVec2(left,y+13*k),title.c_str(),tone(250,250,250,1));hudText(regular,12*k,ImVec2(left,y+34*k),second.c_str(),tone(145,145,145,1));d->PopClipRect();
   auto button=[&](int kind,float centre,float size,bool enabled,bool filled){
    const ImVec2 c(left+centre*k,y+65*k);const float full=size*k,alpha=enabled?1.f:.3f;const bool hover=interactive&&enabled&&std::fabs(io.MousePos.x-c.x)<=full/2&&std::fabs(io.MousePos.y-c.y)<=full/2;
    if(hover)d->AddCircleFilled(c,full*.6f,tone(255,255,255,.09f));if(filled)d->AddCircleFilled(c,full*.5f,hover?tone(255,255,255,alpha):tone(235,235,235,alpha));
    static const char* glyph[4]={"\xee\x80\xa5","\xee\x80\xa4","\xee\x80\xa6","\xee\x80\xa3"};const float g=filled?full*.56f:full;const ImU32 ink=filled?tone(28,28,28,alpha):tone(255,255,255,alpha);
    (void)glyph;{const float h=g*.36f;   // play, pause, previous and next icons are drawn as paths
     if(kind==0)d->AddTriangleFilled(ImVec2(c.x-h*.7f,c.y-h),ImVec2(c.x-h*.7f,c.y+h),ImVec2(c.x+h,c.y),ink);
     else if(kind==1){d->AddRectFilled(ImVec2(c.x-h*.8f,c.y-h),ImVec2(c.x-h*.25f,c.y+h),ink,h*.15f);d->AddRectFilled(ImVec2(c.x+h*.25f,c.y-h),ImVec2(c.x+h*.8f,c.y+h),ink,h*.15f);}
     else if(kind==2){d->AddRectFilled(ImVec2(c.x-h,c.y-h),ImVec2(c.x-h*.7f,c.y+h),ink);d->AddTriangleFilled(ImVec2(c.x+h,c.y-h),ImVec2(c.x+h,c.y+h),ImVec2(c.x-h*.6f,c.y),ink);}
     else{d->AddRectFilled(ImVec2(c.x+h*.7f,c.y-h),ImVec2(c.x+h,c.y+h),ink);d->AddTriangleFilled(ImVec2(c.x-h,c.y-h),ImVec2(c.x-h,c.y+h),ImVec2(c.x+h*.6f,c.y),ink);}}
    return hover&&ImGui::IsMouseClicked(0);};
   if(button(2,12,18,state.canPrevious,false))media::Command(1);if(button(state.playing?1:0,47,28,state.canToggle,true))media::Command(0);if(button(3,82,18,state.canNext,false))media::Command(2);
   const int now=(int)std::max(0.0,state.position),whole=(int)std::max(0.0,state.duration);char time[40]{};snprintf(time,sizeof(time),"%d:%02d / %d:%02d",now/60,now%60,whole/60,whole%60);
   hudText(regular,10*k,ImVec2(right-hudWidth(regular,10*k,time),y+60*k),time,tone(135,135,135,1));
   const float bar=y+94*k,filled=state.duration>0?(float)std::clamp(state.position/state.duration,0.0,1.0):0;d->AddRectFilled(ImVec2(left,bar),ImVec2(right,bar+2*k),tone(70,70,70,1),k);if(filled>0)d->AddRectFilled(ImVec2(left,bar),ImVec2(left+filled*198*k,bar+2*k),tone(245,245,245,1),k);
   if(interactive&&state.canSeek&&state.duration>0&&io.MousePos.x>=left&&io.MousePos.x<right&&io.MousePos.y>=bar-5*k&&io.MousePos.y<bar+7*k&&ImGui::IsMouseClicked(0))media::Command(3,(io.MousePos.x-left)/(198*k));
   drawn.push_back("spotify_manager");
  }
 }
 if(list&&list->enabled){
  // Module List. s = scale x Scale setting, s1 = 1.1 s: accent bar 3 s1, gap 7.5 s1, font 25.5 s1, row 27 s1,
  // margin 10 s. Rows are sorted widest first; Arrangement "Descending" anchors the list at the bottom with the watermark
  // below. Black background 140/255, square corners, vertical gradient bar, text coloured by the gradient at the row
  // middle, t = (y - top) / total height x 1.15 + time x 0.15 x Gradient Speed.
  // Row entry slides in from the edge (smoothstep 3p^2 - 2p^3); vertical offset eases to its slot (1 - exp(-16 dt)).
  // Appearance: p += (target - p)(1 - exp(-14 dt)); a disabled module's row slides out and is removed below 0.01.
  // "[X]" keybind in #aaaaaa after the name (Show Keybinds). Never listed: Module List, "ClickGUI" and, with Module
  // Blacklist on, any module whose name contains a word of Blacklisted Modules (case-insensitive). Show Extra Info uses
  // ExtraInfo() above, wrapped per Extra Info Style. Glow is approximated with offset passes.
  // The watermark is the logo followed by "Nickel" in the gradient, in a 300 x Height box.
  auto alignment=Find(*list,"alignment");auto arrangement=Find(*list,"arrangement");const bool left=alignment&&alignment->text=="Left",bottom=arrangement&&arrangement->text=="Descending";
  auto& place=Place("module_list",left?10:io.DisplaySize.x-10,10);
  const float s=place.scale*num(*list,"scale",1),s1=s*1.1f,pad=3*s1,gap=7.5f*s1,font=25.5f*s1,row=27*s1,mark=num(*list,"height",60),dir=left?1.f:-1.f;
  const bool background=Flag(*list,"background",true),binds=Flag(*list,"show_keybinds",false),lowercase=Flag(*list,"lowercase",false),watermark=Flag(*list,"show_watermark",true);
  const float glow=Flag(*list,"glow",true)?num(*list,"glow_strength",1)*1.6f:0;const auto g=stops(*list,"gradient");const float phase=(float)ImGui::GetTime()*.15f*num(*list,"gradient_speed",3);
  struct Row{const Module* item;std::string name,extra;float nameWidth,width;};std::vector<Row> rows;static std::map<const Module*,ImVec2> state;   // x = entry progress, y = vertical offset (units of 29.7)
  auto lowered=[&](std::string text){if(lowercase)std::transform(text.begin(),text.end(),text.begin(),[](unsigned char ch){return (char)std::tolower(ch);});return text;};
  const bool extras=Flag(*list,"show_extra_info",false);auto style=Find(*list,"extra_info_style");const char first=style&&!style->text.empty()?style->text[0]:'N';std::vector<std::string> banned;
  if(Flag(*list,"module_blacklist",false))if(auto words=Find(*list,"blacklisted_modules")){size_t from=0;const std::string& all=words->text;while(from<all.size()){size_t to=all.find(',',from);if(to==std::string::npos)to=all.size();if(to>from){std::string word=all.substr(from,to-from);std::transform(word.begin(),word.end(),word.begin(),[](unsigned char ch){return (char)std::tolower(ch);});banned.push_back(word);}from=to+1;}}
  static std::map<const Module*,float> target;
  for(auto& item:modules){if(&item==list||item.name=="ClickGUI")continue;bool listed=item.enabled;
   if(listed&&!banned.empty()){std::string plain=item.name;std::transform(plain.begin(),plain.end(),plain.begin(),[](unsigned char ch){return (char)std::tolower(ch);});for(auto& word:banned)if(plain.find(word)!=std::string::npos)listed=false;}
   target[&item]=listed?1.f:0.f;if(!listed){auto found=state.find(&item);if(found==state.end())continue;if(found->second.x<.01f){state.erase(found);continue;}}
   std::string name=lowered(item.name),extra;if(extras){std::string value=ExtraInfo(item);if(!value.empty())extra+=lowered(first=='('?" ("+value+")":first=='['?" ["+value+"]":first=='-'?" - "+value:" "+value);}
   auto bound=keybinds.find(item.id);if(binds&&bound!=keybinds.end()){std::string key=bound->second;std::transform(key.begin(),key.end(),key.begin(),[](unsigned char ch){return (char)std::toupper(ch);});extra+=" ["+key+"]";}
   const float nameWidth=hudWidth(regular,font,name.c_str());rows.push_back({&item,name,extra,nameWidth,nameWidth+(extra.empty()?0:hudWidth(regular,font,extra.c_str()))});}
  if(rows.empty()&&hudEditing){const float width=hudWidth(regular,font,"Module List");rows.push_back({nullptr,"Module List","",width,width});}
  std::stable_sort(rows.begin(),rows.end(),[](const Row& a,const Row& b){return a.width>b.width;});
  const float edge=left?place.x+10*s:place.x-10*s,textEdge=edge+(pad+gap)*dir;float top=place.y+10*s,widest=watermark?305*s:40*s;for(auto& r:rows)widest=std::max(widest,r.width+18*s1);
  const float W=widest+10*s,H=20*s+(watermark?(mark+4)*s:0)+rows.size()*row;float base=place.y+H-10*s;
  if(watermark){
   const float wy=bottom?place.y+H-mark*s-5*s:top-5*s,wx=left?edge-5*s:edge-300*s+5*s,high=mark*s,planet=logo?high:0,size=high*.62f,wordWidth=hudWidth(bold,size,"Nickel");
   float x=left?wx:wx+300*s-planet-high*.12f-wordWidth;if(logo){d->AddImage(logo,ImVec2(x,wy),ImVec2(x+planet,wy+high));x+=planet+high*.12f;}
   for(const char* c="Nickel";*c;++c){const char one[2]={*c,0};const float advance=hudWidth(bold,size,one);const ImU32 color=wave(g,(x+advance/2-wx)/(300*s)*1.15f+phase);
    if(glow>0)for(int ring=0;ring<8;++ring)hudText(bold,size,ImVec2(x+std::cos(ring*.785f)*glow*s,wy+(high-size)/2+std::sin(ring*.785f)*glow*s),one,(color&0x00ffffff)|0x16000000);
    hudText(bold,size,ImVec2(x,wy+(high-size)/2),one,color);x+=advance;}
   if(bottom)base-=(mark+4)*s;else top+=(mark+4)*s;}
  const float total=std::max(1.f,rows.size()*row),ease=1-std::exp(-16*io.DeltaTime);auto phaseAt=[&](float t){return (t>1?1.15f:std::max(t,0.f)*1.15f)+phase;};
  for(size_t i=0;i<rows.size();++i){auto& r=rows[i];float a=1,offset=i*row;
   if(r.item){auto found=state.find(r.item);if(found==state.end())found=state.emplace(r.item,ImVec2(0,i*29.7f)).first;auto& value=found->second;value.x+=(target[r.item]-value.x)*(1-std::exp(-14*io.DeltaTime));value.y+=(i*29.7f-value.y)*ease;const float t=std::clamp(value.x,0.f,1.f);a=(3-2*t)*t*t;offset=value.y/29.7f*row;}
   const float y=bottom?base-row-offset:top+offset,slide=(pad+r.width+15*s1)*(1-a),x=left?textEdge-slide:textEdge-r.width+slide;
   const ImU32 middle=wave(g,phaseAt((row/2+y-top)/total)),upper=wave(g,phaseAt((y-top)/total)),under=wave(g,phaseAt((y+row-top)/total));
   if(background)d->AddRectFilled(ImVec2(left?edge+pad:textEdge-r.width-gap,y),ImVec2(left?textEdge+r.width+gap:edge-pad,y+row),IM_COL32(0,0,0,(int)(a*140)));
   const ImVec2 b0(left?edge:edge-pad,y),b1(left?edge+pad:edge,y+row);
   if(glow>0){for(int ring=3;ring>=1;--ring){const float out=ring*glow*.35f*2*s;d->AddRectFilled(ImVec2(b0.x-out,b0.y),ImVec2(b1.x+out,b1.y),(middle&0x00ffffff)|0x1c000000,out);}
    for(int ring=0;ring<8;++ring)hudText(regular,font,ImVec2(x+std::cos(ring*.785f)*glow*s*.9f,y+std::sin(ring*.785f)*glow*s*.9f),r.name.c_str(),(middle&0x00ffffff)|0x16000000);}
   d->AddRectFilledMultiColor(b0,b1,upper,upper,under,under);hudText(regular,font,ImVec2(x,y),r.name.c_str(),(middle&0x00ffffff)|((ImU32)(a*255)<<24));
   if(!r.extra.empty())hudText(regular,font,ImVec2(x+r.nameWidth,y),r.extra.c_str(),IM_COL32(170,170,170,(int)(a*255)));}
  place.right=!left;place.lo=ImVec2(left?place.x:place.x-W,place.y);place.hi=ImVec2(left?place.x+W:place.x,place.y+H);if(!rows.empty()||watermark)drawn.push_back("module_list");
 }
 // first frame of an element without a saved position: place it at its default fraction of the free space
 for(auto& entry:hudDefaults){auto found=hudPlaces.find(entry.id);if(found==hudPlaces.end()||!found->second.pending)continue;auto& place=found->second;const float w=place.hi.x-place.lo.x,h=place.hi.y-place.lo.y;if(w<=0||h<=0)continue;
  const float left=entry.x*std::max(0.f,io.DisplaySize.x-w);place.x=place.right?left+w:left;place.y=entry.y*std::max(0.f,io.DisplaySize.y-h);place.pending=false;}
 if(hudFade<=0){hudDragging.clear();return;}
 // ---- HUD layout editor: banner and veil, element frames, grab, move / resize and snapping.
 // es = min(width/1280, height/720).
 const ImVec2 mouse=io.MousePos,screen=io.DisplaySize;const ImU32 accent=Acc(hudFade),plain=Rgba(210,204,222,hudFade);
 {const char* title="Press Escape To Exit";const float size=16*es/FontEmFactor;Label(d,semibold,size,ImVec2((screen.x-TextWidth(semibold,size,title))/2,20*es),title,Rgba(239,236,248,hudFade));}
 for(auto& id:drawn){auto& place=hudPlaces[id];const bool chosen=hudSelected==id;const ImU32 ink=chosen?accent:plain;
  // square frame (2 es and purple for the last grabbed element, 1 es otherwise), 6 es handle in the bottom-right corner,
  // name 12 es above (or below when the element touches the top of the screen)
  d->AddRect(place.lo,place.hi,ink,0,0,chosen?2*es:es);d->AddRectFilled(ImVec2(place.hi.x-6*es,place.hi.y-6*es),place.hi,ink,es);
  const Module* owner=ModuleById(id.c_str());Label(d,regular,12*es/FontEmFactor,ImVec2(place.lo.x,place.lo.y<17*es?place.hi.y+2*es:place.lo.y-17*es),owner?owner->name.c_str():id.c_str(),ink);}
 if(hudSnapX)d->AddLine(ImVec2(hudGuide.x,0),ImVec2(hudGuide.x,screen.y),accent,es);
 if(hudSnapY)d->AddLine(ImVec2(0,hudGuide.y),ImVec2(screen.x,hudGuide.y),accent,es);
 if(!hudEditing){hudDragging.clear();hudSnapX=hudSnapY=false;keystrokeListening=-1;return;}
 // Keystrokes: while a key waits, the next mouse button is bound to it (left / right / middle)
 if(keystrokeListening>=0){for(int b=0;b<3;++b)if(io.MouseClicked[b]){KeystrokeBind(keystrokeListening,b==0?-99:b==1?-98:-97);keystrokeListening=-1;hudDragging.clear();return;}}
 // a click that did not move (under 3 px) on one of its keys starts re-binding that key
 if(io.MouseReleased[0]&&hudDragging=="keystrokes"&&!hudResizing){const ImVec2 from=io.MouseClickedPos[0];if(std::fabs(mouse.x-from.x)<3&&std::fabs(mouse.y-from.y)<3)
  for(int i=0;i<7;++i)if(keystrokeDrawn[i]&&from.x>=keystrokeRects[i][0].x&&from.y>=keystrokeRects[i][0].y&&from.x<keystrokeRects[i][1].x&&from.y<keystrokeRects[i][1].y){keystrokeListening=i;break;}}
 if(io.MouseClicked[0]){hudSelected.clear();hudDragging.clear();
  // the last drawn element is on top, so hit-test in reverse; bottom-right corner (6 es) resizes, elsewhere moves
  for(auto it=drawn.rbegin();it!=drawn.rend();++it){auto& place=hudPlaces[*it];if(mouse.x<place.lo.x||mouse.y<place.lo.y||mouse.x>=place.hi.x||mouse.y>=place.hi.y)continue;
   hudSelected=hudDragging=*it;hudResizing=mouse.x>=place.hi.x-6*es&&mouse.y>=place.hi.y-6*es;hudOrigin=place.lo;const ImVec2 held=hudResizing?place.hi:place.lo;hudGrab=ImVec2(mouse.x-held.x,mouse.y-held.y);
   hudBase=ImVec2((place.hi.x-place.lo.x)/place.user,(place.hi.y-place.lo.y)/place.user);hudSnapX=hudSnapY=false;break;}}
 if(hudDragging.empty())return;auto& place=hudPlaces[hudDragging];
 if(!io.MouseDown[0]){SaveHud(hudDragging);hudDragging.clear();hudSnapX=hudSnapY=false;return;}
 if(hudResizing){   // scale = drag projected on the element diagonal, clamped to 0.5..3; the top-left corner stays in place
  const float dx=mouse.x-hudGrab.x-hudOrigin.x,dy=mouse.y-hudGrab.y-hudOrigin.y,length=hudBase.x*hudBase.x+hudBase.y*hudBase.y;
  if(length>1){place.user=std::clamp((dx*hudBase.x+dy*hudBase.y)/length,.5f,3.f);if(place.right)place.x=hudOrigin.x+hudBase.x*place.user;}
  return;}
 // move: guides are the screen edges and centre and the edges and centres of other elements; the element's start,
 // middle or end snaps within 6 es and only releases beyond 6 es x 1.7
 const ImVec2 size(place.hi.x-place.lo.x,place.hi.y-place.lo.y);std::vector<float> guideX{0,screen.x*.5f,screen.x},guideY{0,screen.y*.5f,screen.y};
 for(auto& other:drawn)if(other!=hudDragging){auto& o=hudPlaces[other];guideX.insert(guideX.end(),{o.lo.x,(o.lo.x+o.hi.x)*.5f,o.hi.x});guideY.insert(guideY.end(),{o.lo.y,(o.lo.y+o.hi.y)*.5f,o.hi.y});}
 auto snap=[&](float raw,float length,float extent,const std::vector<float>& guides,float& guide,float& fraction,bool& held){
  const float limit=std::max(0.f,extent-length);
  if(held){const float kept=guide-fraction*length;if(kept>=0&&kept<=limit&&std::fabs(raw-kept)<=6*es*1.7f)return kept;}
  held=false;float best=std::clamp(raw,0.f,limit),nearest=6*es;
  for(float g:guides)for(float f:{0.f,.5f,1.f}){const float value=g-f*length,gap=std::fabs(raw-value);if(value>=0&&value<=limit&&gap<nearest){nearest=gap;best=value;guide=g;fraction=f;held=true;}}
  return best;};
 const float x=snap(mouse.x-hudGrab.x,size.x,screen.x,guideX,hudGuide.x,hudFraction.x,hudSnapX),y=snap(mouse.y-hudGrab.y,size.y,screen.y,guideY,hudGuide.y,hudFraction.y,hudSnapY);
 place.x=place.right?x+size.x:x;place.y=y;
}
}
namespace {
// Configs screen (local configs only). A config is a copy of module-preferences.ini in the neighbouring configs folder.
// Labels: "New config", "No configs yet. New saves your current setup.", Load / Rename / Duplicate / Delete,
// "Saved {} to your configs.", "Loaded config '{}'", "Duplicated as {}.".
std::wstring ConfigFolder(){auto folder=configPath.substr(0,configPath.find_last_of(L"\\/"))+L"\\configs";static std::wstring made;if(made!=folder){CreateDirectoryW(folder.c_str(),nullptr);made=folder;}return folder;}   // created once, not on every call
std::vector<std::string> configNamesCache;double configNamesAt=-10;
std::vector<std::string> ConfigNamesScan(){std::vector<std::string> found;WIN32_FIND_DATAW data{};auto handle=FindFirstFileW((ConfigFolder()+L"\\*.ini").c_str(),&data);if(handle==INVALID_HANDLE_VALUE)return found;
 do{std::wstring name=data.cFileName;found.push_back(Utf8(name.substr(0,name.size()-4)));}while(FindNextFileW(handle,&data));FindClose(handle);std::sort(found.begin(),found.end());return found;}
// Folder scan, kept for 1 s by the Configs screen (it used to list the folder every frame).
std::vector<std::string> ConfigNames(){configNamesCache=ConfigNamesScan();configNamesAt=ImGui::GetTime();return configNamesCache;}
const std::vector<std::string>& ConfigNamesCached(){if(ImGui::GetTime()-configNamesAt>1)ConfigNames();return configNamesCache;}
std::wstring ConfigFile(const std::string& name){return ConfigFolder()+L"\\"+Wide(name)+L".ini";}
void SetCurrentConfig(const std::string& name){currentConfig=name;WritePrivateProfileStringW(L"menu",L"current-config",Wide(name).c_str(),configPath.c_str());}
// ---- Configs screen ----
// Layout, search + "New config", list and empty states, rows (56 u + 6 u, badges, Load, keybind), per-config keybind,
// relative times, details panel and MODULES block. U = 242/210 of our layout units.
std::string configStatus;double configStatusAt=-10,deleteArmed=-1;bool renaming=false;char renameText[65]{},configSearch[65]{},globalSearch[65]{};
std::string configSelected;int configSection=0,globalTag=0;std::vector<std::string> configListed;
void Say(const std::string& text){configStatus=text;configStatusAt=ImGui::GetTime();}
// Import / Export results: the menu closes while the Windows dialog has the focus, so they also show as a notice.
void SayAndNotify(const std::string& text){Say(text);if(notices.size()<16)notices.push_back({8,"Configs",text});}
std::wstring ConfigKeyFile(){return ConfigFolder()+L"\\keys.dat";}
void LoadConfigKeys(const std::vector<std::string>& names){configKeys.clear();configKeyNames.clear();for(auto& name:names){const int code=GetPrivateProfileIntW(L"keys",Wide(name).c_str(),0,ConfigKeyFile().c_str());wchar_t text[65]{};GetPrivateProfileStringW(L"names",Wide(name).c_str(),L"",text,65,ConfigKeyFile().c_str());if(code>0&&text[0]){configKeys[name]=code;configKeyNames[name]=Utf8(text);}}}
void SetConfigKey(const std::string& name,int code,const std::string& keyName){if(code>0){configKeys[name]=code;configKeyNames[name]=keyName;}else{configKeys.erase(name);configKeyNames.erase(name);}
 WritePrivateProfileStringW(L"keys",Wide(name).c_str(),code>0?std::to_wstring(code).c_str():nullptr,ConfigKeyFile().c_str());WritePrivateProfileStringW(L"names",Wide(name).c_str(),code>0?Wide(keyName).c_str():nullptr,ConfigKeyFile().c_str());}
void LoadConfig(const std::string& name){FlushSaves();if(!CopyFileW(ConfigFile(name).c_str(),configPath.c_str(),FALSE))return;ReadPreferences();LoadMenuSettings();
 for(auto& place:hudPlaces){place.second.loaded=false;place.second.pending=false;}hudDragging.clear();++hudIniVersion;   // HUD positions of the loaded config
selected=-1;listening=-1;SetCurrentConfig(name);settingsDirty=false;configEdited=false;Say("Loaded config '"+name+"'");if(notices.size()<16)notices.push_back({7,"Configs","Loaded config '"+name+"'"});}
// per-config keybind, captured inside its row
void BindConfigKey(const std::string& name,unsigned vk,long long lparam){
 if(vk==VK_ESCAPE)return;if(vk==VK_BACK||vk==VK_DELETE){SetConfigKey(name,0,"");Say("Cleared the key for '"+name+"'.");return;}
 wchar_t text[64]{};if(!GetKeyNameTextW((LONG)lparam,text,64))swprintf(text,64,L"Key %u",vk);const std::string key=Utf8(text);
 if(menuKeys[0].code==(int)vk){Say(key+" is your panic key, so it won't load this config.");return;}
 std::string message="Press "+key+" in game to load '"+name+"'.";
 for(auto& other:std::map<std::string,int>(configKeys))if(other.first!=name&&other.second==(int)vk){SetConfigKey(other.first,0,"");message="Moved "+key+" from '"+other.first+"'.";}
 for(auto& item:modules){auto bound=keycodes.find(item.id);if(bound!=keycodes.end()&&bound->second==(int)vk)message=key+" is also bound to "+item.name+". In game it loads this config instead.";}
 SetConfigKey(name,(int)vk,key);Say(message);
}
// relative time: minutes, hours, days, "last week", weeks, months; short forms ("just now", "1 minute ago",
// "1 hour ago", "yesterday") for the singular cases.
std::string TimeAgoNow(const std::wstring& file){WIN32_FILE_ATTRIBUTE_DATA data{};if(!GetFileAttributesExW(file.c_str(),GetFileExInfoStandard,&data))return "";FILETIME now{};GetSystemTimeAsFileTime(&now);
 const auto ticks=[](const FILETIME& t){return ((unsigned long long)t.dwHighDateTime<<32)|t.dwLowDateTime;};const long long minutes=((long long)ticks(now)-(long long)ticks(data.ftLastWriteTime))/600000000LL;
 if(minutes<1)return "just now";if(minutes<60)return minutes==1?"1 minute ago":std::to_string(minutes)+" minutes ago";if(minutes<1440)return minutes<120?"1 hour ago":std::to_string(minutes/60)+" hours ago";if(minutes<2880)return "yesterday";
 if(minutes<10080)return std::to_string(minutes/1440)+" days ago";if(minutes<20160)return "last week";if(minutes<86400)return std::to_string(minutes/10080)+" weeks ago";return std::to_string(minutes/43200)+" months ago";}
// file times read at most once a second per file (Configs rows asked for them every frame)
std::string TimeAgo(const std::wstring& file){static std::map<std::wstring,std::pair<double,std::string>> seen;auto& entry=seen[file];const double now=ImGui::GetTime();if(entry.second.empty()||now-entry.first>1){entry.first=now;entry.second=TimeAgoNow(file);}return entry.second;}
// generic button: style 0 neutral, 1 solid purple, 3 red; disabled = 40 %; pressed offset of 1 u
bool UiButton(ImDrawList* d,const std::string& id,const char* label,ImVec2 lo,float width,float height,int style,bool enabled,float a,float s){
 const float U=242.f/210.f;const ImVec2 hi(lo.x+width*s,lo.y+height*s);const bool over=enabled&&opened&&ImGui::IsMouseHoveringRect(lo,hi);const float h=Animate("button"+id,over?1.f:0.f,ImGui::GetIO().DeltaTime),fade=a*(enabled?1.f:.4f),inset=over&&ImGui::GetIO().MouseDown[0]?U*s:0;
 d->AddRectFilled(ImVec2(lo.x+inset,lo.y+inset),ImVec2(hi.x-inset,hi.y-inset),style==3?Rgba(240,86,106,(.15f+.09f*h)*fade):style==1?ToU32(MixColor(themeLive.accentDeep,ImVec4(1,1,1,1),.14f*h),fade):Rgba(255,255,255,(.09f+.05f*h)*fade),0);
 const float natural=TextWidth(semibold,15,label),size=natural>width-10*U?15*(width-10*U)/natural:15;   // labels too long for the button are scaled down ("New config" in 76 u)
 Label(d,semibold,size*s,ImVec2(lo.x+(width-TextWidth(semibold,size,label))/2*s,lo.y+(height/2-.7f*size)*s),label,style==3?Rgba(246,160,171,fade):style==1?Rgba(255,255,255,fade):Rgba(230,230,235,fade));
 return Hit(("##"+id).c_str(),lo,ImVec2(width*s,height*s))&&enabled;}
// dark text field used by the screens (search, name): radius 8 u; returns true on Enter
bool UiInput(const char* id,const char* hint,char* buffer,size_t size,ImVec2 at,float width,float height,float padLeft,float s,float a){
 const float U=242.f/210.f;ImGui::SetCursorScreenPos(at);ImGui::SetNextItemWidth(width*s);ImGui::PushStyleVar(ImGuiStyleVar_Alpha,a);ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,0);ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(padLeft*s,std::max(0.f,(height*s-ImGui::GetFontSize())/2)));
 ImGui::PushStyleColor(ImGuiCol_FrameBg,ImVec4(.1f,.1f,.125f,.96f));ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,ImVec4(.12f,.12f,.15f,.96f));ImGui::PushStyleColor(ImGuiCol_FrameBgActive,ImVec4(.12f,.12f,.15f,.96f));
 const bool entered=ImGui::InputTextWithHint(id,hint,buffer,size,ImGuiInputTextFlags_EnterReturnsTrue);ImGui::PopStyleColor(3);ImGui::PopStyleVar(3);return entered;}
std::string CleanConfigName(std::string wanted){wanted.erase(std::remove_if(wanted.begin(),wanted.end(),[](unsigned char ch){return ch<32||strchr("\\/:*?\"<>|=[]",ch);}),wanted.end());while(!wanted.empty()&&wanted.back()==' ')wanted.pop_back();while(!wanted.empty()&&wanted.front()==' ')wanted.erase(wanted.begin());return wanted;}
void DrawConfigs(ImVec2 origin,float s,float opacity){
 const float U=242.f/210.f,left=257,right=menuW-22,top=72;auto& io=ImGui::GetIO();auto* d=ImGui::GetWindowDrawList();auto P=[&](float x,float y){return ImVec2(origin.x+x*s,origin.y+y*s);};
 auto names=ConfigNamesCached();{static std::string known;std::string joined;for(auto& name:names)joined+=name+"|";if(joined!=known){known=joined;LoadConfigKeys(names);}}
 if(!configSelected.empty()&&std::find(names.begin(),names.end(),configSelected)==names.end()){configSelected.clear();renaming=false;}
 static float age=0;static int lastFrame=-2;if(ImGui::GetFrameCount()!=lastFrame+1||opacity<.05f)age=0;lastFrame=ImGui::GetFrameCount();age+=io.DeltaTime;
 configSection=0;
 const float listTop=top+42*U,areaW=right-left;
 auto message=[&](const std::string& text,float y){const float rest=1-std::clamp(age/.22f,0.f,1.f);Label(d,regular,12.5f*U*s,P(left+(areaW-TextWidth(regular,12.5f*U,text.c_str()))/2,y+24*U-.7f*12.5f*U),text.c_str(),Rgba(120,120,132,(1-rest*rest*rest)*opacity));};
 auto magnifier=[&](float x,float y){if(DrawGlyph(d,nicons::k_search,P(x-6.5f,y-6.5f),15*s,Rgba(120,120,132,opacity)))return;d->AddCircle(P(x-1.2f,y-1.2f),5*s,Rgba(120,120,132,opacity),0,1.5f*s);d->AddLine(P(x+2.5f,y+2.5f),P(x+6.3f,y+6.3f),Rgba(120,120,132,opacity),1.5f*s);};
 {
  // search, then Import / Export (64 u each) and the 76 u "New config" button, 8 u apart
  UiInput("##mine-search","Search your configs",configSearch,sizeof(configSearch),P(left,top),areaW-228*U,32*U,34*U,s,opacity);magnifier(left+17*U,top+16*U);
  // Import: a config file (.ini with module sections) copied into the configs folder under a free name.
  if(UiButton(d,"import-config","Import",P(right-76*U-8*U-64*U-8*U-64*U,top),64*U,32*U,0,true,opacity,s))
   filedialog::Ask(gameWindowForKeys,false,filedialog::Filter({L"Nickel config (*.ini)",L"*.ini",L"All files",L"*.*"}),L"Import a config",L"",L"ini",[](const std::wstring& path){
    HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(f==INVALID_HANDLE_VALUE){SayAndNotify("This file could not be opened.");return;}
    LARGE_INTEGER size{};GetFileSizeEx(f,&size);std::string head(4096,'\0');DWORD got=0;ReadFile(f,head.data(),(DWORD)head.size(),&got,nullptr);CloseHandle(f);head.resize(got);
    if(size.QuadPart<=0||size.QuadPart>4*1024*1024||head.find('[')==std::string::npos||head.find("enabled=")==std::string::npos){SayAndNotify("This is not a Nickel config.");return;}
    const std::wstring target=filedialog::FreeName(ConfigFolder(),filedialog::Stem(path),L".ini");
    if(!CopyFileW(path.c_str(),target.c_str(),TRUE)){SayAndNotify("The config could not be imported.");return;}
    const std::string name=Utf8(filedialog::Stem(target));ConfigNames();configSelected=name;SayAndNotify("Imported "+name+".");});
  // Export: the selected config (or the loaded one) saved where the player chooses.
  if(UiButton(d,"export-config","Export",P(right-76*U-8*U-64*U,top),64*U,32*U,0,true,opacity,s)){
   const std::string name=!configSelected.empty()?configSelected:currentConfig;
   if(name.empty()||GetFileAttributesW(ConfigFile(name).c_str())==INVALID_FILE_ATTRIBUTES)SayAndNotify("Select a config to export.");
   else filedialog::Ask(gameWindowForKeys,true,filedialog::Filter({L"Nickel config (*.ini)",L"*.ini"}),L"Export a config",Wide(name)+L".ini",L"ini",[name](const std::wstring& path){
    if(CopyFileW(ConfigFile(name).c_str(),path.c_str(),FALSE))SayAndNotify("Exported "+name+".");else SayAndNotify("The config could not be exported.");});}
  if(UiButton(d,"new-config","New config",P(right-76*U,top),76*U,32*U,1,true,opacity,s)){
   std::string name;for(int n=1;n<1000;++n){name="Config "+std::to_string(n);if(std::find(names.begin(),names.end(),name)==names.end())break;}
   FlushSaves();WritePrivateProfileStringW(nullptr,nullptr,nullptr,configPath.c_str());if(GetFileAttributesW(configPath.c_str())==INVALID_FILE_ATTRIBUTES)WritePrivateProfileStringW(L"menu",L"created",L"1",configPath.c_str());
   if(CopyFileW(configPath.c_str(),ConfigFile(name).c_str(),TRUE)){Say("Saved "+name+" to your configs.");names=ConfigNames();SetCurrentConfig(name);configEdited=false;configSelected=name;}}
  std::string query=configSearch;std::transform(query.begin(),query.end(),query.begin(),[](unsigned char c){return (char)std::tolower(c);});configListed.clear();
  for(auto& name:names){std::string low=name;std::transform(low.begin(),low.end(),low.begin(),[](unsigned char c){return (char)std::tolower(c);});if(query.empty()||low.find(query)!=std::string::npos)configListed.push_back(name);}
  if(names.empty())message("No configs yet. New saves your current setup.",listTop);else if(configListed.empty())message("No configs match \""+std::string(configSearch)+"\".",listTop);
  ImGui::SetCursorScreenPos(P(left,listTop));ImGui::BeginChild("##ConfigList",ImVec2(areaW*s,(menuH-20-listTop)*s),false,ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoScrollbar);
  auto* list=ImGui::GetWindowDrawList();const ImVec2 base=ImGui::GetCursorScreenPos();
  for(int i=0;i<(int)configListed.size();++i){const std::string& name=configListed[i];ImGui::PushID(name.c_str());
   // row entry: 0.03 s stagger per row, 0.22 s, rises 10 u
   const float rest=1-std::clamp((age-.03f*std::min(i,12))/.22f,0.f,1.f),e=1-rest*rest*rest,a=opacity*e,y=i*62*U+(1-e)*10*U;auto R=[&](float x,float dy){return ImVec2(base.x+x*s,base.y+(y+dy)*s);};
   const ImVec2 lo=R(0,0),hi=R(areaW,56*U);const bool current=name==currentConfig,chosen=name==configSelected;const float hover=Animate("mine-hover"+name,opened&&ImGui::IsMouseHoveringRect(lo,hi)?1.f:0.f,io.DeltaTime),picked=Animate("mine-selected"+name,chosen?1.f:0.f,io.DeltaTime),glow=std::max(hover*.8f,picked);
   list->AddRectFilled(lo,hi,Rgba(12+(int)(16*glow),12+(int)(16*glow),15+(int)(18*glow),(.55f+.2f*glow)*a),0);list->AddRect(lo,hi,ToU32(MixColor(ImVec4(1,1,1,1),themeLive.accent,picked),(.05f+.5f*picked)*a),0,0,U*s);
   // right side: "Load" (52 x 26 u, except for the active config), keybind pill, clear cross when a key is set
   float edge=areaW-10*U;bool handled=false;
   if(!current){if(UiButton(list,"mine-load","Load",R(edge-52*U,15*U),52*U,26*U,0,true,a,s)){LoadConfig(name);handled=true;}edge-=58*U;}
   {const bool waiting=listening==-200-i;auto bound=configKeyNames.find(name);const char* value=waiting?"Press a key...":bound==configKeyNames.end()?"None":bound->second.c_str();const float w=std::max(76*U,TextWidth(bold,11*U,value)+20*U);const ImVec2 klo=R(edge-w,15*U),khi=R(edge,41*U);
    const float h=Animate("mine-keybind"+name,opened&&ImGui::IsMouseHoveringRect(klo,khi)?1.f:0.f,io.DeltaTime),r=Animate("mine-recording"+name,waiting?1.f:0.f,io.DeltaTime,14),fill=.08f+.04f*h;
    list->AddRectFilled(klo,khi,ToU32(MixColor(ImVec4(1,1,1,1),themeLive.accentDeep,r),(fill+(.42f-fill)*r)*a),0);
    Label(list,bold,11*U*s,R(edge-w+(w-TextWidth(bold,11*U,value))/2,28*U-.7f*11*U),value,!waiting&&bound==configKeyNames.end()?Rgba(120,120,132,a):Rgba(190+(int)(65*r),190+(int)(65*r),198+(int)(57*r),a));
    if(Hit("##mine-keybind",klo,ImVec2(w*s,26*U*s))){listening=waiting?-1:-200-i;handled=true;}
    if(bound!=configKeyNames.end()){const ImVec2 c=R(edge-w-17*U,28*U);const float cross=ImGui::IsMouseHoveringRect(R(edge-w-30*U,15*U),R(edge-w-4*U,41*U))?1.f:.75f;list->AddLine(ImVec2(c.x-4*s,c.y-4*s),ImVec2(c.x+4*s,c.y+4*s),Rgba(170,170,180,cross*a),1.5f*s);list->AddLine(ImVec2(c.x+4*s,c.y-4*s),ImVec2(c.x-4*s,c.y+4*s),Rgba(170,170,180,cross*a),1.5f*s);
     if(Hit("##mine-keybind-clear",R(edge-w-30*U,15*U),ImVec2(26*U*s,26*U*s))){SetConfigKey(name,0,"");Say("Cleared the key for '"+name+"'.");handled=true;}edge-=30*U;}
    edge-=w+7*U;}
   // name 19.124 u bold at (14 u, 9 u); ACTIVE (purple) or EDITED (12 % white) badges; second line "Updated ..."
   float badges=0;const char* badge=current?(configEdited?"EDITED":"ACTIVE"):nullptr;if(badge)badges=7*U+TextWidth(bold,9.5f*U,badge)+12*U;
   const std::string title=Fit(bold,14*U,name,edge-14*U-badges);Label(list,bold,14*U*s,R(14*U,9*U+19.124f*U*.5f-.7f*14*U),title.c_str(),Rgba(255,255,255,a));
   if(badge){const float bx=14*U+TextWidth(bold,14*U,title.c_str())+7*U,bw=TextWidth(bold,9.5f*U,badge)+12*U,by=9*U+19.124f*U*.52f-8*U;const bool edited=configEdited;
    list->AddRectFilled(R(bx,by),R(bx+bw,by+16*U),edited?Rgba(255,255,255,.12f*a):AccDeep(a),0);Label(list,bold,9.5f*U*s,R(bx+6*U,by+8*U-.7f*9.5f*U),badge,edited?Rgba(170,170,180,a):Rgba(255,255,255,a));}
   Label(list,regular,11.5f*U*s,R(14*U,9*U+19.124f*U+2*U+15.709f*U*.5f-.7f*11.5f*U),("Updated "+TimeAgo(ConfigFile(name))).c_str(),Rgba(148,148,160,a));
   if(!handled&&Hit("##mine-row",lo,ImVec2(areaW*s,56*U*s))){configSelected=name;renaming=false;deleteArmed=-1;}
   ImGui::PopID();}
  ImGui::SetCursorPosY(configListed.size()*62*U*s);ImGui::Dummy(ImVec2(1,1));FadeEdges(s,opacity);ImGui::EndChild();
 }
 const float shown=(float)(ImGui::GetTime()-configStatusAt);if(shown<3&&!configStatus.empty())Label(ImGui::GetForegroundDrawList(),regular,13*s,P(left,menuH-28),configStatus.c_str(),Rgba(159,232,191,std::min(1.f,3-shown)*opacity));
}
#include "script_ui.hpp"
void JumpPreAll(uint64_t a7,uint64_t a8){game::Move2JumpPre(a7,a8);game::LongJumpPre(a7,a8);ScriptsJumpPre(a7,a8);}   // jump hook #16: No Jump Delay, then scripts
void JumpPostAll(uint64_t a8){game::Move2JumpPost(a8);game::LongJumpPost(a8);ScriptsJumpPost(a8);}
#include "schematic_ui.hpp"
// Details panel for the selected config, inside the floating right panel.
// Rows (height / gap in u): title 22/4 (or field + Save 32/10), "Updated ..." 16/10, rule 1/10, MODULES 12/6 then one
// bar per category 14/6, Load | Save here 32/6, Rename | Duplicate 32/6, Delete 32/10.
void DrawConfigPanel(ImVec2 origin,float s,float opacity){
 const float U=242.f/210.f,x0=16,width=297-32;auto& io=ImGui::GetIO();auto P=[&](float x,float y){return ImVec2(origin.x+x*s,origin.y+y*s);};
 ImGui::SetNextWindowPos(origin);ImGui::SetNextWindowSize(ImVec2(297*s,menuH*s));ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));ImGui::PushStyleVar(ImGuiStyleVar_Alpha,opacity);
 ImGui::Begin("##NickelConfigPanel",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBackground|(opened.load()?0:ImGuiWindowFlags_NoInputs));
 auto* d=ImGui::GetWindowDrawList();d->AddRectFilled(origin,P(297,menuH),Rgba(15,15,20,.98f*opacity),0);d->AddRect(origin,P(297,menuH),Rgba(26,26,33,opacity),0);WindowFont(s*15.f*FontEmFactor);
 // appearance: 0.2 s cubic, rises 6 u, replayed whenever the selection changes
 static std::string shownFor="\x01";static float since=0;if(shownFor!=configSelected||opacity<.05f){shownFor=configSelected;since=0;}since+=io.DeltaTime;const float rest=1-std::clamp(since/.2f,0.f,1.f),e=1-rest*rest*rest,a=opacity*e;
 float y=16+(1-e)*6*U;auto take=[&](float height,float gap){const float at=y;y+=(height+gap)*U;return at;};
 const std::string name=configSection==0?configSelected:"";
 if(name.empty()){const float at=take(20,10);Label(d,regular,12.5f*U*s,P(x0,at+10*U-.7f*12.5f*U),configSection==0?"Select a config to see its details.":"Select a config to see its details.",Rgba(148,148,160,a));EndWindow();ImGui::PopStyleVar(2);return;}
 const bool current=name==currentConfig;const std::wstring file=ConfigFile(name);
 if(renaming){const float at=take(32,10);const bool entered=UiInput("##rename","Config name",renameText,sizeof(renameText),P(x0,at),width-62*U,32*U,10*U,s,a);
  if(UiButton(d,"rename-save","Save",P(x0+width-56*U,at),56*U,32*U,1,true,a,s)||entered){const std::string wanted=CleanConfigName(renameText);auto names=ConfigNames();
   if(!wanted.empty()&&wanted!=name&&std::find(names.begin(),names.end(),wanted)==names.end()&&MoveFileW(file.c_str(),ConfigFile(wanted).c_str())){auto key=configKeys.find(name);if(key!=configKeys.end()){const int code=key->second;const std::string keyName=configKeyNames[name];SetConfigKey(name,0,"");SetConfigKey(wanted,code,keyName);}if(current)SetCurrentConfig(wanted);configSelected=wanted;shownFor=wanted;}
   renaming=false;EndWindow();ImGui::PopStyleVar(2);return;}
 }else{const float at=take(22,4);Label(d,bold,17*U*s,P(x0,at+11*U-.7f*17*U),Fit(bold,17*U,name,width).c_str(),Rgba(255,255,255,a));}
 {const float at=take(16,10);Label(d,regular,12*U*s,P(x0,at+8*U-.7f*12*U),"Updated",Rgba(148,148,160,a));Label(d,regular,12*U*s,P(x0+TextWidth(regular,12*U,"Updated "),at+8*U-.7f*12*U),TimeAgo(file).c_str(),Rgba(224,224,230,a));}
 {const float at=take(1,10);d->AddLine(P(x0,at),P(x0+width,at),Rgba(255,255,255,.07f*a),U*s);}
 // MODULES: number of enabled modules in this config, then one bar per category (74 u label, 4 u bar, total on the right)
 {static const char* categories[]={"Combat","Movement","Player","Visuals","Utility","HUD"};int counts[6]{},total=0,most=1;
  // counted again only when the selected file changes (name, write time, size), not every frame
  static struct{std::wstring file;FILETIME written{};DWORD size=0;int total=0,counts[6]{},most=1;double checked=-10;}seen;WIN32_FILE_ATTRIBUTE_DATA info{};
  if(seen.file!=file||ImGui::GetTime()-seen.checked>.5){seen.checked=ImGui::GetTime();const bool stat=GetFileAttributesExW(file.c_str(),GetFileExInfoStandard,&info)!=0;
   if(seen.file!=file||!stat||CompareFileTime(&info.ftLastWriteTime,&seen.written)||info.nFileSizeLow!=seen.size){seen.file=file;seen.written=info.ftLastWriteTime;seen.size=info.nFileSizeLow;seen.total=0;seen.most=1;for(int& c:seen.counts)c=0;
    for(auto& item:modules)if(GetPrivateProfileIntW(Wide(item.id).c_str(),L"enabled",0,file.c_str())){++seen.total;for(int i=0;i<6;++i)if(item.category==categories[i])seen.most=std::max(seen.most,++seen.counts[i]);}}}
  total=seen.total;most=seen.most;for(int i=0;i<6;++i)counts[i]=seen.counts[i];
  float at=take(12,6);Label(d,bold,10.5f*U*s,P(x0,at+6*U-.7f*10.5f*U),"MODULES",Rgba(120,120,132,a));Label(d,bold,10.5f*U*s,P(x0+TextWidth(bold,10.5f*U,"MODULES")+8*U,at+6*U-.7f*10.5f*U),std::to_string(total).c_str(),Rgba(224,224,230,a));
  for(int i=0;i<6;++i){at=take(14,6);const std::string count=std::to_string(counts[i]);const float fill=Animate(std::string("bar")+categories[i],(float)counts[i]/most,io.DeltaTime,10),barLeft=x0+74*U,barRight=x0+width-30*U;
   Label(d,regular,11.5f*U*s,P(x0,at+7*U-.7f*11.5f*U),categories[i],Rgba(170,170,180,a));Label(d,regular,11.5f*U*s,P(x0+width-TextWidth(regular,11.5f*U,count.c_str()),at+7*U-.7f*11.5f*U),count.c_str(),Rgba(148,148,160,a));
   d->AddRectFilled(P(barLeft,at+5*U),P(barRight,at+9*U),Rgba(255,255,255,.1f*a),0);if(fill>.001f)d->AddRectFilled(P(barLeft,at+5*U),P(barLeft+(barRight-barLeft)*fill,at+9*U),AccDeep(a),0);}}
 const float half=(width-6*U)/2;
 {const float at=take(32,6);if(UiButton(d,"details-load",current?"Active":"Load",P(x0,at),half,32*U,1,!current,a,s))LoadConfig(name);
  if(UiButton(d,"details-save","Save here",P(x0+half+6*U,at),half,32*U,0,true,a,s)){FlushSaves();WritePrivateProfileStringW(nullptr,nullptr,nullptr,configPath.c_str());if(CopyFileW(configPath.c_str(),file.c_str(),FALSE)){SetCurrentConfig(name);configEdited=false;settingsDirty=false;Say("Saved "+name+" to your configs.");}}}
 {const float at=take(32,6);if(UiButton(d,"details-rename","Rename",P(x0,at),half,32*U,0,true,a,s)){renaming=!renaming;strncpy_s(renameText,name.c_str(),sizeof(renameText)-1);}
  if(UiButton(d,"details-duplicate","Duplicate",P(x0+half+6*U,at),half,32*U,0,true,a,s)){auto names=ConfigNames();std::string copy;for(int n=2;n<1000;++n){copy=name+" "+std::to_string(n);if(std::find(names.begin(),names.end(),copy)==names.end())break;}if(CopyFileW(file.c_str(),ConfigFile(copy).c_str(),TRUE))Say("Duplicated as "+copy+".");}}
 {const float at=take(32,10);const bool armed=deleteArmed>=0&&ImGui::GetTime()-deleteArmed<3;
  if(UiButton(d,"details-delete",armed?"Click again to delete":"Delete",P(x0,at),width,32*U,3,true,a,s)){if(!armed)deleteArmed=ImGui::GetTime();else{DeleteFileW(file.c_str());SetConfigKey(name,0,"");if(current)SetCurrentConfig("");configSelected.clear();deleteArmed=-1;}}}
 EndWindow();ImGui::PopStyleVar(2);
}
// Settings screen: INTERFACE, KEYBINDS, ABOUT, ITEM PROFILES and SCHEMATICS sections, built from shared section
// headers, rows, key pills, steppers and buttons. U = 242/210 of our layout units.
void SaveMenuProfiles(){std::string joined;for(auto& profile:itemProfiles){if(!joined.empty())joined+="|";joined+=profile.name+(profile.weapon?"*":"");}WritePrivateProfileStringW(L"menu",L"item-profiles",Wide(joined+"|").c_str(),configPath.c_str());
 // profile details: "Custom|speed|items|blocks" (comma-separated lists)
 for(size_t i=0;i<itemProfiles.size()+4;++i){const std::wstring key=L"item-profile-"+std::to_wstring(i);if(i<itemProfiles.size()){auto& p=itemProfiles[i];char speed[32];snprintf(speed,sizeof(speed),"%g",p.speed);
  WritePrivateProfileStringW(L"menu",key.c_str(),Wide(std::string(p.customSpeed?"1":"0")+"|"+speed+"|"+JoinList(p.items)+"|"+JoinList(p.blocks)).c_str(),configPath.c_str());}else WritePrivateProfileStringW(L"menu",key.c_str(),nullptr,configPath.c_str());}
 settingsDirty=true;if(!currentConfig.empty())configEdited=true;}
void OpenProfile(int i){auto& p=itemProfiles[i];profileOpen=i;profileOpenedAt=ImGui::GetTime();profileDirty=false;Module m;m.id="@profile";m.name=p.name;m.description=ProfileDetail(p);m.category="Settings";
 auto add=[&](const char* id,const char* name,const char* description,const char* kind,double minimum,double maximum,double step,double number,const std::string& text,int dependsOn){Setting st;st.id=id;st.name=name;st.description=description;st.kind=kind;st.minimum=minimum;st.maximum=maximum;st.step=step;st.number=number;st.text=text;st.dependsOn=dependsOn;st.dependsValue=1;m.settings.push_back(st);};
 add("name","Name","Shown in the profile list. With no items picked, matches items whose ID contains it","text",0,0,0,0,p.name,-1);
 add("items","Items","Items this profile applies to","list",0,0,0,0,JoinList(p.items),-1);
 add("weapon","Weapon","Counts as a weapon for Weapons Only","bool",0,1,1,p.weapon?1:0,"",-1);
 add("custom_mining_speed","Custom mining speed","Use your own mining speed for this item","bool",0,1,1,p.customSpeed?1:0,"",-1);
 add("mining_speed","Speed","Vanilla scale: wood 2, stone 4, iron 6, diamond 8, gold 12","number",1,50,.5,p.speed,"",3);
 add("blocks","Limit to blocks","Blocks the custom speed applies to. Leave empty for every block","list",0,0,0,0,JoinList(p.blocks),3);
 profileModule=m;}
// read back the open profile's settings; the name only changes when it is non-empty and not used by another profile
void SyncProfile(){if(!profileDirty||profileOpen<0||profileOpen>=(int)itemProfiles.size())return;profileDirty=false;auto& p=itemProfiles[profileOpen];
 auto get=[&](const char* id)->Setting*{for(auto& st:profileModule.settings)if(st.id==id)return &st;return nullptr;};
 if(auto st=get("name")){std::string wanted=st->text;wanted.erase(std::remove(wanted.begin(),wanted.end(),'|'),wanted.end());while(!wanted.empty()&&wanted.back()==' ')wanted.pop_back();while(!wanted.empty()&&wanted.front()==' ')wanted.erase(wanted.begin());
  int taken=0;for(int k=0;k<(int)itemProfiles.size();++k)if(k!=profileOpen&&itemProfiles[k].name==wanted)++taken;if(!wanted.empty()&&!taken)p.name=wanted;}
 if(auto st=get("items"))p.items=SplitList(st->text);
 if(auto st=get("weapon"))p.weapon=st->number!=0;
 if(auto st=get("custom_mining_speed"))p.customSpeed=st->number!=0;
 if(auto st=get("mining_speed"))p.speed=(float)std::clamp(st->number,1.0,50.0);
 if(auto st=get("blocks"))p.blocks=SplitList(st->text);
 profileModule.name=p.name;profileModule.description=ProfileDetail(p);SaveMenuProfiles();}
void LoadMenuSettings(){
 for(auto* option:MenuToggles())option->value=GetPrivateProfileIntW(L"menu",Wide(option->key).c_str(),option->initial?1:0,configPath.c_str())!=0;
 for(auto& key:menuKeys){wchar_t name[65]{};GetPrivateProfileStringW(L"menu",Wide(key.key).c_str(),Wide(key.initialName).c_str(),name,65,configPath.c_str());key.code=GetPrivateProfileIntW(L"menu",Wide(std::string(key.key)+"-code").c_str(),key.initialCode,configPath.c_str());if(name[0]&&key.code>0)key.name=Utf8(name);else{key.name.clear();key.code=0;}}
 {wchar_t text[32]{};GetPrivateProfileStringW(L"menu",L"schematic-place-range",L"4.5",text,32,configPath.c_str());schematicRange=std::clamp((float)wcstod(text,nullptr),1.f,6.f);}
 schematicDelay=std::clamp((int)GetPrivateProfileIntW(L"menu",L"schematic-place-delay",75,configPath.c_str()),0,500);schematicPerTick=std::clamp((int)GetPrivateProfileIntW(L"menu",L"schematic-placements-per-tick",1,configPath.c_str()),1,8);
 {wchar_t text[2048]{};GetPrivateProfileStringW(L"menu",L"item-profiles",L"sword*|",text,2048,configPath.c_str());itemProfiles.clear();std::string all=Utf8(text),name;
  for(char ch:all){if(ch=='|'){if(!name.empty()){const bool weapon=name.back()=='*';if(weapon)name.pop_back();if(!name.empty()){ItemProfile p;p.name=name;p.weapon=weapon;itemProfiles.push_back(p);}}name.clear();}else name+=ch;}}
 for(size_t i=0;i<itemProfiles.size();++i){wchar_t text[8192]{};GetPrivateProfileStringW(L"menu",(L"item-profile-"+std::to_wstring(i)).c_str(),L"",text,8192,configPath.c_str());const std::string all=Utf8(text);if(all.empty())continue;
  std::vector<std::string> parts;size_t from=0;for(int k=0;k<4;++k){const size_t start=std::min(from,all.size());size_t to=k<3?all.find('|',start):all.size();if(to==std::string::npos)to=all.size();parts.push_back(all.substr(start,to-start));from=to+1;}
  auto& p=itemProfiles[i];p.customSpeed=parts[0]=="1";const double speed=atof(parts[1].c_str());p.speed=std::isfinite(speed)&&speed>0?(float)std::clamp(speed,1.0,50.0):8.f;p.items=SplitList(parts[2]);p.blocks=SplitList(parts[3]);}
 profileOpen=-1;
}
void DrawMenuSettings(ImVec2 origin,float s,float opacity){
 const float areaW=menuW-279,U=242.f/210.f;auto& io=ImGui::GetIO();ImGui::SetCursorScreenPos(ImVec2(origin.x+257*s,origin.y+74*s));ImGui::BeginChild("##MenuSettings",ImVec2(areaW*s,(menuH-94)*s),false,ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoScrollbar);
 if(OwnProbe()){char value[16]{};if(GetEnvironmentVariableA("NICKEL_REVIEW_SCROLL",value,sizeof(value)))ImGui::SetScrollY((float)atof(value)*s);}   // offline review: scroll position override
 auto* d=ImGui::GetWindowDrawList();const ImVec2 base=ImGui::GetCursorScreenPos();float y=0,top=0;int index=0;
 // screen entry: each row arrives 0.03 s after the previous one (at most 12), over 0.22 s, cubic, rising 8 u
 static float age=0;static int lastFrame=-2;if(ImGui::GetFrameCount()!=lastFrame+1||opacity<.05f)age=0;lastFrame=ImGui::GetFrameCount();age+=io.DeltaTime;
 auto enter=[&](int i){const float rest=1-std::clamp((age-.03f*std::min(i,12))/.22f,0.f,1.f);return 1-rest*rest*rest;};
 auto at=[&](float x,float dy){return ImVec2(base.x+x*s,base.y+(top+dy)*s);};
 // section header: a full-width separator above every section but the first, then the section name in the accent colour
 int section=0;
 auto header=[&](const char* text,const char* glyph){const float a=opacity*enter(index);(void)glyph;if(section++>0){top=y;d->AddLine(at(0,10*U),at(areaW,10*U),Rgba(52,52,62,a),1.5f*U*s);y+=20*U;}
  top=y;Label(d,bold,13.5f*U*s,at(2*U,16*U-.7f*13.5f*U),text,Acc(a));y+=34*U;};
 // row: 52 u + 6 u, radius 12 u, fill (12,11,16) 55 % -> (28,26,38) 75 % on hover, 5 % white border, title 18.441 u,
 // description 15.709 u
 auto row=[&](const std::string& id,const char* title,const char* description){const float e=enter(index++),a=opacity*e;top=y+(1-e)*8*U;const ImVec2 lo=at(0,0),hi=at(areaW,52*U);
  const float hover=Animate("row-hover"+id,opened&&ImGui::IsMouseHoveringRect(lo,hi)?1.f:0.f,io.DeltaTime);
  if(hover>.001f)d->AddRectFilled(lo,hi,Rgba(20,20,27,hover*a),0);d->AddLine(ImVec2(lo.x+12*U*s,hi.y+3*U*s),ImVec2(hi.x-12*U*s,hi.y+3*U*s),Rgba(26,26,33,a),U*s);
  Label(d,semibold,13.5f*U*s,at(12*U,10),Fit(semibold,13.5f*U,title,areaW-192*U).c_str(),Rgba(255,255,255,a));Label(d,regular,11.5f*U*s,at(12*U,31.3f),Fit(regular,11.5f*U,description,areaW-192*U).c_str(),Rgba(130,130,142,a));y+=58*U;return a;};
 // generic button: 28 u tall; style 0 neutral, 1 solid purple, 3 red; disabled = 40 %; pressed offset of 1 u
 auto button=[&](const std::string& id,const char* label,float right,float width,int style,bool enabled,float a){const float x0=right-width;const ImVec2 lo=at(x0,12*U),hi=at(right,40*U);
  const bool over=enabled&&opened&&ImGui::IsMouseHoveringRect(lo,hi);const float h=Animate("button"+id,over?1.f:0.f,io.DeltaTime),fade=a*(enabled?1.f:.4f),inset=over&&io.MouseDown[0]?U*s:0;
  const ImU32 fill=style==3?Rgba(240,86,106,(.15f+.09f*h)*fade):style==1?ToU32(MixColor(themeLive.accentDeep,ImVec4(1,1,1,1),.14f*h),fade):Rgba(255,255,255,(.09f+.05f*h)*fade);
  d->AddRectFilled(ImVec2(lo.x+inset,lo.y+inset),ImVec2(hi.x-inset,hi.y-inset),fill,0);
  Label(d,semibold,15*s,at(x0+(width-TextWidth(semibold,15,label))/2,26*U-10.5f),label,style==3?Rgba(246,160,171,fade):style==1?Rgba(255,255,255,fade):Rgba(230,230,235,fade));
  return Hit(("##"+id).c_str(),lo,ImVec2(width*s,28*U*s))&&enabled;};
 auto toggle=[&](MenuOption& option){const float a=row(option.key,option.title,option.description);Toggle(d,at(areaW-12*U-34,26*U-9),s,Animate(std::string("menu")+option.key,option.value?1.f:0.f,io.DeltaTime),a);
  if(Hit(option.key,at(areaW-12*U-34,26*U-9),ImVec2(34*s,18*s))){option.value=!option.value;WritePrivateProfileStringW(L"menu",Wide(option.key).c_str(),option.value?L"1":L"0",configPath.c_str());}};
 // stepper: 150 u block, 28 u "-" and "+" buttons, 16.392 u value in the middle; returns -1, 0 or +1
 auto stepper=[&](const std::string& id,const char* value,bool canLower,bool canRaise,float a){const float right=areaW-12*U;
  Label(d,regular,12*U*s,at(right-75*U-TextWidth(regular,12*U,value)/2,26*U-.7f*12*U),value,Rgba(214,210,226,a));
  // the minus sign (U+2212) is drawn as a line: it is not in the loaded glyph range
  const bool lower=button(id+"-minus","",right-122*U,28*U,0,canLower,a);d->AddLine(at(right-150*U+9.5f*U,26*U),at(right-150*U+18.5f*U,26*U),Rgba(230,230,235,a*(canLower?1.f:.4f)),1.6f*s);
  if(lower)return -1;return button(id+"-plus","+",right,28*U,0,canRaise,a)?1:0;};
 header("INTERFACE","\xee\x80\x89");for(auto& option:interfaceOptions)toggle(option);
 header("CONNECTION","\xee\x80\x89");for(auto& option:connectionOptions)toggle(option);
 // Theme: one swatch per theme (glow -> background gradient), selection ringed in white; saved in [interface] theme
 {const float a2=row("theme","Theme",(std::string(Th().name)+"  \xc2\xb7  menu colors and animated background").c_str());const float size=20*U,gap=8*U,right=areaW-12*U;
  for(int t=themeCount-1;t>=0;--t){const float x=right-(themeCount-t)*size-(themeCount-1-t)*gap;const ImVec2 lo=at(x,16*U),hi=at(x+size,36*U);
   const ImU32 top=ToU32(HexColor(themes[t].glow),a2),bottom=ToU32(HexColor(themes[t].deep),a2);d->AddRectFilledMultiColor(lo,hi,top,top,bottom,bottom);
   const bool over=opened&&ImGui::IsMouseHoveringRect(lo,hi);d->AddRect(ImVec2(lo.x-2*s,lo.y-2*s),ImVec2(hi.x+2*s,hi.y+2*s),Rgba(255,255,255,(t==themeIndex?.95f:over?.35f:0.f)*a2),0,0,1.5f*s);
   if(over)ImGui::SetTooltip("%s",themes[t].name);
   if(Hit(("##theme"+std::to_string(t)).c_str(),lo,ImVec2(size*s,size*s))&&t!=themeIndex){themeIndex=t;ThemeSave();}}}
 header("KEYBINDS","\xee\x80\xa0");
 for(int i=0;i<2;++i){auto& key=menuKeys[i];const float a=row(key.key,key.title,key.description);
const bool waiting=listening==-100-i;const char* value=waiting?"Press a key...":key.name.empty()?"None":key.name.c_str();
  // key pill: 26 u tall, width = max(76 u, text + 20 u), radius 7 u; 8 % white -> 12 % on hover, 42 % purple while listening
  const float w=std::max(76*U,TextWidth(bold,11*U,value)+20*U),right=areaW-12*U;const ImVec2 lo=at(right-w,13*U),hi=at(right,39*U);
  const float h=Animate(std::string("keybind-hover")+key.key,opened&&ImGui::IsMouseHoveringRect(lo,hi)?1.f:0.f,io.DeltaTime),r=Animate(std::string("keybind-recording")+key.key,waiting?1.f:0.f,io.DeltaTime,14);
  const float fill=.08f+.04f*h;d->AddRectFilled(lo,hi,ToU32(MixColor(ImVec4(1,1,1,1),themeLive.accentDeep,r),(fill+(.42f-fill)*r)*a),0);
  Label(d,bold,11*U*s,at(right-w+(w-TextWidth(bold,11*U,value))/2,26*U-.7f*11*U),value,!waiting&&key.name.empty()?Rgba(120,120,132,a):Rgba(190+(int)(65*r),190+(int)(65*r),198+(int)(57*r),a));
  if(Hit(key.key,lo,ImVec2(w*s,26*U*s)))listening=waiting?-1:-100-i;}
 // ---- ITEM PROFILES: "Profile name" field (168 u) + Add button (56 u), then one row per profile ----
 header("ITEM PROFILES","\xee\x80\x85");
 {const float a=row("profile-add","Add profile","Name a profile, then pick its items."),right=areaW-12*U;
  ImGui::SetCursorScreenPos(at(right-56*U-6*U-168*U,12*U));ImGui::SetNextItemWidth(168*U*s);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha,a);ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,0);ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(9*U*s,std::max(0.f,(28*U*s-ImGui::GetFontSize())/2)));ImGui::PushStyleColor(ImGuiCol_FrameBg,ImVec4(.1f,.1f,.125f,.96f));ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,ImVec4(.12f,.12f,.15f,.96f));ImGui::PushStyleColor(ImGuiCol_FrameBgActive,ImVec4(.12f,.12f,.15f,.96f));
  const bool entered=ImGui::InputTextWithHint("##profile-input","Profile name",profileInput,sizeof(profileInput),ImGuiInputTextFlags_EnterReturnsTrue);ImGui::PopStyleColor(3);ImGui::PopStyleVar(3);
  std::string wanted=profileInput;while(!wanted.empty()&&wanted.back()==' ')wanted.pop_back();while(!wanted.empty()&&wanted.front()==' ')wanted.erase(wanted.begin());wanted.erase(std::remove(wanted.begin(),wanted.end(),'|'),wanted.end());
  if((button("profile-add","Add",right,56*U,0,!wanted.empty(),a)||entered)&&!wanted.empty()){itemProfiles.push_back({wanted,false});profileInput[0]=0;SaveMenuProfiles();}}
 for(int i=0;i<(int)itemProfiles.size();++i){auto& profile=itemProfiles[i];
  // description without a chosen item: Items matching "name", followed by " · Weapon" for a weapon profile
  const std::string detail=ProfileDetail(profile),id="profile"+std::to_string(i);const float a=row(id,profile.name.c_str(),detail.c_str()),right=areaW-12*U;
  // "Open" / "Close" (purple when open): profile settings in the side panel
  const bool isOpen=profileOpen==i;
  if(button(id+"-open",isOpen?"Close":"Open",right-76*U-6*U,70*U,isOpen?1:0,true,a)){if(isOpen)profileOpen=-1;else OpenProfile(i);}
  if(button(id+"-remove","Remove",right,76*U,3,true,a)){itemProfiles.erase(itemProfiles.begin()+i);if(isOpen)profileOpen=-1;else if(profileOpen>i)--profileOpen;SaveMenuProfiles();--i;}}
 // ---- SCHEMATICS ----
 header("SCHEMATICS","\xee\x80\xa2");for(int i=0;i<4;++i)toggle(schematicOptions[i]);
 {char text[32];snprintf(text,sizeof(text),"%.1f blocks",schematicRange);const float a=row("schematic-place-range","Place range","How far away auto place reaches.");
  if(int step=stepper("schematic-place-range",text,schematicRange>1,schematicRange<6,a)){schematicRange=std::clamp(schematicRange+step*.5f,1.f,6.f);WritePrivateProfileStringW(L"menu",L"schematic-place-range",std::to_wstring(schematicRange).c_str(),configPath.c_str());}}
 {char text[32];snprintf(text,sizeof(text),"%d ms",schematicDelay);const float a=row("schematic-place-delay","Place delay","Time between auto placed blocks.");
  if(int step=stepper("schematic-place-delay",text,schematicDelay>0,schematicDelay<500,a)){schematicDelay=std::clamp(schematicDelay+step*25,0,500);WritePrivateProfileStringW(L"menu",L"schematic-place-delay",std::to_wstring(schematicDelay).c_str(),configPath.c_str());}}
 {char text[32];snprintf(text,sizeof(text),"%d",schematicPerTick);const float a=row("schematic-placements-per-tick","Placements per tick","How many blocks auto place puts down at once.");
  if(int step=stepper("schematic-placements-per-tick",text,schematicPerTick>1,schematicPerTick<8,a)){schematicPerTick=std::clamp(schematicPerTick+step,1,8);WritePrivateProfileStringW(L"menu",L"schematic-placements-per-tick",std::to_wstring(schematicPerTick).c_str(),configPath.c_str());}}
 toggle(schematicOptions[4]);
 // ---- ABOUT: red button, 92 u ("Reset all") or 150 u ("Click again to reset"), disabled when everything is default ----
 header("ABOUT","\xee\x80\xa1");
 {const float a=row("reset-all","Reset all settings","Resets every setting on this page to its default.");const bool armed=resetArmed>=0&&ImGui::GetTime()-resetArmed<3;
  bool changed=schematicRange!=4.5f||schematicDelay!=75||schematicPerTick!=1||itemProfiles.size()!=1||itemProfiles[0].name!="sword"||!itemProfiles[0].weapon||!itemProfiles[0].items.empty()||itemProfiles[0].customSpeed||itemProfiles[0].speed!=8||!itemProfiles[0].blocks.empty();
  for(auto* option:MenuToggles())changed|=option->value!=option->initial;for(auto& key:menuKeys)changed|=key.code!=key.initialCode;
  if(button("reset-all",armed?"Click again to reset":"Reset all",areaW-12*U,(armed?150:92)*U,3,changed||armed,a)){
   if(!armed)resetArmed=ImGui::GetTime();
   else{for(const wchar_t* name:{L"schematic-place-range",L"schematic-place-delay",L"schematic-placements-per-tick",L"item-profiles"})WritePrivateProfileStringW(L"menu",name,nullptr,configPath.c_str());
    for(size_t k=0;k<itemProfiles.size()+4;++k)WritePrivateProfileStringW(L"menu",(L"item-profile-"+std::to_wstring(k)).c_str(),nullptr,configPath.c_str());
    for(auto* option:MenuToggles())WritePrivateProfileStringW(L"menu",Wide(option->key).c_str(),nullptr,configPath.c_str());
    for(auto& key:menuKeys){WritePrivateProfileStringW(L"menu",Wide(key.key).c_str(),nullptr,configPath.c_str());WritePrivateProfileStringW(L"menu",Wide(std::string(key.key)+"-code").c_str(),nullptr,configPath.c_str());}
    LoadMenuSettings();profileOpen=-1;resetArmed=-1;}}}
 ImGui::SetCursorPosY(y*s);ImGui::Dummy(ImVec2(1,1));FadeEdges(s,opacity);ImGui::EndChild();
}
}
namespace{std::mutex syntheticGuard;unsigned synthPressLatch=0,synthReleaseLatch=0,synthPrevPress=0;long long synthPressUntil[2]{},synthReleaseUntil[2]{};std::atomic<int> syntheticClicks[2]{};
 long long SynthNow(){LARGE_INTEGER f{},c{};QueryPerformanceFrequency(&f);QueryPerformanceCounter(&c);return (c.QuadPart/f.QuadPart)*1000000000LL+(c.QuadPart%f.QuadPart)*1000000000LL/f.QuadPart;}}
void RecordClick(int button){if(button>=0&&button<2)++syntheticClicks[button];}
int TakeSyntheticClicks(int button){return button>=0&&button<2?syntheticClicks[button].exchange(0):0;}
void ForceButton(int button,bool press,long long durationNs){if(button<0||button>1)return;{std::lock_guard<std::mutex> lock(syntheticGuard);const long long now=SynthNow();const unsigned bit=1u<<button;
 if(press){synthPressLatch|=bit;synthPressUntil[button]=now+durationNs;}else{synthReleaseLatch|=bit;synthReleaseUntil[button]=now+durationNs;}}RecordClick(button);}
void ClearButton(int button){if(button<0||button>1)return;std::lock_guard<std::mutex> lock(syntheticGuard);const unsigned bit=~(1u<<button);synthPressLatch&=bit;synthReleaseLatch&=bit;synthPrevPress&=bit;synthPressUntil[button]=0;synthReleaseUntil[button]=0;}
unsigned SyntheticButtons(unsigned buttons){std::lock_guard<std::mutex> lock(syntheticGuard);const long long now=SynthNow();
 const unsigned press=(unsigned)(now<synthPressUntil[0])|(unsigned)(now<synthPressUntil[1])<<1|synthPressLatch;
 const unsigned keep=~(((((unsigned)(now<synthReleaseUntil[0])|(unsigned)(now<synthReleaseUntil[1])<<1)|synthReleaseLatch)&buttons)|synthPrevPress);
 synthReleaseLatch&=keep;synthPrevPress=press;synthPressLatch=0;return (buttons&keep)|press;}
std::atomic<bool> vsyncDisabled{false};std::atomic<bool> streamHide{false};ImTextureID (*RequestBlur)(ImDrawList*)=nullptr;ImTextureID (*RequestMotionBlur)(ImDrawList*,const float*,float,int,float)=nullptr;void (*WriteLog)(const char*)=nullptr;const char* backendName=nullptr;ImTextureID (*UpdateImage)(int,const unsigned char*,int,int)=nullptr;ImTextureID (*GetImage)(int)=nullptr;
#include "menu_waypoints.hpp"
// Called by the renderer before NewFrame: bakes the queued sizes once the requests have settled for 0.15 s
// (a HUD element being resized asks for a new size every frame). Returns true when the font texture must be recreated.
bool FontsRebuild(){if(fontPending.empty()||ImGui::GetTime()-fontRequestedAt<.15)return false;size_t total=0;for(auto& [base,family]:fontFamilies)total+=family.sizes.size();
 bool added=false;for(auto& [base,key]:fontPending){if(total>=160)break;auto& family=fontFamilies[base];if(family.sizes.count(key)||!family.data)continue;if(AddSizedFont(family,key)){++total;added=true;}}
 fontPending.clear();if(!added)return false;ImGui::GetIO().Fonts->Build();SaveFontSizes();return true;}
void DrawMenu(float opacity,ImTextureID logo){
 windowFonts.clear();   // a new frame starts with no pushed window font
 // "Background blur": blurred game frame under the HUD and the menu, faded with the menu opening
 if(opacity>0&&interfaceOptions[0].value&&RequestBlur){auto* under=ImGui::GetBackgroundDrawList();if(auto blurred=RequestBlur(under))under->AddImage(blurred,ImVec2(0,0),ImGui::GetIO().DisplaySize,ImVec2(0,0),ImVec2(1,1),Rgba(255,255,255,opacity));}
 // "Auto save config": every change is copied into the most recently loaded or created config
 // pending setting writes go to disk 0.3 s after the last change; the auto-saved config copy waits 0.6 s
 {const double quiet=ImGui::GetTime()-settingsChangedAt;if(!pendingSaves.empty()&&quiet>.3)FlushSaves();
 if(settingsDirty&&quiet>.6&&interfaceOptions[3].value&&!currentConfig.empty()){settingsDirty=false;configEdited=false;FlushSaves();WritePrivateProfileStringW(nullptr,nullptr,nullptr,configPath.c_str());if(GetFileAttributesW(ConfigFile(currentConfig).c_str())!=INVALID_FILE_ATTRIBUTES)CopyFileW(configPath.c_str(),ConfigFile(currentConfig).c_str(),FALSE);}}
 DrawHud(logo);
 if(waypointsOpen){DrawWaypointManager(opacity);return;}
 if(opacity<=0||hudEditing||mapOpen)return;auto& io=ImGui::GetIO();
 // The menu is never enlarged beyond scale 1; it only shrinks in windows smaller than 1101 x 618.
 const float fit=std::min({io.DisplaySize.x/1101.f,io.DisplaySize.y/618.f,1.f});
 // the menu always fits the screen (8 px margin): scale first, then width and height, then position
 {const float room=8,wide=io.DisplaySize.x-2*room,tall=io.DisplaySize.y-2*room;if(fit>0&&menuW>0&&menuH>0)menuScale=std::min(menuScale,std::max(.6f,std::min(wide/(menuW*fit),tall/(menuH*fit))));
  const float k=fit*menuScale;if(k>0){menuW=std::min(menuW,std::max(820.f,wide/k));menuH=std::min(menuH,std::max(430.f,tall/k));}}
 const float s=fit*menuScale;if(s<=0)return;
 const float areaW=menuW-279,cardW=(areaW-10)/2;
 ImVec2 origin(std::floor((io.DisplaySize.x-menuW*s)/2),std::floor((io.DisplaySize.y-menuH*s)/2));
 if(menuX>=0&&menuY>=0){origin.x=std::floor(std::clamp(menuX,8.f,std::max(8.f,io.DisplaySize.x-8-menuW*s)));origin.y=std::floor(std::clamp(menuY,8.f,std::max(8.f,io.DisplaySize.y-8-menuH*s)));}
 // keep the settings panel on screen: in a narrow window the menu shifts left
 const bool panel=(tab<6&&selected>=0)||tab==7||tab==8;if(panel&&!dragMode&&origin.x+(menuW+19+297)*s>io.DisplaySize.x-8)origin.x=std::max(8.f,std::floor(io.DisplaySize.x-8-(menuW+19+297)*s));
 auto P=[&](float x,float y){return ImVec2(origin.x+x*s,origin.y+y*s);};
 ImGui::SetNextWindowPos(origin);ImGui::SetNextWindowSize(ImVec2(menuW*s,menuH*s));ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));ImGui::PushStyleVar(ImGuiStyleVar_Alpha,opacity);
 ImGui::Begin("##NickelInGame",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoBringToFrontOnFocus|(opened.load()?0:ImGuiWindowFlags_NoInputs));
 // backdrop behind the menu
 auto* d=ImGui::GetWindowDrawList();if(opacity<.05f)backdropBorn=-1;DrawBackdrop(d,P(229,0),P(menuW,menuH),0,s,opacity);if(interfaceOptions[5].value&&RequestBlur)if(auto soft=RequestBlur(d)){const ImVec2 lo=P(229,0),hi=P(menuW,menuH),screen=io.DisplaySize;   // "Blur theme": the backdrop just drawn, blurred in place
  if(screen.x>0&&screen.y>0)d->AddImage(soft,lo,hi,ImVec2(lo.x/screen.x,lo.y/screen.y),ImVec2(hi.x/screen.x,hi.y/screen.y),Rgba(255,255,255,opacity));}d->AddRectFilled(origin,P(244,menuH),Rgba(8,8,11,opacity),0,ImDrawFlags_RoundCornersLeft);d->AddLine(P(244,0),P(244,menuH),Rgba(26,26,33,opacity),1*s);d->AddRect(origin,P(menuW,menuH),Rgba(255,255,255,.1f*opacity),0);
 if(opened){
  // edge bits: 1 left, 2 right, 4 top, 8 bottom; 16 = moving by the title bar
  const ImVec2 mouse=io.MousePos,corner=P(menuW,menuH);const float grip=7;auto close=[&](float a,float b){return std::fabs(a-b)<=grip;};
  const bool inside=mouse.x>=origin.x-grip&&mouse.x<=corner.x+grip&&mouse.y>=origin.y-grip&&mouse.y<=corner.y+grip;int edge=0;
  if(inside){if(close(mouse.x,origin.x))edge|=1;if(close(mouse.x,corner.x))edge|=2;if(close(mouse.y,origin.y))edge|=4;if(close(mouse.y,corner.y))edge|=8;}
  const bool header=inside&&!edge&&mouse.y<origin.y+64*s&&mouse.x>origin.x+244*s&&mouse.x<corner.x-(tab==5?425:tab<6?270:tab==7||tab==8?296:140)*s;const int mode=dragMode?dragMode:edge;
  if(mode&&mode!=16)ImGui::SetMouseCursor(((mode&3)&&(mode&12))?((((mode&1)&&(mode&4))||((mode&2)&&(mode&8)))?ImGuiMouseCursor_ResizeNWSE:ImGuiMouseCursor_ResizeNESW):(mode&3)?ImGuiMouseCursor_ResizeEW:ImGuiMouseCursor_ResizeNS);
  auto keep=[&]{auto write=[&](const wchar_t* key,float value){WritePrivateProfileStringW(L"menu",key,std::to_wstring(value).c_str(),configPath.c_str());};write(L"width",menuW);write(L"height",menuH);write(L"scale",menuScale);write(L"x",menuX);write(L"y",menuY);};
  if(header&&ImGui::IsMouseDoubleClicked(0)){menuW=1012;menuH=578;menuScale=1;menuX=menuY=-1;dragMode=0;keep();}
  else if(!dragMode&&io.MouseClicked[0]&&(edge||header)){dragMode=edge?edge:16;dragStart=mouse;dragW=menuW;dragH=menuH;dragScale=menuScale;dragX=origin.x;dragY=origin.y;menuX=origin.x;menuY=origin.y;}
  if(dragMode){
   if(!io.MouseDown[0]){dragMode=0;keep();}
   else{const float dx=mouse.x-dragStart.x,dy=mouse.y-dragStart.y;
    if(dragMode==16){menuX=dragX+dx;menuY=dragY+dy;}
    else if((dragMode&3)&&(dragMode&12)){   // corner: uniform scale, the opposite corner stays in place
     const float width=dragW*fit*dragScale,height=dragH*fit*dragScale,roomX=(dragMode&2)?io.DisplaySize.x-8-dragX:dragX+width-8,roomY=(dragMode&8)?io.DisplaySize.y-8-dragY:dragY+height-8;
     const float next=std::clamp(dragScale*(width+((dragMode&2)?dx:-dx))/width,.6f,std::max(.6f,std::min({1.8f,roomX/(dragW*fit),roomY/(dragH*fit)})));   // stops at the screen edges
     menuScale=next;menuX=(dragMode&1)?dragX+width-dragW*fit*next:dragX;menuY=(dragMode&4)?dragY+height-dragH*fit*next:dragY;}
    else{const float unit=fit*menuScale;   // edge: stretch that side only
     // each side stops at the screen edge (8 px margin)
     const float maxRight=std::max(820.f,(io.DisplaySize.x-8-dragX)/unit),maxLeft=std::max(820.f,(dragX+dragW*unit-8)/unit),maxDown=std::max(430.f,(io.DisplaySize.y-8-dragY)/unit),maxUp=std::max(430.f,(dragY+dragH*unit-8)/unit);
     if(dragMode&2)menuW=std::clamp(dragW+dx/unit,820.f,std::min(1600.f,maxRight));if(dragMode&1){const float w=std::clamp(dragW-dx/unit,820.f,std::min(1600.f,maxLeft));menuX=dragX+(dragW-w)*unit;menuW=w;}
     if(dragMode&8)menuH=std::clamp(dragH+dy/unit,430.f,std::min(1000.f,maxDown));if(dragMode&4){const float h=std::clamp(dragH-dy/unit,430.f,std::min(1000.f,maxUp));menuY=dragY+(dragH-h)*unit;menuH=h;}}
   }
  }
 }
 // ---- left column (frame #08080B, 32 u entries with radius 12, active #14141B, 11 px uppercase captions #60606A)
 if(logo)d->AddImage(logo,P(26,24),P(56,54),ImVec2(0,0),ImVec2(1,1),Rgba(255,255,255,opacity));
 {Label(d,semibold,20*s,P(66,28),"Nickel",Rgba(255,255,255,opacity));const float pill=66+TextWidth(semibold,20,"Nickel")+10;
  const float pillW=16+TextWidth(semibold,11,"V1 Beta");d->AddRectFilled(P(pill,31),P(pill+pillW,51),Rgba(35,35,44,opacity),0);Label(d,semibold,11*s,P(pill+8,34.5f),"V1 Beta",Rgba(148,148,160,opacity));}
 const float otherTop=menuH-48-3*34.f;   // OTHER sits at the bottom of the column: Schematics ends 16 u above the edge
 Label(d,semibold,10.5f*s,P(28,88),"FEATURES",Rgba(120,120,132,opacity));Label(d,semibold,10.5f*s,P(28,otherTop-18),"OTHER",Rgba(120,120,132,opacity));
 for(int i=0;i<10;++i){
  const float y=i<6?106.f+i*34.f:otherTop+(i-6)*34.f;bool active=tab==i;auto id="nav"+std::to_string(i);const float hover=NavHover(id,Hover(P(16,y),ImVec2(212*s,32*s))?1:0,io.DeltaTime);
  if(active)d->AddRectFilled(P(16,y),P(228,y+32),Rgba(20,20,27,opacity),0);else if(hover>.001f)d->AddRectFilled(P(16,y),P(228,y+32),Rgba(15,15,20,hover*opacity),0);
  DrawIcon(d,i,P(27,y+7.5f),17*s,active?Acc(opacity):Rgba(148,148,160,opacity));Label(d,semibold,13.5f*s,P(52,y+8),names[i],active?Rgba(255,255,255,opacity):Rgba(148,148,160,opacity));
  if(Hit(id.c_str(),P(16,y),ImVec2(212*s,32*s))){tab=i;if(i<6)category=i;selected=-1;listening=-1;search[0]=0;}
 }
 // ---- right side: animated theme background, a header on top and a floating panel (#0F0F14, radius 20, border #1A1A21)
 // Compact header on every page: "Nickel / <page>" in small type at the height of the search field (module pages add
// the module count next to it and the enabled count left of the search field), so the panel starts right below.
const bool modulesTab=tab<6;const float panelTop=64.f;const std::string crumb=std::string("Nickel  /  ")+(modulesTab&&search[0]?"Search":names[tab]);
 Label(d,regular,12.5f*s,P(266,31),crumb.c_str(),Rgba(255,255,255,.8f*opacity));const float crumbEnd=266+TextWidth(regular,12.5f,crumb.c_str());
 d->AddRectFilled(P(254,panelTop),P(menuW-12,menuH-12),Rgba(15,15,20,.97f*opacity),0);d->AddRect(P(254,panelTop),P(menuW-12,menuH-12),Rgba(26,26,33,opacity),0);
 // "Edit layout" opens the HUD layout editor. It sits 8 u left of the search field at the same height;
 // width = 44 u + text; category icon (14 u) + 6 u + 17.758 u text, centred; 9 % white fill -> 14 % on hover
 // (speed 16), radius 8 u, pressed offset of 1 u.
 if(tab==5){const float u=242.f/210.f,text=TextWidth(semibold,15,"Edit layout"),w=44*u+text,x=menuW-264-8*u-w;const bool over=opened&&Hover(P(x,20),ImVec2(w*s,37*s));
  static float glow=0;glow=std::clamp(glow+(over?16.f:-16.f)*io.DeltaTime,0.f,1.f);const float inset=over&&io.MouseDown[0]?u:0,start=x+(w-(14*u+6*u+text))/2;
  d->AddRectFilled(P(x+inset,20+inset),P(x+w-inset,57-inset),Rgba(255,255,255,(.09f+.05f*glow)*opacity),0);
  DrawIcon(d,5,P(start,38.5f-7*u),14*u*s,Rgba(230,230,235,opacity));Label(d,semibold,15*s,P(start+20*u,28),"Edit layout",Rgba(230,230,235,opacity));
  if(Hit("##editLayout",P(x,20),ImVec2(w*s,37*s))){hudEditing=true;selected=-1;listening=-1;}}
 WindowFont(s*15.f*FontEmFactor);
 if(tab<6){
  // search field spans x 748..990, magnifier on the left, text from x = 785
  ImGui::SetCursorScreenPos(P(menuW-264,20));ImGui::SetNextItemWidth(242*s);ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,0);ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(37*s,8.75f*s));ImGui::PushStyleColor(ImGuiCol_FrameBg,ImVec4(15/255.f,15/255.f,20/255.f,.96f));ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,ImVec4(20/255.f,20/255.f,27/255.f,.97f));ImGui::PushStyleColor(ImGuiCol_FrameBgActive,ImVec4(20/255.f,20/255.f,27/255.f,.98f));
  ImGui::InputTextWithHint("##Search","Search modules",search,sizeof(search));ImGui::PopStyleColor(3);ImGui::PopStyleVar(2);
  if(!DrawGlyph(d,nicons::k_search,P(menuW-251,34),15*s,Rgba(130,130,142,opacity))){d->AddCircle(P(menuW-245.5f,39.5f),5*s,Rgba(130,130,142,opacity),0,1.5f*s);d->AddLine(P(menuW-241.8f,43.2f),P(menuW-238,47),Rgba(130,130,142,opacity),1.5f*s);}
  std::vector<int> list;std::string query=search;std::transform(query.begin(),query.end(),query.begin(),[](unsigned char c){return (char)std::tolower(c);});
  // search: case-insensitive substring match on the module NAME only
  for(int i=0;i<(int)modules.size();++i){auto& m=modules[i];auto hay=m.name;std::transform(hay.begin(),hay.end(),hay.begin(),[](unsigned char c){return (char)std::tolower(c);});if(!m.category.empty()&&(query.empty()?m.category==names[category]:hay.find(query)!=std::string::npos))list.push_back(i);}
  // Card order follows the catalogue (tools/build_native_catalog.py) and also applies to search results.
  // Subtitle on the background: module count, with the enabled count in a highlight (white box, accent text).
  {int on=0;for(int i:list)if(modules[i].enabled)++on;const std::string count=std::string("  \xc2\xb7  ")+std::to_string(list.size())+(list.size()==1?" module":" modules");
   Label(d,regular,12.5f*s,P(crumbEnd,31),count.c_str(),Rgba(255,255,255,.8f*opacity));
   const std::string mark=std::to_string(on)+" enabled";const float w=TextWidth(semibold,12.5f,mark.c_str())+12,x=menuW-264-10-w-(tab==5?(52*242.f/210.f+TextWidth(semibold,15,"Edit layout")):0.f);   // left of the search field (HUD: left of "Edit layout")
   d->AddRectFilled(P(x,29),P(x+w,48),Rgba(255,255,255,opacity));Label(d,semibold,12.5f*s,P(x+6,31),mark.c_str(),AccDeep(opacity));}
  // modules as list rows in two columns: 34 u initial badge, name, description, keybind, toggle;
  // #1A1A21 rule between rows, #14141B hover with radius 12, selection = accent bar on the left
  const float px0=268,pw=menuW-12-14-px0,colW=(pw-14)/2,rowH=60;
  ImGui::SetCursorScreenPos(P(px0,panelTop+10));ImGui::BeginChild("##ModuleCards",ImVec2(pw*s,(menuH-12-panelTop-20)*s),false,ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoScrollbar);
  const auto start=ImGui::GetCursorScreenPos();
  if(list.empty()){const char* empty=query.empty()?"No modules in this category.":"No modules match your search.";Label(ImGui::GetWindowDrawList(),regular,14*s,ImVec2(start.x+(pw*s-TextWidth(regular,14,empty)*s)/2,start.y+170*s),empty,Rgba(120,120,132,opacity));}
  const int rows=((int)list.size()+1)/2;
  // only the rows in view (plus 2 above and below) are built; the scroll range still counts every row
  const float viewH=(menuH-12-panelTop-20)*s,scrollY=ImGui::GetScrollY();const int firstRow=std::max(0,(int)(scrollY/(rowH*s))-2),lastRow=(int)((scrollY+viewH)/(rowH*s))+2;
  for(int n=firstRow*2;n<std::min((int)list.size(),(lastRow+1)*2);++n){
   auto index=list[n];auto& m=modules[index];ImVec2 p(start.x+(n%2)*(colW+14)*s,start.y+(n/2)*rowH*s);ImVec2 size(colW*s,(rowH-4)*s);auto id="card"+m.id;const bool soon=SoonModule(m.id);if(soon&&selected==index)selected=-1;float hover=Animate(id,!soon&&Hover(p,size)?1:0,io.DeltaTime);const float chosen=Animate("chosen"+m.id,index==selected?1:0,io.DeltaTime,14);
   const float lit=Animate("lit"+m.id,m.enabled?1.f:0.f,io.DeltaTime);d=ImGui::GetWindowDrawList();
   const float back=std::max(hover,chosen);if(back>.001f)d->AddRectFilled(p,ImVec2(p.x+size.x,p.y+size.y),Rgba(20,20,27,back*opacity),0);
   if(n/2<rows-1&&back<.5f)d->AddLine(ImVec2(p.x+12*s,p.y+(rowH-2)*s),ImVec2(p.x+size.x-12*s,p.y+(rowH-2)*s),Rgba(26,26,33,(1-back)*opacity),1*s);
   if(chosen>.001f)d->AddRectFilled(ImVec2(p.x,p.y+16*s),ImVec2(p.x+3*s,p.y+(rowH-20)*s),Acc(chosen*opacity),0);
   {const ImVec2 lo(p.x+11*s,p.y+11*s),hi(p.x+45*s,p.y+45*s);d->AddRectFilled(lo,hi,ToU32(MixColor(ImVec4(23/255.f,23/255.f,29/255.f,1),themeLive.accentDeep,lit),opacity),0);
    const ImU32 ink=ToU32(MixColor(ImVec4(148/255.f,148/255.f,160/255.f,1),ImVec4(1,1,1,1),lit),opacity);
    if(!DrawGlyph(d,nicons::ModuleIconOf(m.id.c_str()),ImVec2(lo.x+8*s,lo.y+8*s),18*s,ink)){char letter[2]={(char)std::toupper((unsigned char)(m.name.empty()?'?':m.name[0])),0};Label(d,semibold,15*s,ImVec2(lo.x+(34*s-TextWidth(semibold,15,letter)*s)/2,lo.y+8.5f*s),letter,ink);}}
   auto bound=keybinds.find(m.id);const float right=bound==keybinds.end()?colW-56:colW-92;
   d->PushClipRect(ImVec2(p.x+56*s,p.y),ImVec2(p.x+right*s,p.y+size.y),true);const bool described=interfaceOptions[1].value;   // "Module descriptions" option
   Label(d,semibold,14.5f*s,ImVec2(p.x+56*s,p.y+(described?10:19)*s),m.name.c_str(),Rgba(255,255,255,opacity));if(described)Label(d,regular,12*s,ImVec2(p.x+56*s,p.y+30*s),Fit(regular,12,m.description,right-56-8).c_str(),Rgba(148,148,160,opacity));d->PopClipRect();
   if(bound!=keybinds.end()){const float w=std::max(22.f,TextWidth(semibold,11,bound->second.c_str())+12);d->AddRectFilled(ImVec2(p.x+(colW-58-w)*s,p.y+17*s),ImVec2(p.x+(colW-58)*s,p.y+39*s),Rgba(35,35,44,opacity),0);Label(d,semibold,11*s,ImVec2(p.x+(colW-58-w+(w-TextWidth(semibold,11,bound->second.c_str()))/2)*s,p.y+22*s),bound->second.c_str(),Rgba(170,170,180,opacity));}
   if(!soon&&Hit(id.c_str(),p,ImVec2((colW-55)*s,(rowH-4)*s))){selected=selected==index?-1:index;listening=-1;}
   auto toggleId="toggle"+m.id;ImVec2 t(p.x+(colW-46)*s,p.y+19*s);Toggle(d,t,s,Animate(toggleId,m.enabled?1:0,io.DeltaTime),opacity);if(!soon&&Hit(toggleId.c_str(),ImVec2(t.x-4*s,t.y-8*s),ImVec2(42*s,34*s))){m.enabled=!m.enabled;Save(m);}
   if(soon){   // "Soon": the card is covered and redrawn greyed and blurred (each text drawn 9 times around its place at low
    // opacity), with a pill in the theme accent; the game frame blur is too strong at this size and would erase it
    const ImVec2 hi(p.x+size.x,p.y+size.y);d->AddRectFilled(p,ImVec2(hi.x,hi.y-2*s),Rgba(15,15,20,opacity),0);
    d->AddRectFilled(ImVec2(p.x+11*s,p.y+11*s),ImVec2(p.x+45*s,p.y+45*s),Rgba(23,23,29,.7f*opacity),0);
    const bool described=interfaceOptions[1].value;const std::string desc=Fit(regular,12,m.description,colW-56-8-56);
    for(int gx=-1;gx<=1;++gx)for(int gy=-1;gy<=1;++gy){const float ox=gx*1.6f*s,oy=gy*1.6f*s;
     DrawGlyph(d,nicons::ModuleIconOf(m.id.c_str()),ImVec2(p.x+19*s+ox,p.y+19*s+oy),18*s,Rgba(148,148,160,.07f*opacity));
     Label(d,semibold,14.5f*s,ImVec2(p.x+56*s+ox,p.y+(described?10:19)*s+oy),m.name.c_str(),Rgba(170,170,182,.13f*opacity));
     if(described)Label(d,regular,12*s,ImVec2(p.x+56*s+ox,p.y+30*s+oy),desc.c_str(),Rgba(148,148,160,.08f*opacity));}
    const char* tag="SOON";const float w=TextWidth(semibold,11.5f,tag)+18,h=22,x=p.x+(size.x-w*s)/2,y=p.y+(size.y-h*s)/2;
    d->AddRectFilled(ImVec2(x,y),ImVec2(x+w*s,y+h*s),Acc(opacity),0);Label(d,semibold,11.5f*s,ImVec2(x+9*s,y+4.5f*s),tag,Rgba(255,255,255,opacity));}
  }
  ImGui::SetCursorPosY(rows*rowH*s+6*s);ImGui::Dummy(ImVec2(1,1));const float listScroll=ImGui::GetScrollY(),listLimit=ImGui::GetScrollMaxY();FadeEdges(s,opacity);ImGui::EndChild();
  // scrollbar to the right of the list, only when it overflows (theme accent colour)
  if(listLimit>0){const float view=menuH-12-panelTop-20,total=view+listLimit/s,bar=std::max(30.f,view*view/total),top=panelTop+10+(view-bar)*(listScroll/listLimit);
   ImGui::GetWindowDrawList()->AddRectFilled(P(px0+pw+1,top+6),P(px0+pw+4,top+bar-6),Acc(.85f*opacity),0);}
 }else if(tab==6)DrawMenuSettings(origin,s,opacity);
 else if(tab==7)DrawConfigs(origin,s,opacity);
 else if(tab==8)DrawScripting(origin,s,opacity);
 else if(tab==9)DrawSchematics(origin,s,opacity);
 else{ImGui::SetCursorScreenPos(P(273,90));ImGui::BeginGroup();ImGui::TextWrapped("%s","This section is being reconstructed from the original client.");ImGui::EndGroup();}
 EndWindow();
 // `selected` and `tab` may have changed during this frame (category click or the open card), so read them again
 // here; reusing the earlier `panel` value could index modules[-1].
 if(tab<6&&selected>=0&&selected<(int)modules.size())DrawSettingsPanel(modules[selected],P(menuW+19,0),s,opacity);
 if(tab==7)DrawConfigPanel(P(menuW+19,0),s,opacity);
 if(tab==8)DrawScriptPanel(P(menuW+19,0),s,opacity);
 if(tab==9)DrawSchematicPanel(P(menuW+19,0),s,opacity);
 // open item profile: 0.2 s cubic entry, 6 u slide
 if(tab==6&&profileOpen>=0&&profileOpen<(int)itemProfiles.size()){const float t=std::clamp((float)(ImGui::GetTime()-profileOpenedAt)/.2f,0.f,1.f),e=1-(1-t)*(1-t)*(1-t);
  DrawSettingsPanel(profileModule,P(menuW+19+(1-e)*6,0),s,opacity*e);SyncProfile();}
 ImGui::PopStyleVar(2);
}
}
