// Replaces RenderDragon material shaders as the game reads them.
// - Hook on MC RVA 0x341070 (memory stream constructor (this, data, size, int) through which every ".material.bin"
// is read), installed at injection.
// - EditorHighlight.material.bin: each "FSH\x05" shader entry whose uniform list matches the expected one exactly
// (13 or 17 entries, name + type) receives the embedded bytecode for its platform (last "Direct3D_SM.." in the
// preceding 0x400 bytes). The result is computed once and reused.
// - Sky.material.bin: the file on disk is patched once (8 entries without uniforms followed by "worldPos"); the blob
// read by the game is replaced if it matches the file on disk (state 1) or if Override Pack Sky is on (state 2);
// otherwise state 3; incomplete patch: state 4.
#pragma once
#include "game_read.hpp"
#include <MinHook.h>
#include <mutex>
#include <string>
#include <vector>

namespace game {
struct MaterialState{std::mutex guard;std::vector<uint8_t> highlight;bool highlightBuilt=false;int highlightEntries=0;
 std::vector<uint8_t> skyDisk,skyPatched;bool skyBuilt=false;int skyEntries=0;
 volatile LONG highlightLoads=0,skyState=0,skyLoads=0,overridePack=0,fault=0;void(*log)(const char*)=nullptr;};
inline MaterialState& Materials(){static MaterialState* s=new MaterialState;return *s;}
using MaterialStreamFunction=void*(*)(void*,const uint8_t*,size_t,int);
inline MaterialStreamFunction materialStreamOriginal=nullptr;

inline bool MaterialNamed(const uint8_t* d,size_t n,const char* name){
 static const char kind[]="RenderDragon.CompiledMaterialDefinition";const size_t len=strlen(name);
 if(n<0x43||*(const uint64_t*)d!=0xa11da1a||*(const uint32_t*)(d+8)!=0x27||memcmp(d+0xc,kind,0x27)!=0||*(const uint32_t*)(d+0x3b)!=0x4e4f4e45)return false;
 const uint32_t got=*(const uint32_t*)(d+0x3f);return got<=n-0x43&&got==len&&memcmp(d+0x43,name,len)==0;}
inline bool MaterialResource(int id,const uint8_t** data,size_t* size){HMODULE self{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)&Materials,&self))return false;
 HRSRC r=FindResourceW(self,MAKEINTRESOURCEW(id),RT_RCDATA);if(!r)return false;*data=(const uint8_t*)LockResource(LoadResource(self,r));*size=SizeofResource(self,r);return *data&&*size;}
inline size_t MaterialFind(const uint8_t* d,size_t from,size_t to,const char* needle,size_t len){if(to<from+len)return SIZE_MAX;for(size_t i=from;i+len<=to;++i)if(memcmp(d+i,needle,len)==0)return i;return SIZE_MAX;}
inline size_t MaterialFindLast(const uint8_t* d,size_t from,size_t to,const char* needle,size_t len){if(to<from+len)return SIZE_MAX;for(size_t i=to-len+1;i-->from;)if(memcmp(d+i,needle,len)==0)return i;return SIZE_MAX;}
inline int MaterialPlatform(const uint8_t* d,size_t at){static const char* names[4]={"Direct3D_SM40","Direct3D_SM50","Direct3D_SM60","Direct3D_SM65"};for(int i=0;i<4;++i)if(memcmp(d+at,names[i],13)==0)return i;return -1;}
inline void Append(std::vector<uint8_t>& out,const void* p,size_t n){const uint8_t* b=(const uint8_t*)p;out.insert(out.end(),b,b+n);}

// Expected uniform lists (name, u16 type).
inline int PatchHighlight(const uint8_t* d,size_t n,std::vector<uint8_t>& out){
 struct U{const char* name;uint16_t type;};
 static const U v13[13]={{"HighlightColor",0},{"HighlightAlpha",0x10},{"HiddenBlocksAlpha",0x20},{"BlockEdgeColor",0x30},{"BlockEdgeThickness",0x40},{"OutlineColor",0x50},{"OutlineWidth",0x60},{"PreExposureEnabled",0x70},{"ScreenSize",0x80},{"s_HighlightTextureColor",0},{"s_HighlightTextureDepth",1},{"s_SceneTextureColor",3},{"s_SceneTextureDepth",4}};
 static const U v17[17]={{"HighlightColor",0},{"HighlightAlpha",0x10},{"HiddenBlocksAlpha",0x20},{"CameraPosition",0x30},{"BlockEdgeColor",0x40},{"BlockEdgeThickness",0x50},{"InverseProjView",0x60},{"OutlineColor",0xa0},{"OutlineWidth",0xb0},{"OverlayTextureStretch",0xc0},{"PreExposureEnabled",0xd0},{"ScreenSize",0xe0},{"s_HighlightTextureColor",0},{"s_HighlightTextureDepth",1},{"s_SceneTextureColor",3},{"s_SceneTextureDepth",4},{"s_SelectedBlocksOverlayTexture",5}};
 out.clear();out.reserve(n*5);size_t last=0;int count=0;
 for(size_t p=MaterialFind(d,0,n,"FSH\x05",4);p!=SIZE_MAX;p=MaterialFind(d,p+4,n,"FSH\x05",4)){
  if(p<4||last+4>p||p+10>n)continue;const uint32_t entry=*(const uint32_t*)(d+p-4);const uint16_t inputs=*(const uint16_t*)(d+p+8);if(entry>n-p||(inputs!=13&&inputs!=17))continue;
  const U* list=inputs==13?v13:v17;size_t q=p+10;bool good=true;
  for(int i=0;i<inputs&&good;++i){if(q>=n){good=false;break;}const size_t len=d[q];if(q+1+len+4>n){good=false;break;}if(len!=strlen(list[i].name)||memcmp(d+q+1,list[i].name,len)!=0){good=false;break;}q+=1+len;if(*(const uint16_t*)(d+q+2)!=list[i].type){good=false;break;}q+=6;}
  if(!good||q+4>n)continue;const size_t tail=q+4+*(const uint32_t*)(d+q),end=p+entry;if(tail>end)continue;
  const size_t from=p>0x400?p-0x400:0;const size_t at=MaterialFindLast(d,from,p,"Direct3D_SM",11);if(at==SIZE_MAX||at+13>p)continue;const int platform=MaterialPlatform(d,at);if(platform<0)continue;
  const uint8_t* shader{};size_t size{};if(!MaterialResource(110+platform*2+(inputs==13?0:1),&shader,&size))continue;
  Append(out,d+last,p-4-last);const uint32_t length=(uint32_t)((q-p)+(end-tail)+size+4);Append(out,&length,4);Append(out,d+p,q-p);const uint32_t s32=(uint32_t)size;Append(out,&s32,4);Append(out,shader,size);Append(out,d+tail,end-tail);last=end;++count;}
 Append(out,d+last,n-last);if(!count)out.clear();return count;}

// Entries without uniforms; eight replacements required.
inline int PatchSky(const uint8_t* d,size_t n,std::vector<uint8_t>& out){
 out.clear();out.reserve(n*36);size_t last=0;int count=0;
 for(size_t p=MaterialFind(d,0,n,"FSH\x05",4);p!=SIZE_MAX;p=MaterialFind(d,p+4,n,"FSH\x05",4)){
  if(p<4||last+4>p||p+14>n)continue;const uint32_t entry=*(const uint32_t*)(d+p-4);if(*(const uint16_t*)(d+p+8)!=0||entry>n-p)continue;
  const size_t tail=p+*(const uint32_t*)(d+p+10)+0xe,end=p+entry;if(tail>end)continue;
  const size_t from=p>0x400?p-0x400:0;const size_t at=MaterialFindLast(d,from,p,"Direct3D_SM",11);if(at==SIZE_MAX||at+13>p)continue;
  if(MaterialFind(d,at,p,"\x08\x00\x00\x00worldPos",12)==SIZE_MAX)continue;const int platform=MaterialPlatform(d,at);if(platform<0)continue;
  const uint8_t* shader{};size_t size{};if(!MaterialResource(120+platform,&shader,&size))continue;
  Append(out,d+last,p-4-last);const uint32_t length=(uint32_t)(10+(end-tail)+size+4);Append(out,&length,4);Append(out,d+p,10);const uint32_t s32=(uint32_t)size;Append(out,&s32,4);Append(out,shader,size);Append(out,d+tail,end-tail);last=end;++count;}
 Append(out,d+last,n-last);if(count!=8)out.clear();return count;}

inline bool MaterialWork(const uint8_t** data,size_t* size){
 const uint8_t* d=*data;const size_t n=*size;auto& s=Materials();
 if(MaterialNamed(d,n,"EditorHighlight")){std::lock_guard<std::mutex> lock(s.guard);InterlockedIncrement(&s.highlightLoads);
  if(!s.highlightBuilt){s.highlightBuilt=true;s.highlightEntries=PatchHighlight(d,n,s.highlight);if(s.log){char line[160];snprintf(line,sizeof(line),"Materials: EditorHighlight patched, %d shader entries replaced (%zu -> %zu bytes)",s.highlightEntries,n,s.highlight.size());s.log(line);}}
  if(s.highlight.empty())return false;*data=s.highlight.data();*size=s.highlight.size();return true;}
 if(MaterialNamed(d,n,"Sky")){std::lock_guard<std::mutex> lock(s.guard);InterlockedIncrement(&s.skyLoads);
  if(!s.skyBuilt){s.skyBuilt=true;wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);std::wstring path=exe;path=path.substr(0,path.find_last_of(L"\\/")+1)+L"data\\renderer\\materials\\Sky.material.bin";
   HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(f!=INVALID_HANDLE_VALUE){LARGE_INTEGER len{};if(GetFileSizeEx(f,&len)&&len.QuadPart>0&&len.QuadPart<(64ll<<20)){s.skyDisk.resize((size_t)len.QuadPart);DWORD got{};if(!ReadFile(f,s.skyDisk.data(),(DWORD)len.QuadPart,&got,nullptr)||got!=len.QuadPart)s.skyDisk.clear();}CloseHandle(f);}
   if(MaterialNamed(s.skyDisk.data(),s.skyDisk.size(),"Sky"))s.skyEntries=PatchSky(s.skyDisk.data(),s.skyDisk.size(),s.skyPatched);
   if(s.log){char line[160];snprintf(line,sizeof(line),"Materials: Sky patched from disk, %d shader entries replaced (%zu -> %zu bytes)",s.skyEntries,s.skyDisk.size(),s.skyPatched.size());s.log(line);}}
  if(s.skyPatched.empty()){InterlockedExchange(&s.skyState,4);return false;}
  const bool vanilla=n==s.skyDisk.size()&&memcmp(d,s.skyDisk.data(),n)==0;
  if(!vanilla&&!s.overridePack){InterlockedExchange(&s.skyState,3);return false;}
  InterlockedExchange(&s.skyState,vanilla?1:2);*data=s.skyPatched.data();*size=s.skyPatched.size();return true;}
 return false;}
inline bool MaterialGuarded(const uint8_t** data,size_t* size){__try{return MaterialWork(data,size);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Materials().fault,1);return false;}}
inline void* MaterialStreamDetour(void* self,const uint8_t* data,size_t size,int flag){
 if(data&&size>=0x43&&!Materials().fault){const uint8_t* d=data;size_t n=size;if(MaterialGuarded(&d,&n)){data=d;size=n;}}
 return materialStreamOriginal(self,data,size,flag);}
// Installed at injection, before EditorHighlight is first read (it loads on demand); prologue checked.
inline int InstallMaterialHook(void(*log)(const char*)){
 static int status=0;if(status)return status;status=-1;Materials().log=log;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size)){if(log)log("Materials: unsupported game image");return status;}
 static const uint8_t prologue[]={0x55,0x56,0x57,0x53,0x48,0x83,0xec,0x38,0x48,0x8d,0x6c,0x24,0x30,0x48,0xc7,0x45,0x00,0xfe,0xff,0xff,0xff,0x4c,0x89,0xc6};
 for(size_t i=0;i<sizeof(prologue);++i){uint8_t v{};if(!Read(base+Rva(0x341070)+i,&v)||v!=prologue[i]){if(log)log("Materials: unexpected game code at 0x341070, hook not installed");return status;}}
 void* target=(void*)(base+Rva(0x341070));if(MH_CreateHook(target,(void*)MaterialStreamDetour,(void**)&materialStreamOriginal)!=MH_OK){if(log)log("Materials: could not create hook");return status;}
 if(MH_EnableHook(target)!=MH_OK){MH_RemoveHook(target);if(log)log("Materials: could not enable hook");return status;}
 status=1;if(log)log("Materials: material stream hook 0x341070 installed (EditorHighlight, Sky)");return status;}
}
