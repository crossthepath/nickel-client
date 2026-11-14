// Schematics tab: "Search schematics", "Folder" button, "Saving '{}' ({:.0f}%)" banner, rows (name, size
// "{:.1f} MB" / "{} KB", "{}, loaded", Load / Unload), empty states. Side panel: unloaded file (format, size, Load) or
// loaded schematic (missing-type warning, POSITION with X-/X+/Y-/Y+/Z-/Z+ and Move to me, ROTATION, LAYERS, DISPLAY,
// Material list, Unload). Layout reuses the Scripting tab components.
std::vector<std::string> schematicLower;   // lowered names of sh.files, rebuilt with each rescan
char schematicSearch[64]{};std::string schematicPicked;bool schematicMaterials=false;
inline std::string ScSizeText(uint64_t bytes){char t[32];if(bytes>=1048576)snprintf(t,sizeof(t),"%.1f MB",bytes/1048576.0);else snprintf(t,sizeof(t),"%llu KB",(unsigned long long)((bytes+1023)>>10));return t;}
inline const char* ScFormatName(const std::string& ext){return ext=="mcstructure"?"Bedrock structure":ext=="litematic"?"Litematic":ext=="schem"?"Sponge schematic":"Java structure";}
void DrawSchematics(ImVec2 origin,float s,float opacity){
 const float U=242.f/210.f,left=257,right=menuW-22,top=72;auto& io=ImGui::GetIO();auto* d=ImGui::GetWindowDrawList();auto P=[&](float x,float y){return ImVec2(origin.x+x*s,origin.y+y*s);};
 auto& sh=game::Schematics();if(ImGui::GetTime()-sh.filesAt>1.0||sh.filesAt==0){sh.filesAt=ImGui::GetTime();auto files=game::ScListFiles();std::lock_guard<std::recursive_mutex> l(sh.guard);sh.files=std::move(files);schematicLower.clear();schematicLower.reserve(sh.files.size());for(auto& f:sh.files)schematicLower.push_back(game::ScLower(f.name));}
 const float areaW=right-left;float listTop=top+42*U;
 UiInput("##schematic-search","Search schematics",schematicSearch,sizeof(schematicSearch),P(left,top),areaW-152*U,32*U,34*U,s,opacity);
 // Import: a .schem or .mcstructure file copied into the schematics folder.
 if(UiButton(d,"schematic-import","Import",P(right-72*U-8*U-64*U,top),64*U,32*U,0,true,opacity,s))
  filedialog::Ask(gameWindowForKeys,false,filedialog::Filter({L"Schematic (*.schem;*.mcstructure)",L"*.schem;*.mcstructure"}),L"Import a schematic",L"",L"",[](const std::wstring& path){
   const std::wstring ext=filedialog::Extension(path);if(ext!=L".schem"&&ext!=L".mcstructure"){game::ScSay("Only .schem and .mcstructure files can be imported.");return;}
   const std::wstring dir=game::ScFolder();if(dir.empty()){game::ScSay("Couldn't open the schematics folder.");return;}std::error_code ec;std::filesystem::create_directories(dir,ec);
   const std::wstring target=filedialog::FreeName(dir,filedialog::Stem(path),ext);
   if(CopyFileW(path.c_str(),target.c_str(),TRUE)){const std::string t="Imported "+Utf8(filedialog::Stem(target))+".";game::ScSay(t);if(notices.size()<16)notices.push_back({8,"Schematics",t});}else game::ScSay("The schematic could not be imported.");});
 d->AddCircle(P(left+17*U-1.2f,top+16*U-1.2f),5*s,Rgba(120,120,132,opacity),0,1.5f*s);d->AddLine(P(left+17*U+2.5f,top+16*U+2.5f),P(left+17*U+6.3f,top+16*U+6.3f),Rgba(120,120,132,opacity),1.5f*s);
 if(UiButton(d,"schematic-folder","Folder",P(right-72*U,top),72*U,32*U,1,true,opacity,s))game::ScOpenFolder();
 std::lock_guard<std::recursive_mutex> lock(sh.guard);
 if(sh.cap.active){char t[160];snprintf(t,sizeof(t),"Saving '%s' (%.0f%%)",sh.cap.name.c_str(),sh.capProgress*100);Label(d,regular,12.5f*U*s,P(left,listTop),t,Acc(opacity));listTop+=24*U;}
 std::string q=game::ScLower(schematicSearch);std::vector<const game::ScFile*> shown;for(size_t i=0;i<sh.files.size();++i)if(q.empty()||schematicLower[i].find(q)!=std::string::npos)shown.push_back(&sh.files[i]);
 std::vector<std::string> loadedLower;loadedLower.reserve(sh.list.size());for(auto& sc:sh.list)loadedLower.push_back(game::ScLower(sc->name));
 auto message=[&](const std::string& text,float y){Label(d,regular,12.5f*U*s,P(left+(areaW-TextWidth(regular,12.5f*U,text.c_str()))/2,y+24*U),text.c_str(),Rgba(120,120,132,opacity));};
 if(sh.files.empty()){message("No schematics yet. Save one with .schem create <name> <x y z> <x y z>,",listTop);message("or put .mcstructure, .litematic, .schem or .nbt files in the folder.",listTop+20*U);}
 else if(shown.empty())message("No schematics match \""+std::string(schematicSearch)+"\".",listTop);
 ImGui::SetCursorScreenPos(P(left,listTop));ImGui::BeginChild("##SchematicList",ImVec2(areaW*s,(menuH-20-listTop)*s),false,ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoScrollbar);
 auto* list=ImGui::GetWindowDrawList();const ImVec2 base=ImGui::GetCursorScreenPos();
 for(int row=0;row<(int)shown.size();++row){const auto& f=*shown[row];ImGui::PushID(f.name.c_str());const float y=row*62*U;auto R=[&](float x,float dy){return ImVec2(base.x+x*s,base.y+(y+dy)*s);};
  const ImVec2 lo=R(0,0),hi=R(areaW,56*U);const bool chosen=schematicPicked==f.name;const float hover=Animate("schem-hover"+f.name,opened&&ImGui::IsMouseHoveringRect(lo,hi)?1.f:0.f,io.DeltaTime),glow=std::max(hover*.8f,chosen?1.f:0.f);
  list->AddRectFilled(lo,hi,Rgba(12+(int)(16*glow),11+(int)(15*glow),16+(int)(22*glow),(.55f+.2f*glow)*opacity),0);list->AddRect(lo,hi,ToU32(MixColor(ImVec4(1,1,1,1),themeLive.accent,chosen),(.05f+.5f*chosen)*opacity),0,0,U*s);
  int loadedId=0;const std::string fl=game::ScLower(f.name);for(size_t i=0;i<sh.list.size();++i)if(loadedLower[i]==fl)loadedId=sh.list[i]->id;bool handled=false;
  if(UiButton(list,"schematic-row",loadedId?"Unload":"Load",R(areaW-10*U-62*U,15*U),62*U,26*U,loadedId?0:1,true,opacity,s)){if(loadedId)game::ScUnload(loadedId);else game::ScLoad(f.name);schematicPicked=f.name;handled=true;}
  Label(list,bold,14*U*s,R(14*U,9*U+19.124f*U*.5f-.7f*14*U),Fit(bold,14*U,f.name,areaW-110*U).c_str(),Rgba(255,255,255,opacity));
  const std::string sub=loadedId?ScSizeText(f.size)+", loaded":ScSizeText(f.size);Label(list,regular,11.5f*U*s,R(14*U,9*U+19.124f*U+2*U+15.709f*U*.5f-.7f*11.5f*U),sub.c_str(),loadedId?Rgba(159,232,191,opacity):Rgba(148,148,160,opacity));
  if(!handled&&Hit("##schem-row",lo,ImVec2(areaW*s,56*U*s)))schematicPicked=f.name;ImGui::PopID();}
 ImGui::SetCursorPosY(shown.size()*62*U*s);ImGui::Dummy(ImVec2(1,1));FadeEdges(s,opacity);ImGui::EndChild();
}
void DrawSchematicPanel(ImVec2 origin,float s,float opacity){
 const float U=242.f/210.f,x0=16,width=297-32;auto P=[&](float x,float y){return ImVec2(origin.x+x*s,origin.y+y*s);};
 ImGui::SetNextWindowPos(origin);ImGui::SetNextWindowSize(ImVec2(297*s,menuH*s));ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));ImGui::PushStyleVar(ImGuiStyleVar_Alpha,opacity);
 ImGui::Begin("##NickelSchematicPanel",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBackground|(opened.load()?0:ImGuiWindowFlags_NoInputs));
 auto* d=ImGui::GetWindowDrawList();d->AddRectFilled(origin,P(297,menuH),Rgba(13,13,15,.9f*opacity),0);d->AddRect(origin,P(297,menuH),Rgba(60,60,72,.5f*opacity),0);WindowFont(s*15.f*FontEmFactor);
 const float a=opacity;float y=16;auto take=[&](float height,float gap){const float at=y;y+=(height+gap)*U;return at;};
 auto section=[&](const char* t){const float at=take(12,6);Label(d,bold,10.5f*U*s,P(x0,at),t,Rgba(120,120,132,a));};
 auto& sh=game::Schematics();std::lock_guard<std::recursive_mutex> lock(sh.guard);const game::ScFile* file=nullptr;for(auto& f:sh.files)if(f.name==schematicPicked)file=&f;
 game::ScSchem* sc=nullptr;for(auto& x:sh.list)if(game::ScLower(x->name)==game::ScLower(schematicPicked))sc=x.get();
 if(!file&&!sc){const float at=take(20,10);Label(d,regular,12.5f*U*s,P(x0,at+10*U-.7f*12.5f*U),"Select a schematic to see its details.",Rgba(148,148,160,a));EndWindow();ImGui::PopStyleVar(2);return;}
 {const float at=take(22,4);Label(d,bold,17*U*s,P(x0,at+11*U-.7f*17*U),Fit(bold,17*U,schematicPicked,width).c_str(),Rgba(255,255,255,a));}
 if(!sc){const float at=take(16,10);const std::string t=std::string(ScFormatName(file->ext))+", "+ScSizeText(file->size);Label(d,regular,12*U*s,P(x0,at+8*U-.7f*12*U),t.c_str(),Rgba(148,148,160,a));
  const float b=take(32,10);if(UiButton(d,"schematic-load-selected","Load",P(x0,b),width,32*U,1,true,a,s))game::ScLoad(schematicPicked);EndWindow();ImGui::PopStyleVar(2);return;}
 sh.selected=sc->id;int w,h,dd;game::ScDims(*sc,&w,&h,&dd);
 {const float at=take(16,6);char t[64];snprintf(t,sizeof(t),"%d \xc3\x97 %d \xc3\x97 %d",w,h,dd);Label(d,regular,12*U*s,P(x0,at+8*U-.7f*12*U),t,Rgba(148,148,160,a));}
 if(sc->unresolvedTypes){const float at=take(16,6);const std::string t=std::to_string(sc->unresolvedTypes)+" block types aren't in this version";Label(d,regular,12*U*s,P(x0,at+8*U-.7f*12*U),t.c_str(),Rgba(246,160,171,a));}
 const int id=sc->id;
 section("POSITION");{const float at=take(16,6);char t[96];snprintf(t,sizeof(t),"X %d   Y %d   Z %d",sc->px,sc->py,sc->pz);Label(d,regular,12.5f*U*s,P(x0,at),t,Rgba(224,224,230,a));}
 {const float at=take(28,6);static const char* labels[6]={"X-","Y-","Z-","X+","Y+","Z+"};const float bw=(width-5*4*U)/6;
  for(int i=0;i<6;++i)if(UiButton(d,std::string("schem-nudge")+labels[i],labels[i],P(x0+i*(bw+4*U),at),bw,28*U,3,true,a,s)){const int axis=i%3,step=i<3?-1:1;game::ScEdit(id,[axis,step](const game::Context&,game::ScSchem& x){(axis==0?x.px:axis==1?x.py:x.pz)+=step;x.bucketsReady=false;});}}
 {const float at=take(28,10);if(UiButton(d,"schem-move-me","Move to me",P(x0,at),width,28*U,3,true,a,s))game::ScEdit(id,[](const game::Context& c,game::ScSchem& x){uint64_t st{};float f[3]{},lift=0;game::Read(c.actor+0x240,&st);if(st)game::Floats(st,f,3);uintptr_t off{};if(game::Element(c.registry,c.entity,"OffsetsComponent",0x44,&off))game::Floats(off,&lift,1);x.px=(int)std::floor(f[0]);x.py=(int)std::floor(f[1]-lift);x.pz=(int)std::floor(f[2]);x.bucketsReady=false;});}
 section("ROTATION");{const float at=take(16,6);std::string t=sc->rot==0?"Not rotated":("Rotated "+std::to_string(sc->rot*90)+"\xc2\xb0");if(sc->mx||sc->mz)t+=", mirrored";Label(d,regular,12.5f*U*s,P(x0,at),t.c_str(),Rgba(224,224,230,a));}
 {const float at=take(28,10);const float bw=(width-3*4*U)/4;
  if(UiButton(d,"schem-rot-left","Rotate left",P(x0,at),bw,28*U,3,true,a,s))game::ScTransform(id,-1,false,false);if(UiButton(d,"schem-rot-right","Rotate right",P(x0+bw+4*U,at),bw,28*U,3,true,a,s))game::ScTransform(id,1,false,false);
  if(UiButton(d,"schem-mirror-x","Mirror X",P(x0+2*(bw+4*U),at),bw,28*U,sc->mx?0:3,true,a,s))game::ScTransform(id,0,true,false);if(UiButton(d,"schem-mirror-z","Mirror Z",P(x0+3*(bw+4*U),at),bw,28*U,sc->mz?0:3,true,a,s))game::ScTransform(id,0,false,true);}
 section("LAYERS");{const float at=take(28,6);static const char* modes[3]={"All","Only one","Up to"};const float bw=(width-2*4*U)/3;
  for(int i=0;i<3;++i)if(UiButton(d,std::string("schem-layer-mode")+modes[i],modes[i],P(x0+i*(bw+4*U),at),bw,28*U,sc->layerMode==i?0:3,true,a,s))game::ScEdit(id,[i](const game::Context&,game::ScSchem& x){x.layerMode=i;});}
 if(sc->layerMode){const float at=take(28,10);char t[48];snprintf(t,sizeof(t),"Layer %d of %d",sc->layer+1,sc->data.sy);Label(d,regular,12.5f*U*s,P(x0,at+7*U),t,Rgba(224,224,230,a));
  if(UiButton(d,"schem-layer-down","-",P(x0+width-64*U,at),30*U,28*U,3,sc->layer>0,a,s))game::ScEdit(id,[](const game::Context&,game::ScSchem& x){x.layer=std::max(0,x.layer-1);});
  if(UiButton(d,"schem-layer-up","+",P(x0+width-30*U,at),30*U,28*U,3,sc->layer<sc->data.sy-1,a,s))game::ScEdit(id,[](const game::Context&,game::ScSchem& x){x.layer=std::min(x.data.sy-1,x.layer+1);});}
 section("DISPLAY");{const float at=take(28,10);if(UiButton(d,"schematic-show",sc->show?"Show ghost: On":"Show ghost: Off",P(x0,at),width,28*U,sc->show?0:3,true,a,s))game::ScEdit(id,[](const game::Context&,game::ScSchem& x){x.show=!x.show;});}
 {const float at=take(32,6);const float half=(width-6*U)/2;if(UiButton(d,"schematic-material-list","Material list",P(x0,at),half,32*U,1,true,a,s)){schematicMaterials=!schematicMaterials;sh.wantMaterials=schematicMaterials;}
  if(UiButton(d,"schematic-unload","Unload",P(x0+half+6*U,at),half,32*U,3,true,a,s))game::ScUnload(id);}
 if(schematicMaterials){// Material list window: "{} needed, {} in inventory", "{} block types, {} blocks left to place"
  sh.wantMaterials=true;sh.materialsFor=id;const auto& needed=sh.materialNeeded;auto have=sh.materialHave;const size_t left=sh.materialLeft;
  {const float at=take(16,6);const std::string t=std::to_string(needed.size())+" block types, "+std::to_string(left)+" blocks left to place";Label(d,bold,11.5f*U*s,P(x0,at),t.c_str(),Rgba(224,224,230,a));}
  for(auto& [nm,count]:needed){if(y>menuH-24)break;const float at=take(16,2);std::string shownName=nm.compare(0,10,"minecraft:")==0?nm.substr(10):nm;Label(d,regular,11.5f*U*s,P(x0,at),Fit(regular,11.5f*U,shownName,width*.5f).c_str(),Rgba(224,224,230,a));
   const std::string t=std::to_string(count)+" needed, "+std::to_string(have[nm])+" in inventory";Label(d,regular,11*U*s,P(x0+width-TextWidth(regular,11*U,t.c_str()),at),t.c_str(),have[nm]>=count?Rgba(159,232,191,a):Rgba(148,148,160,a));}}
 EndWindow();ImGui::PopStyleVar(2);
}
