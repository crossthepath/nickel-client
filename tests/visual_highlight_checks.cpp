// Reference vectors for the highlight style -> shader element conversion.
#include "nickel/visual_highlight.hpp"
#include <cstdio>
#include <string>
using namespace nickel::visual;
static std::string hexs(const uint8_t* p,size_t n){static const char* d="0123456789abcdef";std::string s;for(size_t i=0;i<n;++i){s+=d[p[i]>>4];s+=d[p[i]&15];}return s;}
static void unhex(const char* h,uint8_t* out,size_t n){for(size_t i=0;i<n;++i){auto v=[](char c){return c<='9'?c-'0':c-'a'+10;};out[i]=(uint8_t)(v(h[2*i])<<4|v(h[2*i+1]));}}
int main(){int failed=0;
 struct Case{const char* style;const char* out;};
 const Case cases[]={
  {"000000000000803f9a99193e6aa61a3e9a99993e0200c03fcdcccc3d0000a040000000000155d61f0000000000000000feffffffffffffff00a86f5c3c000000149f6f5c3c00000014d0b60f6801000013b5cf3da2f6b93f00b93a7cc4050000",
   "00000000000000000000000000000000000000000000803f9a99193e6aa61a3e9a99993e00000000000000000000000000000000eb6849440000000000000000000000000000000000000000000000000080c044eb684944"},
  {"010000000000803f9a99193e6aa61a3e9a99993e0200c03fcdcccc3d0000a040000000000155d61f0000000000000000feffffffffffffff00a86f5c3c000000149f6f5c3c00000014d0b60f6801000013b5cf3da2f6b93f00b93a7cc4050000",
   "00000000000000000000000000000000000000000000803f9a99193e6aa61a3e9a99993e00000000000000000000000000000000eb68494400000000000000000000000000000000000000000000000000a0c044eb684944"},
  {"000000000000803f9a99193e6aa61a3e9a99993e0200c03fcdcccc3d0000a040000000000055d61f0000803f0000803f0000803f0000803f00000040000000000000c03f000000000000000000000000000000000000000001ffffffc4050000",
   "0000000000000000000000000000000000000000000000000000000000000000000000000000803f0000803f0000803f0000803feb6849440000000000000000000000000000c03f000000400000000000000000eb684944"},
  {"010000000000803f9a99193e6aa61a3e9a99993e0200c03fcdcccc3d0000a040000000000155d61f0000803f0000803f0000803f0000803f00000040000000000000c03f000000000000000000000000000000000000000001ffffffc4050000",
   "00000000000000000000000000000000000000000000803f9a99193e6aa61a3e9a99993e0000803f0000803f0000803f0000803feb6849440000000000000000000000000000c03f000000400000000000a0c044eb684944"}};
 const uint8_t origin[12]={0x30,0x2d,0xe8,0x43,0x8a,0x3d,0x9b,0x42,0x46,0x96,0x1a,0x44};
 for(const auto& c:cases){HighlightStyle st{};unhex(c.style,reinterpret_cast<uint8_t*>(&st),0x60);uint8_t want[0x58];unhex(c.out,want,0x58);float time;std::memcpy(&time,want+0x34,4);
  uint8_t got[0x58];build_style_item(got,st,time,origin,false,false);
  if(std::memcmp(got,want,0x58)){std::fprintf(stderr,"style item mismatch\n want %s\n got  %s\n",hexs(want,0x58).c_str(),hexs(got,0x58).c_str());++failed;}}
 // Rainbow: phase 0 -> pure red x0.85+0.15; dt capped at 0.1 s.
 HighlightStyle st{};st.fillEnabled=1;st.mode=2;st.speed=4;RainbowSlot slot{};slot.last=0;
 rainbow_step(slot,st,1000);if(std::fabs(slot.phase-0.1)>1e-9){std::fprintf(stderr,"rainbow dt cap failed %f\n",slot.phase);++failed;}
 HighlightStyle off{};off.fillEnabled=1;off.mode=0;RainbowSlot s2{};if(rainbow_step(s2,off,10)){std::fprintf(stderr,"rainbow only in mode 2\n");++failed;}
 for(uint64_t flags: {0ull,2ull,0x100ull,0x100000000ull})if(exclude_from_highlight_pass(flags)){std::fprintf(stderr,"ordinary pass excluded\n");++failed;}
 for(uint64_t flags: {1ull,3ull,0x101ull,0x100000001ull})if(!exclude_from_highlight_pass(flags)){std::fprintf(stderr,"highlight pass not excluded\n");++failed;}
 return failed?1:0;}
