#pragma once
// Actor/block highlighting (Chams, Walls, Outline ESP, Hand Chams, Block/Chest ESP).
// - HighlightStyle: structure filled by the *HighlightStyleEvent listeners (0x60 bytes).
// - build_style_item: style item inserted into the game's frame builder (0x58 bytes).
// - rainbow_step: per-slot Rainbow colour (mode 2).
#include <cmath>
#include <cstdint>
#include <cstring>
namespace nickel::visual {
// Game function that emits the highlighted actor. The neighbouring dispatcher at 0x6646980 takes a descriptor as its
// third argument, NOT the actor context whose byte +0x7a enables the Highlighted variant.
inline constexpr uint32_t kHighlightActorEmitRva = 0x6698e30;
inline constexpr uint32_t kHighlightContextFlagOffset = 0x7a;
inline constexpr uint32_t kHighlightExcludePassRva[2] = {0xbc301c0,0xbc324a0};
// Passes excluded from highlighting: the game stubs test r8b,1 and return 0 when it is set.
inline constexpr bool exclude_from_highlight_pass(uint64_t flags){return (flags&1)!=0;}
#pragma pack(push,1)
struct HighlightStyle {           // 0x60-byte layout
 uint8_t flags;                   // +0x00 (slot bits supplied by the caller)
 uint8_t pad0[3];
 float fill[4];                   // +0x04 fill RGBA
 uint8_t mode;                    // +0x14 0 Glow, 1 Shiny, 2 Rainbow
 uint8_t pad1[3];
 float intensity;                 // +0x18 (x64, clamped 0..255 -> byte 1 of the flags word)
 float speed;                     // +0x1c (Rainbow speed)
 uint8_t shinyFlag;               // +0x20
 uint8_t pad2[3];
 uint8_t fillEnabled;             // +0x24
 uint8_t pad3[3];
 float outline[4];                // +0x28 outline RGBA
 float outlineB[2];               // +0x38, +0x3c
 float outlineC[2];               // +0x40, +0x44 (bytes +0x44/+0x45 read as flags)
 float outlineD[3];               // +0x48, +0x4c, +0x50
 uint8_t pad4[4];
 uint8_t outlineEnabled;          // +0x58
 uint8_t pad5[7];
};
#pragma pack(pop)
static_assert(sizeof(HighlightStyle)==0x60,"HighlightStyle layout");

// Builds the style item (param_4 = 12 bytes of camera position wrapped to 1024).
inline void build_style_item(uint8_t out[0x58],const HighlightStyle& styleIn,float time,const uint8_t origin[12],bool p5,bool p6){
 const uint8_t* s=reinterpret_cast<const uint8_t*>(&styleIn);std::memset(out,0,0x58);
 uint8_t zero[16]{};const uint8_t fillOn=s[0x24],outlineOn=s[0x58];
 std::memcpy(out+0x14,fillOn?s+4:zero,16);
 auto put32=[&](int at,const void* v){std::memcpy(out+at,v,4);};
 uint32_t last=0;
 if(!outlineOn){
  if(fillOn&&s[0x14]==1){std::memcpy(out+0x24,origin,12);}
  put32(0x34,&time);
 }else{
  std::memcpy(out+0x24,s+0x28,16);put32(0x34,&time);
  put32(0x38,s+0x48);put32(0x3c,s+0x4c);put32(0x40,s+0x50);put32(0x44,s+0x40);put32(0x48,s+0x38);std::memcpy(&last,s+0x3c,4);
 }
 std::memcpy(out+0x4c,&last,4);
 uint32_t bits=(p5?(uint32_t)fillOn<<5:0);bits=p6?bits+0x10:bits;
 uint32_t flags=(outlineOn?(uint32_t)s[0x44]*2:0)|s[0]|bits;
 uint8_t outlineAgain=outlineOn;
 if(fillOn){
  float f;std::memcpy(&f,s+0x18,4);f*=64.f;float c=f>255.f?255.f:(f<0.f?0.f:f);
  const uint32_t level=(uint32_t)(int)std::nearbyint(c);   // rounded integer conversion
  const uint32_t base=level<<8|flags;flags=base|4;
  if(s[0x24]==1){if(s[0x14]==1)flags=base|0xc;if(s[0x20]==1)flags|=(uint32_t)outlineAgain<<0x11;}
 }
 const uint32_t extra=outlineAgain?(uint32_t)s[0x45]<<6:0;const float packed=(float)(int)(extra|flags);
 put32(0x50,&packed);put32(0x54,&time);
}

// Rainbow (fillEnabled == 1 and mode == 2); phase and time are tracked per slot.
struct RainbowSlot{double phase=0,last=0;};
inline bool rainbow_step(RainbowSlot& slot,HighlightStyle& s,double nowMs){
 if(s.fillEnabled!=1||s.mode!=2)return false;
 double dt=(nowMs-slot.last)/1000.0;if(dt<0)dt=0;if(dt>0.1)dt=0.1;   // dt clamped to 0.1 s
 slot.phase=std::fmod((double)s.speed*dt*0.25+slot.phase,1.0);slot.last=nowMs;
 const float h=(float)slot.phase;auto ch=[&](float off){float x=(h+off)-std::floor(h+off);float v=std::fabs(x*6.f-3.f)-1.f;return v<0?0.f:(v>1?1.f:v);};
 const float r=ch(0.f),g=ch(.6666667f),b=ch(.33333334f);
 s.fill[0]=r*.85f+.15f;s.fill[1]=g*.85f+.15f;s.fill[2]=b*.85f+.15f;return true;
}
}
