// Validated game builds, as seen from inside the game process. The overlay only activates in a Minecraft.Windows.exe
// whose version resource names a validated build AND whose code (.text), constants (.rdata) and data (.data) sections
// sit at that build's addresses with that build's sizes: every offset and hook of the overlay depends on that layout.
// The launcher (desktop/GameBuilds.cs) also compares the bytes of these sections with the build before loading.
// Keep both tables in step (tools/check_game_builds.py, run by ctest).
#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstring>

namespace game {
struct GameSection{char name[8];uint32_t address,size;};
struct GameBuildProfile{const char* name;uint16_t version[3];GameSection sections[3];};
inline constexpr GameBuildProfile kGameBuilds[]={
 {"Minecraft 1.26.62.1",{1,26,62},{{".text",0x1000u,0xeec65d6u},{".rdata",0xeec8000u,0x3441284u},{".data",0x1230a000u,0x8d4b68u}}},
};

// File version (major.minor.build.private) from a module's version resource (VS_FIXEDFILEINFO).
inline bool GameVersion(HMODULE image,uint16_t version[4]){
 const HRSRC found=FindResourceW(image,MAKEINTRESOURCEW(1),MAKEINTRESOURCEW(16));if(!found)return false;   // 16 = RT_VERSION
 const HGLOBAL loaded=LoadResource(image,found);const auto* data=loaded?(const uint8_t*)LockResource(loaded):nullptr;const DWORD size=SizeofResource(image,found);
 if(!data||size<sizeof(VS_FIXEDFILEINFO))return false;
 for(DWORD at=0;at+sizeof(VS_FIXEDFILEINFO)<=size;at+=4){VS_FIXEDFILEINFO info;memcpy(&info,data+at,sizeof info);if(info.dwSignature!=0xFEEF04BDu)continue;
  version[0]=HIWORD(info.dwFileVersionMS);version[1]=LOWORD(info.dwFileVersionMS);version[2]=HIWORD(info.dwFileVersionLS);version[3]=LOWORD(info.dwFileVersionLS);return true;}
 return false;}

// True when the PE image (headers at image) has every section of the profile at its address with its size.
inline bool LayoutMatches(const uint8_t* image,const GameBuildProfile& build){
 if(!image)return false;auto dos=(const IMAGE_DOS_HEADER*)image;if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<=0||dos->e_lfanew>0x1000)return false;
 auto nt=(const IMAGE_NT_HEADERS64*)(image+dos->e_lfanew);
 if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)return false;
 const IMAGE_SECTION_HEADER* sections=IMAGE_FIRST_SECTION(nt);const unsigned count=nt->FileHeader.NumberOfSections;if(count==0||count>96)return false;
 for(const auto& want:build.sections){bool found=false;
  for(unsigned i=0;i<count&&!found;++i)found=!memcmp(sections[i].Name,want.name,8)&&sections[i].VirtualAddress==want.address&&sections[i].Misc.VirtualSize==want.size;
  if(!found)return false;}
 return true;}

// Index of the validated build the image holds (version and layout), or -1.
inline int MatchBuild(const uint8_t* image,const uint16_t version[4]){
 for(int i=0;i<(int)(sizeof(kGameBuilds)/sizeof(kGameBuilds[0]));++i){const auto& b=kGameBuilds[i];
  if(version[0]==b.version[0]&&version[1]==b.version[1]&&version[2]==b.version[2]&&LayoutMatches(image,b))return i;}
 return -1;}
}
