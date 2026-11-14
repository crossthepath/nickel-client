// MiniMap faces, built at run time from the player's own game: nothing is shipped.
// - Mobs: the actor's name key ("entity.<id>.name", actor vtable +0xf0) gives the identifier; entity/<id>.entity.json
//   (through the game resource system) gives the default texture and geometry; the geometry (models/entity/*.geo.json,
//   formats 1.8 and 1.12, parent geometry followed once) gives the head bone, and the front face of its largest cube is
//   cut from the texture (box UV: (u + depth, v + depth), width x height; per-face UV: the north face), scaled from the
//   geometry's texture size to the PNG size. Any step that fails leaves the type as a dot.
// Textures are PNG (WIC) or TGA (DecodeTga).
// - Players: the 8 x 8 face of the skin with the hat layer (game_skin.hpp SkinFaceOf), refreshed every 10 s.
// Faces are drawn into 16 px cells of a 256 x 256 atlas (image 4), letterboxed, nearest neighbour; at most two mob types
// are resolved per frame and the atlas is uploaded at most five times per second.
#pragma once
#include "menu_icons.hpp"
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mapfaces {
// TGA (newer entity textures): uncompressed (2) or RLE (10) true colour, 24 or 32 bits, any origin -> top-down RGBA.
inline bool DecodeTga(const std::string& b,std::vector<unsigned char>& rgba,UINT& w,UINT& h){
 if(b.size()<18)return false;const unsigned char* d=(const unsigned char*)b.data();const int type=d[2],bpp=d[16],desc=d[17];w=d[12]|(d[13]<<8);h=d[14]|(d[15]<<8);
 if((type!=2&&type!=10)||(bpp!=24&&bpp!=32)||!w||!h||w>4096||h>4096||d[1]!=0)return false;const size_t px=(size_t)w*h,n=bpp/8;size_t at=18+d[0];rgba.assign(px*4,0);
 auto put=[&](size_t i,const unsigned char* s){const size_t row=i/w,col=i%w,y=(desc&0x20)?row:h-1-row,x=(desc&0x10)?w-1-col:col;unsigned char* o=&rgba[(y*w+x)*4];o[0]=s[2];o[1]=s[1];o[2]=s[0];o[3]=n==4?s[3]:255;};
 for(size_t i=0;i<px;){if(type==2){if(at+n>b.size())return false;put(i++,d+at);at+=n;continue;}
  if(at>=b.size())return false;const unsigned char head=d[at++];const size_t count=(head&0x7f)+1;
  if(head&0x80){if(at+n>b.size())return false;for(size_t k=0;k<count&&i<px;++k)put(i++,d+at);at+=n;}
  else for(size_t k=0;k<count&&i<px;++k){if(at+n>b.size())return false;put(i++,d+at);at+=n;}}
 return true;}
struct Atlas{
 static constexpr int Cell=16,PerRow=16,Size=Cell*PerRow;
 std::vector<unsigned char> pixels=std::vector<unsigned char>((size_t)Size*Size*4,0);int used=0;bool dirty=false;double uploaded=0;
 std::map<int,int> mobCell;std::map<int,uint64_t> pendingActor;std::vector<int> queue;   // mob type -> cell (-1 pending, -2 none)
 struct Player{int cell=-1;double at=-100,seen=0;};std::map<uint64_t,Player> players;std::vector<int> freeCells;   // actor -> face cell; cells of players gone 30 s
 std::map<std::string,icons::Json> geoFiles;std::set<std::string> missingFiles;
 // write w x h RGBA pixels (row stride `stride` pixels) into a cell, letterboxed and scaled with the nearest pixel
 void Put(int cell,const unsigned char* rgba,int w,int h,int stride){if(cell<0||w<=0||h<=0)return;const int cx=(cell%PerRow)*Cell,cy=(cell/PerRow)*Cell,side=std::max(w,h);
  const int dw=std::max(1,w*Cell/side),dh=std::max(1,h*Cell/side),ox=(Cell-dw)/2,oy=(Cell-dh)/2;
  for(int y=0;y<Cell;++y)for(int x=0;x<Cell;++x){unsigned char* o=&pixels[((size_t)(cy+y)*Size+cx+x)*4];o[0]=o[1]=o[2]=o[3]=0;
   if(x<ox||y<oy||x>=ox+dw||y>=oy+dh)continue;const int sx=(x-ox)*w/dw,sy=(y-oy)*h/dh;memcpy(o,rgba+((size_t)sy*stride+sx)*4,4);o[3]=255;}dirty=true;}
 int NewCell(){if(!freeCells.empty()){const int c=freeCells.back();freeCells.pop_back();return c;}return used<PerRow*PerRow?used++:-2;}
 static std::string Last(const std::string& path){if(!icons::ReadPackFile)return {};auto all=icons::ReadPackFile(path);for(auto it=all.rbegin();it!=all.rend();++it)if(!it->empty())return *it;return {};}
 static double Num(const icons::Json* j,int i){if(!j||j->kind!=icons::Json::Array||(int)j->items.size()<=i)return 0;return atof(j->items[i].text.c_str());}
 // geometry by identifier in a parsed file: format 1.8 ("geometry.x" or "geometry.x:geometry.parent" keys) or 1.12
 static const icons::Json* FindGeo(const icons::Json& file,const std::string& id,std::string* parent,double* tw,double* th){
  for(auto& f:file.fields){const std::string& k=f.first;const size_t colon=k.find(':');
   if(k==id||(colon==id.size()&&k.compare(0,colon,id)==0)){if(parent)*parent=colon!=std::string::npos?k.substr(colon+1):std::string();
    if(auto w=f.second.get("texturewidth"))*tw=atof(w->text.c_str());if(auto h=f.second.get("textureheight"))*th=atof(h->text.c_str());return &f.second;}}
  if(auto list=file.get("minecraft:geometry"))for(auto& g:list->items){const icons::Json* d=g.get("description");const icons::Json* ident=d?d->get("identifier"):nullptr;if(!ident||ident->text!=id)continue;
   if(auto w=d->get("texture_width"))*tw=atof(w->text.c_str());if(auto h=d->get("texture_height"))*th=atof(h->text.c_str());if(parent)parent->clear();return &g;}
  return nullptr;}
 const icons::Json* File(const std::string& path){auto f=geoFiles.find(path);if(f!=geoFiles.end())return &f->second;if(missingFiles.count(path))return nullptr;
  const std::string text=Last(path);if(text.empty()){missingFiles.insert(path);return nullptr;}return &geoFiles.emplace(path,icons::ParseText(text)).first->second;}
 // candidate files for a geometry id: geometry.cow.v1.8 -> cow.v1.8, cow, cow_v1.8 (.geo.json), the entity id, models/mobs.json
 const icons::Json* Geometry(const std::string& id,const std::string& entity,std::string* parent,double* tw,double* th){
  const std::string base=id.rfind("geometry.",0)==0?id.substr(9):id;std::vector<std::string> names{base};
  for(size_t v=base.find(".v");v!=std::string::npos;v=base.find(".v",v+1)){names.push_back(base.substr(0,v));names.push_back(base.substr(0,v)+"_"+base.substr(v+1));}
  const size_t dot=base.find('.');if(dot!=std::string::npos)names.push_back(base.substr(0,dot));names.push_back(entity);
  std::vector<std::string> paths;for(auto& n:names)paths.push_back("models/entity/"+n+".geo.json");paths.push_back("models/mobs.json");
  for(auto& p:paths)if(const icons::Json* file=File(p))if(const icons::Json* g=FindGeo(*file,id,parent,tw,th))return g;
  return nullptr;}
 // front face of the head: x, y, width, height in texture units
 static bool HeadFace(const icons::Json& geo,double r[4]){const icons::Json* bones=geo.get("bones");if(!bones)return false;const icons::Json* pick=nullptr;
  auto hasCubes=[](const icons::Json& b){const icons::Json* c=b.get("cubes");return c&&!c->items.empty();};
  for(auto& b:bones->items){const icons::Json* n=b.get("name");if(!n||!hasCubes(b))continue;std::string low=n->text;for(auto& ch:low)ch=(char)tolower((unsigned char)ch);if(low=="head"){pick=&b;break;}}
  if(!pick)for(auto& b:bones->items)if(hasCubes(b)){pick=&b;break;}
  if(!pick)return false;double best=-1;
  for(auto& c:pick->get("cubes")->items){const icons::Json* size=c.get("size");const icons::Json* uv=c.get("uv");if(!size||!uv)continue;const double w=Num(size,0),h=Num(size,1),d=Num(size,2);double q[4];
   if(uv->kind==icons::Json::Array){q[0]=Num(uv,0)+d;q[1]=Num(uv,1)+d;q[2]=w;q[3]=h;}
   else{const icons::Json* north=uv->get("north");if(!north)continue;q[0]=Num(north->get("uv"),0);q[1]=Num(north->get("uv"),1);q[2]=Num(north->get("uv_size"),0);q[3]=Num(north->get("uv_size"),1);}
   if(q[2]<=0||q[3]<=0)continue;if(w*h>best){best=w*h;memcpy(r,q,sizeof(q));}}
  return best>0;}
 int Resolve(const std::string& entity){
  const icons::Json file=icons::ParsePacks("entity/"+entity+".entity.json");const icons::Json* ce=file.get("minecraft:client_entity");const icons::Json* desc=ce?ce->get("description"):nullptr;if(!desc)return -2;
  auto first=[](const icons::Json* map){if(!map||map->kind!=icons::Json::Object||map->fields.empty())return std::string();if(auto d=map->get("default"))return d->text;return map->fields.front().second.text;};
  const std::string texture=first(desc->get("textures")),geometry=first(desc->get("geometry"));if(texture.empty()||geometry.empty())return -2;
  std::string parent;double tw=64,th=64;const icons::Json* geo=Geometry(geometry,entity,&parent,&tw,&th);double r[4];bool ok=geo&&HeadFace(*geo,r);
  if(geo&&!ok&&!parent.empty()){double pw=tw,ph=th;if(const icons::Json* g=Geometry(parent,entity,nullptr,&pw,&ph))ok=HeadFace(*g,r);}
  if(!ok)return -2;std::vector<unsigned char> px;UINT w=0,h=0;const std::string png=Last(texture+".png");
  if(png.empty()?!DecodeTga(Last(texture+".tga"),px,w,h):!icons::Icons().Decode(png,px,w,h))return -2;
  const double fx=w/(tw>0?tw:64),fy=h/(th>0?th:64);const int x0=(int)(r[0]*fx),y0=(int)(r[1]*fy),fw=(int)(r[2]*fx),fh=(int)(r[3]*fy);
  if(x0<0||y0<0||fw<=0||fh<=0||x0+fw>(int)w||y0+fh>(int)h)return -2;const int cell=NewCell();if(cell<0)return -2;Put(cell,&px[((size_t)y0*w+x0)*4],fw,fh,(int)w);return cell;}
 // cell of a mob type (-1 pending, -2 none); the actor gives the entity id when the type is resolved
 int Mob(int type,uint64_t actor){auto f=mobCell.find(type);if(f!=mobCell.end())return f->second;if(!actor)return -1;mobCell[type]=-1;pendingActor[type]=actor;queue.push_back(type);return -1;}
 void Work(int budget,const std::function<std::string(uint64_t)>& entityOf,void(*log)(const char*)){
  while(budget-->0&&!queue.empty()){const int type=queue.front();queue.erase(queue.begin());const std::string entity=entityOf(pendingActor[type]);pendingActor.erase(type);
   const int cell=entity.empty()?-2:Resolve(entity);mobCell[type]=cell;
   if(log){static int told=0;if(told++<24)log(("MiniMap faces: "+(entity.empty()?"type "+std::to_string(type&0xff):entity)+(cell>=0?" -> cell "+std::to_string(cell):" -> dot")).c_str());}}}
 // player face cell, refreshed every 10 s (-1 none yet)
 int PlayerFace(uint64_t actor,double now,const std::function<bool(uint64_t,unsigned char*)>& faceOf){
  if(players.size()>32)for(auto it=players.begin();it!=players.end();)if(now-it->second.seen>30){if(it->second.cell>=0)freeCells.push_back(it->second.cell);it=players.erase(it);}else ++it;
  Player& p=players[actor];p.seen=now;if(now-p.at<10)return p.cell;p.at=now;unsigned char px[64*4];
  if(!faceOf(actor,px))return p.cell;if(p.cell<0)p.cell=NewCell();if(p.cell>=0)Put(p.cell,px,8,8,8);return p.cell;}
};
inline Atlas& Faces(){static Atlas* a=new Atlas;return *a;}
}
