// Client packet sending:
// - create: game function at RVA 0x2376780(&shared_ptr, id);
// - sender: client->vtable[0x910](client), send: sender->vtable[0x20](sender, packet);
// - chat: TextPacket #9; +0x30 = 0; +0x38 = xuid (UserEntityIdentifierComponent+0xd0);
// variant at +0xa0 (index at +0xe8) = 1: { type 1 (Chat), author (actor+0xbe8) at +0xa8, message at +0xc8 };
// - command: CommandRequestPacket #0x4d; command at +0x30; origin at +0x50 { type 0 (player),
// random v4 UUID at +0x58, empty request id at +0x68, ActorUniqueID at +0x88 }; +0x90 = 0x37, +0x94 = 0;
// - local chat message: game function at RVA 0x6da9820(actor, std::string*, optional<std::string>*).
// Packet strings are MSVC std::string (same heap as the game); the packet is released by the game's shared_ptr.
#pragma once
#include "game_visual_entities.hpp"
#include <random>
#include <string>

namespace game {
struct GamePacket{uint64_t object=0,control=0;};
inline void ReleasePacket(GamePacket& p){if(!p.control)return;auto* c=reinterpret_cast<volatile long*>(p.control);
 if(InterlockedDecrement(c+2)==0){auto table=*reinterpret_cast<uint64_t**>(p.control);reinterpret_cast<void(*)(uint64_t)>(table[0])(p.control);if(InterlockedDecrement(c+3)==0)reinterpret_cast<void(*)(uint64_t)>(table[1])(p.control);}p.control=p.object=0;}
inline GamePacket CreatePacket(const Context& c,int id){GamePacket p{};reinterpret_cast<void(*)(GamePacket*,int)>(c.base+Rva(0x2376780))(&p,id);return p;}
inline bool SendPacket(const Context& c,uint64_t packet){const uint64_t get=Virtual0(c,c.client,0x910);if(!get||!packet)return false;
 const uint64_t sender=reinterpret_cast<uint64_t(*)(uint64_t)>(get)(c.client);const uint64_t send=Virtual0(c,sender,0x20);if(!sender||!send)return false;reinterpret_cast<void(*)(uint64_t,uint64_t)>(send)(sender,packet);return true;}
// local receive: sender+0x28 null -> network handler *(sender+0x20); connection = first element of the array at
// +0xe8/+0xf0; callback = *(*(*(client+0x1d0)+0xd0)+0x50); packet+0x20 (dispatcher)->vtable[1](dispatcher,
// connection, callback, &packet shared_ptr)
inline bool ReceivePacket(const Context& c,GamePacket* p){const uint64_t get=Virtual0(c,c.client,0x910);if(!get||!p->object)return false;
 const uint64_t sender=reinterpret_cast<uint64_t(*)(uint64_t)>(get)(c.client);uint8_t closed{};uint64_t network{},begin{},end{},connection{},mc{},holder{},callback{},dispatcher{};
 if(!sender||!Read(sender+0x28,&closed)||closed||!Read(sender+0x20,&network)||!network||!Read(network+0xe8,&begin)||!Read(network+0xf0,&end)||begin==end||!Read(begin,&connection)||!connection)return false;
 if(!Read(c.client+0x1d0,&mc)||!mc||!Read(mc+0xd0,&holder)||!holder||!Read(holder+0x50,&callback)||!callback||!Read(p->object+0x20,&dispatcher)||!dispatcher)return false;
 const uint64_t handle=Virtual0(c,dispatcher,8);if(!handle)return false;reinterpret_cast<void(*)(uint64_t,uint64_t,uint64_t,GamePacket*)>(handle)(dispatcher,connection,callback,p);return true;}
inline std::string& PacketText(uint64_t at){return *reinterpret_cast<std::string*>(at);}
struct OptionalText{std::string text;bool has=false;};
inline void ClientMessage(const Context& c,const std::string& text){std::string line=text;OptionalText none{};reinterpret_cast<void(*)(uint64_t,std::string*,OptionalText*)>(c.base+Rva(0x6da9820))(c.actor,&line,&none);}
inline bool SendChat(const Context& c,const std::string& message){
 uintptr_t user{};if(!Element(c.registry,c.entity,"UserEntityIdentifierComponent",0x250,&user))return false;   // 0x250-byte element, xuid at +0xd0
 GamePacket p=CreatePacket(c,9);if(!p.object)return false;
 *reinterpret_cast<uint8_t*>(p.object+0x30)=0;PacketText(p.object+0x38)=PacketText(user+0xd0);
 char author[256]{};RawText(c.actor+0xbe8,author,sizeof(author));
 auto& index=*reinterpret_cast<int8_t*>(p.object+0xe8);
 if(index!=1){*reinterpret_cast<uint8_t*>(p.object+0xa0)=1;new(reinterpret_cast<void*>(p.object+0xa8)) std::string(author);new(reinterpret_cast<void*>(p.object+0xc8)) std::string(message);index=1;}
 else{*reinterpret_cast<uint8_t*>(p.object+0xa0)=1;PacketText(p.object+0xa8)=author;PacketText(p.object+0xc8)=message;}
 const bool sent=SendPacket(c,p.object);ReleasePacket(p);return sent;}
inline bool SendCommand(const Context& c,const std::string& command){
 uintptr_t element{};uint64_t unique{};if(!Element(c.registry,c.entity,"ActorUniqueIDComponent",8,&element)||!Read(element,&unique))return false;
 GamePacket p=CreatePacket(c,0x4d);if(!p.object)return false;
 PacketText(p.object+0x30)=command;
 static std::mt19937 random{std::random_device{}()};uint64_t high=((uint64_t)random()<<32)|random(),low=((uint64_t)random()<<32)|random();
 high=(high&0xffffffffffff0fffull)|0x4000ull;low=(low&0x3fffffffffffffffull)|0x8000000000000000ull;
 *reinterpret_cast<uint64_t*>(p.object+0x50)=0;*reinterpret_cast<uint64_t*>(p.object+0x58)=high;*reinterpret_cast<uint64_t*>(p.object+0x60)=low;
 PacketText(p.object+0x68).clear();*reinterpret_cast<uint64_t*>(p.object+0x88)=unique;*reinterpret_cast<uint32_t*>(p.object+0x90)=0x37;*reinterpret_cast<uint8_t*>(p.object+0x94)=0;
 const bool sent=SendPacket(c,p.object);ReleasePacket(p);return sent;}
}
