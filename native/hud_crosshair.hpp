// Custom Crosshair. Included by menu.cpp inside its anonymous namespace (after Place, Label, Find, Flag:
// no #include here).
// - The native "cursor" control is no longer drawn and its render time is recorded (native controls hook,
// game_controls.hpp).
// - GameUpdate: UI scale (GuiData +0x40 [0]/[4], else 1), third person, movement = min(1, horizontal speed / 0.28),
// player aiming at an entity.
// - Attack: time of the last attack.
// - Drawing: nothing if the native crosshair has not been rendered for 200 ms, or in third person with Hide in Third
// Person; attack pulse = 1 - (t / 250 ms); animated gap -> (movement x Movement Spread + pulse x Attack Spread) with
// Dynamic Gap, smoothing 1 - exp(-18 dt); scale = UI scale if Scale with GUI;
// thickness t = max(1, round(Thickness x s)), size = max(1, round(Size x s)), gap = round((Gap + animated) x s),
// outline = max(1, round(Outline Thickness x s)) if Outline; centre = floor(screen / 2) - floor(t / 2).
// Shapes: Cross / T = 4 / 3 rectangles (top arm removed for T) + Center Dot; Dot = disc of radius Size / 2;
// Circle = ring of radius Size + gap, stroke t (outline: stroke 2 x outline + t underneath);
// Custom = 17 x 17 grid from the "Pattern" setting (hash 0x9c993a0a), cells of max(1, round(Pixel Size x s)),
// origin = centre - round(cell / 2) - 8 cells. Rectangles: outline first (grown by the outline thickness), then fill.
// Colour: Target Color if Target Highlight is on and an entity is targeted, otherwise Color.
// Drawn with ImGui over the game frame.
struct CrosshairState{float gap=0;double lastFrame=0;};CrosshairState crosshairState;
void DrawCrosshair(){
 static Module* mod=ModuleById("custom_crosshair");auto& slots=game::Controls();
 const bool on=mod&&mod->enabled;{AcquireSRWLockExclusive(&slots.lock);auto& s=slots.slots[5];s.enabled=on;s.hide=on;ReleaseSRWLockExclusive(&slots.lock);}
 if(!on)return;game::InstallControlHook(WriteLog);game::InstallEvents(WriteLog);
 auto num=[&](const char* id,float fallback){auto st=Find(*mod,id);return st?(float)st->number:fallback;};
 auto color=[&](const char* id,const char* fallback){auto st=Find(*mod,id);return ImGui::ColorConvertFloat4ToU32(ParseColor(st&&!st->text.empty()?st->text:std::string(fallback)));};
 // Native crosshair rendered within the last 200 ms (screens, pause menu, ...: nothing is drawn otherwise)
 const uint64_t seen=game::ControlSeenTick(5);if(!seen||GetTickCount64()-seen>200)return;
 int perspective=0;uint64_t options{};if(Flag(*mod,"hide_in_third_person",true)&&game::FreeLookPerspective(&perspective,&options)&&perspective!=0)return;
 game::DebugInfo info{};const bool gui=game::ReadDebug(&info,false)&&info.gui&&info.guiScale>0;const float scale=Flag(*mod,"scale_with_gui",true)&&gui?info.guiScale:1.f;
 float movement=0;{game::Player p{};if(game::ReadPlayer(&p))movement=std::fmin(1.f,std::sqrt(p.velocity[0]*p.velocity[0]+p.velocity[2]*p.velocity[2])/.28f);}
 float pulse=0;{uint64_t at=0;{std::lock_guard<std::mutex> lock(game::Shared().guard);at=game::Shared().attackTick;}if(at){const double t=(double)(GetTickCount64()-at)/250.0;if(t<=1.0)pulse=1-(float)std::fmax(0.0,t);}}
 const float target=Flag(*mod,"dynamic_gap",false)?movement*num("movement_spread",3)+pulse*num("attack_spread",3):0.f;
 const double now=ImGui::GetTime();const float dt=crosshairState.lastFrame>0?(float)(now-crosshairState.lastFrame):0.f;crosshairState.lastFrame=now;const float k=1-std::exp(dt*-18.f);crosshairState.gap=target*k+(1-k)*crosshairState.gap;
 game::Target aimed{};const bool targeting=Flag(*mod,"target_highlight",true)&&game::ReadCrosshairTarget(&aimed);
 const ImU32 fill=targeting?color("target_color","#ff4040ff"):color("color","#ffffffff");const bool outline=Flag(*mod,"outline",true);const ImU32 edge=color("outline_color","#000000cc");
 const float ot=outline?std::fmax(1.f,std::round(num("outline_thickness",.5f)*scale)):0.f,t=std::fmax(1.f,std::round(num("thickness",1)*scale)),size=std::fmax(1.f,std::round(num("size",5)*scale)),gap=std::round((num("gap",2)+crosshairState.gap)*scale);
 const ImVec2 screen=ImGui::GetIO().DisplaySize;const float half=std::floor(t*.5f),x0=std::floor(screen.x*.5f)-half,y0=std::floor(screen.y*.5f)-half,x1=x0+t,y1=y0+t;
 auto* d=ImGui::GetBackgroundDrawList();std::vector<ImVec4> rects;
 auto rectangles=[&]{if(ot>0)for(auto& r:rects)d->AddRectFilled(ImVec2(r.x-ot,r.y-ot),ImVec2(r.z+ot,r.w+ot),edge);for(auto& r:rects)d->AddRectFilled(ImVec2(r.x,r.y),ImVec2(r.z,r.w),fill);};
 std::string shape="Cross";if(auto st=Find(*mod,"shape"))shape=st->text;const ImVec2 middle((x0+x1)*.5f,(y0+y1)*.5f);
 if(shape=="Cross"||shape=="T"){
  rects.push_back(ImVec4(x1+gap,y0,x1+gap+size,y1));rects.push_back(ImVec4(x0-gap-size,y0,x0-gap,y1));rects.push_back(ImVec4(x0,y1+gap,x1,y1+gap+size));
  if(shape!="T")rects.push_back(ImVec4(x0,y0-gap-size,x1,y0-gap));if(Flag(*mod,"center_dot",false))rects.push_back(ImVec4(x0,y0,x1,y1));rectangles();}
 else if(shape=="Dot"){const float r=std::fmax(size*.5f,.5f);d->AddCircleFilled(middle,r,fill,32);if(ot>0)d->AddCircle(middle,r,edge,32,ot);}
 else if(shape=="Circle"){const float r=size+gap;if(ot>0)d->AddCircle(middle,r,edge,48,ot*2+t);d->AddCircle(middle,r,fill,48,t);if(Flag(*mod,"center_dot",false)){rects.push_back(ImVec4(x0,y0,x1,y1));rectangles();}}
 else{const float cell=std::fmax(1.f,std::round(num("pixel_size",1)*scale));const float ox=std::floor(screen.x*.5f)-std::round(cell*.5f)-8*cell,oy=std::floor(screen.y*.5f)-std::round(cell*.5f)-8*cell;
  std::string text=crosshairDefault;if(auto st=Find(*mod,"pattern"))if(!st->text.empty())text=st->text;const auto rows=CrosshairRows(text);
  for(int y=0;y<17;++y)for(int x=0;x<17;++x)if((rows[y]>>x)&1)rects.push_back(ImVec4(ox+x*cell,oy+y*cell,ox+(x+1)*cell,oy+(y+1)*cell));rectangles();}
}
