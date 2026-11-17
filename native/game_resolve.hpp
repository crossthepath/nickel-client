// Finds Nickel's game addresses in a Minecraft build that has no profile in game_build.hpp: a copy whose bytes a
// launcher changed somewhere Nickel does not use, code patched at start-up elsewhere, or a relinked hotfix of the same
// game version. The reference build's functions that hold every game address Nickel uses (generated_signatures.hpp,
// tools/gen_signatures.py) are looked up among the running game's functions of the same size (its exception table,
// .pdata), compared byte for byte except their relative fields (rel32 branch targets and RIP-relative displacements,
// the only bytes a relinked build changes in an identical function), as tools/port_build.py does offline. Functions
// with several identical copies take the position of their nearest unique neighbour; leaf functions (no .pdata entry)
// the position of the functions around them, then are compared up to their first ret. Data addresses are read from
// the RIP-relative instructions of found functions. The result is a translation profile like the generated ones;
// any function not found, or found twice with no neighbour to decide, refuses the build (game features stay off).
#pragma once
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "game_build.hpp"
#include "generated_signatures.hpp"
extern "C" {
#include "../vendor/minhook/src/hde/hde64.h"
}

namespace game {
// Versions the overlay tries to adapt to when no profile matches: the 1.26.60 line Nickel's profiles come from.
inline bool AdaptableVersion(const uint16_t version[4]){return version[0]==1&&version[1]==26&&version[2]>=60&&version[2]<=69;}

struct AdaptedBuild{std::vector<RvaRange> ranges;std::vector<RvaPair> data;std::string error;unsigned functions=0,dataFound=0;};

// Bytes compared in a reference function: all except the rel32 branch targets and RIP-relative displacements, decoded
// with MinHook's hde64 (as CodeIs). A stretch hde64 cannot decode is compared whole.
inline std::vector<uint8_t> SignatureMask(const uint8_t* code,uint32_t size){
 std::vector<uint8_t> mask(size,1);
 for(uint32_t at=0;at<size;){uint8_t window[32]{};memcpy(window,code+at,size-at<16?size-at:16);hde64s hs{};const unsigned len=hde64_disasm(window,&hs);
  if(!len||(hs.flags&F_ERROR)||at+len>size)break;
  const unsigned imm=(hs.flags&F_IMM64)?8:(hs.flags&F_IMM32)?4:(hs.flags&F_IMM16)?2:(hs.flags&F_IMM8)?1:0;
  if((hs.flags&F_RELATIVE)&&imm>=4)for(unsigned k=len-imm;k<len;++k)mask[at+k]=0;                                  // call / jmp / jcc rel32
  else if((hs.flags&F_MODRM)&&hs.modrm_mod==0&&hs.modrm_rm==5)for(unsigned k=len-imm-4;k<len-imm;++k)mask[at+k]=0;   // [rip+disp32]
  at+=len;}
 return mask;}

inline bool SignatureMatches(const uint8_t* code,const uint8_t* ref,const std::vector<uint8_t>& mask){
 const size_t n=mask.size();for(size_t i=0;i<n;++i)if(mask[i]&&code[i]!=ref[i])return false;return true;}

inline std::string HexRva(uint32_t v){char text[16];snprintf(text,sizeof text,"0x%x",v);return text;}

// image: the loaded game (module base), headers and sections at their RVAs. Fills ranges and data, or error.
inline AdaptedBuild AdaptBuild(const uint8_t* image){
 AdaptedBuild out;
 auto fail=[&](std::string why){out.error=std::move(why);out.ranges.clear();out.data.clear();return out;};
 if(!image)return fail("no game image");
 auto dos=(const IMAGE_DOS_HEADER*)image;if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<=0||dos->e_lfanew>0x1000)return fail("not a program");
 auto nt=(const IMAGE_NT_HEADERS64*)(image+dos->e_lfanew);
 if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC||nt->OptionalHeader.NumberOfRvaAndSizes<4)return fail("not a 64-bit program");
 const uint32_t imageSize=nt->OptionalHeader.SizeOfImage;
 uint32_t textStart=0,textEnd=0;const IMAGE_SECTION_HEADER* sections=IMAGE_FIRST_SECTION(nt);
 for(unsigned i=0;i<nt->FileHeader.NumberOfSections&&i<96;++i)if(!memcmp(sections[i].Name,".text",6)){textStart=sections[i].VirtualAddress;textEnd=textStart+sections[i].Misc.VirtualSize;break;}
 if(!textStart||textEnd>imageSize)return fail("no code section");
 const auto& exceptions=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
 if(!exceptions.VirtualAddress||exceptions.Size<12||(uint64_t)exceptions.VirtualAddress+exceptions.Size>imageSize)return fail("no function table");
 const auto* table=(const RUNTIME_FUNCTION*)(image+exceptions.VirtualAddress);const size_t entries=exceptions.Size/sizeof(RUNTIME_FUNCTION);

 constexpr unsigned count=(unsigned)(sizeof(kSignatureFunctions)/sizeof(kSignatureFunctions[0]));
 std::vector<std::vector<uint8_t>> masks(count);std::unordered_map<uint32_t,std::vector<unsigned>> bySize;
 for(unsigned k=0;k<count;++k){const auto& f=kSignatureFunctions[k];masks[k]=SignatureMask(kSignatureBytes+f.bytes,f.size);if(!f.leaf)bySize[f.size].push_back(k);}
 // Candidates: the game's functions of the same size whose compared bytes are the reference's (the first two kept,
 // with the count).
 std::vector<uint32_t> first(count,0),matches(count,0);
 for(size_t i=0;i<entries;++i){const uint32_t begin=table[i].BeginAddress,end=table[i].EndAddress;
  if(end<=begin||begin<textStart||end>textEnd)continue;
  const auto hit=bySize.find(end-begin);if(hit==bySize.end())continue;
  for(const unsigned k:hit->second)if(SignatureMatches(image+begin,kSignatureBytes+kSignatureFunctions[k].bytes,masks[k])){if(!matches[k]++)first[k]=begin;}}
 // True when the game's function table has a function of size bytes at rva holding reference function k.
 auto functionAt=[&](unsigned k,int64_t rva){
  if(rva<textStart||rva>=textEnd)return false;size_t lo=0,hi=entries;
  while(lo<hi){const size_t mid=(lo+hi)/2;if(table[mid].BeginAddress<rva)lo=mid+1;else hi=mid;}
  return lo<entries&&table[lo].BeginAddress==rva&&table[lo].EndAddress-table[lo].BeginAddress==kSignatureFunctions[k].size&&
   SignatureMatches(image+rva,kSignatureBytes+kSignatureFunctions[k].bytes,masks[k]);};
 // Position of each function (delta from the reference); one with several identical copies takes the delta of its
 // nearest unique neighbour, before or after it, when that places one of its copies.
 std::vector<int64_t> delta(count,0);std::vector<char> placed(count,0);
 for(unsigned k=0;k<count;++k)if(!kSignatureFunctions[k].leaf&&matches[k]==1){delta[k]=(int64_t)first[k]-kSignatureFunctions[k].ref;placed[k]=1;}
 // A function with several identical copies: the copy whose .pdata neighbours (8 before, 8 after) have the reference's
 // sizes, when exactly one has them.
 {std::unordered_map<uint32_t,std::vector<unsigned>> common;
  for(unsigned k=0;k<count;++k)if(!kSignatureFunctions[k].leaf&&matches[k]>1)common[kSignatureFunctions[k].size].push_back(k);
  std::vector<uint32_t> hits(count,0),at(count,0);const size_t side=kSignatureContext;
  if(!common.empty())for(size_t i=side;i+side<entries;++i){const uint32_t begin=table[i].BeginAddress,end=table[i].EndAddress;
   if(end<=begin||begin<textStart||end>textEnd)continue;const auto hit=common.find(end-begin);if(hit==common.end())continue;
   for(const unsigned k:hit->second){const uint32_t* sizes=kSignatureNeighbours[k];bool same=true;
    for(size_t j=0;j<side&&same;++j)same=table[i-side+j].EndAddress-table[i-side+j].BeginAddress==sizes[j]&&table[i+1+j].EndAddress-table[i+1+j].BeginAddress==sizes[side+j];
    if(same&&SignatureMatches(image+begin,kSignatureBytes+kSignatureFunctions[k].bytes,masks[k])&&!hits[k]++)at[k]=begin;}}
  for(unsigned k=0;k<count;++k)if(hits[k]==1){delta[k]=(int64_t)at[k]-kSignatureFunctions[k].ref;placed[k]=1;}}
 std::vector<char> unique(placed);
 auto neighbour=[&](unsigned k,int step)->const int64_t*{for(int j=(int)k+step;j>=0&&j<(int)count;j+=step)if(unique[j])return &delta[j];return nullptr;};
 for(unsigned k=0;k<count;++k){const auto& f=kSignatureFunctions[k];if(f.leaf||placed[k])continue;
  if(!matches[k])return fail("game function "+HexRva(f.ref)+" not found (changed or patched)");
  for(const int step:{-1,1}){const int64_t* d=neighbour(k,step);if(d&&functionAt(k,(int64_t)f.ref+*d)){delta[k]=*d;placed[k]=1;break;}}
  if(!placed[k])return fail("game function "+HexRva(f.ref)+" found "+std::to_string(matches[k])+" times, none where its neighbours are");}
 for(unsigned k=0;k<count;++k){const auto& f=kSignatureFunctions[k];if(!f.leaf)continue;
  for(const int step:{-1,1}){const int64_t* d=neighbour(k,step);if(!d)continue;const int64_t at=(int64_t)f.ref+*d;
   if(at>=textStart&&at+f.size<=textEnd&&SignatureMatches(image+at,kSignatureBytes+f.bytes,masks[k])){delta[k]=*d;placed[k]=1;break;}}
  if(!placed[k])return fail("game code at "+HexRva(f.ref)+" not found (changed or patched)");}
 out.functions=count;
 // Data: the target of each listed RIP-relative instruction, the same for all of them.
 constexpr unsigned dataCount=(unsigned)(sizeof(kSignatureData)/sizeof(kSignatureData[0]));
 for(unsigned i=0;i<dataCount;){const uint32_t ref=kSignatureData[i].ref;int64_t target=-1;bool clash=false;
  for(;i<dataCount&&kSignatureData[i].ref==ref;++i){const auto& r=kSignatureData[i];const auto& f=kSignatureFunctions[r.function];
   const int64_t at=(int64_t)f.ref+delta[r.function]+r.offset;if(at<textStart||at+16>textEnd)continue;
   uint8_t window[32]{};memcpy(window,image+at,16);hde64s hs{};const unsigned len=hde64_disasm(window,&hs);
   if(!len||(hs.flags&F_ERROR)||!(hs.flags&F_MODRM)||hs.modrm_mod!=0||hs.modrm_rm!=5)continue;
   const unsigned imm=(hs.flags&F_IMM64)?8:(hs.flags&F_IMM32)?4:(hs.flags&F_IMM16)?2:(hs.flags&F_IMM8)?1:0;
   int32_t disp;memcpy(&disp,window+len-imm-4,4);const int64_t t=at+len+disp;
   if(target<0)target=t;else if(target!=t)clash=true;}
  if(clash)return fail("game data "+HexRva(ref)+" is ambiguous");
  if(target<=0||target>=imageSize)return fail("game data "+HexRva(ref)+" not found");
  out.data.push_back({ref,(uint32_t)target});++out.dataFound;}
 // Code ranges: consecutive functions with one delta, each range extended to the next one (as tools/port_build.py).
 std::vector<std::pair<uint32_t,int64_t>> order;for(unsigned k=0;k<count;++k)order.push_back({kSignatureFunctions[k].ref,delta[k]});
 std::sort(order.begin(),order.end());
 std::vector<std::pair<uint32_t,int64_t>> runs;for(const auto& o:order)if(runs.empty()||runs.back().second!=o.second)runs.push_back(o);
 const uint32_t refStart=kSignatureText[0],refEnd=kSignatureText[0]+kSignatureText[1];
 for(size_t r=0;r<runs.size();++r)out.ranges.push_back({r?runs[r].first:refStart,r+1<runs.size()?runs[r+1].first:refEnd,(int32_t)runs[r].second});
 return out;}

// Makes the adapted build the current one: adaptedProfile gets its name, code section and the tables of adapted (kept
// for the life of the process). False, with nothing changed, when AdaptBuild failed.
inline bool UseAdaptedBuild(const uint8_t* image,const uint16_t version[4],AdaptedBuild&& adapted){
 static AdaptedBuild kept;static char name[64];
 if(!adapted.error.empty()||!image)return false;
 kept=std::move(adapted);
 snprintf(name,sizeof name,"Minecraft %u.%u.%u.%u (adapted)",version[0],version[1],version[2],version[3]);
 GameBuildProfile p{};p.name=name;p.version[0]=version[0];p.version[1]=version[1];p.version[2]=version[2];
 auto dos=(const IMAGE_DOS_HEADER*)image;auto nt=(const IMAGE_NT_HEADERS64*)(image+dos->e_lfanew);p.entryPoint=nt->OptionalHeader.AddressOfEntryPoint;
 const IMAGE_SECTION_HEADER* sections=IMAGE_FIRST_SECTION(nt);
 for(unsigned i=0;i<nt->FileHeader.NumberOfSections&&i<96;++i)if(!memcmp(sections[i].Name,".text",6)){memcpy(p.sections[0].name,".text",6);p.sections[0].address=sections[i].VirtualAddress;p.sections[0].size=sections[i].Misc.VirtualSize;}
 p.ranges=kept.ranges.data();p.rangeCount=(unsigned)kept.ranges.size();p.data=kept.data.data();p.dataCount=(unsigned)kept.data.size();
 adaptedProfile=p;currentBuild=kAdaptedBuild;return true;}
}
