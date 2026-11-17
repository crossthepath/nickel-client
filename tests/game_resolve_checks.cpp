// Adapted builds (native/game_resolve.hpp) on real game files: each copy under build/ingame/mc-<version>/ is mapped as
// an image (never run). The addresses AdaptBuild finds must be the ones tools/port_build.py verified offline for
// 1.26.63.1 and 1.26.65.1, and the reference build must map to itself. A byte changed in a function Nickel does not use
// is accepted; a byte changed in one it uses, or a hook jump written over it, refuses the build. The checks that need
// a game copy are skipped when it is absent (continuous integration has none).
#include "game_read.hpp"
#include <chrono>
#include <cstdio>
#include <string>

namespace {
using namespace game;
int failed=0;
void Check(bool ok,const std::string& name){std::printf("%s %s\n",ok?"ok  ":"FAIL",name.c_str());if(!ok)++failed;}

uint8_t* MapImage(const std::wstring& path){
 const HMODULE h=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);
 return h?(uint8_t*)((uintptr_t)h&~(uintptr_t)3):nullptr;}

GameBuildProfile ProfileOf(const AdaptedBuild& a){
 GameBuildProfile p{};p.ranges=a.ranges.data();p.rangeCount=(unsigned)a.ranges.size();p.data=a.data.data();p.dataCount=(unsigned)a.data.size();return p;}

AdaptedBuild Timed(const uint8_t* image,const char* what){
 const auto start=std::chrono::steady_clock::now();AdaptedBuild a=AdaptBuild(image);
 std::printf("     %s: %lld ms, %u functions, %u data addresses%s%s\n",what,(long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count(),
  a.functions,a.dataFound,a.error.empty()?"":", refused: ",a.error.c_str());
 return a;}

// Every address the profile of build index has for a signature function, leaf or data address is the adapted one.
template<unsigned N,unsigned M> void SameAsProfile(const AdaptedBuild& a,int index,const RvaPair(&points)[N],const RvaPair(&data)[M],const char* name){
 const GameBuildProfile adapted=ProfileOf(a);bool all=true;unsigned compared=0;
 for(const auto& f:kSignatureFunctions){const uint32_t want=TranslateRva(kGameBuilds[index],f.ref);if(TranslateRva(adapted,f.ref)!=want){all=false;std::printf("     function 0x%x: 0x%x, profile 0x%x\n",f.ref,TranslateRva(adapted,f.ref),want);}++compared;}
 for(const auto& p:points)for(const auto& f:kSignatureFunctions)if(p.from>=f.ref&&p.from<f.ref+f.size){if(TranslateRva(adapted,p.from)!=p.to){all=false;std::printf("     point 0x%x: 0x%x, profile 0x%x\n",p.from,TranslateRva(adapted,p.from),p.to);}++compared;break;}
 for(const auto& d:data)for(const auto& s:kSignatureData)if(s.ref==d.from){if(TranslateRva(adapted,d.from)!=d.to){all=false;std::printf("     data 0x%x: 0x%x, profile 0x%x\n",d.from,TranslateRva(adapted,d.from),d.to);}++compared;break;}
 Check(a.error.empty()&&all&&compared>300,std::string(name)+": "+std::to_string(compared)+" adapted addresses equal the verified profile");}

bool Writable(uint8_t* at,size_t n){DWORD old;return VirtualProtect(at,n,PAGE_WRITECOPY,&old)||VirtualProtect(at,n,PAGE_READWRITE,&old);}
}

int main(){
 const uint16_t v62[4]={1,26,62,1},v65[4]={1,26,65,3},v70[4]={1,26,70,1},v127[4]={1,27,0,1},v52[4]={1,26,52,3},v21[4]={1,21,62,1};
 Check(AdaptableVersion(v62)&&AdaptableVersion(v65)&&AdaptableVersion(v70)&&AdaptableVersion(v127)&&!AdaptableVersion(v52)&&!AdaptableVersion(v21),"adaptable versions are 1.26.60 and later");
 Check(sizeof(kSignatureFunctions)/sizeof(kSignatureFunctions[0])>300&&sizeof(kSignatureData)/sizeof(kSignatureData[0])>=13,"the generated signatures cover Nickel's functions and data");
 {const uint8_t code[]={0xe8,1,2,3,4,0x48,0x8b,0x05,5,6,7,8,0xc3};const auto m=SignatureMask(code,sizeof code);
  Check(m.size()==13&&!m[1]&&!m[4]&&m[0]&&m[5]&&m[7]&&!m[8]&&!m[11]&&m[12],"the mask hides rel32 call targets and RIP-relative displacements only");}
 Check(!AdaptBuild(nullptr).error.empty(),"no image is refused");
 {const uint8_t* self=(const uint8_t*)GetModuleHandleW(nullptr);Check(!AdaptBuild(self).error.empty(),"a program that is not the game is refused");}

 const std::wstring root=NICKEL_SOURCE_DIR L"/build/ingame/";
 uint8_t* ref=MapImage(root+L"mc-1.26.62/Minecraft.Windows.exe");
 if(!ref){std::printf("skip game copies: build/ingame/mc-1.26.62 is absent\n");}
 else{
  const AdaptedBuild same=Timed(ref,"1.26.62.1");bool identity=same.error.empty();
  for(const auto& r:same.ranges)identity=identity&&r.delta==0;for(const auto& d:same.data)identity=identity&&d.from==d.to;
  Check(identity,"the reference build adapts to itself (no address moves)");
  // A byte changed in a function Nickel does not use: from the 1000th entry of the table, the first one no signature overlaps.
  auto nt=(const IMAGE_NT_HEADERS64*)(ref+((const IMAGE_DOS_HEADER*)ref)->e_lfanew);
  const auto* table=(const RUNTIME_FUNCTION*)(ref+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress);
  uint32_t other=0;for(int i=1000;!other;++i){bool used=false;for(const auto& f:kSignatureFunctions)used=used||(table[i].BeginAddress<f.ref+f.size&&f.ref<table[i].EndAddress);if(!used)other=table[i].BeginAddress+1;}
  if(Writable(ref+other,1)){const uint8_t keep=ref[other];ref[other]^=0x5a;
   Check(Timed(ref,"1.26.62.1, another function changed").error.empty(),"a byte changed in a function Nickel does not use is accepted");ref[other]=keep;}
  else Check(false,"the mapped game copy could not be made writable");
  // A function Nickel uses (one with a .pdata entry): its first byte changed, then a hook jump written over its start.
  const SignatureFunction* used=nullptr;for(const auto& f:kSignatureFunctions)if(!f.leaf&&f.size>=32){used=&f;if(f.ref>0x2000000)break;}
  if(used&&Writable(ref+used->ref,5)){uint8_t keep[5];memcpy(keep,ref+used->ref,5);
   ref[used->ref]^=0x01;const AdaptedBuild one=Timed(ref,"1.26.62.1, a used function changed");
   Check(!one.error.empty()&&one.error.find(HexRva(used->ref))!=std::string::npos,"a byte changed in a function Nickel uses refuses the build and names it ("+one.error+")");
   const uint8_t jump[5]={0xe9,0x10,0x20,0x30,0x40};memcpy(ref+used->ref,jump,5);
   Check(!Timed(ref,"1.26.62.1, a used function hooked").error.empty(),"a hook jump written over a function Nickel uses refuses the build");
   memcpy(ref+used->ref,keep,5);}
  Check(Timed(ref,"1.26.62.1, restored").error.empty(),"the restored copy adapts again");
  FreeLibrary((HMODULE)((uintptr_t)ref|2));
 }
 if(uint8_t* image=MapImage(root+L"mc-1.26.63/Minecraft.Windows.exe")){SameAsProfile(Timed(image,"1.26.63.1"),1,kPoints_1_26_63_1,kData_1_26_63_1,"1.26.63.1");FreeLibrary((HMODULE)((uintptr_t)image|2));}
 else std::printf("skip 1.26.63.1: build/ingame/mc-1.26.63 is absent\n");
 if(uint8_t* image=MapImage(root+L"mc-1.26.65/Minecraft.Windows.exe")){SameAsProfile(Timed(image,"1.26.65.1"),2,kPoints_1_26_65_1,kData_1_26_65_1,"1.26.65.1");FreeLibrary((HMODULE)((uintptr_t)image|2));}
 else std::printf("skip 1.26.65.1: build/ingame/mc-1.26.65 is absent\n");
 // 1.26.60.5: the function that game_world / game_reach / game_hitbox use changed, so the build is refused.
 if(uint8_t* image=MapImage(root+L"mc-1.26.60/Minecraft.Windows.exe")){const AdaptedBuild a=Timed(image,"1.26.60.5");
  Check(!a.error.empty()&&a.error.find("0x4e09290")!=std::string::npos,"1.26.60.5, whose world function changed, is refused and the function named");FreeLibrary((HMODULE)((uintptr_t)image|2));}
 else std::printf("skip 1.26.60.5: build/ingame/mc-1.26.60 is absent\n");
 std::printf("%s\n",failed?"game resolve checks FAILED":"game resolve checks passed");return failed?1:0;}
