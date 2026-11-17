// Waypoint manager window (Manager Key of the Waypoints module): list, Edit / Delete and the New / Edit form.
// Included by menu.cpp inside namespace overlay, after the synthetic click helpers; it uses menu.cpp's
// drawing helpers and state and does not compile on its own.
// ---- Waypoint manager (Manager Key): title "Waypoints", subtitle, "New waypoint" button, Edit / Delete rows sorted by
// current dimension first, then distance. A new point is named "Waypoint N" at the rounded position with the least-used
// palette colour. The form has Name, X/Y/Z, Color, Save and Use my position.
// Sizes are layout units x k, k = min(width/1280, height/720); text sizes x 0.8436.
namespace waypointui{
struct Entry{std::string name;int x=0,y=0,z=0,dim=-1;bool hasColor=false;unsigned color=0;};
struct Form{bool open=false,editing=false;int index=-1;std::string original;std::string field[4];int focus=-1;int dim=-1;std::vector<std::pair<unsigned,bool>> colors;int chosen=0;std::string error;};
Form form;float scroll=0;
const unsigned palette[8]={0x59d9ff,0x4fd18b,0xffd23f,0xff8a3d,0xf0566a,0xd36aff,0x8b41f0,0xffffff};   // not a game address: colours
std::vector<std::string> Split(const std::string& all){std::vector<std::string> out;size_t from=0;while(from<all.size()){size_t to=all.find(',',from);if(to==std::string::npos)to=all.size();if(to>from)out.push_back(all.substr(from,to-from));from=to+1;}return out;}
bool Parse(const std::string& entry,Entry* out){std::vector<std::string> parts;{size_t at=0;while(at<entry.size()){size_t space=entry.find(' ',at);if(space==std::string::npos)space=entry.size();if(space>at)parts.push_back(entry.substr(at,space-at));at=space+1;}}
 Entry e;for(int pass=0;pass<2&&!parts.empty();++pass){std::string last=parts.back();std::transform(last.begin(),last.end(),last.begin(),[](unsigned char ch){return (char)std::tolower(ch);});
  if(last.size()==7&&last[0]=='#'){e.hasColor=true;e.color=(unsigned)strtoul(last.c_str()+1,nullptr,16);parts.pop_back();}else if(last=="overworld"||last=="nether"||last=="end"){e.dim=last=="overworld"?0:last=="nether"?1:2;parts.pop_back();}}
 if(parts.size()<4)return false;char* stop=nullptr;e.z=strtol(parts[parts.size()-1].c_str(),&stop,10);if(*stop)return false;e.y=strtol(parts[parts.size()-2].c_str(),&stop,10);if(*stop)return false;e.x=strtol(parts[parts.size()-3].c_str(),&stop,10);if(*stop)return false;
 for(size_t i=0;i+3<parts.size();++i){if(!e.name.empty())e.name+=" ";e.name+=parts[i];}*out=e;return true;}
std::string Serialize(const Entry& e){std::string s=e.name+" "+std::to_string(e.x)+" "+std::to_string(e.y)+" "+std::to_string(e.z);if(e.dim>=0)s+=e.dim==0?" overworld":e.dim==1?" nether":" end";if(e.hasColor){char c[16];snprintf(c,sizeof(c)," #%06x",e.color&0xffffff);s+=c;}return s;}   // not a game address: colour mask
Setting* EntriesSetting(Module* m){for(auto& s:m->settings)if(s.id=="entries")return &s;return nullptr;}
bool Feet(float* feet){game::Context c{};return game::Local(&c)&&game::ReadVisualFeet(c,feet);}
void OpenForm(Module* m,int index){
 form=Form{};form.open=true;form.index=index;auto* list=EntriesSetting(m);auto items=Split(list?list->text:"");Entry e;
 if(index>=0&&index<(int)items.size()&&Parse(items[index],&e)){form.editing=true;form.original=e.name;}
 else{ // new waypoint
  std::vector<Entry> all;for(auto& item:items){Entry o;if(Parse(item,&o))all.push_back(o);}
  for(int n=(int)items.size()+1;;++n){std::string name="Waypoint "+std::to_string(n);bool used=false;for(auto& o:all)if(o.name==name)used=true;if(!used){e.name=name;break;}}
  float f[3];if(Feet(f)){e.x=(int)std::floor(f[0]);e.y=(int)std::floor(f[1]);e.z=(int)std::floor(f[2]);}int dim=0;if(game::ReadDimension(&dim))e.dim=dim;
  int count[8]{};for(auto& o:all)if(o.hasColor)for(int i=0;i<8;++i)if(palette[i]==(o.color&0xffffff))++count[i];int best=0;for(int i=1;i<8;++i)if(count[i]<count[best])best=i;e.hasColor=true;e.color=palette[best];}   // not a game address: colour mask
 form.field[0]=e.name;form.field[1]=std::to_string(e.x);form.field[2]=std::to_string(e.y);form.field[3]=std::to_string(e.z);form.dim=e.dim;
 for(unsigned c:palette)form.colors.push_back({c,true});int found=-1;for(size_t i=0;i<form.colors.size();++i)if(e.hasColor&&form.colors[i].first==(e.color&0xffffff))found=(int)i;   // not a game address: colour mask
 if(found<0){form.colors.push_back({e.color,e.hasColor});found=(int)form.colors.size()-1;}form.chosen=found;}
// save the form; true = saved
bool SaveForm(Module* m){
 auto* list=EntriesSetting(m);if(!list)return false;std::string name=form.field[0];while(!name.empty()&&name.front()==' ')name.erase(name.begin());while(!name.empty()&&name.back()==' ')name.pop_back();
 if(name.empty()){form.error="Enter a name";return false;}
 Entry e;e.name=name;int* target[3]={&e.x,&e.y,&e.z};for(int i=0;i<3;++i){const std::string& t=form.field[1+i];char* stop=nullptr;const long v=strtol(t.c_str(),&stop,10);if(t.empty()||*stop){form.error="Coordinates must be whole numbers";return false;}*target[i]=(int)v;}
 e.dim=form.dim;e.color=form.colors[form.chosen].first;e.hasColor=form.colors[form.chosen].second;if(Serialize(e).size()>256){form.error="That name is too long";return false;}
 auto items=Split(list->text);for(size_t i=0;i<items.size();++i){Entry o;if((int)i!=form.index&&Parse(items[i],&o)&&o.name==name){form.error="A waypoint with that name already exists";return false;}}
 if(form.editing&&form.index>=0&&form.index<(int)items.size())items[form.index]=Serialize(e);else items.push_back(Serialize(e));
 std::string joined;for(auto& item:items){if(!joined.empty())joined+=",";joined+=item;}list->text=joined;Save(*m,list);return true;}
}
void DrawWaypointManager(float opacity){
 using namespace waypointui;auto& io=ImGui::GetIO();Module* m=ModuleById("waypoints");if(!m||opacity<=0)return;
 const float W=io.DisplaySize.x,H=io.DisplaySize.y,k=std::min(W/1280.f,H/720.f),T=.8436f*k;auto* d=ImGui::GetForegroundDrawList();const ImVec2 mouse=io.MousePos;const bool live=opened;
 auto inside=[&](ImVec2 lo,ImVec2 hi){return mouse.x>=lo.x&&mouse.y>=lo.y&&mouse.x<hi.x&&mouse.y<hi.y;};
 auto text=[&](ImFont* f,float size,ImVec2 pos,const std::string& t,ImU32 c){Label(d,f,size*T,pos,t.c_str(),c);};
 auto width=[&](ImFont* f,float size,const std::string& t){return TextWidth(f,size*T,t.c_str());};
 // window button: 0 neutral, 1 purple, 2 red; 16.392 text centred
 auto button=[&](ImVec2 lo,ImVec2 hi,const char* label,int style){const bool over=live&&inside(lo,hi);ImU32 fill,ink;
  if(style==2){fill=over?Rgba(90,30,36,opacity):Rgba(60,24,28,opacity);ink=Rgba(246,160,171,opacity);}else if(style==1){fill=over?AccHover(opacity):AccDeep(opacity);ink=Rgba(255,255,255,opacity);}
  else{fill=over?Rgba(35,35,44,opacity):Rgba(20,20,27,opacity);ink=Rgba(235,235,240,opacity);}
  d->AddRectFilled(lo,hi,fill,0);d->AddRect(lo,hi,Rgba(255,255,255,22/255.f*opacity),0,0,k);const float tw=width(semibold,16.392f,label);text(semibold,16.392f,ImVec2((lo.x+hi.x-tw)/2,(lo.y+hi.y-16.392f*k)/2),label,ink);
  return over&&ImGui::IsMouseClicked(0);};
 d->AddRectFilled(ImVec2(0,0),ImVec2(W,H),Rgba(0,0,0,110/255.f*opacity));
 const float ww=std::min(W*.86f,k*960),wh=H*.8f,x0=(W-ww)/2,y0=(H-wh)/2,x1=x0+ww,y1=y0+wh;
 d->AddRectFilled(ImVec2(x0,y0),ImVec2(x1,y1),Rgba(16,15,22,238/255.f*opacity),0);d->AddRect(ImVec2(x0,y0),ImVec2(x1,y1),Rgba(255,255,255,18/255.f*opacity),0,0,k);
 // waypoint list
 auto* list=EntriesSetting(m);auto items=Split(list?list->text:"");int current=0;const bool dimKnown=game::ReadDimension(&current);float feet[3];const bool feetKnown=Feet(feet);
 struct Row{int index;Entry e;bool same,hasDist;float dist;};std::vector<Row> rows;
 for(size_t i=0;i<items.size();++i){Entry e;if(!Parse(items[i],&e))continue;Row r{(int)i,e,e.dim<0||(dimKnown&&e.dim==current),false,0};
  if(r.same&&feetKnown){const float dx=e.x+.5f-feet[0],dy=e.y+.5f-feet[1],dz=e.z+.5f-feet[2];r.dist=std::sqrt(dx*dx+dy*dy+dz*dz);r.hasDist=true;}rows.push_back(r);}
 std::stable_sort(rows.begin(),rows.end(),[](const Row& a,const Row& b){if(a.same!=b.same)return a.same;return (a.hasDist?a.dist:0)<(b.hasDist?b.dist:0);});
 int same=0;for(auto& r:rows)same+=r.same;
 const std::string title="Waypoints";std::string sub;
 if(rows.empty())sub="Mark places you want to find again";else if(same==(int)rows.size())sub=std::to_string(same)+" "+(same==1?"waypoint":"waypoints");else sub=std::to_string(same)+" in this dimension, "+std::to_string(rows.size()-same)+" elsewhere";
 text(bold,23.222f,ImVec2(x0+20*k,y0+14*k),title,Rgba(255,255,255,opacity));text(regular,16.392f,ImVec2(x0+20*k,y0+14*k+23.222f*k+2*k),sub,Rgba(148,148,160,opacity));
 const float by0=y0+18*k,by1=by0+28*k;if(button(ImVec2(x1-48*k,by0),ImVec2(x1-20*k,by1),"\xc3\x97",0)){waypointsOpen=false;form=Form{};if(gameWindowForKeys)CloseMenu(gameWindowForKeys);return;}
 const ImVec2 clo(x0+16*k,y0+72*k),chi(x1-16*k,y1-16*k);
 if(!form.open){
  if(button(ImVec2(x1-48*k-8*k-132*k,by0),ImVec2(x1-48*k-8*k,by1),"New waypoint",1)){OpenForm(m,-1);return;}
  if(rows.empty()){text(regular,16.392f,ImVec2(clo.x+4*k,clo.y+4*k),"No waypoints yet. Press New waypoint to add one where you stand.",Rgba(148,148,160,opacity));return;}
  // rows of 56 u + 6 u, 48 u scroll per notch
  const float rh=56*k,gap=6*k;if(live&&inside(clo,chi)&&io.MouseWheel!=0)scroll-=io.MouseWheel*48*k;scroll=std::clamp(scroll,0.f,std::max(0.f,rows.size()*(rh+gap)-gap-(chi.y-clo.y)));
  d->PushClipRect(clo,chi,true);int act=-1,del=-1;
  for(size_t i=0;i<rows.size();++i){const Row& r=rows[i];const float top=clo.y+i*(rh+gap)-scroll,bottom=top+rh;if(bottom<clo.y||top>chi.y)continue;const float a=(r.same?1.f:.45f)*opacity,mid=(top+bottom)/2;
   const bool over=live&&inside(ImVec2(clo.x,std::max(top,clo.y)),ImVec2(chi.x,std::min(bottom,chi.y)));d->AddRectFilled(ImVec2(clo.x,top),ImVec2(chi.x,bottom),over?Rgba(26,26,33,230/255.f*opacity):Rgba(17,17,23,220/255.f*opacity),0);
   float tx=clo.x+16*k;unsigned c=r.e.hasColor?r.e.color:0;if(!r.e.hasColor){const ImVec4 v=ParseColor(Find(*m,"color")?Find(*m,"color")->text:"#59d9ffff");c=((unsigned)(v.x*255)<<16)|((unsigned)(v.y*255)<<8)|(unsigned)(v.z*255);}
   d->AddCircleFilled(ImVec2(tx+7*k,mid),7*k,Rgba((c>>16)&255,(c>>8)&255,c&255,a));d->AddCircle(ImVec2(tx+7*k,mid),7*k,Rgba(255,255,255,48/255.f*(r.same?1.f:.45f)*opacity),0,k);tx+=14*k+7*k+7*k;
   float right=chi.x-12*k;const char* labels[2]={"Delete","Edit"};const int styles[2]={2,0};
   for(int b=0;b<2;++b){const ImVec2 lo(right-76*k,mid-14*k),hi(right,mid+14*k);if(button(lo,hi,labels[b],styles[b])){if(b==0)del=r.index;else act=r.index;}right-=76*k+gap;}
   std::string distance;if(r.hasDist){char t[24];if(std::round(r.dist)<1000)snprintf(t,sizeof(t),"%.0fm",r.dist);else if(r.dist/1000>=9.95f)snprintf(t,sizeof(t),"%.0fkm",r.dist/1000);else snprintf(t,sizeof(t),"%.1fkm",r.dist/1000);distance=t;}
   if(!distance.empty())text(semibold,17.758f,ImVec2(right+gap-8*k-width(semibold,17.758f,distance),mid-17.758f*k/2),distance,Rgba(255,255,255,a));
   static const char* names[3]={"Overworld","Nether","End"};std::string detail=std::to_string(r.e.x)+", "+std::to_string(r.e.y)+", "+std::to_string(r.e.z);if(r.e.dim<0)detail+=", any dimension";else if(!r.same)detail+=std::string(", ")+names[r.e.dim];
   const float room=right+gap-88*k-tx;text(semibold,17.075f,ImVec2(tx,top+10*k),Fit(semibold,17.075f*T,r.e.name,room),Rgba(235,235,240,a));text(regular,15.026f,ImVec2(tx,top+31*k),Fit(regular,15.026f*T,detail,room),Rgba(148,148,160,a));}
  d->PopClipRect();
  if(del>=0&&list){items.erase(items.begin()+del);std::string joined;for(auto& item:items){if(!joined.empty())joined+=",";joined+=item;}list->text=joined;Save(*m,list);}
  if(act>=0)OpenForm(m,act);
  return;}
 // edit form
 const float fx=clo.x+4*k;float y=clo.y+4*k;text(bold,19.124f,ImVec2(fx,y),form.editing?"Edit waypoint":"New waypoint",Rgba(255,255,255,opacity));y+=34*k;
 const float rowW=std::min(560*k,chi.x-fx);static double caretTime=0;caretTime+=io.DeltaTime;
 auto field=[&](int i,const char* label,float x,float w){text(regular,15.026f,ImVec2(x,y),label,Rgba(148,148,160,opacity));const ImVec2 lo(x,y+15.026f*k+6*k),hi(x+w,y+15.026f*k+6*k+32*k);const bool focus=form.focus==i;
  d->AddRectFilled(lo,hi,Rgba(20,20,27,opacity),0);d->AddRect(lo,hi,focus?AccDeep(opacity):Rgba(255,255,255,26/255.f*opacity),0,0,k);
  std::string shown=form.field[i];if(focus&&std::fmod(caretTime,1.0)<.5)shown+="|";d->PushClipRect(lo,hi,true);text(regular,17.075f,ImVec2(lo.x+10*k,(lo.y+hi.y-17.075f*k)/2),shown,Rgba(255,255,255,opacity));d->PopClipRect();
  if(live&&ImGui::IsMouseClicked(0)&&inside(lo,hi)){form.focus=i;caretTime=0;}};
 field(0,"Name",fx,rowW);y+=15.026f*k+6*k+32*k+10*k;
 {const float w=(rowW-2*10*k)/3;field(1,"X",fx,w);field(2,"Y",fx+w+10*k,w);field(3,"Z",fx+2*(w+10*k),w);y+=15.026f*k+6*k+32*k+10*k;}
 // text input: characters and Backspace; Name up to 32 characters, X/Y/Z up to 9 numeric characters
 if(live&&form.focus>=0){std::string& t=form.field[form.focus];const size_t limit=form.focus==0?32:9;
  for(int i=0;i<io.InputQueueCharacters.Size;++i){const ImWchar ch=io.InputQueueCharacters[i];if(ch<32||ch>126)continue;if(form.focus>0&&!(std::isdigit(ch)||ch=='-'))continue;if(t.size()<limit)t+=(char)ch;}
  if(ImGui::IsKeyPressed(ImGuiKey_Backspace)&&!t.empty())t.pop_back();if(ImGui::IsKeyPressed(ImGuiKey_Tab))form.focus=(form.focus+1)%4;}
 text(regular,15.026f,ImVec2(fx,y),"Color",Rgba(148,148,160,opacity));y+=6*k+15.026f*k;
 for(size_t i=0;i<form.colors.size();++i){const ImVec2 c(fx+11*k+i*32*k,y+11*k);unsigned v=form.colors[i].first;if(!form.colors[i].second){const ImVec4 mc=ParseColor(Find(*m,"color")?Find(*m,"color")->text:"#59d9ffff");v=((unsigned)(mc.x*255)<<16)|((unsigned)(mc.y*255)<<8)|(unsigned)(mc.z*255);}
  if((int)i==form.chosen)d->AddCircle(c,14*k,Rgba(255,255,255,opacity),0,2*k);d->AddCircleFilled(c,11*k,Rgba((v>>16)&255,(v>>8)&255,v&255,opacity));d->AddCircle(c,11*k,Rgba(255,255,255,48/255.f*opacity),0,k);
  if(live&&ImGui::IsMouseClicked(0)&&std::hypot(mouse.x-c.x,mouse.y-c.y)<=4*k+11*k)form.chosen=(int)i;}
 y+=4*k+22*k+10*k;
 if(!form.error.empty()){text(regular,17.075f,ImVec2(fx,y),form.error,Rgba(246,160,171,opacity));y+=10*k+17.075f*k;}
 float bx=fx;const bool enter=live&&(ImGui::IsKeyPressed(ImGuiKey_Enter)||ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
 if(button(ImVec2(bx,y),ImVec2(bx+140*k,y+28*k),"Save",1)||enter){form.error.clear();if(SaveForm(m))form=Form{};return;}bx+=140*k+8*k;
 if(button(ImVec2(bx,y),ImVec2(bx+140*k,y+28*k),"Use my position",0)){form.error.clear();float f[3];if(Feet(f)){form.field[1]=std::to_string((int)std::floor(f[0]));form.field[2]=std::to_string((int)std::floor(f[1]));form.field[3]=std::to_string((int)std::floor(f[2]));}else form.error="Your position is not available";}bx+=140*k+8*k;
 if(button(ImVec2(bx,y),ImVec2(bx+140*k,y+28*k),"Cancel",0))form=Form{};
}
