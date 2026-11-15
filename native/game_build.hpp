// Validated game builds, as seen from inside the game process. The overlay only activates in a Minecraft.Windows.exe
// whose version resource names a validated build, whose code (.text), constants (.rdata), data (.data), unwind table
// (.pdata) and relocations (.reloc) sections sit at that build's addresses with that build's sizes, whose entry point
// and load-time directories (exports, imports, exception table, relocations, TLS, load configuration, import address
// table, delayed imports, .NET header) are the build's, and whose loaded code hashes to the build's code (SHA-256 of .text, which has no relocations). Every
// offset and hook of the overlay depends on that build. The launcher (desktop/GameBuilds.cs) runs the same checks on
// the file and on the loaded code before it loads the overlay. Keep both tables in step (tools/check_game_builds.py).
#pragma once
#include <Windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <cstring>
#pragma comment(lib,"bcrypt.lib")

namespace game {
struct GameSection{char name[8];uint32_t address,size;};
struct GameDirectory{int index;uint32_t address,size;};
struct GameBuildProfile{const char* name;uint16_t version[3];uint32_t entryPoint;GameDirectory directories[10];GameSection sections[5];uint8_t textSha256[32];};
inline constexpr GameBuildProfile kGameBuilds[]={
 {"Minecraft 1.26.62.1",{1,26,62},0xeea5e90u,
  {{0,0xf9a4468u,0x162u},{1,0xf9a45ccu,0x6f4u},{3,0x12bdf000u,0xb59b60u},{5,0x13742000u,0xfa3c4u},{9,0xf4f39c0u,0x28u},{10,0xefc6990u,0x140u},{11,0x0u,0x0u},{12,0xf9a6f20u,0x2260u},{13,0x0u,0x0u},{14,0x0u,0x0u}},
  {{".text",0x1000u,0xeec65d6u},{".rdata",0xeec8000u,0x3441284u},{".data",0x1230a000u,0x8d4b68u},{".pdata",0x12bdf000u,0xb59b60u},{".reloc",0x13742000u,0xfa3c4u}},
  {0xa3,0x44,0x3f,0x50,0xff,0x15,0x09,0x94,0xfc,0x0e,0xfd,0x0d,0xc3,0x5a,0xce,0xb2,0x40,0x9d,0x45,0xdd,0x1c,0xd0,0x31,0x11,0xbd,0x1b,0xbf,0xe8,0xde,0x4e,0xca,0x15}},
};

// File version (major.minor.build.private) from a module's version resource (VS_FIXEDFILEINFO).
inline bool GameVersion(HMODULE image,uint16_t version[4]){
 const HRSRC found=FindResourceW(image,MAKEINTRESOURCEW(1),MAKEINTRESOURCEW(16));if(!found)return false;   // 16 = RT_VERSION
 const HGLOBAL loaded=LoadResource(image,found);const auto* data=loaded?(const uint8_t*)LockResource(loaded):nullptr;const DWORD size=SizeofResource(image,found);
 if(!data||size<sizeof(VS_FIXEDFILEINFO))return false;
 for(DWORD at=0;at+sizeof(VS_FIXEDFILEINFO)<=size;at+=4){VS_FIXEDFILEINFO info;memcpy(&info,data+at,sizeof info);if(info.dwSignature!=0xFEEF04BDu)continue;
  version[0]=HIWORD(info.dwFileVersionMS);version[1]=LOWORD(info.dwFileVersionMS);version[2]=HIWORD(info.dwFileVersionLS);version[3]=LOWORD(info.dwFileVersionLS);return true;}
 return false;}

// True when the PE image (headers at image) has the profile's entry point, directories and section layout.
inline bool LayoutMatches(const uint8_t* image,const GameBuildProfile& build){
 if(!image)return false;auto dos=(const IMAGE_DOS_HEADER*)image;if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<=0||dos->e_lfanew>0x1000)return false;
 auto nt=(const IMAGE_NT_HEADERS64*)(image+dos->e_lfanew);
 if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)return false;
 if(nt->FileHeader.SizeOfOptionalHeader<sizeof(IMAGE_OPTIONAL_HEADER64)||nt->OptionalHeader.NumberOfRvaAndSizes<16)return false;
 if(nt->OptionalHeader.AddressOfEntryPoint!=build.entryPoint)return false;
 for(const auto& d:build.directories){const auto& have=nt->OptionalHeader.DataDirectory[d.index];if(have.VirtualAddress!=d.address||have.Size!=d.size)return false;}
 const IMAGE_SECTION_HEADER* sections=IMAGE_FIRST_SECTION(nt);const unsigned count=nt->FileHeader.NumberOfSections;if(count==0||count>96)return false;
 for(const auto& want:build.sections){unsigned found=0;
  for(unsigned i=0;i<count;++i)if(!memcmp(sections[i].Name,want.name,8)){++found;if(sections[i].VirtualAddress!=want.address||sections[i].Misc.VirtualSize!=want.size)return false;}
  if(found!=1)return false;}   // missing or duplicated
 return true;}

// SHA-256 of size bytes at data, through Windows CNG (BCryptHash).
inline bool Sha256(const void* data,uint32_t size,uint8_t out[32]){
 return BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,(PUCHAR)data,size,out,32));}

// True when the loaded code of the image (its .text, at the profile's address and size) hashes to the profile's code.
inline bool CodeMatches(const uint8_t* image,const GameBuildProfile& build){
 uint8_t digest[32]{};return Sha256(image+build.sections[0].address,build.sections[0].size,digest)&&!memcmp(digest,build.textSha256,32);}

// Index of the validated build the image holds (version, layout and, when checkCode, loaded code), or -1.
inline int MatchBuild(const uint8_t* image,const uint16_t version[4],bool checkCode=true){
 for(int i=0;i<(int)(sizeof(kGameBuilds)/sizeof(kGameBuilds[0]));++i){const auto& b=kGameBuilds[i];
  if(version[0]==b.version[0]&&version[1]==b.version[1]&&version[2]==b.version[2]&&LayoutMatches(image,b)&&(!checkCode||CodeMatches(image,b)))return i;}
 return -1;}
}
