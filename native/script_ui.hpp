// Scripting tab: lists the scripts in the folder with their state (Running, Stopped, Stopped after errors,
// Failed to load), a Start / Stop button per row, name search and "Open folder"; the right side shows the
// selected script's details (registered modules, memory, frame time, Reload / Stop, latest errors with
// their line). Reuses the Configs screen components; included by menu.cpp after DrawConfigPanel.
void DrawScripting(ImVec2 origin,float s,float opacity){
 const float U=242.f/210.f,left=257,right=menuW-22,top=72;auto& io=ImGui::GetIO();auto* d=ImGui::GetWindowDrawList();auto P=[&](float x,float y){return ImVec2(origin.x+x*s,origin.y+y*s);};
 static float age=0;static int lastFrame=-2;if(ImGui::GetFrameCount()!=lastFrame+1||opacity<.05f)age=0;lastFrame=ImGui::GetFrameCount();age+=io.DeltaTime;
 const float listTop=top+42*U,areaW=right-left;
 auto message=[&](const std::string& text,float y){const float rest=1-std::clamp(age/.22f,0.f,1.f);Label(d,regular,12.5f*U*s,P(left+(areaW-TextWidth(regular,12.5f*U,text.c_str()))/2,y+24*U-.7f*12.5f*U),text.c_str(),Rgba(120,120,132,(1-rest*rest*rest)*opacity));};
 UiInput("##script-search","Search scripts",scriptSearch,sizeof(scriptSearch),P(left,top),areaW-246*U,32*U,34*U,s,opacity);
 // Import: a .luau file copied into the scripts folder (it starts at the next folder scan); Export: the selected script.
 if(UiButton(d,"import-script","Import",P(right-94*U-8*U-64*U-8*U-64*U,top),64*U,32*U,0,true,opacity,s))
  filedialog::Ask(gameWindowForKeys,false,filedialog::Filter({L"Luau script (*.luau;*.lua)",L"*.luau;*.lua"}),L"Import a script",L"",L"luau",[](const std::wstring& path){
   const std::wstring folder=ScriptFolder();CreateDirectoryW(folder.substr(0,folder.find_last_of(L'\\')).c_str(),nullptr);CreateDirectoryW(folder.c_str(),nullptr);
   const std::wstring target=filedialog::FreeName(folder,filedialog::Stem(path),L".luau");
   if(CopyFileW(path.c_str(),target.c_str(),TRUE))notices.push_back({8,"Scripting","Imported "+Utf8(filedialog::Stem(target))});else notices.push_back({8,"Scripting","The script could not be imported"});});
 {const bool any=scriptSelected>=0&&scriptSelected<(int)scripts.size();
  if(UiButton(d,"export-script","Export",P(right-94*U-8*U-64*U,top),64*U,32*U,0,any,opacity,s)&&any){const std::wstring file=scripts[scriptSelected]->file;const std::string name=scripts[scriptSelected]->name;
   filedialog::Ask(gameWindowForKeys,true,filedialog::Filter({L"Luau script (*.luau)",L"*.luau"}),L"Export a script",Wide(name)+L".luau",L"luau",[file,name](const std::wstring& path){
    if(CopyFileW(file.c_str(),path.c_str(),FALSE))notices.push_back({8,"Scripting","Exported "+name});else notices.push_back({8,"Scripting","The script could not be exported"});});}}
 if(!DrawGlyph(d,nicons::k_search,P(left+17*U-6.5f,top+16*U-6.5f),15*s,Rgba(120,120,132,opacity))){d->AddCircle(P(left+17*U-1.2f,top+16*U-1.2f),5*s,Rgba(120,120,132,opacity),0,1.5f*s);d->AddLine(P(left+17*U+2.5f,top+16*U+2.5f),P(left+17*U+6.3f,top+16*U+6.3f),Rgba(120,120,132,opacity),1.5f*s);}
 if(UiButton(d,"open-scripts","Open folder",P(right-94*U,top),94*U,32*U,1,true,opacity,s)){const std::wstring folder=ScriptFolder();CreateDirectoryW(folder.substr(0,folder.find_last_of(L'\\')).c_str(),nullptr);CreateDirectoryW(folder.c_str(),nullptr);ShellExecuteW(nullptr,L"open",folder.c_str(),nullptr,nullptr,SW_SHOWNORMAL);}
 std::lock_guard<std::recursive_mutex> lock(scriptGuard);
 std::string query=scriptSearch;std::transform(query.begin(),query.end(),query.begin(),[](unsigned char c){return (char)std::tolower(c);});std::vector<int> shown;
 for(int i=0;i<(int)scripts.size();++i){std::string low=scripts[i]->name;std::transform(low.begin(),low.end(),low.begin(),[](unsigned char c){return (char)std::tolower(c);});if(query.empty()||low.find(query)!=std::string::npos)shown.push_back(i);}
 if(scripts.empty())message("No scripts yet. Put .luau files in the scripts folder.",listTop);else if(shown.empty())message("No scripts match \""+std::string(scriptSearch)+"\".",listTop);
 ImGui::SetCursorScreenPos(P(left,listTop));ImGui::BeginChild("##ScriptList",ImVec2(areaW*s,(menuH-20-listTop)*s),false,ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoScrollbar);
 auto* list=ImGui::GetWindowDrawList();const ImVec2 base=ImGui::GetCursorScreenPos();
 for(int row=0;row<(int)shown.size();++row){Script& sc=*scripts[shown[row]];ImGui::PushID(sc.name.c_str());
  const float rest=1-std::clamp((age-.03f*std::min(row,12))/.22f,0.f,1.f),e=1-rest*rest*rest,a=opacity*e,y=row*62*U+(1-e)*10*U;auto R=[&](float x,float dy){return ImVec2(base.x+x*s,base.y+(y+dy)*s);};
  const ImVec2 lo=R(0,0),hi=R(areaW,56*U);const bool chosen=scriptSelected==shown[row];const float hover=Animate("script-hover"+sc.name,opened&&ImGui::IsMouseHoveringRect(lo,hi)?1.f:0.f,io.DeltaTime),picked=Animate("script-selected"+sc.name,chosen?1.f:0.f,io.DeltaTime),glow=std::max(hover*.8f,picked);
  list->AddRectFilled(lo,hi,Rgba(12+(int)(16*glow),11+(int)(15*glow),16+(int)(22*glow),(.55f+.2f*glow)*a),0);list->AddRect(lo,hi,ToU32(MixColor(ImVec4(1,1,1,1),themeLive.accent,picked),(.05f+.5f*picked)*a),0,0,U*s);
  bool handled=false;const bool running=sc.state==1;
  if(UiButton(list,"script-toggle",running?"Stop":"Start",R(areaW-10*U-56*U,15*U),56*U,26*U,running?0:1,true,a,s)){if(running)StopScript(sc);else StartScript(sc);handled=true;}
  Label(list,bold,14*U*s,R(14*U,9*U+19.124f*U*.5f-.7f*14*U),Fit(bold,14*U,sc.name,areaW-100*U).c_str(),Rgba(255,255,255,a));
  const ImU32 tone=sc.state==1?Rgba(159,232,191,a):sc.state==0?Rgba(148,148,160,a):Rgba(246,160,171,a);
  Label(list,regular,11.5f*U*s,R(14*U,9*U+19.124f*U+2*U+15.709f*U*.5f-.7f*11.5f*U),ScriptStateName(sc.state),tone);
  if(!handled&&Hit("##script-row",lo,ImVec2(areaW*s,56*U*s)))scriptSelected=shown[row];
  ImGui::PopID();}
 ImGui::SetCursorPosY(shown.size()*62*U*s);ImGui::Dummy(ImVec2(1,1));FadeEdges(s,opacity);ImGui::EndChild();
}
void DrawScriptPanel(ImVec2 origin,float s,float opacity){
 const float U=242.f/210.f,x0=16,width=297-32;auto& io=ImGui::GetIO();auto P=[&](float x,float y){return ImVec2(origin.x+x*s,origin.y+y*s);};
 ImGui::SetNextWindowPos(origin);ImGui::SetNextWindowSize(ImVec2(297*s,menuH*s));ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));ImGui::PushStyleVar(ImGuiStyleVar_Alpha,opacity);
 ImGui::Begin("##NickelScriptPanel",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBackground|(opened.load()?0:ImGuiWindowFlags_NoInputs));
 auto* d=ImGui::GetWindowDrawList();d->AddRectFilled(origin,P(297,menuH),Rgba(13,13,15,.9f*opacity),0);d->AddRect(origin,P(297,menuH),Rgba(60,60,72,.5f*opacity),0);WindowFont(s*15.f*FontEmFactor);
 static int shownFor=-2;static float since=0;if(shownFor!=scriptSelected||opacity<.05f){shownFor=scriptSelected;since=0;}since+=io.DeltaTime;const float rest=1-std::clamp(since/.2f,0.f,1.f),e=1-rest*rest*rest,a=opacity*e;
 float y=16+(1-e)*6*U;auto take=[&](float height,float gap){const float at=y;y+=(height+gap)*U;return at;};
 std::lock_guard<std::recursive_mutex> lock(scriptGuard);
 if(scriptSelected<0||scriptSelected>=(int)scripts.size()){const float at=take(20,10);Label(d,regular,12.5f*U*s,P(x0,at+10*U-.7f*12.5f*U),"Select a script to see its details.",Rgba(148,148,160,a));EndWindow();ImGui::PopStyleVar(2);return;}
 Script& sc=*scripts[scriptSelected];
 {const float at=take(22,4);Label(d,bold,17*U*s,P(x0,at+11*U-.7f*17*U),Fit(bold,17*U,sc.name,width).c_str(),Rgba(255,255,255,a));}
 {const float at=take(16,10);const ImU32 tone=sc.state==1?Rgba(159,232,191,a):sc.state==0?Rgba(148,148,160,a):Rgba(246,160,171,a);Label(d,regular,12*U*s,P(x0,at+8*U-.7f*12*U),ScriptStateName(sc.state),tone);}
 {const float at=take(1,10);d->AddLine(P(x0,at),P(x0+width,at),Rgba(255,255,255,.07f*a),U*s);}
 // three tiles: modules, memory, frame time
 {const float at=take(52,10);char memory[32],frame[32];snprintf(memory,sizeof(memory),"%.1f MB",sc.memory/1048576.0);snprintf(frame,sizeof(frame),"%.2f ms",sc.frameMs);const std::string count=std::to_string(sc.mods.size());
  const char* terms[3]={"MODULES","MEMORY","FRAME TIME"};const std::string values[3]={count,memory,frame};const float w=(width-2*6*U)/3;
  for(int i=0;i<3;++i){const float x=x0+i*(w+6*U);d->AddRectFilled(P(x,at),P(x+w,at+52*U),Rgba(0,0,0,.22f*a),0);Label(d,bold,9.5f*U*s,P(x+9*U,at+9*U),terms[i],Rgba(120,120,132,a));Label(d,bold,14*U*s,P(x+9*U,at+27*U),values[i].c_str(),Rgba(230,230,235,a));}}
 {const float at=take(32,10);const float half=(width-6*U)/2;if(UiButton(d,"script-reload","Reload",P(x0,at),half,32*U,1,true,a,s))StartScript(sc);
  if(UiButton(d,"script-stop","Stop",P(x0+half+6*U,at),half,32*U,3,sc.state==1,a,s))StopScript(sc);}
 if(!sc.mods.empty()){const float at=take(12,6);Label(d,bold,10.5f*U*s,P(x0,at),"MODULES",Rgba(120,120,132,a));
  for(auto& m:sc.mods){const float row=take(16,4);const bool on=m.index>=0&&modules[m.index].enabled;Label(d,regular,12.5f*U*s,P(x0,row),Fit(regular,12.5f*U,m.name,width-60*U).c_str(),Rgba(224,224,230,a));
   const char* tag=on?"On":"Off";Label(d,bold,11*U*s,P(x0+width-TextWidth(bold,11*U,tag),row+1*U),tag,on?Acc(a):Rgba(120,120,132,a));}y+=6*U;}
 if(!sc.errors.empty()){const float at=take(12,6);Label(d,bold,10.5f*U*s,P(x0,at),"ERRORS",Rgba(246,160,171,a));
  ImGui::SetCursorScreenPos(P(x0,y));ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+width*s);ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.96f,.63f,.67f,a));WindowFont(s*12.f*FontEmFactor);
  for(auto it=sc.errors.rbegin();it!=sc.errors.rend();++it)ImGui::TextWrapped("%s",it->c_str());ImGui::PopStyleColor();ImGui::PopTextWrapPos();}
 EndWindow();ImGui::PopStyleVar(2);
}
