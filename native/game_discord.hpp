// Discord Presence: speaks the discord-rpc protocol directly over Discord's local pipe (\\.\pipe\discord-ipc-0..9):
// handshake (op 0, v 1, client_id) then SET_ACTIVITY (op 1) with Minecraft's PID.
// Activity:
// - details: "Currently Idle..." with no level, "Playing Singleplayer" or "Playing Multiplayer" (remote server),
// truncated to 127 bytes; no state;
// - start time = when the module was enabled;
// - large image = Nickel app icon, text "Nickel"; small image = Minecraft app icon, text "Minecraft";
// - "Website" and "Discord" buttons pointing to nickelclient.com and the Discord server.
// Application id: kNickelDiscordApp, or [discord] client_id / icon in %APPDATA%\Nickel\discord.ini.
// Without an id nothing is sent.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include "game_events.hpp"

namespace game {
inline const char* kNickelDiscordApp = "1569624154066387026";   // Nickel Discord application id
struct DiscordShared{std::mutex guard;std::atomic<bool> enabled{false},running{false};std::string details,sent;int64_t start=0;std::string appId,icon;void(*log)(const char*)=nullptr;};
inline DiscordShared& Discord(){static auto* s=new DiscordShared;return *s;}
inline std::string DiscordEscape(const std::string& in){std::string o;for(char ch:in){if(ch=='"'||ch=='\\'){o+='\\';o+=ch;}else if((unsigned char)ch<0x20)o+=' ';else o+=ch;}return o;}
inline bool DiscordWrite(HANDLE pipe,uint32_t op,const std::string& body){std::string frame(8,'\0');memcpy(&frame[0],&op,4);const uint32_t n=(uint32_t)body.size();memcpy(&frame[4],&n,4);frame+=body;
 DWORD done=0;return WriteFile(pipe,frame.data(),(DWORD)frame.size(),&done,nullptr)&&done==frame.size();}
inline bool DiscordRead(HANDLE pipe,std::string* body){uint32_t head[2]{};DWORD got=0,avail=0;if(!PeekNamedPipe(pipe,nullptr,0,nullptr,&avail,nullptr))return false;if(avail<8)return true;
 if(!ReadFile(pipe,head,8,&got,nullptr)||got!=8||head[1]>65536)return false;std::string b(head[1],'\0');if(head[1]&&(!ReadFile(pipe,&b[0],head[1],&got,nullptr)||got!=head[1]))return false;if(body)*body=b;return true;}
inline HANDLE DiscordOpen(){for(int i=0;i<10;++i){char name[64];snprintf(name,sizeof name,"\\\\.\\pipe\\discord-ipc-%d",i);
  HANDLE h=CreateFileA(name,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);if(h!=INVALID_HANDLE_VALUE)return h;}return INVALID_HANDLE_VALUE;}
inline void DiscordLoadIds(DiscordShared& s){char path[MAX_PATH]{};if(!GetEnvironmentVariableA("APPDATA",path,MAX_PATH))return;std::string ini=std::string(path)+"\\Nickel\\discord.ini";
 char v[128]{};GetPrivateProfileStringA("discord","client_id",kNickelDiscordApp,v,sizeof v,ini.c_str());s.appId=v;GetPrivateProfileStringA("discord","icon","https://cdn.discordapp.com/app-icons/1569624154066387026/f2767b7d518e909a6356a7b17c14bf66.png",v,sizeof v,ini.c_str());s.icon=v;}
// Background thread: connects to Discord, sends the activity when it changes (at most once every 15 s, like
// discord-rpc) and clears it on shutdown.
inline void DiscordThread(){auto& s=Discord();HANDLE pipe=INVALID_HANDLE_VALUE;ULONGLONG lastSend=0,lastTry=0;bool told=false;
 while(true){const bool on=s.enabled.load();std::string details,appId,icon;int64_t start=0;{std::lock_guard<std::mutex> lock(s.guard);details=s.details;appId=s.appId;icon=s.icon;start=s.start;}
  if(!on){if(pipe!=INVALID_HANDLE_VALUE){DiscordWrite(pipe,1,"{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":"+std::to_string(GetCurrentProcessId())+"},\"nonce\":\"nickel-clear\"}");CloseHandle(pipe);pipe=INVALID_HANDLE_VALUE;}
   std::lock_guard<std::mutex> lock(s.guard);s.sent.clear();s.running=false;return;}
  if(appId.empty()){if(!told&&s.log){told=true;s.log("Discord Presence: no Discord application id (kNickelDiscordApp or %APPDATA%\\Nickel\\discord.ini [discord] client_id)");}Sleep(1000);continue;}
  if(pipe==INVALID_HANDLE_VALUE){if(GetTickCount64()-lastTry<5000){Sleep(250);continue;}lastTry=GetTickCount64();pipe=DiscordOpen();
   if(pipe==INVALID_HANDLE_VALUE){Sleep(250);continue;}
   if(!DiscordWrite(pipe,0,"{\"v\":1,\"client_id\":\""+DiscordEscape(appId)+"\"}")){CloseHandle(pipe);pipe=INVALID_HANDLE_VALUE;continue;}
   if(s.log)s.log("Discord Presence: connected to Discord");std::lock_guard<std::mutex> lock(s.guard);s.sent.clear();}
  std::string reply;if(!DiscordRead(pipe,&reply)){if(s.log)s.log("Discord Presence: pipe read failed, reconnecting");CloseHandle(pipe);pipe=INVALID_HANDLE_VALUE;continue;}
  if(!reply.empty()&&reply.find("\"ERROR\"")!=std::string::npos&&s.log){std::string t="Discord Presence: "+reply.substr(0,180);s.log(t.c_str());}
  else if(!reply.empty()&&reply.find("SET_ACTIVITY")!=std::string::npos&&s.log){static std::string last;std::string t="Discord Presence: activity set ("+details+")";if(t!=last){last=t;s.log(t.c_str());}}
  std::string sent;{std::lock_guard<std::mutex> lock(s.guard);sent=s.sent;}
  if(details!=sent&&GetTickCount64()-lastSend>=(sent.empty()?0:15000)){
   const std::string large=icon.empty()?std::string():"\"large_image\":\""+DiscordEscape(icon)+"\",";
   const std::string body="{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":"+std::to_string(GetCurrentProcessId())+",\"activity\":{\"details\":\""+DiscordEscape(details)+"\","
    "\"timestamps\":{\"start\":"+std::to_string(start)+"},\"assets\":{"+large+"\"large_text\":\"Nickel\","
    "\"small_image\":\"https://cdn.discordapp.com/app-icons/1402418491272986635/166fbad351ecdd02d11a3b464748f66b.png\",\"small_text\":\"Minecraft\"},"
    "\"buttons\":[{\"label\":\"Website\",\"url\":\"https://nickelclient.com\"},{\"label\":\"Discord\",\"url\":\"https://discord.gg/pn2FdFhY9j\"}]}},\"nonce\":\"nickel-"+std::to_string(GetTickCount64())+"\"}";
   if(DiscordWrite(pipe,1,body)){lastSend=GetTickCount64();std::lock_guard<std::mutex> lock(s.guard);s.sent=details;}else{CloseHandle(pipe);pipe=INVALID_HANDLE_VALUE;continue;}}
  Sleep(250);}}
// Enable: activity start time, ids re-read. Disable: the thread clears the activity and exits.
inline void DiscordSetEnabled(bool on,void(*log)(const char*)){auto& s=Discord();if(on==s.enabled.load())return;
 if(on){{std::lock_guard<std::mutex> lock(s.guard);s.log=log;DiscordLoadIds(s);s.start=(int64_t)time(nullptr);s.sent.clear();if(s.details.empty())s.details="Currently Idle...";}
  s.enabled=true;if(!s.running.exchange(true))std::thread(DiscordThread).detach();}
 else s.enabled=false;}
// GameUpdate: text depends on the level and the connection.
inline void DiscordDetails(bool inLevel,bool multiplayer){auto& s=Discord();if(!s.enabled)return;std::string d=!inLevel?"Currently Idle...":(multiplayer?"Playing Multiplayer":"Playing Singleplayer");
 if(d.size()>127)d.resize(127);std::lock_guard<std::mutex> lock(s.guard);s.details=d;}
// GameUpdate #30: level = client->vtable[0xf0]; multiplayer = client->vtable[0x370] true and the 16-bit field
// at client+0x6f8 non-zero (offsets from the signature table).
inline void DiscordGameUpdate(){static std::atomic<bool> fault{false};if(!Discord().enabled)return;__try{Context c{};if(!Client(&c))return;
 const uint64_t lv=Virtual0(c,c.client,0xf0),mp=Virtual0(c,c.client,0x370);uint64_t level=lv?reinterpret_cast<uint64_t(*)(uint64_t)>(lv)(c.client):0;bool multi=false;
 if(level&&mp&&(reinterpret_cast<uint8_t(*)(uint64_t)>(mp)(c.client)&1)){uint16_t n{};multi=Read(c.client+0x6f8,&n)&&n!=0;}
 DiscordDetails(level!=0,multi);}__except(EXCEPTION_EXECUTE_HANDLER){if(!fault.exchange(true)&&Discord().log)Discord().log("Discord Presence: fault while reading the game state (retried on the next update)");}}
}
