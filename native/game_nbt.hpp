// NBT (Bedrock little-endian / Java big-endian), gzip/zlib decompression (inflate, RFC 1951) and schematic
// formats:
// 0 Bedrock .mcstructure (read and write), 1 Litematic .litematic, 2 Sponge .schem, 3 Java structure .nbt;
// the format is chosen from the file content.
// Java block properties are kept as string states; blocks whose name does not exist in Bedrock stay
// "unresolved".
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace nbt {
enum : uint8_t{End=0,Byte=1,Short=2,Int=3,Long=4,Float=5,Double=6,ByteArray=7,String=8,List=9,Compound=10,IntArray=11,LongArray=12};
struct Tag{uint8_t type=End;int64_t i=0;double d=0;std::string s;uint8_t listType=End;std::vector<Tag> list;std::vector<std::pair<std::string,Tag>> comp;std::vector<int64_t> array;
 const Tag* get(const std::string& k)const{for(auto& p:comp)if(p.first==k)return &p.second;return nullptr;}
 Tag& add(const std::string& k,Tag t){comp.push_back({k,std::move(t)});return comp.back().second;}
 int64_t num()const{return type==Float||type==Double?(int64_t)d:i;}};
inline Tag MakeInt(int32_t v){Tag t;t.type=Int;t.i=v;return t;}
inline Tag MakeString(std::string v){Tag t;t.type=String;t.s=std::move(v);return t;}
inline Tag MakeCompound(){Tag t;t.type=Compound;return t;}
inline Tag MakeList(uint8_t of){Tag t;t.type=List;t.listType=of;return t;}

struct Reader{const uint8_t* p;size_t n,at=0;bool big;bool ok=true;int depth=0;
 bool need(size_t k){if(at+k>n){ok=false;return false;}return true;}
 uint64_t raw(int k){if(!need(k))return 0;uint64_t v=0;for(int b=0;b<k;++b){const uint64_t byte=p[at+b];v|=big?byte<<(8*(k-1-b)):byte<<(8*b);}at+=k;return v;}
 std::string str(){const size_t len=(size_t)raw(2);if(!need(len))return {};std::string s((const char*)p+at,len);at+=len;return s;}
 void payload(uint8_t type,Tag& t){t.type=type;if(++depth>512){ok=false;return;}
  switch(type){case Byte:t.i=(int8_t)raw(1);break;case Short:t.i=(int16_t)raw(2);break;case Int:t.i=(int32_t)raw(4);break;case Long:t.i=(int64_t)raw(8);break;
   case Float:{uint32_t v=(uint32_t)raw(4);float f;memcpy(&f,&v,4);t.d=f;t.i=(int64_t)f;break;}case Double:{uint64_t v=raw(8);double f;memcpy(&f,&v,8);t.d=f;t.i=(int64_t)f;break;}
   case ByteArray:{const int32_t c=(int32_t)raw(4);if(c<0||!need((size_t)c)){ok=false;break;}t.array.resize(c);for(int32_t k=0;k<c;++k)t.array[k]=(int8_t)p[at+k];at+=c;break;}
   case String:t.s=str();break;
   case List:{t.listType=(uint8_t)raw(1);const int32_t c=(int32_t)raw(4);if(c<0||c>(int32_t)(n-at+1)*8){ok=false;break;}t.list.resize(c);for(int32_t k=0;k<c&&ok;++k)payload(t.listType,t.list[k]);break;}
   case Compound:for(;ok;){const uint8_t ty=(uint8_t)raw(1);if(!ok||ty==End)break;std::string k=str();Tag v;payload(ty,v);t.comp.push_back({std::move(k),std::move(v)});}break;
   case IntArray:{const int32_t c=(int32_t)raw(4);if(c<0||!need((size_t)c*4)){ok=false;break;}t.array.resize(c);for(int32_t k=0;k<c;++k)t.array[k]=(int32_t)raw(4);break;}
   case LongArray:{const int32_t c=(int32_t)raw(4);if(c<0||!need((size_t)c*8)){ok=false;break;}t.array.resize(c);for(int32_t k=0;k<c;++k)t.array[k]=(int64_t)raw(8);break;}
   default:ok=false;}
  --depth;}};
inline bool Read(const std::vector<uint8_t>& data,bool big,Tag* root){Reader r{data.data(),data.size(),0,big};const uint8_t ty=(uint8_t)r.raw(1);if(!r.ok||ty!=Compound)return false;r.str();r.payload(Compound,*root);return r.ok;}
struct Writer{std::vector<uint8_t> out;
 void raw(uint64_t v,int k){for(int b=0;b<k;++b)out.push_back((uint8_t)(v>>(8*b)));}   // little-endian (Bedrock)
 void str(const std::string& s){raw(s.size(),2);out.insert(out.end(),s.begin(),s.end());}
 void payload(const Tag& t){switch(t.type){case Byte:raw((uint64_t)t.i,1);break;case Short:raw((uint64_t)t.i,2);break;case Int:raw((uint64_t)t.i,4);break;case Long:raw((uint64_t)t.i,8);break;
   case Float:{float f=(float)t.d;uint32_t v;memcpy(&v,&f,4);raw(v,4);break;}case Double:{uint64_t v;memcpy(&v,&t.d,8);raw(v,8);break;}
   case ByteArray:raw(t.array.size(),4);for(auto v:t.array)raw((uint64_t)v,1);break;case String:str(t.s);break;
   case List:raw(t.list.empty()&&t.listType==End?End:t.listType,1);raw(t.list.size(),4);for(auto& e:t.list)payload(e);break;
   case Compound:for(auto& [k,v]:t.comp){raw(v.type,1);str(k);payload(v);}raw(End,1);break;
   case IntArray:raw(t.array.size(),4);for(auto v:t.array)raw((uint64_t)v,4);break;case LongArray:raw(t.array.size(),4);for(auto v:t.array)raw((uint64_t)v,8);break;}}};
inline std::vector<uint8_t> WriteLE(const Tag& root){Writer w;w.raw(Compound,1);w.str("");w.payload(root);return w.out;}

// ---- inflate (RFC 1951), gzip (RFC 1952) / zlib (RFC 1950)
struct Bits{const uint8_t* p;size_t n,at=0;uint32_t buf=0;int count=0;bool ok=true;
 int bit(){if(!count){if(at>=n){ok=false;return 0;}buf=p[at++];count=8;}const int b=buf&1;buf>>=1;--count;return b;}
 int bits(int k){int v=0;for(int i=0;i<k;++i)v|=bit()<<i;return v;}};
struct Huff{uint16_t counts[16]{},symbols[320]{};};
inline void HuffBuild(Huff& h,const uint8_t* lengths,int n){memset(h.counts,0,sizeof h.counts);for(int i=0;i<n;++i)++h.counts[lengths[i]];h.counts[0]=0;uint16_t offs[16]{};
 for(int i=1;i<16;++i)offs[i]=offs[i-1]+h.counts[i-1];for(int i=0;i<n;++i)if(lengths[i])h.symbols[offs[lengths[i]]++]=(uint16_t)i;}
inline int HuffDecode(Bits& b,const Huff& h){int code=0,first=0,index=0;for(int len=1;len<16;++len){code|=b.bit();const int count=h.counts[len];if(code-count<first)return h.symbols[index+(code-first)];index+=count;first+=count;first<<=1;code<<=1;if(!b.ok)return -1;}return -1;}
inline bool Inflate(const uint8_t* data,size_t n,std::vector<uint8_t>& out,size_t limit){Bits b{data,n};
 static const uint16_t lbase[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};static const uint8_t lext[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
 static const uint16_t dbase[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};static const uint8_t dext[30]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
 for(int last=0;!last;){last=b.bit();const int type=b.bits(2);if(!b.ok)return false;
  if(type==0){b.count=0;if(b.at+4>n)return false;const uint16_t len=data[b.at]|data[b.at+1]<<8;b.at+=4;if(b.at+len>n)return false;out.insert(out.end(),data+b.at,data+b.at+len);b.at+=len;if(out.size()>limit)return false;continue;}
  Huff lit,dist;if(type==1){uint8_t l[288];for(int i=0;i<144;++i)l[i]=8;for(int i=144;i<256;++i)l[i]=9;for(int i=256;i<280;++i)l[i]=7;for(int i=280;i<288;++i)l[i]=8;HuffBuild(lit,l,288);uint8_t d[30];memset(d,5,30);HuffBuild(dist,d,30);}
  else if(type==2){const int hlit=b.bits(5)+257,hdist=b.bits(5)+1,hclen=b.bits(4)+4;static const uint8_t order[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};uint8_t cl[19]{};for(int i=0;i<hclen;++i)cl[order[i]]=(uint8_t)b.bits(3);
   Huff ch;HuffBuild(ch,cl,19);uint8_t lens[320]{};for(int i=0;i<hlit+hdist;){const int sym=HuffDecode(b,ch);if(sym<0)return false;
    if(sym<16)lens[i++]=(uint8_t)sym;else{int rep=0;uint8_t v=0;if(sym==16){if(!i)return false;v=lens[i-1];rep=3+b.bits(2);}else if(sym==17)rep=3+b.bits(3);else rep=11+b.bits(7);if(i+rep>hlit+hdist)return false;while(rep--)lens[i++]=v;}}
   HuffBuild(lit,lens,hlit);HuffBuild(dist,lens+hlit,hdist);}
  else return false;
  for(;;){const int sym=HuffDecode(b,lit);if(sym<0)return false;if(sym<256){out.push_back((uint8_t)sym);continue;}if(sym==256)break;const int li=sym-257;if(li>=29)return false;const int len=lbase[li]+b.bits(lext[li]);
   const int ds=HuffDecode(b,dist);if(ds<0||ds>=30)return false;const size_t d=(size_t)(dbase[ds]+b.bits(dext[ds]));if(d>out.size())return false;const size_t from=out.size()-d;for(int k=0;k<len;++k)out.push_back(out[from+k]);if(out.size()>limit)return false;}}
 return b.ok;}
inline bool Decompress(const std::vector<uint8_t>& in,std::vector<uint8_t>& out,size_t limit){
 if(in.size()>18&&in[0]==0x1f&&in[1]==0x8b&&in[2]==8){const uint8_t flags=in[3];size_t at=10;if(flags&4){if(at+2>in.size())return false;at+=2+(in[at]|in[at+1]<<8);}
  if(flags&8){while(at<in.size()&&in[at])++at;++at;}if(flags&16){while(at<in.size()&&in[at])++at;++at;}if(flags&2)at+=2;if(at>=in.size())return false;return Inflate(in.data()+at,in.size()-at,out,limit);}
 if(in.size()>2&&(in[0]&0x0f)==8&&((in[0]<<8)|in[1])%31==0)return Inflate(in.data()+2,in.size()-2,out,limit);
 out=in;return true;}

// ---- common schematic
struct BlockEntry{std::string name;Tag states;int32_t version=0;};
struct Data{int sx=0,sy=0,sz=0;std::vector<BlockEntry> palette;std::vector<int32_t> l0,l1;bool hasOrigin=false;int ox=0,oy=0,oz=0;int format=0;};
inline size_t Index(const Data& d,int x,int y,int z){return ((size_t)x*d.sy+y)*d.sz+z;}
inline bool ReadMcstructure(const Tag& r,Data& d){const Tag* size=r.get("size");const Tag* st=r.get("structure");if(!size||!st||size->list.size()<3)return false;
 d.sx=(int)size->list[0].i;d.sy=(int)size->list[1].i;d.sz=(int)size->list[2].i;if(d.sx<=0||d.sy<=0||d.sz<=0)return false;const size_t total=(size_t)d.sx*d.sy*d.sz;if(total>0x4000000)return false;   // not a game address: size limit
 const Tag* idx=st->get("block_indices");const Tag* pal=st->get("palette");const Tag* def=pal?pal->get("default"):nullptr;const Tag* bp=def?def->get("block_palette"):nullptr;if(!idx||!bp)return false;
 for(auto& e:bp->list){BlockEntry b;if(auto n=e.get("name"))b.name=n->s;if(auto s=e.get("states"))b.states=*s;else b.states=MakeCompound();if(auto v=e.get("version"))b.version=(int32_t)v->i;d.palette.push_back(std::move(b));}
 auto layer=[&](size_t k,std::vector<int32_t>& out){out.assign(total,-1);if(k>=idx->list.size())return;const Tag& l=idx->list[k];if(l.list.size()!=total)return;for(size_t i=0;i<total;++i)out[i]=(int32_t)l.list[i].i;};
 layer(0,d.l0);layer(1,d.l1);if(auto o=r.get("structure_world_origin"))if(o->list.size()>=3){d.hasOrigin=true;d.ox=(int)o->list[0].i;d.oy=(int)o->list[1].i;d.oz=(int)o->list[2].i;}d.format=0;return true;}
// Java properties "name[a=b,c=d]" / Properties compound -> string states.
inline BlockEntry JavaEntry(const std::string& full){BlockEntry b;b.states=MakeCompound();const size_t open=full.find('[');b.name=full.substr(0,open);
 if(open!=std::string::npos){std::string inner=full.substr(open+1,full.size()-open-2);size_t at=0;while(at<inner.size()){size_t comma=inner.find(',',at);if(comma==std::string::npos)comma=inner.size();const std::string kv=inner.substr(at,comma-at);
   const size_t eq=kv.find('=');if(eq!=std::string::npos)b.states.add(kv.substr(0,eq),MakeString(kv.substr(eq+1)));at=comma+1;}}
 if(b.name.find(':')==std::string::npos)b.name="minecraft:"+b.name;return b;}
inline BlockEntry JavaCompound(const Tag& e){BlockEntry b;b.states=MakeCompound();if(auto n=e.get("Name"))b.name=n->s;if(auto p=e.get("Properties"))for(auto& [k,v]:p->comp)b.states.add(k,MakeString(v.s));
 if(b.name.find(':')==std::string::npos)b.name="minecraft:"+b.name;return b;}
inline bool ReadJavaStructure(const Tag& r,Data& d){const Tag* size=r.get("size");const Tag* blocks=r.get("blocks");const Tag* pal=r.get("palette");if(!pal)if(auto ps=r.get("palettes"))if(!ps->list.empty())pal=&ps->list[0];
 if(!size||!blocks||!pal||size->list.size()<3)return false;d.sx=(int)size->list[0].i;d.sy=(int)size->list[1].i;d.sz=(int)size->list[2].i;if(d.sx<=0||d.sy<=0||d.sz<=0)return false;const size_t total=(size_t)d.sx*d.sy*d.sz;if(total>0x4000000)return false;   // not a game address: size limit
 for(auto& e:pal->list)d.palette.push_back(JavaCompound(e));d.l0.assign(total,-1);d.l1.assign(total,-1);
 for(auto& b:blocks->list){const Tag* pos=b.get("pos");const Tag* st=b.get("state");if(!pos||!st||pos->list.size()<3)continue;const int x=(int)pos->list[0].i,y=(int)pos->list[1].i,z=(int)pos->list[2].i;
  if(x<0||y<0||z<0||x>=d.sx||y>=d.sy||z>=d.sz)continue;d.l0[Index(d,x,y,z)]=(int32_t)st->i;}d.format=3;return true;}
inline bool ReadSponge(const Tag& root,Data& d){const Tag* r=root.get("Schematic");if(!r||r->type!=Compound)r=&root;const Tag* w=r->get("Width");const Tag* h=r->get("Height");const Tag* l=r->get("Length");if(!r->get("Version")||!w||!h||!l)return false;
 const Tag* pal=r->get("Palette");const Tag* data=r->get("BlockData");if(const Tag* bl=r->get("Blocks")){if(!pal)pal=bl->get("Palette");if(!data)data=bl->get("Data");}if(!pal||!data)return false;
 d.sx=(int)w->i;d.sy=(int)h->i;d.sz=(int)l->i;if(d.sx<=0||d.sy<=0||d.sz<=0)return false;const size_t total=(size_t)d.sx*d.sy*d.sz;if(total>0x4000000)return false;   // not a game address: size limit
 int maxId=-1;for(auto& [k,v]:pal->comp)maxId=std::max<int>(maxId,(int)v.i);if(maxId<0||maxId>65535)return false;d.palette.resize(maxId+1);for(auto& p:d.palette){p.name="minecraft:air";p.states=MakeCompound();}
 for(auto& [k,v]:pal->comp)d.palette[(size_t)v.i]=JavaEntry(k);d.l0.assign(total,-1);d.l1.assign(total,-1);
 size_t at=0,index=0;while(at<data->array.size()&&index<total){uint32_t value=0;int shift=0;for(;;){const uint8_t b=(uint8_t)data->array[at++];value|=(uint32_t)(b&0x7f)<<shift;shift+=7;if(!(b&0x80)||at>=data->array.size())break;}
  const int x=(int)(index%d.sx),z=(int)((index/d.sx)%d.sz),y=(int)(index/((size_t)d.sx*d.sz));d.l0[Index(d,x,y,z)]=(int32_t)value;++index;}   // Sponge order: y, z, x
 if(const Tag* off=r->get("Offset"))if(off->array.size()>=3){d.hasOrigin=true;d.ox=(int)off->array[0];d.oy=(int)off->array[1];d.oz=(int)off->array[2];}d.format=2;return true;}
inline bool ReadLitematic(const Tag& r,Data& d){const Tag* regions=r.get("Regions");if(!regions||regions->comp.empty())return false;
 // Regions merged into their bounding box (positions relative to the schematic origin).
 int lo[3]={1<<30,1<<30,1<<30},hi[3]={-(1<<30),-(1<<30),-(1<<30)};
 struct R{int p[3],s[3];const Tag* pal;const Tag* states;};std::vector<R> list;
 for(auto& [name,reg]:regions->comp){const Tag* pos=reg.get("Position");const Tag* size=reg.get("Size");const Tag* pal=reg.get("BlockStatePalette");const Tag* bs=reg.get("BlockStates");if(!pos||!size||!pal||!bs)continue;R x{};
  const char* axes[3]={"x","y","z"};for(int a=0;a<3;++a){auto pa=pos->get(axes[a]);auto sa=size->get(axes[a]);x.p[a]=pa?(int)pa->i:0;x.s[a]=sa?(int)sa->i:0;const int s=x.s[a];const int from=s<0?x.p[a]+s+1:x.p[a],to=s<0?x.p[a]:x.p[a]+s-1;lo[a]=std::min(lo[a],from);hi[a]=std::max(hi[a],to);}
  x.pal=pal;x.states=bs;list.push_back(x);}
 if(list.empty())return false;d.sx=hi[0]-lo[0]+1;d.sy=hi[1]-lo[1]+1;d.sz=hi[2]-lo[2]+1;const size_t total=(size_t)d.sx*d.sy*d.sz;if(d.sx<=0||d.sy<=0||d.sz<=0||total>0x4000000)return false;   // not a game address: size limit
 d.l0.assign(total,-1);d.l1.assign(total,-1);
 for(auto& x:list){const int base=(int)d.palette.size();for(auto& e:x.pal->list)d.palette.push_back(JavaCompound(e));const int count=(int)x.pal->list.size();int bits=2;while((1<<bits)<count)++bits;
  const int ax=std::abs(x.s[0]),ay=std::abs(x.s[1]),az=std::abs(x.s[2]);const int ox=x.s[0]<0?x.p[0]+x.s[0]+1:x.p[0],oy=x.s[1]<0?x.p[1]+x.s[1]+1:x.p[1],oz=x.s[2]<0?x.p[2]+x.s[2]+1:x.p[2];
  const auto& longs=x.states->array;const uint64_t mask=(1ull<<bits)-1;
  for(int y=0;y<ay;++y)for(int z=0;z<az;++z)for(int xx=0;xx<ax;++xx){const uint64_t i=((uint64_t)y*az+z)*ax+xx;const uint64_t bitAt=i*bits;const size_t w=(size_t)(bitAt>>6);const int off=(int)(bitAt&63);if(w>=longs.size())continue;
   uint64_t v=(uint64_t)longs[w]>>off;if(off+bits>64&&w+1<longs.size())v|=(uint64_t)longs[w+1]<<(64-off);const int id=(int)(v&mask);if(id>=count)continue;
   d.l0[Index(d,ox+xx-lo[0],oy+y-lo[1],oz+z-lo[2])]=base+id;}}
 d.format=1;return true;}
// Detection by content; 0 = little-endian .mcstructure, otherwise Java big-endian.
inline bool Decode(const std::vector<uint8_t>& file,Data& d){std::vector<uint8_t> raw;if(!Decompress(file,raw,0x10000000))return false;   // not a game address: size limit
 Tag le;if(Read(raw,false,&le)&&le.get("structure")&&le.get("size")&&ReadMcstructure(le,d))return true;
 Tag be;if(!Read(raw,true,&be))return false;if(be.get("Regions"))return ReadLitematic(be,d);
 {const Tag* r=be.get("Schematic");if(!r||r->type!=Compound)r=&be;if(r->get("Version")&&r->get("Width")&&(r->get("Palette")||r->get("Blocks")))return ReadSponge(be,d);}
 if(be.get("size")&&be.get("blocks")&&(be.get("palette")||be.get("palettes")))return ReadJavaStructure(be,d);return false;}
// .mcstructure writer
inline std::vector<uint8_t> EncodeMcstructure(const Data& d){Tag root=MakeCompound();root.add("format_version",MakeInt(1));Tag size=MakeList(Int);for(int v:{d.sx,d.sy,d.sz})size.list.push_back(MakeInt(v));root.add("size",size);
 Tag st=MakeCompound();Tag indices=MakeList(List);for(const auto* l:{&d.l0,&d.l1}){Tag layer=MakeList(Int);layer.list.reserve(l->size());for(int32_t v:*l)layer.list.push_back(MakeInt(v));indices.list.push_back(std::move(layer));}
 st.add("block_indices",std::move(indices));st.add("entities",MakeList(Compound));Tag pal=MakeCompound();Tag def=MakeCompound();Tag bp=MakeList(Compound);
 for(auto& b:d.palette){Tag e=MakeCompound();e.add("name",MakeString(b.name));Tag s=b.states;if(s.type!=Compound)s=MakeCompound();e.add("states",s);e.add("version",MakeInt(b.version));bp.list.push_back(std::move(e));}
 def.add("block_palette",std::move(bp));def.add("block_position_data",MakeCompound());pal.add("default",std::move(def));st.add("palette",std::move(pal));root.add("structure",std::move(st));
 Tag origin=MakeList(Int);for(int v:{d.ox,d.oy,d.oz})origin.list.push_back(MakeInt(v));root.add("structure_world_origin",origin);return WriteLE(root);}
}
