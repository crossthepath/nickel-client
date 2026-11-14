#pragma once
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <vector>
namespace nickel::visual {
struct VisualRoster{bool known=false;std::unordered_set<uint64_t> ids;};
template<class Read> VisualRoster read_visual_roster(uintptr_t container,Read read){
 if(!container||container>std::numeric_limits<uintptr_t>::max()-0x10)return {};
 uint64_t head=0,count=0,first=0;
 if(!read(container+8,&head)||!read(container+0x10,&count)||!head||count>4096)return {};
 if(!read(head,&first))return {};
 if(count==0&&first!=head)return {};
 VisualRoster roster;std::unordered_set<uintptr_t> seen;
 if(count){
  if(!first||first==head)return {};
  uint64_t node=first,lastNext=0;
  for(uint64_t i=0;i<count;++i){
   if(!node||node==head||node>std::numeric_limits<uintptr_t>::max()-0x20||!seen.insert((uintptr_t)node).second)return {};
   uint64_t next=0,uid=0;
   if(!read(node,&next)||!read(node+0x20,&uid))return {};
   roster.ids.insert(uid);lastNext=next;node=next;
  }
  if(lastNext!=head)return {};
 }
 uint64_t head2=0,count2=0;
 if(!read(container+8,&head2)||!read(container+0x10,&count2)||head2!=head||count2!=count)return {};
 roster.known=true;return roster;
}
inline bool visual_bot(bool enabled,bool player,bool local,const VisualRoster& roster,bool uniqueIdKnown,uint64_t uid){
 return enabled&&player&&!local&&roster.known&&!roster.ids.empty()&&uniqueIdKnown&&!roster.ids.count(uid);
}
struct FoliageTargets{bool known=false;std::vector<uintptr_t> values;};
template<class Read> FoliageTargets read_foliage_targets(uintptr_t renderer,Read read){
 FoliageTargets result{};
 if(!renderer||renderer>std::numeric_limits<uintptr_t>::max()-0x30)return result;
 uint64_t head=0,first=0;
 if(!read(renderer+0x30,&head)||!head||!read(head,&first))return result;
 std::vector<uintptr_t> values;bool valid=true;
 if(first!=head){
  std::unordered_set<uint64_t> seen;uint64_t node=first;
  for(size_t i=0;i<256&&node&&node!=head;++i){
   if(node>std::numeric_limits<uintptr_t>::max()-0x20||!seen.insert(node).second){valid=false;break;}
   uint64_t next=0,target=0,probe=0;
   if(!read(node,&next)||!read(node+0x18,&target)||!target||!read(target,&probe)){valid=false;break;}
   values.push_back((uintptr_t)target);node=next;
  }
  if(node!=head)valid=false;
 }
 if(!valid)return result;
 uint64_t head2=0,first2=0;
 if(!read(renderer+0x30,&head2)||head2!=head||!read(head,&first2)||first2!=first)return result;
 result.known=true;result.values=std::move(values);return result;
}
struct FoliageRefreshState{bool requested=false;uint64_t generation=0,applied=0;};
inline void request_foliage(FoliageRefreshState& state,bool wanted){
 if(state.requested==wanted)return;state.requested=wanted;++state.generation;
}
inline bool foliage_refresh_pending(const FoliageRefreshState& state){return state.generation!=state.applied;}
inline void acknowledge_foliage(FoliageRefreshState& state,uint64_t observed){
 if(observed>state.applied&&observed<=state.generation)state.applied=observed;
}
}
