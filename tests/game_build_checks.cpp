// Game build gate (native/game_build.hpp): the overlay activates only in a validated build, recognised by its version
// resource, its entry point and load-time directories, the address and size of its code, constant, data and relocation
// sections, and the SHA-256 of its loaded code. Checks run on synthetic PE headers and buffers (no game file needed),
// plus the version resource reader on kernel32.dll and on this test executable.
#include "game_build.hpp"
#include <cstdio>
#include <vector>

namespace {
using namespace game;
const GameBuildProfile& B=kGameBuilds[0];
// Synthetic x64 image headers holding build 0's header facts and sections, then edited.
template<class Edit> std::vector<uint8_t> Headers(Edit edit,int extra=0){
 std::vector<uint8_t> image(0x1000);auto dos=(IMAGE_DOS_HEADER*)image.data();dos->e_magic=IMAGE_DOS_SIGNATURE;dos->e_lfanew=0x80;
 auto nt=(IMAGE_NT_HEADERS64*)(image.data()+0x80);nt->Signature=IMAGE_NT_SIGNATURE;nt->FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64;nt->FileHeader.NumberOfSections=(WORD)(5+extra);
 nt->FileHeader.SizeOfOptionalHeader=sizeof(IMAGE_OPTIONAL_HEADER64);nt->OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;nt->OptionalHeader.NumberOfRvaAndSizes=16;
 nt->OptionalHeader.AddressOfEntryPoint=B.entryPoint;for(const auto& d:B.directories){nt->OptionalHeader.DataDirectory[d.index].VirtualAddress=d.address;nt->OptionalHeader.DataDirectory[d.index].Size=d.size;}
 nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY].VirtualAddress=0x134d0000;   // signature, free to differ
 auto s=IMAGE_FIRST_SECTION(nt);for(int i=0;i<5;++i){memcpy(s[i].Name,B.sections[i].name,8);s[i].VirtualAddress=B.sections[i].address;s[i].Misc.VirtualSize=B.sections[i].size;}
 for(int i=0;i<extra;++i){memcpy(s[5+i].Name,".launch",7);s[5+i].VirtualAddress=0x13900000u+0x1000u*i;s[5+i].Misc.VirtualSize=0x200;}
 edit(nt,s);return image;}
auto None=[](IMAGE_NT_HEADERS64*,IMAGE_SECTION_HEADER*){};
bool Layout(const std::vector<uint8_t>& h,const uint16_t v[4]){return MatchBuild(h.data(),v,false)==0;}
}

int main(){
 int failed=0;auto check=[&](bool ok,const char* name){std::printf("%s %s\n",ok?"ok  ":"FAIL",name);if(!ok)++failed;};
 const uint16_t v62[4]={1,26,62,1},v62b[4]={1,26,62,9},v63[4]={1,26,63,1},v25[4]={1,25,62,1};
 check(Layout(Headers(None),v62),"the validated build's headers are recognised");
 check(Layout(Headers(None),v62b),"another revision number of the same build is recognised");
 check(Layout(Headers(None,2),v62),"the build with sections added by a launcher is recognised");
 check(Layout(Headers([](auto nt,auto){nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY].Size=0x9999;}),v62),"another signature is accepted");
 check(!Layout(Headers(None),v63)&&!Layout(Headers(None),v25),"the same layout with another version is refused");
 check(!Layout(Headers([](auto,auto s){s[0].Misc.VirtualSize+=0x4b0;}),v62),"a code section of another size is refused (1.26.63.1 has .text +0x4b0)");
 check(!Layout(Headers([](auto,auto s){s[1].Name[5]='2';}),v62),"a build without its constant section is refused");
 check(!Layout(Headers([](auto,auto s){memcpy(s[5].Name,".text\0\0",8);},1),v62),"a build with two code sections is refused");
 check(!Layout(Headers([](auto nt,auto){nt->OptionalHeader.AddressOfEntryPoint+=0x10;}),v62),"another entry point is refused");
 check(!Layout(Headers([](auto nt,auto){nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress=0x13900000;},1),v62),"imports moved to an added section are refused");
 check(!Layout(Headers([](auto nt,auto){nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size+=8;}),v62),"changed TLS callbacks are refused");
 check(!Layout(Headers([](auto nt,auto){nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size+=8;}),v62),"changed relocations are refused");
 check(!Layout(Headers([](auto nt,auto){nt->FileHeader.Machine=IMAGE_FILE_MACHINE_I386;}),v62),"a 32-bit image is refused");
 {auto bad=Headers(None);bad[0]='Z';check(!Layout(bad,v62),"a buffer that is not a program is refused");}
 check(MatchBuild(nullptr,v62)<0,"no image is refused");
 // Loaded code: SHA-256 through CNG, and CodeMatches against a small profile made for the test.
 uint8_t digest[32]{};const uint8_t abc[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
 check(Sha256("abc",3,digest)&&!memcmp(digest,abc,32),"SHA-256 of \"abc\" is the standard value");
 std::vector<uint8_t> image(0x1100,0x5a);GameBuildProfile profile=B;profile.sections[0]={".text",0x1000u,0x100u};Sha256(image.data()+0x1000,0x100,profile.textSha256);
 check(CodeMatches(image.data(),profile),"loaded code equal to the build is accepted");
 image[0x1080]^=1;check(!CodeMatches(image.data(),profile),"loaded code that differs by one byte is refused");
 uint16_t v[4]{};check(GameVersion(GetModuleHandleW(L"kernel32.dll"),v)&&v[0]>=6,"the version of a versioned module is read");
 check(MatchBuild((const uint8_t*)GetModuleHandleW(L"kernel32.dll"),v)<0,"a Windows module is not a game build");
 uint16_t none[4]{};check(!GameVersion(GetModuleHandleW(nullptr),none),"a module without a version resource has no version");
 std::printf("%s\n",failed?"game build checks FAILED":"game build checks passed");return failed?1:0;}
