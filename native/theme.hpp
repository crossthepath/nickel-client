// Nickel menu themes: dark UI tokens (#0D0D0F, #14141B, #23232C, text #FFFFFF / #9494A0) and an animated
// "blueprint" backdrop whose colours follow the theme.
// - Orange: radial gradient #FF9348 -> #E8590F -> #B13704 -> #7E2602, construction lines, axes, concentric arcs,
// dashed angle rays, graduated rulers;
// - Blueprint: blue plan with a fine grid, arcs and rays, dark vignette;
// - Cutting Mat: green mat (fine grid, thick major squares, 45-degree diagonal, numbered rulers, sliding highlights);
// - Violet (violet), Crimson, Graphite.
// Animation: the glow breathes (18 s), rays sweep a few degrees (15 s), arcs draw in when the menu opens,
// a reticle travels along the bottom ruler; theme changes cross-fade.
// Included by menu.cpp inside its anonymous namespace, after Rgba / Label.
#pragma once

struct ThemeDef{const char* name;int pattern;uint32_t glow,base,deep,ink;uint32_t accent,accentDeep;};
const ThemeDef themes[]={
 {"Orange",0,0xFF9348,0xE8590F,0xB13704,0x7E2602,0xFF8A3D,0xE8590F},   // not a game address: colours
 {"Blueprint",1,0x35AEFF,0x0A72D6,0x0753A3,0x022B5E,0x4DABFF,0x0A72D6},   // not a game address: colours
 {"Cutting Mat",2,0x31C99A,0x0E9A6F,0x08734F,0x033F2A,0x34D399,0x0E9A6F},   // not a game address: colours
 {"Violet",0,0xB07CFF,0x6E5BFF,0x4B3BB0,0x1B1450,0xB47BFF,0x8B41F0},   // not a game address: colours
 {"Crimson",0,0xFF7B6B,0xE23A3A,0xA61E24,0x520A0E,0xFF6363,0xE23A3A},   // not a game address: colours
 {"Graphite",1,0x8A8A96,0x45454F,0x2A2A33,0x0D0D0F,0xEBEBF0,0x6E6E7A},   // not a game address: colours
};
constexpr int themeCount=(int)(sizeof(themes)/sizeof(themes[0]));
int themeIndex=0;
// Displayed colours: fade towards the selected theme.
struct ThemeLive{ImVec4 glow,base,deep,ink,accent,accentDeep;float pattern[3]{1,0,0};bool ready=false;};ThemeLive themeLive;
inline ImVec4 HexColor(uint32_t h){return ImVec4(((h>>16)&255)/255.f,((h>>8)&255)/255.f,(h&255)/255.f,1);}
inline ImVec4 MixColor(ImVec4 a,ImVec4 b,float t){return ImVec4(a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t,a.w+(b.w-a.w)*t);}
inline const ThemeDef& Th(){return themes[std::clamp(themeIndex,0,themeCount-1)];}
inline void ThemeTick(float dt){const ThemeDef& t=Th();auto& l=themeLive;
 if(!l.ready){l.glow=HexColor(t.glow);l.base=HexColor(t.base);l.deep=HexColor(t.deep);l.ink=HexColor(t.ink);l.accent=HexColor(t.accent);l.accentDeep=HexColor(t.accentDeep);for(int i=0;i<3;++i)l.pattern[i]=i==t.pattern?1.f:0.f;l.ready=true;return;}
 const float k=1-std::exp(-6*dt);l.glow=MixColor(l.glow,HexColor(t.glow),k);l.base=MixColor(l.base,HexColor(t.base),k);l.deep=MixColor(l.deep,HexColor(t.deep),k);l.ink=MixColor(l.ink,HexColor(t.ink),k);
 l.accent=MixColor(l.accent,HexColor(t.accent),k);l.accentDeep=MixColor(l.accentDeep,HexColor(t.accentDeep),k);for(int i=0;i<3;++i)l.pattern[i]+=((i==t.pattern?1.f:0.f)-l.pattern[i])*k;}
inline ImU32 ToU32(ImVec4 c,float a){c.w=std::clamp(a,0.f,1.f);return ImGui::ColorConvertFloat4ToU32(c);}
inline ImU32 Acc(float a){if(!themeLive.ready)ThemeTick(0);return ToU32(themeLive.accent,a);}
inline ImU32 AccDeep(float a){if(!themeLive.ready)ThemeTick(0);return ToU32(themeLive.accentDeep,a);}
inline ImU32 AccHover(float a){if(!themeLive.ready)ThemeTick(0);return ToU32(MixColor(themeLive.accentDeep,ImVec4(1,1,1,1),.14f),a);}
inline ImU32 AccDim(float a){if(!themeLive.ready)ThemeTick(0);return ToU32(MixColor(themeLive.accentDeep,ImVec4(13/255.f,13/255.f,15/255.f,1),.55f),a);}
inline void ThemeSave(){if(!configPath.empty())WritePrivateProfileStringW(L"interface",L"theme",std::to_wstring(themeIndex).c_str(),configPath.c_str());}
inline void ThemeLoad(){if(configPath.empty())return;themeIndex=std::clamp((int)GetPrivateProfileIntW(L"interface",L"theme",0,configPath.c_str()),0,themeCount-1);themeLive.ready=false;}

// ---- animated backdrop
double backdropBorn=-1;   // menu open time: start of the arc animation
inline ImVec4 BackdropColor(const ImVec2& p,const ImVec2& gc,float radius){const auto& l=themeLive;float t=std::sqrt((p.x-gc.x)*(p.x-gc.x)+(p.y-gc.y)*(p.y-gc.y))/radius;t=std::clamp(t,0.f,1.f);
 if(t<.42f)return MixColor(l.glow,l.base,t/.42f);if(t<.78f)return MixColor(l.base,l.deep,(t-.42f)/.36f);return MixColor(l.deep,l.ink,(t-.78f)/.22f);}
// Fills a rounded rectangle with a radial gradient (mesh: 4 x 48 outline points, 10 rings towards the centre).
inline void BackdropGradient(ImDrawList* d,ImVec2 lo,ImVec2 hi,float r,ImVec2 gc,float radius,float opacity){
 const float w=hi.x-lo.x,h=hi.y-lo.y;r=std::min(r,std::min(w,h)/2);
 // The outline ring only depends on the rectangle and the corner radius: rebuilt when those change.
 static std::vector<ImVec2> ring;static ImVec2 ringLo,ringHi;static float ringR=-1;
 if(ringR!=r||ringLo.x!=lo.x||ringLo.y!=lo.y||ringHi.x!=hi.x||ringHi.y!=hi.y){ringLo=lo;ringHi=hi;ringR=r;ring.clear();
 auto side=[&](ImVec2 a,ImVec2 b,int n){for(int i=0;i<n;++i)ring.push_back(ImVec2(a.x+(b.x-a.x)*i/n,a.y+(b.y-a.y)*i/n));};
 auto corner=[&](ImVec2 c,float a0){for(int i=0;i<8;++i){const float a=a0+1.5707963f*i/8;ring.push_back(ImVec2(c.x+std::cos(a)*r,c.y+std::sin(a)*r));}};
 corner(ImVec2(lo.x+r,lo.y+r),3.14159265f);side(ImVec2(lo.x+r,lo.y),ImVec2(hi.x-r,lo.y),40);corner(ImVec2(hi.x-r,lo.y+r),-1.5707963f);side(ImVec2(hi.x,lo.y+r),ImVec2(hi.x,hi.y-r),24);
 corner(ImVec2(hi.x-r,hi.y-r),0);side(ImVec2(hi.x-r,hi.y),ImVec2(lo.x+r,hi.y),40);corner(ImVec2(lo.x+r,hi.y-r),1.5707963f);side(ImVec2(lo.x,hi.y-r),ImVec2(lo.x,lo.y+r),24);}
 // Colour lookup over t in [0,1] (256 entries, alpha applied once) instead of a gradient evaluation per vertex.
 static ImU32 lut[256];for(int i=0;i<256;++i){const float t=i/255.f;const auto& l=themeLive;ImVec4 c=t<.42f?MixColor(l.glow,l.base,t/.42f):t<.78f?MixColor(l.base,l.deep,(t-.42f)/.36f):MixColor(l.deep,l.ink,(t-.78f)/.22f);lut[i]=ToU32(c,opacity);}
 auto colorAt=[&](const ImVec2& p){const float dx=p.x-gc.x,dy=p.y-gc.y;const float t=std::clamp(std::sqrt(dx*dx+dy*dy)/radius,0.f,1.f);return lut[(int)(t*255.f+.5f)];};
 const ImVec2 mid((lo.x+hi.x)/2,(lo.y+hi.y)/2);const int n=(int)ring.size(),K=10;const ImVec2 uv=ImGui::GetFontTexUvWhitePixel();
 d->PrimReserve(n*K*6,n*K+1);const ImDrawIdx base=(ImDrawIdx)d->_VtxCurrentIdx;
 d->PrimWriteVtx(mid,uv,colorAt(mid));
 for(int k=1;k<=K;++k)for(int i=0;i<n;++i){const float f=(float)k/K;const ImVec2 p(mid.x+(ring[i].x-mid.x)*f,mid.y+(ring[i].y-mid.y)*f);d->PrimWriteVtx(p,uv,colorAt(p));}
 auto at=[&](int k,int i){return (ImDrawIdx)(base+1+(k-1)*n+((i%n+n)%n));};
 for(int i=0;i<n;++i){d->PrimWriteIdx(base);d->PrimWriteIdx(at(1,i));d->PrimWriteIdx(at(1,i+1));}
 for(int k=1;k<K;++k)for(int i=0;i<n;++i){d->PrimWriteIdx(at(k,i));d->PrimWriteIdx(at(k+1,i));d->PrimWriteIdx(at(k+1,i+1));d->PrimWriteIdx(at(k,i));d->PrimWriteIdx(at(k+1,i+1));d->PrimWriteIdx(at(k,i+1));}
 // Release indices reserved but not written (inner ring uses n*3 instead of n*6).
 d->PrimUnreserve(n*3,0);}
inline void DashedLine(ImDrawList* d,ImVec2 a,ImVec2 b,ImU32 c,float th,float on,float off){const float dx=b.x-a.x,dy=b.y-a.y,len=std::sqrt(dx*dx+dy*dy);if(len<1)return;const float ux=dx/len,uy=dy/len;
 for(float t=0;t<len;t+=on+off){const float e=std::min(len,t+on);d->AddLine(ImVec2(a.x+ux*t,a.y+uy*t),ImVec2(a.x+ux*e,a.y+uy*e),c,th);}}
// Length of a ray from o in direction (ux,uy) to the rectangle edge.
inline float RayLength(ImVec2 o,float ux,float uy,ImVec2 lo,ImVec2 hi){float t=1e9f;if(ux>1e-4f)t=std::min(t,(hi.x-o.x)/ux);if(ux<-1e-4f)t=std::min(t,(lo.x-o.x)/ux);if(uy>1e-4f)t=std::min(t,(hi.y-o.y)/uy);if(uy<-1e-4f)t=std::min(t,(lo.y-o.y)/uy);return std::max(0.f,t);}
inline void BackdropText(ImDrawList* d,float size,ImVec2 p,const char* text,ImU32 c,bool center){if((c>>24)<10)return;   // under 4% alpha: invisible
 if(center)p.x-=TextWidth(tiny,size,text)/2;Label(d,tiny,size,p,text,c);}
std::string Number(float v){char b[16];if(std::fabs(v-std::round(v))<.01f)snprintf(b,sizeof(b),"%d",(int)std::round(v));else snprintf(b,sizeof(b),"%.1f",v);return b;}

// Plan pattern (Orange, Violet, Crimson): origin at the bottom left, unit = height / 11.
inline void PatternPlan(ImDrawList* d,ImVec2 lo,ImVec2 hi,float s,float a,double time,float traced){
 const float m=40*s,u=(hi.y-lo.y-2*m)/10.5f;const ImVec2 o(lo.x+m,hi.y-m);const float right=hi.x-m,top=lo.y+m*.4f;
 auto W=[&](float alpha){return Rgba(255,255,255,alpha*a);};
 for(float x=o.x+5*u;x<right-2;x+=5*u)d->AddLine(ImVec2(x,lo.y),ImVec2(x,o.y),W(.42f),1*s);
 for(float y=o.y-5*u;y>lo.y+2;y-=5*u)d->AddLine(ImVec2(lo.x,y),ImVec2(hi.x,y),W(.42f),1*s);
 d->AddLine(ImVec2(o.x,top),ImVec2(o.x,o.y),W(.6f),1*s);d->AddLine(ImVec2(o.x,o.y),ImVec2(right,o.y),W(.6f),1*s);d->AddLine(ImVec2(right,top),ImVec2(right,o.y),W(.6f),1*s);
 // Rulers: tick every 0.5 unit, number every 2.5.
 for(int i=0;o.x+i*.5f*u<=right+1;++i){const float x=o.x+i*.5f*u;const bool major=i%5==0;d->AddLine(ImVec2(x,o.y),ImVec2(x,o.y+(major?7:4)*s),W(.55f),1*s);if(major)BackdropText(d,9*s,ImVec2(x,o.y+10*s),Number(i*.5f).c_str(),W(.72f),true);}
 for(int i=1;o.y-i*.5f*u>=top;++i){const float y=o.y-i*.5f*u;const bool major=i%5==0;d->AddLine(ImVec2(o.x-(major?7:4)*s,y),ImVec2(o.x,y),W(.55f),1*s);d->AddLine(ImVec2(right,y),ImVec2(right+(major?7:4)*s,y),W(.55f),1*s);
  if(major){const std::string t=Number(i*.5f);BackdropText(d,9*s,ImVec2(right+10*s,y-6*s),t.c_str(),W(.72f),false);}}
 // Arcs (radius 4, 8, 12), drawn in on open, ticks every 15 degrees.
 for(int r=4;r<=12;r+=4){const float R=r*u;const float end=-1.5707963f*traced;d->PathArcTo(o,R,0,end,48);d->PathStroke(W(.5f),0,1*s);
  for(int g=1;g<6;++g){const float ang=-g*.2617994f;if(ang<end)break;d->AddLine(ImVec2(o.x+std::cos(ang)*(R-3*s),o.y+std::sin(ang)*(R-3*s)),ImVec2(o.x+std::cos(ang)*(R+3*s),o.y+std::sin(ang)*(R+3*s)),W(.5f),1*s);}}
 // Rays at 15, 30, 45 and 60 degrees, sweeping a few degrees.
 for(int i=1;i<=4;++i){const float deg=15.f*i+1.6f*(float)std::sin(time*6.2831853/15+i*1.3);const float ang=-deg*.0174533f;const float ux=std::cos(ang),uy=std::sin(ang);
  const float lab=4.6f*u,len=RayLength(o,ux,uy,lo,hi);DashedLine(d,o,ImVec2(o.x+ux*(lab-12*s),o.y+uy*(lab-12*s)),W(.48f),1*s,3*s,4*s);DashedLine(d,ImVec2(o.x+ux*(lab+12*s),o.y+uy*(lab+12*s)),ImVec2(o.x+ux*len,o.y+uy*len),W(.48f),1*s,3*s,4*s);
  char t[8];snprintf(t,sizeof(t),"%d\xc2\xb0",15*i);BackdropText(d,9*s,ImVec2(o.x+ux*lab,o.y+uy*lab-6*s),t,W(.78f),true);}
 // Reticle travelling along the bottom ruler.
 {const float f=(float)std::fmod(time/14.0,1.0);const float x=o.x+(right-o.x)*(.5f-.5f*std::cos(f*6.2831853f));d->AddLine(ImVec2(x,o.y-14*s),ImVec2(x,o.y+14*s),W(.7f),1*s);d->AddCircle(ImVec2(x,o.y),4*s,W(.75f),16,1*s);}}

// Blueprint pattern (Blueprint, Graphite): fine grid, major squares, arcs, rays, bottom and side rulers.
inline void PatternGrid(ImDrawList* d,ImVec2 lo,ImVec2 hi,float s,float a,double time,float traced){
 const float m=32*s,u=(hi.y-lo.y-2*m)/26.f;const ImVec2 o(lo.x+m,hi.y-m);const float right=hi.x-m;
 auto W=[&](float alpha){return Rgba(255,255,255,alpha*a);};
 int i=0;for(float x=o.x;x<=right+1;x+=u,++i)d->AddLine(ImVec2(x,lo.y),ImVec2(x,o.y),W(i%5==0?.4f:.1f),1*s);
 i=0;for(float y=o.y;y>=lo.y;y-=u,++i)d->AddLine(ImVec2(lo.x,y),ImVec2(hi.x,y),W(i%5==0?.4f:.1f),1*s);
 d->AddLine(ImVec2(o.x,o.y),ImVec2(right,o.y),W(.6f),1*s);
 for(int k=0;o.x+k*u<=right+1;++k){const float x=o.x+k*u;d->AddLine(ImVec2(x,o.y),ImVec2(x,o.y+(k%5==0?7:4)*s),W(.5f),1*s);if(k%5==0)BackdropText(d,9*s,ImVec2(x,o.y+10*s),Number((float)k).c_str(),W(.68f),true);}
 for(int k=5;o.y-k*u>=lo.y+8*s;k+=5){const float y=o.y-k*u;const std::string t=Number((float)k);BackdropText(d,9*s,ImVec2(o.x-8*s-TextWidth(tiny,9*s,t.c_str()),y-6*s),t.c_str(),W(.68f),false);BackdropText(d,9*s,ImVec2(right+8*s,y-6*s),t.c_str(),W(.68f),false);}
 for(int r=10;r<=30;r+=10){d->PathArcTo(o,r*u,0,-1.5707963f*traced,64);d->PathStroke(W(.42f),0,1*s);}
 for(int k=1;k<=4;++k){const float deg=15.f*k+1.2f*(float)std::sin(time*6.2831853/17+k);const float ang=-deg*.0174533f,ux=std::cos(ang),uy=std::sin(ang);const float len=RayLength(o,ux,uy,lo,hi);
  DashedLine(d,o,ImVec2(o.x+ux*len,o.y+uy*len),W(.36f),1*s,3*s,4*s);char t[8];snprintf(t,sizeof(t),"%d\xc2\xb0",15*k);const float lab=(k==1?10.5f:6.5f)*u;BackdropText(d,9*s,ImVec2(o.x+ux*lab,o.y+uy*lab-12*s),t,W(.72f),true);}
 {const float f=(float)std::fmod(time/16.0,1.0);const float x=o.x+(right-o.x)*(.5f-.5f*std::cos(f*6.2831853f));d->AddLine(ImVec2(x,lo.y),ImVec2(x,o.y),W(.16f),1*s);d->AddCircle(ImVec2(x,o.y),4*s,W(.7f),16,1*s);}}

// Cutting Mat pattern: fine grid, thick squares every 5, diagonal, numbered rulers, highlights.
inline void PatternMat(ImDrawList* d,ImVec2 lo,ImVec2 hi,float s,float a,double time,float traced){
 const float m=18*s,u=(hi.y-lo.y-2*m)/28.f;const ImVec2 o(lo.x+m,hi.y-m);const float right=hi.x-m,top=lo.y+m;
 auto W=[&](float alpha){return Rgba(255,255,255,alpha*a);};
 // Diagonal highlights sliding slowly.
 {const float w=hi.x-lo.x,h=hi.y-lo.y,band=w*.07f,shift=(float)std::fmod(time*14.0*s,(double)w*.36f);
  for(int k=-2;k<6;++k){const float x0=lo.x+k*w*.36f+shift;const ImVec2 q[4]={ImVec2(x0,lo.y),ImVec2(x0+band,lo.y),ImVec2(x0+band-h*.8f,hi.y),ImVec2(x0-h*.8f,hi.y)};
   d->AddConvexPolyFilled(q,4,Rgba(255,255,255,.05f*a));const ImVec2 q2[4]={ImVec2(x0+band*.25f,lo.y),ImVec2(x0+band*.75f,lo.y),ImVec2(x0+band*.75f-h*.8f,hi.y),ImVec2(x0+band*.25f-h*.8f,hi.y)};d->AddConvexPolyFilled(q2,4,Rgba(255,255,255,.04f*a));}}
 int i=0;for(float x=o.x;x<=right+1;x+=u,++i)d->AddLine(ImVec2(x,top),ImVec2(x,o.y),W(i%5==0?.9f:.16f),(i%5==0?2.f:1.f)*s);
 i=0;for(float y=o.y;y>=top-1;y-=u,++i)d->AddLine(ImVec2(o.x,y),ImVec2(right,y),W(i%5==0?.9f:.16f),(i%5==0?2.f:1.f)*s);
 d->AddRect(ImVec2(o.x,top),ImVec2(right,o.y),W(.9f),0,0,2*s);
 {const float len=std::min(right-o.x,o.y-top)*traced;d->AddLine(o,ImVec2(o.x+len,o.y-len),W(.85f),1.5f*s);}
 for(int k=0;o.x+k*u<=right+1;++k){const float x=o.x+k*u;const std::string t=Number((float)k);BackdropText(d,8*s,ImVec2(x,hi.y-m+3*s),t.c_str(),W(.7f),true);BackdropText(d,8*s,ImVec2(x,lo.y+2*s),t.c_str(),W(.7f),true);}
 for(int k=0;o.y-k*u>=top-1;++k){const float y=o.y-k*u;const std::string t=Number((float)k);BackdropText(d,8*s,ImVec2(lo.x+m/2,y-5*s),t.c_str(),W(.7f),true);BackdropText(d,8*s,ImVec2(hi.x-m/2,y-5*s),t.c_str(),W(.7f),true);}}

// Full backdrop: theme gradient then patterns (patterns cross-fade on theme change).
inline void DrawBackdrop(ImDrawList* d,ImVec2 lo,ImVec2 hi,float r,float s,float opacity){
 auto& io=ImGui::GetIO();ThemeTick(io.DeltaTime);const double time=ImGui::GetTime();if(backdropBorn<0)backdropBorn=time;
 const float traced=std::clamp((float)((time-backdropBorn)/1.4),0.f,1.f),ease=1-(1-traced)*(1-traced)*(1-traced);
 const float w=hi.x-lo.x,h=hi.y-lo.y,breathe=1+.05f*(float)std::sin(time*6.2831853/18);const auto& p=themeLive.pattern;
 // Glow: top centre (plan), right of centre (grid), centre (mat).
 const ImVec2 gc(lo.x+w*(.5f*p[0]+.64f*p[1]+.5f*p[2])+w*.03f*(float)std::sin(time*6.2831853/22),lo.y+h*(-.06f*p[0]+.5f*p[1]+.45f*p[2]));
 const float radius=std::max(w,h)*(1.0f*p[0]+.62f*p[1]+.95f*p[2])*breathe;
 BackdropGradient(d,lo,hi,r,gc,radius,opacity);
 d->PushClipRect(ImVec2(lo.x+r*.3f,lo.y+r*.3f),ImVec2(hi.x-r*.3f,hi.y-r*.3f),true);
 if(p[0]>.01f)PatternPlan(d,lo,hi,s,opacity*p[0]*.8f,time,ease);
 if(p[1]>.01f)PatternGrid(d,lo,hi,s,opacity*p[1]*.8f,time,ease);
 if(p[2]>.01f)PatternMat(d,lo,hi,s,opacity*p[2]*.8f,time,ease);
 d->PopClipRect();}

// ---- Nickel icons: stroked paths on a 24-unit grid (stroke 1.7). Order matches the menu tabs: Combat, Movement,
// Player, Visuals, Utility, HUD, Settings, Configs, Scripting, Schematics; 11 and 12 are the PUBLIC Scripts and Configs.
// Icon font (ui/assets/nickel-icons.ttf, see icon_codepoints.hpp): Lucide glyphs and brand logos. The em square
// (1000 units, ascent 1000, descent 0) is the 24 x 24 icon box, so a glyph drawn at `size` from `lo` fills [lo, lo+size].
inline ImFont* iconFont=nullptr;
inline bool DrawGlyph(ImDrawList* d,unsigned code,ImVec2 lo,float size,ImU32 c){if(!iconFont||!code||!iconFont->FindGlyphNoFallback((ImWchar)code))return false;
 const char text[4]={(char)(0xE0|(code>>12)),(char)(0x80|((code>>6)&0x3F)),(char)(0x80|(code&0x3F)),0};   // UTF-8 of a BMP code point
 d->AddText(iconFont,size,ImVec2(std::round(lo.x),std::round(lo.y)),c,text);return true;}
inline void DrawIcon(ImDrawList* d,int kind,ImVec2 lo,float size,ImU32 c){
 {static const unsigned codes[10]={nicons::k_cat_combat,nicons::k_cat_movement,nicons::k_cat_player,nicons::k_cat_visuals,nicons::k_cat_utility,
   nicons::k_cat_hud,nicons::k_cat_settings,nicons::k_cat_configs,nicons::k_cat_scripting,nicons::k_cat_schematics};
  if(kind<11&&DrawGlyph(d,codes[kind>=0&&kind<10?kind:9],lo,size,c))return;}   // line drawings below are the fallback when the font is missing (11, 12: PUBLIC, line drawings only)
 const float k=size/24.f,th=std::max(1.f,1.7f*k);auto P=[&](float x,float y){return ImVec2(lo.x+x*k,lo.y+y*k);};
 auto L=[&](float x0,float y0,float x1,float y1){d->AddLine(P(x0,y0),P(x1,y1),c,th);};
 auto poly=[&](std::initializer_list<ImVec2> pts,bool closed){std::vector<ImVec2> v;for(auto& q:pts)v.push_back(P(q.x,q.y));d->AddPolyline(v.data(),(int)v.size(),c,closed?ImDrawFlags_Closed:0,th);};
 switch(kind){
  case 0: L(4,4,16,16);L(13,19,19,13);L(17,17,20,20);L(20,4,8,16);L(5,13,11,19);L(7,17,4,20);break;                       // Combat: crossed blades
  case 1: poly({ImVec2(7,3),ImVec2(13,3),ImVec2(13,11),ImVec2(18,13),ImVec2(20.5f,15.5f),ImVec2(20.5f,20),ImVec2(4,20),ImVec2(4,16),ImVec2(7,13)},true);
          L(7,6.5f,13,6.5f);L(4,17.2f,20.5f,17.2f);break;                                                         // Movement: boot (cuff and sole lines)
  case 2: d->AddCircle(P(12,8),4*k,c,24,th);d->PathArcTo(P(12,22),8*k,3.14159265f,6.2831853f,24);d->PathStroke(c,0,th);break; // Player
  case 3: d->PathArcTo(P(12,15.64f),10.64f*k,-2.792f,-.349f,28);d->PathStroke(c,0,th);d->PathArcTo(P(12,8.36f),10.64f*k,.349f,2.792f,28);d->PathStroke(c,0,th);
          d->AddCircle(P(12,12),4*k,c,24,th);d->AddCircleFilled(P(12,12),1.8f*k,c,16);break;                                  // Visuals: open eye (lids from y 5 to 19), iris and pupil
  case 4: d->AddRect(P(4,4),P(10.5f,10.5f),c,1.8f*k,0,th);d->AddRect(P(13.5f,4),P(20,10.5f),c,1.8f*k,0,th);d->AddRect(P(4,13.5f),P(10.5f,20),c,1.8f*k,0,th);
          d->AddRect(P(13.5f,13.5f),P(20,20),c,1.8f*k,0,th);break;                                                               // Utility: four tiles
  case 5: d->AddRect(P(3,4),P(21,20),c,2.5f*k,0,th);L(9,4,9,20);L(9,12,21,12);break;                                              // HUD: layout
  case 6: L(3,6,12.8f,6);L(17.2f,6,21,6);d->AddCircle(P(15,6),2.2f*k,c,16,th);L(3,12,5.8f,12);L(10.2f,12,21,12);d->AddCircle(P(8,12),2.2f*k,c,16,th);
          L(3,18,14.8f,18);L(19.2f,18,21,18);d->AddCircle(P(17,18),2.2f*k,c,16,th);break;                                       // Settings: sliders
  case 7: poly({ImVec2(3,7),ImVec2(3,19),ImVec2(21,19),ImVec2(21,9),ImVec2(11.5f,9),ImVec2(9.5f,6),ImVec2(3,6)},true);break;     // Configs: folder
  case 8: poly({ImVec2(8,7),ImVec2(3,12),ImVec2(8,17)},false);poly({ImVec2(16,7),ImVec2(21,12),ImVec2(16,17)},false);L(14,5,10,19);break; // Scripting
  case 11: d->AddCircle(P(12,12),9*k,c,32,th);d->AddEllipse(P(12,12),ImVec2(4*k,9*k),c,0,24,th);L(3,12,21,12);break;                // PUBLIC Scripts: globe
  case 12: poly({ImVec2(3,7),ImVec2(3,19),ImVec2(21,19),ImVec2(21,9),ImVec2(11.5f,9),ImVec2(9.5f,6),ImVec2(3,6)},true);
           L(12,11.5f,12,16.5f);poly({ImVec2(9.5f,14.2f),ImVec2(12,16.7f),ImVec2(14.5f,14.2f)},false);break;                    // PUBLIC Configs: folder with an arrow
  default: poly({ImVec2(12,3),ImVec2(20,7.5f),ImVec2(20,16.5f),ImVec2(12,21),ImVec2(4,16.5f),ImVec2(4,7.5f)},true);
          poly({ImVec2(4,7.5f),ImVec2(12,12),ImVec2(20,7.5f)},false);L(12,12,12,21);break;                                   // Schematics: cube
 }}
