// Visuals: first-person held items, armour, third-person nametags and block outline (RVAs for Minecraft 1.26.62.01).
// hook #230  RVA 0x532b5a0  held item render (rcx, rdx, r8, r9b): flags the thread during the call
// hook #231  RVA 0x531b490  main hand (8 args); #232 RVA 0x531a580 off hand (5 args):
// matrix stack = *(*(ctx+0x28)+0x18)+0x40 (std::deque of mat4: map +8, map size +0x10, start +0x18, count +0x20,
// dirty flag +0x38). The top is written before the game call and the saved copy MUST be restored after it.
// M = P · T(O) · (T(p) · X · T(-p)) · L, L = P^-1 · B, p = translation of L:
// View Model sets O, Block Hit sets B, Item Inspect sets X.
// hook #2    RVA 0x58be290  attachable render (rcx, item stack, u32* slot, r9, actor): No Armor skips it for
// helmets / chestplates / leggings / boots (by item name suffix).
// hook #236  RVA 0x539de60  targeted block outline (rcx, rdx, r8, int3* position, byte): Block Outline skips it and
// redraws through the game's Tessellator (begin 0x54175a0, colour 0x5417890, vertex 0x5417950, render 0x5ad6750,
// material "name_tag_depth_tested" or "name_tag" from the group at RVA 0x129c8290, table +8).
// Third Person Nametags: two-byte patch in game code at RVA 0x5397e23, "74 DB" -> "90 90"; restored on disable.
// Item Inspect key rotations are applied X, then Y, then Z. Block Hit "Exhibition" uses an approximate pose;
// "1.7" and "1.8" share the same path.
#pragma once
#include "game_render.hpp"

namespace game {
struct Mat4{float m[16];};
inline Mat4 Identity(){Mat4 r{};r.m[0]=r.m[5]=r.m[10]=r.m[15]=1;return r;}
inline Mat4 operator*(const Mat4& a,const Mat4& b){Mat4 r{};for(int c=0;c<4;++c)for(int row=0;row<4;++row){float sum=0;for(int k=0;k<4;++k)sum+=a.m[k*4+row]*b.m[c*4+k];r.m[c*4+row]=sum;}return r;}
inline Mat4 Translation(float x,float y,float z){Mat4 r=Identity();r.m[12]=x;r.m[13]=y;r.m[14]=z;return r;}
inline Mat4 Rotation(float degrees,float x,float y,float z){const float a=degrees*.017453292f,c=std::cos(a),s=std::sin(a),length=std::sqrt(x*x+y*y+z*z);if(length<1e-6f)return Identity();x/=length;y/=length;z/=length;const float t=1-c;Mat4 r=Identity();
 r.m[0]=c+x*x*t;r.m[1]=y*x*t+z*s;r.m[2]=z*x*t-y*s;r.m[4]=x*y*t-z*s;r.m[5]=c+y*y*t;r.m[6]=z*y*t+x*s;r.m[8]=x*z*t+y*s;r.m[9]=y*z*t-x*s;r.m[10]=c+z*z*t;return r;}
inline bool Inverse(const Mat4& in,Mat4* out){const float* m=in.m;float inv[16];
 inv[0]=m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];inv[4]=-m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
 inv[8]=m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];inv[12]=-m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
 inv[1]=-m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];inv[5]=m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
 inv[9]=-m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];inv[13]=m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
 inv[2]=m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];inv[6]=-m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
 inv[10]=m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];inv[14]=-m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
 inv[3]=-m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];inv[7]=m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
 inv[11]=-m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];inv[15]=m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
 const float det=m[0]*inv[0]+m[1]*inv[4]+m[2]*inv[8]+m[3]*inv[12];if(!std::isfinite(det)||std::fabs(det)<1e-12f)return false;for(int i=0;i<16;++i)out->m[i]=inv[i]/det;return true;}
// Decompose into translation / quaternion / scale and blend (lerp, shortest-path slerp, lerp).
struct Pose{float t[3];float q[4];float s[3];};
inline Pose Decompose(const Mat4& in){Pose p{};const float* m=in.m;for(int i=0;i<3;++i){p.t[i]=m[12+i];p.s[i]=std::sqrt(m[i*4]*m[i*4]+m[i*4+1]*m[i*4+1]+m[i*4+2]*m[i*4+2]);if(p.s[i]<1e-6f)p.s[i]=1e-6f;}
 float r[3][3];for(int c=0;c<3;++c)for(int row=0;row<3;++row)r[c][row]=m[c*4+row]/p.s[c];const float trace=r[0][0]+r[1][1]+r[2][2];
 if(trace>0){const float k=std::sqrt(trace+1)*2;p.q[3]=k/4;p.q[0]=(r[1][2]-r[2][1])/k;p.q[1]=(r[2][0]-r[0][2])/k;p.q[2]=(r[0][1]-r[1][0])/k;}
 else if(r[0][0]>r[1][1]&&r[0][0]>r[2][2]){const float k=std::sqrt(1+r[0][0]-r[1][1]-r[2][2])*2;p.q[3]=(r[1][2]-r[2][1])/k;p.q[0]=k/4;p.q[1]=(r[1][0]+r[0][1])/k;p.q[2]=(r[2][0]+r[0][2])/k;}
 else if(r[1][1]>r[2][2]){const float k=std::sqrt(1+r[1][1]-r[0][0]-r[2][2])*2;p.q[3]=(r[2][0]-r[0][2])/k;p.q[0]=(r[1][0]+r[0][1])/k;p.q[1]=k/4;p.q[2]=(r[2][1]+r[1][2])/k;}
 else{const float k=std::sqrt(1+r[2][2]-r[0][0]-r[1][1])*2;p.q[3]=(r[0][1]-r[1][0])/k;p.q[0]=(r[2][0]+r[0][2])/k;p.q[1]=(r[2][1]+r[1][2])/k;p.q[2]=k/4;}
 return p;}
inline Mat4 Compose(const Pose& p){const float x=p.q[0],y=p.q[1],z=p.q[2],w=p.q[3];Mat4 r=Identity();r.m[0]=1-2*(y*y+z*z);r.m[1]=2*(x*y+z*w);r.m[2]=2*(x*z-y*w);r.m[4]=2*(x*y-z*w);r.m[5]=1-2*(x*x+z*z);r.m[6]=2*(y*z+x*w);r.m[8]=2*(x*z+y*w);r.m[9]=2*(y*z-x*w);r.m[10]=1-2*(x*x+y*y);
 for(int c=0;c<3;++c)for(int row=0;row<3;++row)r.m[c*4+row]*=p.s[c];r.m[12]=p.t[0];r.m[13]=p.t[1];r.m[14]=p.t[2];return r;}
inline Mat4 Blend(const Mat4& from,const Mat4& to,float t){Pose a=Decompose(from),b=Decompose(to),o{};for(int i=0;i<3;++i){o.t[i]=a.t[i]+(b.t[i]-a.t[i])*t;o.s[i]=a.s[i]+(b.s[i]-a.s[i])*t;}
 float dot=a.q[0]*b.q[0]+a.q[1]*b.q[1]+a.q[2]*b.q[2]+a.q[3]*b.q[3];if(dot<0){dot=-dot;for(float& v:b.q)v=-v;}float wa=1-t,wb=t;if(dot<=.9999999f){const float angle=std::acos(std::clamp(dot,-1.f,1.f)),sine=std::sin(angle);if(sine>1e-6f){wa=std::sin((1-t)*angle)/sine;wb=std::sin(t*angle)/sine;}}
 float length=0;for(int i=0;i<4;++i){o.q[i]=a.q[i]*wa+b.q[i]*wb;length+=o.q[i]*o.q[i];}length=std::sqrt(length);if(length>1e-6f)for(float& v:o.q)v/=length;return Compose(o);}

// ---- settings from the menu (written by the HUD thread, read by the game render thread)
struct HandShared{
 SRWLOCK lock=SRWLOCK_INIT;volatile long fault=0,seen=0,playing=0;
 bool viewModel=false;float mainOffset[3]{},offOffset[3]{};
 bool blockHit=false,always=false,allItems=false;int animation=0;float blockScale=1;
 bool inspect=false,cancelOnAttack=true;int inspectMode=0;float inspectSpeed=1;volatile long inspectRequest=0;
 bool noArmor=false,armorSlot[4]{};int armorTarget=0;
 bool outline=false,visibleOnly=true,overlay=true,faceOnly=false;float outlineColor[4]{1,1,1,1},overlayColor[4]{1,1,1,.25f},thickness=1.5f;volatile long outlineFault=0,outlineSeen=0;
};
inline HandShared& Hand(){static HandShared* shared=new HandShared;return *shared;}
inline double HandClock(){LARGE_INTEGER f{},n{};QueryPerformanceFrequency(&f);QueryPerformanceCounter(&n);return n.QuadPart*1000.0/f.QuadPart;}   // milliseconds

// Top and parent of the matrix stack (MSVC deque: one 0x40-byte mat4 per block).
inline bool StackLevel(uint64_t stack,uint64_t back,uint64_t* matrix){uint64_t map{},size{},first{},count{};if(!Read(stack+8,&map)||!Read(stack+0x10,&size)||!Read(stack+0x18,&first)||!Read(stack+0x20,&count)||!map||!size||(size&(size-1))||count<=back||count>4096)return false;return Read(map+(((first+count-1-back)&(size-1))*8),matrix)&&*matrix;}
inline bool EndsWith(const char* text,const char* suffix){const size_t a=strlen(text),b=strlen(suffix);return a>=b&&!strcmp(text+a-b,suffix);}
// Name (without namespace) of the selected slot's item: *(player+0x5e0): int slot at +0x10, container *(+0xb8).
inline bool HeldName(const Context& context,char* name,size_t capacity,int* slot,uint64_t* item){
 uint64_t supplies{},inventory{};int32_t selected{};if(!Read(context.actor+0x5e0,&supplies)||!supplies||!Read(supplies+0x10,&selected)||selected<0||selected>40||!Read(supplies+0xb8,&inventory)||!inventory)return false;const uint64_t get=Virtual(context,inventory,0x38);if(!get)return false;
 const uint64_t stack=reinterpret_cast<uint64_t(*)(uint64_t,int)>(get)(inventory,selected);char full[64]{};if(!ValidStack(stack)||!Text(ItemOf(stack)+0xd8,full,sizeof(full)))return false;strncpy_s(name,capacity,Bare(full),_TRUNCATE);*slot=selected;*item=ItemOf(stack);return true;
}
// Java's transformFirstPersonItem(h, s).
inline Mat4 FirstPerson(float h,float s){const float root=std::sqrt(std::max(s,0.f)),pi=3.14159265f;return Translation(.56f,-.52f-.6f*h,-.72f)*Rotation(45,0,1,0)*Rotation(-20*std::sin(s*s*pi),0,1,0)*Rotation(-20*std::sin(root*pi),0,0,1)*Rotation(-80*std::sin(root*pi),1,0,0);}
struct InspectKey{float time,x,y,z,ax,ay,az;};
inline const InspectKey* InspectTable(int which,int* count){
 static const InspectKey spin[]={{0,0,0,0,0,0,0},{350,-.17f,.12f,-.12f,0,-30,10},{650,-.17f,.13f,-.12f,10,-30,10},{1350,-.17f,.14f,-.12f,-350,-30,10},{1800,-.17f,.13f,-.12f,-360,-30,10},{2400,0,0,0,-360,0,0}};
 static const InspectKey toss[]={{0,0,0,0,0,0,0},{350,-.16f,.06f,-.12f,15,-25,0},{550,-.16f,.04f,-.12f,25,-25,0},{1000,-.16f,.4f,-.12f,-180,-25,0},{1450,-.16f,.05f,-.12f,-360,-25,0},{1650,-.16f,.07f,-.12f,-355,-25,0},{2250,0,0,0,-360,0,0}};
 static const InspectKey twirl[]={{0,0,0,0,0,0,0},{300,-.16f,.15f,-.12f,-10,-20,0},{700,-.16f,.15f,-.12f,-10,-45,0},{1500,-.15f,.16f,-.12f,-10,-45,720},{1800,-.15f,.15f,-.12f,-5,-45,720},{2400,0,0,0,0,0,720}};
 static const InspectKey sword[]={{0,0,0,0,0,0,0},{350,-.18f,.13f,-.12f,0,-35,20},{800,-.18f,.13f,-.12f,10,-80,35},{1200,-.17f,.14f,-.12f,12,-82,35},{1750,-.16f,.14f,-.12f,10,-80,395},{2000,-.16f,.13f,-.12f,8,-70,395},{2500,0,0,0,0,0,360}};
 static const InspectKey tool[]={{0,0,0,0,0,0,0},{350,-.16f,.12f,-.12f,-15,-30,10},{900,-.16f,.13f,-.12f,-18,-35,12},{1500,-.16f,.13f,-.12f,-15,150,10},{1900,-.16f,.13f,-.12f,-10,155,15},{2400,0,0,0,0,0,0}};
 static const InspectKey bow[]={{0,0,0,0,0,0,0},{350,-.2f,.09f,-.12f,0,-20,0},{900,-.2f,.1f,-.12f,0,-70,-15},{1400,-.19f,.1f,-.12f,5,-75,-10},{1900,-.18f,.1f,-.12f,365,-75,-10},{2400,0,0,0,360,0,0}};
 static const InspectKey plain[]={{0,0,0,0,0,0,0},{350,-.16f,.12f,-.12f,15,-20,0},{900,-.16f,.12f,-.12f,15,-60,0},{1400,-.16f,.13f,-.12f,15,300,0},{1800,-.16f,.12f,-.12f,10,300,0},{2400,0,0,0,0,360,0}};
 switch(which){case 1:*count=6;return spin;case 2:*count=7;return toss;case 3:*count=6;return twirl;case 10:*count=7;return sword;case 11:*count=6;return tool;case 12:*count=6;return bow;default:*count=6;return plain;}
}
struct HandSaved{uint64_t top=0,stack=0;Mat4 copy{};};
// Computes and writes the stack top for one hand; true if something was written (then `saved` must be restored).
inline bool HandWork(void* self,void* ctx,void* actor,int hand,HandSaved* saved){
 auto& shared=Hand();AcquireSRWLockShared(&shared.lock);const bool viewModel=shared.viewModel,blockHit=shared.blockHit,always=shared.always,allItems=shared.allItems,inspect=shared.inspect,cancelOnAttack=shared.cancelOnAttack;const int animation=shared.animation,inspectMode=shared.inspectMode;
 const float blockScale=shared.blockScale,inspectSpeed=shared.inspectSpeed;float offset[3];memcpy(offset,hand?shared.offOffset:shared.mainOffset,12);ReleaseSRWLockShared(&shared.lock);
 if(!viewModel&&!blockHit&&!inspect)return false;Context context{};if(!Local(&context)||(uint64_t)actor!=context.actor)return false;
 uint64_t holder{},camera{},top{},parent{};if(!Read((uintptr_t)ctx+0x28,&holder)||!holder||!Read(holder+0x18,&camera)||!camera)return false;const uint64_t stack=camera+0x40;if(!StackLevel(stack,0,&top)||!StackLevel(stack,1,&parent))return false;
 Mat4 current{},under{},inverse{};if(!Floats(top,current.m,16)||!Floats(parent,under.m,16)||!Inverse(under,&inverse))return false;
 Mat4 target=current,extra=Identity();float shift[3]={0,0,0};bool changed=false;const Mat4 local=inverse*current;const double now=HandClock();
 if(viewModel){shift[0]=hand?-offset[0]:offset[0];shift[1]=offset[1];shift[2]=-offset[2];changed=changed||shift[0]!=0||shift[1]!=0||shift[2]!=0;}
 const uint64_t useOf=Virtual(context,context.actor,0x498);const int using_=useOf?reinterpret_cast<int(*)(uint64_t)>(useOf)(context.actor):0;float swingNow{},swingOld{},alpha{};Read(context.actor+0x42c,&swingNow);Read(context.actor+0x478,&swingOld);Read((uintptr_t)ctx+0xb0,&alpha);alpha=std::clamp(alpha,0.f,1.f);
 char name[64]{};int slot=-1;uint64_t item=0;const bool holding=HeldName(context,name,sizeof(name),&slot,&item);const bool sword=holding&&EndsWith(name,"_sword");
 // Block Hit (main hand): 100 ms rise and fall, smoothed blend between the game pose and Java's blocking pose.
 if(hand==0){static double last=0;static float progress=0;const float dt=last?(float)((now-last)/100):0;last=now;
  if(!blockHit||using_>0)progress=0;else{const bool active=holding&&(allItems||sword)&&(always||((GetAsyncKeyState(VK_RBUTTON)&0x8000)&&shared.playing));progress=std::clamp(progress+(active?dt:-dt),0.f,1.f);}
  if(blockHit&&progress>0){float newer{},older{};Read((uintptr_t)self+0x18c,&newer);Read((uintptr_t)self+0x190,&older);const float h=1-(older+(newer-older)*alpha);float delta=swingNow-swingOld;if(delta<0)delta+=1;const float s=std::clamp(swingOld+alpha*delta,0.f,1.f),root=std::sqrt(s),pi=3.14159265f;
   const Mat4 swing=Translation(-.4f*std::sin(root*pi),.2f*std::sin(2*root*pi),-.2f*std::sin(s*pi))*FirstPerson(h,s);Mat4 swingInverse{};
   if(Inverse(swing,&swingInverse)){const Mat4 base=swingInverse*local,pose=animation==2?Translation(-.04f,.13f,0)*FirstPerson(h/2.5f,0):FirstPerson(h,0),block=Translation(-.5f,.2f,0)*Rotation(30,0,1,0)*Rotation(-80,1,0,0)*Rotation(60,0,1,0);
    Mat4 goal=pose*base*block;for(int i=0;i<12;++i)goal.m[i]*=blockScale;const float t=(3-2*progress)*progress*progress;target=under*(t>=1?goal:Blend(local,goal,t));changed=true;}}}
 // Item Inspect (main hand): interpolated keys (angles in degrees), 175 ms fade in and out.
 if(hand==0){struct Step{float v[6];};static int state=0,table=0,heldSlot=-1;static uint64_t heldItem=0;static double last=0;static float time=0,blend=0;static Step start{},shown{};const float dt=last?(float)(now-last):0;last=now;
  if(inspect&&InterlockedExchange(&shared.inspectRequest,0)&&holding){state=1;time=0;blend=0;start=shown;heldSlot=slot;heldItem=item;
   table=inspectMode?inspectMode:(!strcmp(name,"trident")||EndsWith(name,"_spear"))?3:(!strcmp(name,"crossbow")||!strcmp(name,"bow"))?12:sword?10:(EndsWith(name,"_axe")||EndsWith(name,"_pickaxe")||EndsWith(name,"_shovel")||EndsWith(name,"_hoe"))?11:13;}
  if(state==1&&(!holding||slot!=heldSlot||item!=heldItem||using_>=1||!shared.playing||(cancelOnAttack&&swingNow>0)||!inspect)){state=2;blend=0;start=shown;}
  if(state==1){time+=inspectSpeed*dt;blend+=dt;int count=0;const InspectKey* keys=InspectTable(table,&count);if(time>=keys[count-1].time){state=0;shown=Step{};}
   else{int next=1;while(next<count-1&&keys[next].time<=time)++next;const InspectKey& a=keys[next-1];const InspectKey& b=keys[next];float u=std::clamp((time-a.time)/(b.time-a.time),0.f,1.f);u=u*u*(3-2*u);const float goal[6]={a.x+(b.x-a.x)*u,a.y+(b.y-a.y)*u,a.z+(b.z-a.z)*u,a.ax+(b.ax-a.ax)*u,a.ay+(b.ay-a.ay)*u,a.az+(b.az-a.az)*u};
    float f=std::min(blend/175,1.f);f=f*f*(3-2*f);for(int i=0;i<6;++i)shown.v[i]=start.v[i]+(goal[i]-start.v[i])*f;}}
  else if(state==2){blend+=dt;float f=std::min(blend/175,1.f);f=f*f*(3-2*f);for(int i=0;i<6;++i)shown.v[i]=start.v[i]*(1-f);if(blend>=175){state=0;shown=Step{};}}
  if(state){extra=Translation(shown.v[0],shown.v[1],shown.v[2])*Rotation(shown.v[3],1,0,0)*Rotation(shown.v[4],0,1,0)*Rotation(shown.v[5],0,0,1);changed=true;}}
 if(!changed)return false;Mat4 targetLocal=inverse*target;const float p[3]={targetLocal.m[12],targetLocal.m[13],targetLocal.m[14]};
 const Mat4 result=under*Translation(shift[0],shift[1],shift[2])*(Translation(p[0],p[1],p[2])*extra*Translation(-p[0],-p[1],-p[2]))*targetLocal;for(float value:result.m)if(!std::isfinite(value))return false;
 saved->top=top;saved->stack=stack;saved->copy=current;memcpy((void*)top,result.m,64);*(uint8_t*)(stack+0x38)=1;InterlockedExchange(&shared.seen,1);return true;
}
inline bool HandGuarded(void* self,void* ctx,void* actor,int hand,HandSaved* saved){__try{return HandWork(self,ctx,actor,hand,saved);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Hand().fault,1);return false;}}
inline void HandRestore(const HandSaved& saved){__try{memcpy((void*)saved.top,saved.copy.m,64);*(uint8_t*)(saved.stack+0x38)=1;}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Hand().fault,1);}}
using HandRenderFunction=void(*)(void*,void*,void*,uint8_t);using MainHandFunction=void(*)(void*,void*,void*,void*,uint8_t,uint8_t,uint8_t,uint8_t);using OffHandFunction=void(*)(void*,void*,void*,void*,uint8_t);
inline HandRenderFunction handRenderOriginal=nullptr;inline MainHandFunction mainHandOriginal=nullptr;inline OffHandFunction offHandOriginal=nullptr;inline thread_local bool insideHandRender=false,insideMainHand=false;
// Freecam cancels ItemInHandRenderEvent unconditionally while active.
inline volatile long handRenderCancel=0;
// Hand highlight listeners (Hand Chams, game_highlight.hpp).
inline void(*handRenderPre)()=nullptr;inline void(*handRenderPost)()=nullptr;inline void(*offHandPre)(uint64_t*)=nullptr;inline void(*offHandPost)(uint64_t)=nullptr;
inline void HandRenderDetour(void* a,void* b,void* c,uint8_t d){if(handRenderCancel)return;insideHandRender=true;if(handRenderPre)handRenderPre();handRenderOriginal(a,b,c,d);if(handRenderPost)handRenderPost();insideHandRender=false;}
inline void MainHandDetour(void* self,void* ctx,void* actor,void* d,uint8_t e,uint8_t f,uint8_t g,uint8_t h){HandSaved saved{};const bool written=h==1&&(f&1)&&insideHandRender&&!Hand().fault&&HandGuarded(self,ctx,actor,0,&saved);insideMainHand=true;mainHandOriginal(self,ctx,actor,d,e,f,g,h);insideMainHand=false;if(written)HandRestore(saved);}
inline void OffHandDetour(void* self,void* ctx,void* c,void* d,uint8_t e){uint64_t mark=~0ull;if(offHandPre)offHandPre(&mark);
 if(!insideHandRender||insideMainHand||Hand().fault){offHandOriginal(self,ctx,c,d,e);if(offHandPost)offHandPost(mark);return;}Context context{};HandSaved saved{};const bool written=Local(&context)&&HandGuarded(self,ctx,(void*)context.actor,1,&saved);offHandOriginal(self,ctx,c,d,e);if(written)HandRestore(saved);if(offHandPost)offHandPost(mark);}

// ---- No Armor
using AttachableFunction=void(*)(void*,void*,uint32_t*,void*,void*);inline AttachableFunction attachableOriginal=nullptr;
inline bool ArmorHidden(void* stack,uint32_t* slotPointer,void* actor){
 auto& shared=Hand();if(!shared.noArmor)return false;uint32_t slot{};if(!Read((uintptr_t)slotPointer,&slot)||slot>3||!shared.armorSlot[slot])return false;
 if(shared.armorTarget){Context context{};const bool self=Local(&context)&&(uint64_t)actor==context.actor;if((shared.armorTarget==1)!=self)return false;}
 char name[64]{};const uint64_t item=ItemOf((uint64_t)stack);if(!item||!Text(item+0xd8,name,sizeof(name)))return false;
 return EndsWith(name,"_helmet")||EndsWith(name,"_cap")||EndsWith(name,"_chestplate")||EndsWith(name,"_tunic")||EndsWith(name,"_leggings")||EndsWith(name,"_boots");
}
inline void AttachableDetour(void* a,void* stack,uint32_t* slot,void* d,void* actor){if(ArmorHidden(stack,slot,actor))return;attachableOriginal(a,stack,slot,d,actor);}

// ---- Block Outline: drawn with the game's Tessellator (camera-facing quad ribbons, then flushed)
struct HashedName{uint64_t hash;std::string text;uint64_t last;};
// The game's HashedString is 64-bit FNV-1 (multiply THEN xor), not FNV-1a (name_tag -> 0x9d4d4085a61f9885,
// name_tag_depth_tested -> 0xc2fad6537c4a4e6d). FNV-1a looks up an unusable material and nothing gets drawn.
inline uint64_t HashedStringHash(const std::string& text){uint64_t hash=0xcbf29ce484222325ull;for(unsigned char byte:text)hash=(hash*0x100000001b3ull)^byte;return hash;}
inline uint64_t* Material(const Context& context,bool depthTested){
 static uint64_t pointers[2][2]{};static bool made[2]{};const int which=depthTested?1:0;if(made[which])return pointers[which][0]?pointers[which]:nullptr;made[which]=true;
 static HashedName* names[2]{};names[which]=new HashedName{};names[which]->text=depthTested?"name_tag_depth_tested":"name_tag";names[which]->hash=HashedStringHash(names[which]->text);
 const uint64_t group=context.base+0x129c8290,make=Virtual(context,group,8);if(!make)return nullptr;reinterpret_cast<void*(*)(uint64_t,uint64_t*,HashedName*)>(make)(group,pointers[which],names[which]);return pointers[which][0]?pointers[which]:nullptr;
}
struct Pen{const Context* context;uint64_t tess,camera;Mat4 view,back;float nearPlane;};
inline void PenVertex(const Pen& pen,const float* color,float x,float y,float z){reinterpret_cast<void(*)(uint64_t,float,float,float,float)>(pen.context->base+0x5417890)(pen.tess,color[0],color[1],color[2],color[3]);reinterpret_cast<void(*)(uint64_t,float,float,float)>(pen.context->base+0x5417950)(pen.tess,x,y,z);}
inline void PenQuad(const Pen& pen,const float* color,const float corners[4][3]){for(int i=0;i<4;++i)PenVertex(pen,color,corners[i][0],corners[i][1],corners[i][2]);for(int i=3;i>=0;--i)PenVertex(pen,color,corners[i][0],corners[i][1],corners[i][2]);}
inline void PenSegment(const Pen& pen,const float* a,const float* b,float thickness,const float* colorA,const float* colorB){
 auto toView=[&](const float* p,float* out){for(int row=0;row<3;++row)out[row]=pen.view.m[row]*p[0]+pen.view.m[4+row]*p[1]+pen.view.m[8+row]*p[2]+pen.view.m[12+row];};float va[3],vb[3];toView(a,va);toView(b,vb);const float n=pen.nearPlane;
 if(-va[2]<n&&-vb[2]<n)return;if(-va[2]<n||-vb[2]<n){float* behind=-va[2]<n?va:vb;const float* front=-va[2]<n?vb:va;const float t=(-n-behind[2])/(front[2]-behind[2]);for(int i=0;i<3;++i)behind[i]+=(front[i]-behind[i])*t;}
 float dx=vb[0]/vb[2]-va[0]/va[2],dy=vb[1]/vb[2]-va[1]/va[2];const float length=std::sqrt(dx*dx+dy*dy);if(length<=1e-5f)return;dx/=length;dy/=length;const float wa=thickness*.001f*-va[2],wb=thickness*.001f*-vb[2];
 const float view[4][3]={{va[0]-dy*wa,va[1]+dx*wa,va[2]},{va[0]+dy*wa,va[1]-dx*wa,va[2]},{vb[0]+dy*wb,vb[1]-dx*wb,vb[2]},{vb[0]-dy*wb,vb[1]+dx*wb,vb[2]}};float world[4][3];
 for(int i=0;i<4;++i)for(int row=0;row<3;++row)world[i][row]=pen.back.m[row]*view[i][0]+pen.back.m[4+row]*view[i][1]+pen.back.m[8+row]*view[i][2]+pen.back.m[12+row];
 for(int i=0;i<4;++i)PenVertex(pen,i<2?colorA:colorB,world[i][0],world[i][1],world[i][2]);for(int i=3;i>=0;--i)PenVertex(pen,i<2?colorA:colorB,world[i][0],world[i][1],world[i][2]);
}
inline void PenSegment(const Pen& pen,const float* a,const float* b,float thickness,const float* color){PenSegment(pen,a,b,thickness,color,color);}
inline void PenBegin(const Pen& pen,int vertices){reinterpret_cast<void(*)(uint64_t,int,int,int,bool)>(pen.context->base+0x54175a0)(pen.tess,0,1,vertices,false);}
inline void PenFlush(const Pen& pen,uint64_t ctx,uint64_t* material){
 uint64_t colour{},top{};const uint64_t stack=pen.camera+0x40;if(!Read(ctx+0x30,&colour)||!colour)return;const bool hasTop=StackLevel(stack,0,&top);uint8_t savedColour[17];float savedShift[3];Mat4 savedTop{};memcpy(savedColour,(void*)colour,17);memcpy(savedShift,(void*)(pen.tess+0x1cc),12);
 const float white[4]={1,1,1,1},zero[3]={0,0,0};memcpy((void*)colour,white,16);*(uint8_t*)(colour+16)=1;memcpy((void*)(pen.tess+0x1cc),zero,12);
 if(hasTop){memcpy(savedTop.m,(void*)top,64);Mat4 scaled=savedTop;for(int c=0;c<4;++c)for(int row=0;row<3;++row)scaled.m[c*4+row]*=.999f;memcpy((void*)top,scaled.m,64);*(uint8_t*)(stack+0x38)=1;}
 alignas(16) uint8_t blank[0x58]{};reinterpret_cast<void(*)(uint64_t,uint64_t,uint64_t*,uint8_t*)>(pen.context->base+0x5ad6750)(ctx,pen.tess,material,blank);
 memcpy((void*)colour,savedColour,17);memcpy((void*)(pen.tess+0x1cc),savedShift,12);if(hasTop){memcpy((void*)top,savedTop.m,64);*(uint8_t*)(stack+0x38)=1;}
}
inline bool PenOpen(const Context& context,uint64_t ctx,Pen* pen){uint64_t top{};pen->context=&context;if(!Read(ctx+0xb0,&pen->tess)||!pen->tess||!Read(ctx+0x18,&pen->camera)||!pen->camera||!StackLevel(pen->camera,0,&top)||!Floats(top,pen->view.m,16)||!Floats(pen->camera+0xc0,pen->back.m,16)||!Read(pen->camera+0x138,&pen->nearPlane))return false;pen->nearPlane=std::max(.001f,pen->nearPlane);return true;}
inline void OutlineWork(void* renderer,void* holder,const int* position){
 auto& shared=Hand();Context context{};if(!Local(&context))return;uint64_t ctx{};float camera[3];if(!Read((uintptr_t)holder+0x28,&ctx)||!ctx||!Floats((uintptr_t)renderer+0x658,camera,3))return;Pen pen{};if(!PenOpen(context,ctx,&pen))return;
 uint64_t* material=Material(context,shared.visibleOnly);if(!material)return;float lo[3],hi[3];for(int i=0;i<3;++i){lo[i]=position[i]-camera[i];hi[i]=lo[i]+1;}
 auto corner=[&](int index,const float* low,const float* high,float* out){static const int pick[8][3]={{0,0,0},{1,0,0},{1,0,1},{0,0,1},{0,1,0},{1,1,0},{1,1,1},{0,1,1}};for(int i=0;i<3;++i)out[i]=pick[index][i]?high[i]:low[i];};
 static const int faces[6][4]={{0,1,2,3},{4,5,6,7},{0,1,5,4},{3,2,6,7},{0,3,7,4},{1,2,6,5}};   // bottom, top, north (z-), south (z+), west (x-), east (x+)
 if(shared.overlay){float low[3],high[3];for(int i=0;i<3;++i){low[i]=lo[i]-.002f;high[i]=hi[i]+.002f;}int only=-1;bool draw=true;
  if(shared.faceOnly){draw=false;uint64_t level{},hit{};int32_t kind{},face{},at[3]{};if(Read(context.actor+0x200,&level)&&level){const uint64_t get=Virtual(context,level,0xa78);if(get&&CallGetter(get,level,&hit)&&hit&&Read(hit+0x18,&kind)&&kind==0&&Read(hit+0x1c,&face)&&Read(hit+0x20,&at[0])&&Read(hit+0x24,&at[1])&&Read(hit+0x28,&at[2])&&at[0]==position[0]&&at[1]==position[1]&&at[2]==position[2]&&face>=0&&face<6){only=face;draw=true;}}}
  if(draw){PenBegin(pen,only>=0?8:0x30);for(int f=0;f<6;++f){if(only>=0&&f!=only)continue;float quad[4][3];for(int i=0;i<4;++i)corner(faces[f][i],low,high,quad[i]);PenQuad(pen,shared.overlayColor,quad);}PenFlush(pen,ctx,material);}}
 {const float centre[3]={(lo[0]+hi[0])/2,(lo[1]+hi[1])/2,(lo[2]+hi[2])/2},pad=std::max(.001f,std::sqrt(centre[0]*centre[0]+centre[1]*centre[1]+centre[2]*centre[2])*shared.thickness*.001f);float low[3],high[3];for(int i=0;i<3;++i){low[i]=lo[i]-pad;high[i]=hi[i]+pad;}
  static const int edges[12][2]={{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};PenBegin(pen,0x60);for(auto& edge:edges){float a[3],b[3];corner(edge[0],low,high,a);corner(edge[1],low,high,b);PenSegment(pen,a,b,shared.thickness,shared.outlineColor);}PenFlush(pen,ctx,material);}
 InterlockedExchange(&shared.outlineSeen,1);
}
inline void OutlineGuarded(void* renderer,void* holder,const int* position){__try{OutlineWork(renderer,holder,position);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&Hand().outlineFault,1);}}
using OutlineFunction=void(*)(void*,void*,void*,int*,uint8_t);inline OutlineFunction outlineOriginal=nullptr;
inline void OutlineDetour(void* renderer,void* holder,void* c,int* position,uint8_t e){auto& shared=Hand();if(!shared.outline||shared.outlineFault){outlineOriginal(renderer,holder,c,position,e);return;}OutlineGuarded(renderer,holder,position);}

// Installs a hook after checking its first bytes; 1 installed, -1 refused.
inline int InstallOne(uint32_t rva,const uint8_t* bytes,int count,void* detour,void** original,const char* what,void(*log)(const char*)){
 uintptr_t base{};uint32_t size{};char line[160]{};if(!Supported(&base,&size))return -1;for(int i=0;i<count;++i){uint8_t byte{};if(!Read(base+rva+i,&byte)||byte!=bytes[i]){snprintf(line,sizeof(line),"%s: unexpected bytes at the game function, no hook installed",what);if(log)log(line);return -1;}}
 if(MH_CreateHook((void*)(base+rva),detour,original)!=MH_OK||MH_EnableHook((void*)(base+rva))!=MH_OK){snprintf(line,sizeof(line),"%s: MinHook could not install the hook",what);if(log)log(line);return -1;}snprintf(line,sizeof(line),"%s: hook installed",what);if(log)log(line);return 1;
}
// Hook #230 is shared by the hand modules (View Model...) and Freecam; installed once.
inline const uint8_t handRenderBytes[24]={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0x68,0x02,0x00,0x00,0x48,0x8d,0xac,0x24,0x80};
inline int InstallHandRenderHook(void(*log)(const char*)){static int state=0;if(state)return state;state=InstallOne(0x532b5a0,handRenderBytes,24,(void*)HandRenderDetour,(void**)&handRenderOriginal,"Held item (render)",log);return state;}
inline int InstallHandHooks(void(*log)(const char*)){static int state=0;if(state)return state;state=-1;
 const uint8_t a[24]={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0x68,0x02,0x00,0x00,0x48,0x8d,0xac,0x24,0x80},b[24]={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0x88,0x08,0x00,0x00,0x48,0x8d,0xac,0x24,0x80};
 if(InstallHandRenderHook(log)!=1)return state;if(InstallOne(0x531b490,b,24,(void*)MainHandDetour,(void**)&mainHandOriginal,"Held item (main hand)",log)!=1)return state;
 if(InstallOne(0x531a580,a,24,(void*)OffHandDetour,(void**)&offHandOriginal,"Held item (offhand)",log)!=1)return state;state=1;return state;}
inline int InstallArmorHook(void(*log)(const char*)){static int state=0;if(state)return state;const uint8_t bytes[24]={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0x68,0x01,0x00,0x00,0x48,0x8d,0xac,0x24,0x80};state=InstallOne(0x58be290,bytes,24,(void*)AttachableDetour,(void**)&attachableOriginal,"No Armor",log);return state;}
inline int InstallOutlineHook(void(*log)(const char*)){static int state=0;if(state)return state;const uint8_t bytes[24]={0x41,0x57,0x41,0x56,0x56,0x57,0x55,0x53,0x48,0x83,0xec,0x48,0x4d,0x89,0xce,0x4c,0x89,0xc6,0x48,0x89,0xd7,0x48,0x89,0xcb};state=InstallOne(0x539de60,bytes,24,(void*)OutlineDetour,(void**)&outlineOriginal,"Block Outline",log);return state;}
// Third Person Nametags: the "je" of the nametag loop becomes two "nop"s; `on` false restores the game's bytes.
inline int PatchOwnNametag(bool on,void(*log)(const char*)){
 static bool applied=false;if(on==applied)return 1;uintptr_t base{};uint32_t size{};if(!Supported(&base,&size))return -1;uint8_t* at=(uint8_t*)(base+0x5397e23);const uint8_t before[10]={0x48,0x8b,0x1e,0x48,0x3b,0x9d,0xb8,0x00,0x00,0x00};
 for(int i=0;i<10;++i){uint8_t byte{};if(!Read(base+0x5397e19+i,&byte)||byte!=before[i]){if(log)log("Third Person Nametags: unexpected code before the patch site, nothing written");return -1;}}
 uint8_t now[2]{};if(!Read((uintptr_t)at,&now[0])||!Read((uintptr_t)at+1,&now[1]))return -1;const uint8_t jump[2]={0x74,0xdb},nops[2]={0x90,0x90};if(memcmp(now,on?jump:nops,2)){if(log)log("Third Person Nametags: unexpected bytes at the patch site, nothing written");return -1;}
 DWORD old{};if(!VirtualProtect(at,2,PAGE_EXECUTE_READWRITE,&old))return -1;memcpy(at,on?nops:jump,2);VirtualProtect(at,2,old,&old);FlushInstructionCache(GetCurrentProcess(),at,2);applied=on;if(log)log(on?"Third Person Nametags: patch applied":"Third Person Nametags: original bytes restored");return 1;
}
}
