// Connection transport for servers joined by address.
// MinecraftGame::joinMultiplayerWithAddress (game RVA 0x8b1a40) normally runs a transport prober for IPv4, IPv6 and
// unknown addresses: an HTTP request over TCP picks NetherNet when the server answers it, and RakNet is only used when
// a remote flag allows it (off by default), so a RakNet-only server cannot be joined. Force RakNet takes the game's own
// no-probe branch (the conditional jump at RVA 0x8b1a9c becomes an unconditional one), where a game table maps these
// address types to transport 0, RakNet. The patch is verified against the expected bytes before it is written and the
// original bytes are put back when the setting is turned off. It applies from the next join.
// RakNet fallback keeps the prober but, when it gets no NetherNet answer, uses RakNet instead of failing: the test of
// the remote flag (test al,al / je at RVA 0x8df8e6) loses its jump, so the RakNet branch is always taken there.
#pragma once
#include "game_read.hpp"
#include <windows.h>
#include <atomic>
#include <cstring>

namespace game {
inline constexpr uint32_t kTransportJump=0x8b1a9c;
inline const uint8_t kTransportProbe[6]={0x0f,0x85,0xfb,0x00,0x00,0x00};   // jne: run the prober
inline const uint8_t kTransportDirect[6]={0xe9,0xfc,0x00,0x00,0x00,0x90};  // jmp: direct branch (RakNet), then nop
// Bytes around the jump, checked once: test r14b,r14b / jne / mov rax,[rbp+0xcf8] / movzx eax,word [rax] / cmp eax,8.
inline const uint8_t kTransportContext[22]={0x45,0x84,0xf6,0x0f,0x85,0xfb,0x00,0x00,0x00,0x48,0x8b,0x85,0xf8,0x0c,0x00,0x00,0x0f,0xb7,0x00,0x83,0xf8,0x08};

inline bool TransportWrite(uintptr_t at,const uint8_t* bytes){DWORD old{};if(!VirtualProtect((void*)at,6,PAGE_EXECUTE_READWRITE,&old))return false;
 memcpy((void*)at,bytes,6);DWORD unused{};VirtualProtect((void*)at,6,old,&unused);FlushInstructionCache(GetCurrentProcess(),(void*)at,6);return true;}
// Applies Force RakNet (true) or the game's prober (false). Returns false when the game code is not the expected one.
inline bool ForceRakNet(bool on,void(*log)(const char*)){static int checked=0;static std::atomic<int> applied{-1};uintptr_t base{};uint32_t size{};
 if(!Supported(&base,&size))return false;const uintptr_t at=base+kTransportJump;
 if(!checked){uint8_t now[22]{};bool readable=true;for(int i=0;i<22;++i)if(!Read(at-3+i,&now[i])){readable=false;break;}
  const bool probe=readable&&memcmp(now,kTransportContext,22)==0;const bool direct=readable&&memcmp(now,kTransportContext,3)==0&&memcmp(now+3,kTransportDirect,6)==0&&memcmp(now+9,kTransportContext+9,13)==0;
  checked=probe||direct?1:-1;if(checked<0&&log)log("Force RakNet: unexpected game code at the transport choice, setting unavailable");}
 if(checked<0)return false;if(applied.load()==(int)on)return true;
 const bool ok=TransportWrite(at,on?kTransportDirect:kTransportProbe);if(ok){applied=(int)on;if(log)log(on?"Force RakNet: on (servers joined by address use RakNet)":"Force RakNet: off (the game picks the transport)");}
 return ok;}

inline constexpr uint32_t kFallbackJump=0x8df8e8;
inline const uint8_t kFallbackContext[13]={0x84,0xc0,0x0f,0x84,0x95,0x01,0x00,0x00,0x48,0x8d,0x87,0x58,0x03};   // test al,al / je / lea rax,[rdi+0x358]
inline const uint8_t kFallbackJe[6]={0x0f,0x84,0x95,0x01,0x00,0x00};
inline const uint8_t kFallbackNop[6]={0x90,0x90,0x90,0x90,0x90,0x90};
// Applies RakNet fallback (true) or the game's behaviour (false). Returns false when the game code is not the expected one.
inline bool RakNetFallback(bool on,void(*log)(const char*)){static int checked=0;static std::atomic<int> applied{-1};uintptr_t base{};uint32_t size{};
 if(!Supported(&base,&size))return false;const uintptr_t at=base+kFallbackJump;
 if(!checked){uint8_t now[13]{};bool readable=true;for(int i=0;i<13;++i)if(!Read(at-2+i,&now[i])){readable=false;break;}
  const bool game=readable&&memcmp(now,kFallbackContext,13)==0;const bool patched=readable&&now[0]==0x84&&now[1]==0xc0&&memcmp(now+2,kFallbackNop,6)==0&&memcmp(now+8,kFallbackContext+8,5)==0;
  checked=game||patched?1:-1;if(checked<0&&log)log("RakNet fallback: unexpected game code at the transport fallback, setting unavailable");}
 if(checked<0)return false;if(applied.load()==(int)on)return true;
 const bool ok=TransportWrite(at,on?kFallbackNop:kFallbackJe);if(ok){applied=(int)on;if(log)log(on?"RakNet fallback: on (RakNet when a server gives no NetherNet answer)":"RakNet fallback: off");}
 return ok;}
}
