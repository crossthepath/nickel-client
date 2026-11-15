// Game build gate (native/game_build.hpp): the overlay activates only in a validated build, recognised by its version
// resource and by the address and size of its code, constant and data sections. Checks run on synthetic PE headers
// (no game file needed) plus the version resource reader on kernel32.dll and on this test executable.
#include "game_build.hpp"
#include <cstdio>
#include <vector>

namespace {
using namespace game;
// Synthetic x64 image headers holding the sections of build 0, optionally edited.
std::vector<uint8_t> Headers(int extra=0,uint32_t textSize=kGameBuilds[0].sections[0].size,const char* rdataName=".rdata",WORD machine=IMAGE_FILE_MACHINE_AMD64){
 std::vector<uint8_t> image(0x1000);auto dos=(IMAGE_DOS_HEADER*)image.data();dos->e_magic=IMAGE_DOS_SIGNATURE;dos->e_lfanew=0x80;
 auto nt=(IMAGE_NT_HEADERS64*)(image.data()+0x80);nt->Signature=IMAGE_NT_SIGNATURE;nt->FileHeader.Machine=machine;nt->FileHeader.NumberOfSections=(WORD)(3+extra);
 nt->FileHeader.SizeOfOptionalHeader=sizeof(IMAGE_OPTIONAL_HEADER64);nt->OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;
 auto s=IMAGE_FIRST_SECTION(nt);const auto& b=kGameBuilds[0];
 for(int i=0;i<3;++i){memcpy(s[i].Name,b.sections[i].name,8);s[i].VirtualAddress=b.sections[i].address;s[i].Misc.VirtualSize=b.sections[i].size;}
 s[0].Misc.VirtualSize=textSize;memset(s[1].Name,0,8);memcpy(s[1].Name,rdataName,strlen(rdataName));
 for(int i=0;i<extra;++i){memcpy(s[3+i].Name,".launch",7);s[3+i].VirtualAddress=0x13800000u+0x1000u*i;s[3+i].Misc.VirtualSize=0x200;}
 return image;}
}

int main(){
 int failed=0;auto check=[&](bool ok,const char* name){std::printf("%s %s\n",ok?"ok  ":"FAIL",name);if(!ok)++failed;};
 const uint16_t v62[4]={1,26,62,1},v62b[4]={1,26,62,9},v63[4]={1,26,63,1},v25[4]={1,25,62,1};
 check(MatchBuild(Headers().data(),v62)==0,"the validated build is recognised");
 check(MatchBuild(Headers().data(),v62b)==0,"another revision number of the same build is recognised");
 check(MatchBuild(Headers(2).data(),v62)==0,"the build with sections added by a launcher is recognised");
 check(MatchBuild(Headers().data(),v63)<0&&MatchBuild(Headers().data(),v25)<0,"the same layout with another version is refused");
 check(MatchBuild(Headers(0,kGameBuilds[0].sections[0].size+0x4b0).data(),v62)<0,"a code section of another size is refused (1.26.63.1 has .text +0x4b0)");
 check(MatchBuild(Headers(0,kGameBuilds[0].sections[0].size,".rdat2").data(),v62)<0,"a build without its constant section is refused");
 check(MatchBuild(Headers(0,kGameBuilds[0].sections[0].size,".rdata",IMAGE_FILE_MACHINE_I386).data(),v62)<0,"a 32-bit image is refused");
 {auto bad=Headers();bad[0]='Z';check(MatchBuild(bad.data(),v62)<0,"a buffer that is not a program is refused");}
 check(MatchBuild(nullptr,v62)<0,"no image is refused");
 uint16_t v[4]{};check(GameVersion(GetModuleHandleW(L"kernel32.dll"),v)&&v[0]>=6,"the version of a versioned module is read");
 check(MatchBuild((const uint8_t*)GetModuleHandleW(L"kernel32.dll"),v)<0,"a Windows module is not a game build");
 uint16_t none[4]{};check(!GameVersion(GetModuleHandleW(nullptr),none),"a module without a version resource has no version");
 std::printf("%s\n",failed?"game build checks FAILED":"game build checks passed");return failed?1:0;}
