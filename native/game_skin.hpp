// Skin Stealer (Utility): saves the skin of the player under the crosshair as a PNG.
// - Target: level hit result (*(player+0x200))->vtable[0xa78]; must be type 1 (entity) and a player,
// otherwise the message "Aim at a player before activating Skin Stealer".
// - Player list entry: level->vtable[0x9f8]() -> linked list (head at +8), ActorUniqueID key at node+0x20;
// skin image = *(entry+0xa0): format +0x80 (4 = RGBA8), width +0x84, height +0x88, data *(+0xa0), size
// +0xa8.
// - File: %APPDATA%\Nickel\skins\<name>.png. Messages module.skin_stealer.result.*; the module then turns
// itself off.
#pragma once
#include "game_packets.hpp"
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <atomic>
#include <string>
#include <vector>
#pragma comment(lib,"windowscodecs.lib")

namespace game {
struct SkinStealerShared{std::atomic<bool> request{false},done{false};};
inline SkinStealerShared& SkinStealer(){static auto* s=new SkinStealerShared;return *s;}
inline bool SkinSavePng(const std::wstring& path,const uint8_t* rgba,UINT w,UINT h){using Microsoft::WRL::ComPtr;CoInitializeEx(nullptr,COINIT_MULTITHREADED);ComPtr<IWICImagingFactory> f;ComPtr<IWICStream> s;ComPtr<IWICBitmapEncoder> e;ComPtr<IWICBitmapFrameEncode> fr;
 if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f)))||FAILED(f->CreateStream(&s))||FAILED(s->InitializeFromFilename(path.c_str(),GENERIC_WRITE))||FAILED(f->CreateEncoder(GUID_ContainerFormatPng,nullptr,&e))||FAILED(e->Initialize(s.Get(),WICBitmapEncoderNoCache))||FAILED(e->CreateNewFrame(&fr,nullptr))||FAILED(fr->Initialize(nullptr))||FAILED(fr->SetSize(w,h)))return false;
 // The PNG encoder's native 32-bit format is BGRA (it may refuse RGBA): the red and blue channels are swapped first.
 WICPixelFormatGUID fmt=GUID_WICPixelFormat32bppBGRA;if(FAILED(fr->SetPixelFormat(&fmt))||fmt!=GUID_WICPixelFormat32bppBGRA)return false;
 std::vector<uint8_t> bgra(rgba,rgba+(size_t)w*h*4);for(size_t i=0;i+3<bgra.size();i+=4)std::swap(bgra[i],bgra[i+2]);
 return SUCCEEDED(fr->WritePixels(h,w*4,w*h*4,bgra.data()))&&SUCCEEDED(fr->Commit())&&SUCCEEDED(e->Commit());}
inline std::wstring SkinWide(const std::string& t){int n=MultiByteToWideChar(CP_UTF8,0,t.c_str(),-1,nullptr,0);std::wstring w(n>0?n-1:0,L'\0');if(n>0)MultiByteToWideChar(CP_UTF8,0,t.c_str(),-1,w.data(),n);return w;}
inline void SkinStealerWork(){Context c{};if(!Local(&c))return;uint64_t level{},hit{},holder{},actor{};int32_t kind{};uint32_t entity{};uintptr_t owners{},slot{};
 auto say=[&](const std::string& text){ClientMessage(c,text);};
 bool target=Read(c.actor+0x200,&level)&&level;if(target){const uint64_t get=Virtual0(c,level,0xa78);target=get&&CallGetter(get,level,&hit)&&hit&&Read(hit+0x18,&kind)&&kind==1&&Read(hit+0x38,&holder)&&holder&&Read(hit+0x48,&entity);}
 if(target)target=Storage(holder+0x30,Fnv("ActorOwnerComponent"),&owners)&&InStorage(owners,entity,8,&slot)&&Read(slot,&actor)&&actor&&Has(holder+0x30,entity,"PlayerComponent");
 if(!target){say("Aim at a player before activating Skin Stealer");return;}
 char name[256]{};RawText(actor+0xbe8,name,sizeof(name));const std::string who=name;
 uintptr_t element{};uint64_t unique{};if(!Element(holder+0x30,entity,"ActorUniqueIDComponent",8,&element)||!Read(element,&unique)){say("Player '"+who+"' is not available");return;}
 const uint64_t listOf=Virtual0(c,level,0x9f8);uint64_t list{},head{},node{},entry=0;if(listOf&&CallGetter(listOf,level,&list)&&list&&Read(list+8,&head)&&head&&Read(head,&node))
  for(int guard=0;guard<4096&&node&&node!=head;++guard){uint64_t key{};if(Read(node+0x20,&key)&&key==unique){entry=node+0x20;break;}if(!Read(node,&node))break;}
 if(!entry){say("Player '"+who+"' is not available");return;}
 uint64_t skin{};if(!Read(entry+0xa0,&skin)||!skin){say(who+"'s skin is not loaded");return;}
 int32_t format{};uint32_t w{},h{};uint64_t data{},size{};if(!Read(skin+0x80,&format)||format!=4||!Read(skin+0x84,&w)||!Read(skin+0x88,&h)||!w||!h||w>0x1fffffff||(int32_t)h<0||!Read(skin+0xa0,&data)||!data||!Read(skin+0xa8,&size)||size<(uint64_t)w*h*4){say(who+"'s skin data is invalid");return;}
 PWSTR roaming=nullptr;std::wstring dir;if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData,0,nullptr,&roaming))&&roaming){dir=std::wstring(roaming)+L"\\Nickel";CoTaskMemFree(roaming);CreateDirectoryW(dir.c_str(),nullptr);dir+=L"\\skins";CreateDirectoryW(dir.c_str(),nullptr);}
 if(dir.empty()||GetFileAttributesW(dir.c_str())==INVALID_FILE_ATTRIBUTES){say("Failed to create the Nickel skins directory");return;}
 std::string file=who;for(auto& ch:file)if(strchr("\\/:*?\"<>|",ch))ch='_';if(file.empty())file="player";
 std::vector<uint8_t> pixels((size_t)w*h*4);memcpy(pixels.data(),(const void*)data,pixels.size());
 if(!SkinSavePng(dir+L"\\"+SkinWide(file)+L".png",pixels.data(),w,h)){DeleteFileW((dir+L"\\"+SkinWide(file)+L".png").c_str());say("Failed to save "+who+"'s skin");return;}
 say("Saved "+who+"'s skin to skins/"+file+".png");}
inline void SkinStealerGuarded(){static volatile long stopped_=0;if(!game::FeatureStopped(&stopped_))__try{SkinStealerWork();}__except(game::FeatureFault("SkinStealerGuarded",&stopped_,GetExceptionInformation())){game::FeatureFlush();}}
inline void SkinStealerTick(){auto& s=SkinStealer();if(s.request.exchange(false)){SkinStealerGuarded();s.done=true;}}
}
